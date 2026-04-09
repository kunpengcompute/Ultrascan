#ifndef PBE_RUNTIME_INLINE_H
#define PBE_RUNTIME_INLINE_H

#include "fdr_internal.h"
#include "hao_runtime.h"
#include "hs_compile.h"
#include "util/bitutils.h"
#include "util/compare.h"
#include "util/cpuid_flags.h"
#include "util/simd_utils.h"
#include "util/unaligned.h"

#include <string.h>

#define PBE_BATCH_FALLBACK_WIDTH 4U
#define PBE_BATCH_MAX_WIDTH 32U
#define PBE_BITMAP_GROUPED_BYTES 4U

struct PBEPositionContext {
    size_t endPos;
    u32 key;
    u32 validMask8;
    u8 laneWindowReady;
    u8 reserved0;
    u16 reserved1;
    u64a window64;
    u8 laneWindow32[PBE_RUNTIME_RULE_VECTOR_BYTES];
    u32 validMask32;
};

struct PBEBitmapProbeState {
    u32 laneCount;
    u32 byteIndex[PBE_BATCH_MAX_WIDTH];
    u8 bitShift[PBE_BATCH_MAX_WIDTH];
    u8 bitMask[PBE_BATCH_MAX_WIDTH];
    u8 gatheredBytes[PBE_BATCH_MAX_WIDTH];
    u32 activeMask;
    u32 activeLaneIndex[PBE_BATCH_MAX_WIDTH];
    u32 activePrimaryIdx[PBE_BATCH_MAX_WIDTH];
    u32 activeEncoded[PBE_BATCH_MAX_WIDTH];
};

struct PBEMaskClassBatchState {
    u32 laneCount;
    u32 classCount;
    u32 activeClassCount;
    u32 classKeys[PBE_RUNTIME_MAX_MASK_CLASSES][PBE_BATCH_MAX_WIDTH];
    u32 classEncoded[PBE_RUNTIME_MAX_MASK_CLASSES][PBE_BATCH_MAX_WIDTH];
    u32 classActiveMask[PBE_RUNTIME_MAX_MASK_CLASSES];
    u8 activeClassIndex[PBE_RUNTIME_MAX_MASK_CLASSES];
};

static really_inline
int pbeGetByteAt(const struct FDR_Runtime_Args *a, s64a pos, u8 *out) {
    if (!a || !out) {
        return 0;
    }
    if (pos >= 0) {
        if ((u64a)pos >= a->len) {
            return 0;
        }
        *out = a->buf[pos];
        return 1;
    }

    if (!a->buf_history || !a->len_history) {
        return 0;
    }

    // pos is negative, index from end of history.
    s64a idx = (s64a)a->len_history + pos;
    if (idx < 0 || (u64a)idx >= a->len_history) {
        return 0;
    }
    *out = a->buf_history[idx];
    return 1;
}

static really_inline
u8 pbeNormalizeByte(u8 c) {
    return ourisalpha(c) ? (u8)mytoupper((char)c) : c;
}

static really_inline
int pbeRuntimeCanUseBextFastPath(void) {
    static int cached = -1;
    if (cached != -1) {
        return cached;
    }
#if defined(HS_BUILD_HAVE_SVEBITPERM)
    cached = !!(cpuid_flags() & HS_CPU_FEATURES_SVEBITPERM);
#else
    cached = 0;
#endif
    return cached;
}

static really_inline
u32 pbeSuggestedBatchWidth(const struct PBERuntimeHeader *hdr) {
    if (hdr && hdr->extractMode == PBE_RUNTIME_EXTRACT_MODE_BEXT &&
        pbeRuntimeCanUseBextFastPath()) {
        u32 lanes = pbeExtractPackedBitsSveBitPermLaneCount();
        if (!lanes) {
            return PBE_BATCH_FALLBACK_WIDTH;
        }
        if (lanes > PBE_BATCH_MAX_WIDTH) {
            lanes = PBE_BATCH_MAX_WIDTH;
        }
        return lanes;
    }

    return PBE_BATCH_FALLBACK_WIDTH;
}

static really_inline
u64a pbeByteReverse64(u64a v) {
    v = ((v & 0x00ff00ff00ff00ffULL) << 8)
        | ((v >> 8) & 0x00ff00ff00ff00ffULL);
    v = ((v & 0x0000ffff0000ffffULL) << 16)
        | ((v >> 16) & 0x0000ffff0000ffffULL);
    return (v << 32) | (v >> 32);
}

static really_inline
int pbeCanDirectLoadCurrentWindow64(const struct FDR_Runtime_Args *a,
                                    size_t endPos, u32 windowBytes) {
    return a && a->buf && windowBytes == PBE_RUNTIME_BYTES_PER_RULE_SLOT &&
           endPos + 1 >= PBE_RUNTIME_BYTES_PER_RULE_SLOT;
}

static really_inline
u64a pbeLoadWindow64Normalized(const struct FDR_Runtime_Args *a,
                               size_t endPos, u32 windowBytes) {
    u64a window = 0;
    u32 i;

    if (!a) {
        return 0;
    }

    if (!windowBytes || windowBytes > PBE_RUNTIME_BYTES_PER_RULE_SLOT) {
        windowBytes = PBE_RUNTIME_BYTES_PER_RULE_SLOT;
    }

    if (pbeCanDirectLoadCurrentWindow64(a, endPos, windowBytes)) {
        const u8 *start = a->buf + endPos + 1 - windowBytes;
        return pbeByteReverse64(unaligned_load_u64a(start));
    }

    for (i = 0; i < windowBytes; i++) {
        u8 b = 0;
        if (!pbeGetByteAt(a, (s64a)endPos - (s64a)i, &b)) {
            continue;
        }
        window |= ((u64a)b) << (i * 8U);
    }

    return window;
}

static really_inline
u32 pbeComputeValidMask8(const struct FDR_Runtime_Args *a, size_t endPos) {
    u64a available;

    if (!a) {
        return 0;
    }

    available = (u64a)endPos + 1 + a->len_history;
    if (!available) {
        return 0;
    }
    if (available >= PBE_RUNTIME_BYTES_PER_RULE_SLOT) {
        return 0xffU;
    }

    return ((1U << (u32)available) - 1U)
           << (PBE_RUNTIME_BYTES_PER_RULE_SLOT - (u32)available);
}

static really_inline
void pbeBuildLaneWindow32FromWindow(u64a window, u32 validMask8,
                                    u8 *window32, u32 *validMask32) {
    u8 laneWindow[PBE_RUNTIME_BYTES_PER_RULE_SLOT] = {0};
    u32 j;
    u32 slot;

    if (!window32 || !validMask32) {
        return;
    }

    *validMask32 = 0;
    for (j = 0; j < PBE_RUNTIME_BYTES_PER_RULE_SLOT; j++) {
        const u32 srcByte = PBE_RUNTIME_BYTES_PER_RULE_SLOT - 1U - j;
        laneWindow[j] = pbeNormalizeByte((u8)(window >> (srcByte * 8U)));
    }

    for (slot = 0; slot < PBE_RUNTIME_RULE_SLOTS_PER_ENTRY; slot++) {
        const u32 laneBase = slot * PBE_RUNTIME_BYTES_PER_RULE_SLOT;
        memcpy(window32 + laneBase, laneWindow, PBE_RUNTIME_BYTES_PER_RULE_SLOT);
        *validMask32 |= (validMask8 << laneBase);
    }
}

static really_inline
u32 pbeExtractKeyScalarFromWindow(
    const struct PBERuntimeBitSelector *selectors, u32 selectorCount,
    u64a window) {
    u32 key = 0;
    u32 i;

    for (i = 0; i < selectorCount && i < PBE_RUNTIME_MAX_SELECTORS; i++) {
        const struct PBERuntimeBitSelector *s = &selectors[i];
        const u32 bitIndex = (u32)s->byteOffset * 8U + (u32)s->bitOffset;
        if (window & ((u64a)1 << bitIndex)) {
            key |= (1U << i);
        }
    }

    return key;
}

static really_inline
u64a pbeExtractPackedBitsFallback(u64a window, u64a mask) {
    u64a packed = 0;
    u32 outBit = 0;

    while (mask && outBit < PBE_RUNTIME_MAX_SELECTORS) {
        const u64a lowest = mask & (0 - mask);
        if (window & lowest) {
            packed |= ((u64a)1 << outBit);
        }
        mask &= mask - 1;
        outBit++;
    }

    return packed;
}

static really_inline
u32 pbePackedKeyMask(u32 selectorCount) {
    if (!selectorCount) {
        return 0;
    }
    if (selectorCount >= 32U) {
        return 0xffffffffU;
    }
    return (1U << selectorCount) - 1U;
}

static really_inline
u32 pbeProjectKeyToClass(u32 fullKey, u32 classMask) {
    return compress32(fullKey, classMask);
}

static really_inline
int pbeMaskClassIsHot(const struct PBERuntimeMaskClass *klass) {
    return klass && (klass->flags & PBE_RUNTIME_MASK_CLASS_FLAG_HOT);
}

static really_inline
u32 pbeExtractKeyBext(const struct PBERuntimeHeader *hdr, u64a window) {
    const u64a packed = pbeRuntimeCanUseBextFastPath()
                            ? pbeExtractPackedBitsSveBitPerm(window,
                                                             hdr->bextMask)
                            : pbeExtractPackedBitsFallback(window,
                                                           hdr->bextMask);
    return (u32)(packed & pbePackedKeyMask(hdr->selectorCount));
}

static really_inline
void pbeExtractKeysFromWindows(const struct PBERuntimeHeader *hdr,
                               const struct PBERuntimeBitSelector *selectors,
                               const u64a *windows, u32 count, u32 *keys) {
    u32 i;

    if (!hdr || !windows || !keys) {
        return;
    }

    if (hdr->extractMode == PBE_RUNTIME_EXTRACT_MODE_BEXT) {
        u64a packed[PBE_BATCH_MAX_WIDTH] = {0};
        const u32 keyMask = pbePackedKeyMask(hdr->selectorCount);

        assert(count <= PBE_BATCH_MAX_WIDTH);
        if (pbeRuntimeCanUseBextFastPath()) {
            pbeExtractPackedBitsSveBitPermBatch(windows, count, hdr->bextMask,
                                                packed);
        } else {
            for (i = 0; i < count; i++) {
                packed[i] = pbeExtractPackedBitsFallback(windows[i],
                                                         hdr->bextMask);
            }
        }

        for (i = 0; i < count; i++) {
            keys[i] = (u32)(packed[i] & keyMask);
        }
        return;
    }

    for (i = 0; i < count; i++) {
        keys[i] = pbeExtractKeyScalarFromWindow(selectors, hdr->selectorCount,
                                                windows[i]);
    }
}

static really_inline
u32 pbeExtractKeyFromWindow(const struct PBERuntimeHeader *hdr,
                            const struct PBERuntimeBitSelector *selectors,
                            u64a window) {
    if (hdr->extractMode == PBE_RUNTIME_EXTRACT_MODE_BEXT) {
        return pbeExtractKeyBext(hdr, window);
    }

    return pbeExtractKeyScalarFromWindow(selectors, hdr->selectorCount, window);
}

static really_inline
u8 haoNormalizeByte(u8 c) {
    return ourisalpha(c) ? (u8)mytoupper((char)c) : c;
}

static really_inline
int haoRuntimeCanUseBextFastPath(void) {
    return pbeRuntimeCanUseBextFastPath();
}

static really_inline
u64a haoByteReverse64(u64a v) {
    v = ((v & 0x00ff00ff00ff00ffULL) << 8)
        | ((v >> 8) & 0x00ff00ff00ff00ffULL);
    v = ((v & 0x0000ffff0000ffffULL) << 16)
        | ((v >> 16) & 0x0000ffff0000ffffULL);
    return (v << 32) | (v >> 32);
}

static really_inline
int haoCanDirectLoadCurrentWindow64(const struct FDR_Runtime_Args *a,
                                    size_t endPos, u32 windowBytes) {
    return a && a->buf && windowBytes == HAO_RUNTIME_BYTES_PER_RULE_SLOT &&
           endPos + 1 >= HAO_RUNTIME_BYTES_PER_RULE_SLOT;
}

static really_inline
u64a haoExtractPackedBitsFallback(u64a window, u64a mask) {
    return pbeExtractPackedBitsFallback(window, mask);
}

static really_inline
u64a haoLoadWindow64Normalized(const struct FDR_Runtime_Args *a,
                               size_t endPos, u32 windowBytes) {
    u64a window = 0;
    u32 i;

    if (!a) {
        return 0;
    }

    if (!windowBytes || windowBytes > HAO_RUNTIME_BYTES_PER_RULE_SLOT) {
        windowBytes = HAO_RUNTIME_BYTES_PER_RULE_SLOT;
    }

    if (haoCanDirectLoadCurrentWindow64(a, endPos, windowBytes)) {
        const u8 *start = a->buf + endPos + 1 - windowBytes;
        return haoByteReverse64(unaligned_load_u64a(start));
    }

    for (i = 0; i < windowBytes; i++) {
        u8 b = 0;
        if (!pbeGetByteAt(a, (s64a)endPos - (s64a)i, &b)) {
            continue;
        }
        window |= ((u64a)b) << (i * 8U);
    }

    return window;
}

static really_inline
u32 haoComputeValidMask8(const struct FDR_Runtime_Args *a, size_t endPos) {
    u64a available;

    if (!a) {
        return 0;
    }

    available = (u64a)endPos + 1 + a->len_history;
    if (!available) {
        return 0;
    }
    if (available >= HAO_RUNTIME_BYTES_PER_RULE_SLOT) {
        return 0xffU;
    }

    return ((1U << (u32)available) - 1U)
           << (HAO_RUNTIME_BYTES_PER_RULE_SLOT - (u32)available);
}

static really_inline
void haoBuildLaneWindow32FromWindow(u64a window, u32 validMask8,
                                    u8 *window32, u32 *validMask32) {
    u8 laneWindow[HAO_RUNTIME_BYTES_PER_RULE_SLOT] = {0};
    u32 j;
    u32 slot;

    if (!window32 || !validMask32) {
        return;
    }

    *validMask32 = 0;
    for (j = 0; j < HAO_RUNTIME_BYTES_PER_RULE_SLOT; j++) {
        const u32 srcByte = HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U - j;
        laneWindow[j] = haoNormalizeByte((u8)(window >> (srcByte * 8U)));
    }

    for (slot = 0; slot < HAO_RUNTIME_RULE_SLOTS_PER_ENTRY; slot++) {
        const u32 laneBase = slot * HAO_RUNTIME_BYTES_PER_RULE_SLOT;
        memcpy(window32 + laneBase, laneWindow, HAO_RUNTIME_BYTES_PER_RULE_SLOT);
        *validMask32 |= (validMask8 << laneBase);
    }
}

static really_inline
u32 haoExtractKeyScalarFromWindow(
    const struct HAORuntimeBitSelector *selectors, u32 selectorCount,
    u64a window) {
    u32 key = 0;
    u32 i;

    for (i = 0; i < selectorCount && i < HAO_RUNTIME_MAX_SELECTORS; i++) {
        const struct HAORuntimeBitSelector *s = &selectors[i];
        const u32 bitIndex = (u32)s->byteOffset * 8U + (u32)s->bitOffset;
        if (window & ((u64a)1 << bitIndex)) {
            key |= (1U << i);
        }
    }

    return key;
}

static really_inline
u32 haoPackedKeyMask(u32 selectorCount) {
    if (!selectorCount) {
        return 0;
    }
    if (selectorCount >= 32U) {
        return 0xffffffffU;
    }
    return (1U << selectorCount) - 1U;
}

static really_inline
u32 haoExtractKeyBext(const struct HAORuntimeHeader *hdr, u64a window) {
    const u64a packed = haoRuntimeCanUseBextFastPath()
                            ? pbeExtractPackedBitsSveBitPerm(window,
                                                             hdr->bextMask)
                            : haoExtractPackedBitsFallback(window,
                                                           hdr->bextMask);
    return (u32)(packed & haoPackedKeyMask(hdr->selectorCount));
}

static really_inline
void haoExtractKeysFromWindows(const struct HAORuntimeHeader *hdr,
                               const struct HAORuntimeBitSelector *selectors,
                               const u64a *windows, u32 count, u32 *keys) {
    u32 i;

    if (!hdr || !windows || !keys) {
        return;
    }

    if (hdr->extractMode == HAO_RUNTIME_EXTRACT_MODE_BEXT) {
        u64a packed[PBE_BATCH_MAX_WIDTH] = {0};
        const u32 keyMask = haoPackedKeyMask(hdr->selectorCount);

        assert(count <= PBE_BATCH_MAX_WIDTH);
        if (haoRuntimeCanUseBextFastPath()) {
            pbeExtractPackedBitsSveBitPermBatch(windows, count, hdr->bextMask,
                                                packed);
        } else {
            for (i = 0; i < count; i++) {
                packed[i] = haoExtractPackedBitsFallback(windows[i],
                                                         hdr->bextMask);
            }
        }

        for (i = 0; i < count; i++) {
            keys[i] = (u32)(packed[i] & keyMask);
        }
        return;
    }

    for (i = 0; i < count; i++) {
        keys[i] = haoExtractKeyScalarFromWindow(selectors, hdr->selectorCount,
                                                windows[i]);
    }
}

static really_inline
u32 haoExtractKeyFromWindow(const struct HAORuntimeHeader *hdr,
                            const struct HAORuntimeBitSelector *selectors,
                            u64a window) {
    if (hdr->extractMode == HAO_RUNTIME_EXTRACT_MODE_BEXT) {
        return haoExtractKeyBext(hdr, window);
    }

    return haoExtractKeyScalarFromWindow(selectors, hdr->selectorCount, window);
}

static really_inline
void pbeBuildPositionContext(const struct PBERuntimeHeader *hdr,
                             const struct PBERuntimeBitSelector *selectors,
                             const struct FDR_Runtime_Args *a, size_t endPos,
                             struct PBEPositionContext *ctx, int fillKey) {
    const u64a window64 =
        pbeLoadWindow64Normalized(a, endPos, hdr->windowBytes);
    const u32 validMask8 = pbeComputeValidMask8(a, endPos);

    assert(ctx);
    memset(ctx, 0, sizeof(*ctx));
    ctx->endPos = endPos;
    ctx->window64 = window64;
    ctx->validMask8 = validMask8;
    if (fillKey) {
        ctx->key = pbeExtractKeyFromWindow(hdr, selectors, window64);
    }
}

static really_inline
void pbeEnsureLaneWindowContext(struct PBEPositionContext *ctx) {
    if (!ctx || ctx->laneWindowReady) {
        return;
    }

    pbeBuildLaneWindow32FromWindow(ctx->window64, ctx->validMask8,
                                   ctx->laneWindow32, &ctx->validMask32);
    ctx->laneWindowReady = 1;
}

static really_inline
int pbePrimaryBitmapHasValue(const u8 *bitmap, u32 bitmapSize, u32 idx) {
    if (!bitmap || idx / 8U >= bitmapSize) {
        return 0;
    }
    return !!(bitmap[idx / 8U] & (1U << (idx % 8U)));
}

static really_inline
u32 pbeEntryLaneMaskFromByteMatches(
    const struct PBERuntimeSecondaryHashEntry *entry, u32 byteMatchMask) {
    const u32 tailOnlyMask = entry->tailMask & ~entry->headMask;
    u32 laneMask = 0;
    u32 slot;

    for (slot = 0; slot < entry->ruleCount &&
                   slot < PBE_RUNTIME_RULE_SLOTS_PER_ENTRY; slot++) {
        const u32 laneBase = slot * PBE_RUNTIME_BYTES_PER_RULE_SLOT;
        const u32 laneBits = 0xffU << laneBase;
        const u32 laneTail = tailOnlyMask & laneBits;
        const u32 laneHead = entry->headMask & laneBits;

        if (!(entry->tailMask & laneBits)) {
            continue;
        }
        if (laneTail && ((byteMatchMask & laneTail) != laneTail)) {
            continue;
        }
        if (laneHead && ((byteMatchMask & laneHead) != laneHead)) {
            continue;
        }
        laneMask |= (1U << slot);
    }

    return laneMask;
}

static really_inline
u32 pbeEntryMatchMaskFromContextScalar(
    const struct PBERuntimeSecondaryHashEntry *entry,
    struct PBEPositionContext *ctx) {
    u32 byteMatchMask = 0;
    u32 i;

    if (!entry || !entry->ruleCount || !ctx) {
        return 0;
    }

    pbeEnsureLaneWindowContext(ctx);

    for (i = 0; i < PBE_RUNTIME_RULE_VECTOR_BYTES; i++) {
        const u32 bit = 1U << i;
        if (!(entry->tailMask & bit) || !(ctx->validMask32 & bit)) {
            continue;
        }
        if (ctx->laneWindow32[i] == entry->ruleVector[i]) {
            byteMatchMask |= bit;
        }
    }

    return pbeEntryLaneMaskFromByteMatches(entry, byteMatchMask);
}

static really_inline
u32 pbeEntrySingleSlotMatchMaskFromContext(
    const struct PBERuntimeSecondaryHashEntry *entry,
    struct PBEPositionContext *ctx) {
    const u32 tailOnlyMask = entry->tailMask & ~entry->headMask;
    u32 mask = tailOnlyMask;

    if (!entry || !ctx || !entry->ruleCount) {
        return 0;
    }

    pbeEnsureLaneWindowContext(ctx);

    while (mask) {
        const u32 bit = mask & (0U - mask);
        const u32 idx = ctz32(mask);
        if (!(ctx->validMask32 & bit) ||
            ctx->laneWindow32[idx] != entry->ruleVector[idx]) {
            return 0;
        }
        mask &= mask - 1;
    }

    mask = entry->headMask;
    while (mask) {
        const u32 bit = mask & (0U - mask);
        const u32 idx = ctz32(mask);
        if (!(ctx->validMask32 & bit) ||
            ctx->laneWindow32[idx] != entry->ruleVector[idx]) {
            return 0;
        }
        mask &= mask - 1;
    }

    return 1;
}

static really_inline
u32 pbeEntryMatchMaskFromContextVector(
    const struct PBERuntimeSecondaryHashEntry *entry,
    struct PBEPositionContext *ctx) {
    if (!entry || !entry->ruleCount || !ctx) {
        return 0;
    }

    pbeEnsureLaneWindowContext(ctx);

    if (entry->ruleCount == 1) {
        return pbeEntrySingleSlotMatchMaskFromContext(entry, ctx);
    }

#if defined(HAVE_NEON) || defined(HAVE_SSE2)
    {
        const m128 inputLo = loadu128(ctx->laneWindow32);
        const m128 inputHi = loadu128(ctx->laneWindow32 + 16);
        const m128 ruleLo = loadu128(entry->ruleVector);
        const m128 ruleHi = loadu128(entry->ruleVector + 16);
        const u32 eqLo = movemask128(eq128(inputLo, ruleLo));
        const u32 eqHi = movemask128(eq128(inputHi, ruleHi));
        const u32 byteMatchMask = (eqLo | (eqHi << 16)) & ctx->validMask32 &
                                  entry->tailMask;

        return pbeEntryLaneMaskFromByteMatches(entry, byteMatchMask);
    }
#else
    return pbeEntryMatchMaskFromContextScalar(entry, ctx);
#endif
}

static really_inline
u32 pbeBuildBatchContexts(const struct PBERuntimeHeader *hdr,
                          const struct PBERuntimeBitSelector *selectors,
                          const struct FDR_Runtime_Args *a, size_t startPos,
                          u32 batchWidth, struct PBEPositionContext *ctxs) {
    u64a windows[PBE_BATCH_MAX_WIDTH] = {0};
    u32 keys[PBE_BATCH_MAX_WIDTH] = {0};
    u32 laneCount = 0;

    assert(batchWidth <= PBE_BATCH_MAX_WIDTH);

    while (laneCount < batchWidth && startPos + laneCount < a->len) {
        const size_t pos = startPos + laneCount;
        pbeBuildPositionContext(hdr, selectors, a, pos, &ctxs[laneCount], 0);
        windows[laneCount] = ctxs[laneCount].window64;
        laneCount++;
    }

    pbeExtractKeysFromWindows(hdr, selectors, windows, laneCount, keys);
    for (u32 lane = 0; lane < laneCount; lane++) {
        ctxs[lane].key = keys[lane];
    }

    return laneCount;
}

static really_inline
void pbeProjectBatchKeysToClass(const struct PBEPositionContext *ctxs,
                                u32 laneCount, u32 classMask, u32 fullKeyMask,
                                u32 *classKeys) {
    u32 lane;

    if (!ctxs || !classKeys || laneCount > PBE_BATCH_MAX_WIDTH) {
        return;
    }

    if (classMask == fullKeyMask) {
        for (lane = 0; lane < laneCount; lane++) {
            classKeys[lane] = ctxs[lane].key;
        }
        return;
    }

    for (lane = 0; lane < laneCount; lane++) {
        classKeys[lane] = pbeProjectKeyToClass(ctxs[lane].key, classMask);
    }
}

static really_inline
void pbePrepareBitmapProbeStateFromPrimaryIdx(const u32 *primaryIdx,
                                              u32 laneCount,
                                              struct PBEBitmapProbeState *state) {
    u32 lane;

    if (!primaryIdx || !state || laneCount > PBE_BATCH_MAX_WIDTH) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->laneCount = laneCount;
    for (lane = 0; lane < laneCount; lane++) {
        state->activePrimaryIdx[lane] = primaryIdx[lane];
        state->byteIndex[lane] = primaryIdx[lane] >> 3;
        state->bitShift[lane] = (u8)(primaryIdx[lane] & 7U);
        state->bitMask[lane] = (u8)(1U << state->bitShift[lane]);
    }
}

static really_inline
u32 pbeProbeBitmapScalar(const u8 *bitmap, u32 bitmapSize,
                         const struct PBEBitmapProbeState *state) {
    u32 activeMask = 0;
    u32 lane;

    if (!state) {
        return 0;
    }

    for (lane = 0; lane < state->laneCount; lane++) {
        if (pbePrimaryBitmapHasValue(bitmap, bitmapSize,
                                     state->activePrimaryIdx[lane])) {
            activeMask |= (1U << lane);
        }
    }

    return activeMask;
}

static really_inline
int pbeProbeBitmapGrouped(const u8 *bitmap, u32 bitmapSize,
                          const struct PBEBitmapProbeState *state,
                          u32 *activeMaskOut) {
    u32 lane;
    u32 minByte;
    u32 maxByte;
    u32 activeMask = 0;

    if (!bitmap || !state || !state->laneCount || !activeMaskOut) {
        return 0;
    }

    minByte = state->byteIndex[0];
    maxByte = state->byteIndex[0];
    for (lane = 0; lane < state->laneCount; lane++) {
        const u32 idx = state->byteIndex[lane];
        if (idx >= bitmapSize) {
            return 0;
        }
        if (idx < minByte) {
            minByte = idx;
        }
        if (idx > maxByte) {
            maxByte = idx;
        }
    }

    if (maxByte - minByte + 1 > PBE_BITMAP_GROUPED_BYTES) {
        return 0;
    }

    for (lane = 0; lane < state->laneCount; lane++) {
        const u8 byte = bitmap[state->byteIndex[lane]];
        if (byte & state->bitMask[lane]) {
            activeMask |= (1U << lane);
        }
    }

    *activeMaskOut = activeMask;
    return 1;
}

static really_inline
u32 pbeProbeBitmapPacked(const u8 *bitmap, u32 bitmapSize,
                         struct PBEBitmapProbeState *state) {
    u32 activeMask = 0;
    u32 lane;

    if (!state) {
        return 0;
    }

    if (pbeProbeBitmapGrouped(bitmap, bitmapSize, state, &activeMask)) {
        state->activeMask = activeMask;
        return activeMask;
    }

    for (lane = 0; lane < state->laneCount; lane++) {
        const u32 idx = state->byteIndex[lane];
        state->gatheredBytes[lane] = idx < bitmapSize ? bitmap[idx] : 0;
    }

#if defined(HAVE_NEON) || defined(HAVE_SSE2)
    for (lane = 0; lane < state->laneCount; lane += 16U) {
        const u32 lanesThisRound = MIN(16U, state->laneCount - lane);
        const u32 laneMask = lanesThisRound == 16U
                                 ? 0xffffU
                                 : ((1U << lanesThisRound) - 1U);
        const m128 gathered = loadu128(state->gatheredBytes + lane);
        const m128 masks = loadu128(state->bitMask + lane);
        const m128 masked = and128(gathered, masks);
        const u32 zeroMask = movemask128(eq128(masked, zeroes128()));
        activeMask |= ((~zeroMask) & laneMask) << lane;
    }
#else
    for (lane = 0; lane < state->laneCount; lane++) {
        if (state->gatheredBytes[lane] & state->bitMask[lane]) {
            activeMask |= (1U << lane);
        }
    }
#endif

    state->activeMask = activeMask;
    return activeMask;
}

static really_inline
u32 pbeCompactAndLoadPrimary(const u32 *primaryHashTable,
                             struct PBEBitmapProbeState *state) {
    u32 activeCount = 0;
    u32 lane;

    if (!primaryHashTable || !state) {
        return 0;
    }

    for (lane = 0; lane < state->laneCount; lane++) {
        if (!(state->activeMask & (1U << lane))) {
            continue;
        }
        state->activePrimaryIdx[activeCount] = state->activePrimaryIdx[lane];
        state->activeLaneIndex[activeCount] = lane;
        state->activeEncoded[activeCount] =
            primaryHashTable[state->activePrimaryIdx[lane]];
        activeCount++;
    }

    return activeCount;
}

static really_inline
void pbeFillClassEncodedValues(const u8 *primaryBitmap, u32 bitmapSize,
                               const u32 *primaryHashTable,
                               const u32 *classKeys, u32 laneCount,
                               u32 *encodedOut, u32 *activeMaskOut) {
    struct PBEBitmapProbeState probe;
    u32 activeCount;
    u32 i;

    if (!primaryBitmap || !primaryHashTable || !classKeys || !encodedOut ||
        !activeMaskOut || laneCount > PBE_BATCH_MAX_WIDTH) {
        return;
    }

    memset(encodedOut, 0, sizeof(u32) * laneCount);
    *activeMaskOut = 0;

    pbePrepareBitmapProbeStateFromPrimaryIdx(classKeys, laneCount, &probe);
    *activeMaskOut = pbeProbeBitmapPacked(primaryBitmap, bitmapSize, &probe);
    activeCount = pbeCompactAndLoadPrimary(primaryHashTable, &probe);

    for (i = 0; i < activeCount; i++) {
        encodedOut[probe.activeLaneIndex[i]] = probe.activeEncoded[i];
    }
}

static really_inline
void pbeBuildMaskClassBatchState(const struct PBERuntimeHeader *hdr,
                                 const struct PBERuntimeMaskClass *classes,
                                 const struct PBEPositionContext *ctxs,
                                 u32 laneCount,
                                 struct PBEMaskClassBatchState *state) {
    u32 classIdx;
    const u32 fullKeyMask = pbePackedKeyMask(hdr->keyBits);

    if (!hdr || !classes || !ctxs || !state ||
        hdr->classCount > PBE_RUNTIME_MAX_MASK_CLASSES ||
        laneCount > PBE_BATCH_MAX_WIDTH) {
        return;
    }

    state->laneCount = laneCount;
    state->classCount = hdr->classCount;
    state->activeClassCount = 0;

    for (classIdx = 0; classIdx < hdr->classCount; classIdx++) {
        const struct PBERuntimeMaskClass *klass = &classes[classIdx];
        const u8 *primaryBitmap =
            (const u8 *)hdr + klass->primaryBitmapOffset;
        const u32 *primaryHashTable =
            (const u32 *)((const u8 *)hdr + klass->primaryOffset);

        if (!pbeMaskClassIsHot(klass)) {
            continue;
        }

        pbeProjectBatchKeysToClass(ctxs, laneCount, klass->classMask,
                                   fullKeyMask,
                                   state->classKeys[classIdx]);
        pbeFillClassEncodedValues(primaryBitmap, klass->primaryBitmapSize,
                                  primaryHashTable,
                                  state->classKeys[classIdx], laneCount,
                                  state->classEncoded[classIdx],
                                  &state->classActiveMask[classIdx]);
        if (state->classActiveMask[classIdx]) {
            state->activeClassIndex[state->activeClassCount++] = (u8)classIdx;
        }
    }
}

static really_inline
void pbeDecodePrimaryValue(u32 encoded, u32 *offset, u32 *count) {
    if (offset) {
        *offset = encoded & PBE_RUNTIME_L1_OFFSET_MASK;
    }
    if (count) {
        *count = encoded >> PBE_RUNTIME_L1_COUNT_SHIFT;
    }
}

#endif // PBE_RUNTIME_INLINE_H

