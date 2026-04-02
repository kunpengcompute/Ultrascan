#ifndef PBE_RUNTIME_INLINE_H
#define PBE_RUNTIME_INLINE_H

#include "fdr_internal.h"
#include "pbe_runtime.h"
#include "hs_compile.h"
#include "util/bitutils.h"
#include "util/compare.h"
#include "util/cpuid_flags.h"
#include "util/simd_utils.h"
#include "util/unaligned.h"

#include <string.h>

#define PBE_BATCH_FALLBACK_WIDTH 4U
#define PBE_BATCH_MAX_WIDTH 32U

struct PBEPositionContext {
    size_t endPos;
    u32 key;
    u32 primaryIdx;
    u32 exactEncoded;
    u32 wildcardEncoded;
    u64a window64;
    u8 laneWindow32[PBE_RUNTIME_RULE_VECTOR_BYTES];
    u32 validMask32;
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
void pbeBuildPositionContext(const struct PBERuntimeHeader *hdr,
                             const struct PBERuntimeBitSelector *selectors,
                             const struct FDR_Runtime_Args *a, size_t endPos,
                             u32 wildcardEncoded,
                             struct PBEPositionContext *ctx, int fillKey) {
    const u64a window64 =
        pbeLoadWindow64Normalized(a, endPos, hdr->windowBytes);
    const u32 validMask8 = pbeComputeValidMask8(a, endPos);

    assert(ctx);
    memset(ctx, 0, sizeof(*ctx));
    ctx->endPos = endPos;
    ctx->wildcardEncoded = wildcardEncoded;
    ctx->window64 = window64;
    pbeBuildLaneWindow32FromWindow(window64, validMask8, ctx->laneWindow32,
                                   &ctx->validMask32);
    if (fillKey) {
        ctx->key = pbeExtractKeyFromWindow(hdr, selectors, window64);
        ctx->primaryIdx = (ctx->key < hdr->primaryCount) ? ctx->key : 0;
    }
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
    const struct PBEPositionContext *ctx) {
    u32 byteMatchMask = 0;
    u32 i;

    if (!entry || !entry->ruleCount || !ctx) {
        return 0;
    }

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
    const struct PBEPositionContext *ctx) {
    const u32 tailOnlyMask = entry->tailMask & ~entry->headMask;
    u32 mask = tailOnlyMask;

    if (!entry || !ctx || !entry->ruleCount) {
        return 0;
    }

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
    const struct PBEPositionContext *ctx) {
    if (!entry || !entry->ruleCount || !ctx) {
        return 0;
    }

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
                          u32 wildcardEncoded, u32 batchWidth,
                          struct PBEPositionContext *ctxs) {
    u64a windows[PBE_BATCH_MAX_WIDTH] = {0};
    u32 keys[PBE_BATCH_MAX_WIDTH] = {0};
    u32 laneCount = 0;

    assert(batchWidth <= PBE_BATCH_MAX_WIDTH);

    while (laneCount < batchWidth && startPos + laneCount < a->len) {
        const size_t pos = startPos + laneCount;
        pbeBuildPositionContext(hdr, selectors, a, pos, wildcardEncoded,
                                &ctxs[laneCount], 0);
        windows[laneCount] = ctxs[laneCount].window64;
        laneCount++;
    }

    pbeExtractKeysFromWindows(hdr, selectors, windows, laneCount, keys);
    for (u32 lane = 0; lane < laneCount; lane++) {
        ctxs[lane].key = keys[lane];
        ctxs[lane].primaryIdx = (keys[lane] < hdr->primaryCount) ? keys[lane] : 0;
    }

    return laneCount;
}

static really_inline
u32 pbeBatchBitmapMask(const u8 *bitmap, u32 bitmapSize,
                       const u32 *primaryIdx, u32 laneCount) {
    u32 activeMask = 0;
    u32 lane;

    for (lane = 0; lane < laneCount; lane++) {
        if (pbePrimaryBitmapHasValue(bitmap, bitmapSize, primaryIdx[lane])) {
            activeMask |= (1U << lane);
        }
    }

    return activeMask;
}

static really_inline
u32 pbeCompactPrimaryLanes(const u32 *primaryIdx, u32 laneCount,
                           u32 activeMask, u32 *activePrimaryIdx,
                           u32 *activeLaneIndex) {
    u32 activeCount = 0;
    u32 lane;

    for (lane = 0; lane < laneCount; lane++) {
        if (!(activeMask & (1U << lane))) {
            continue;
        }
        activePrimaryIdx[activeCount] = primaryIdx[lane];
        activeLaneIndex[activeCount] = lane;
        activeCount++;
    }

    return activeCount;
}

static really_inline
void pbeBatchLoadPrimaryValues(const u32 *primaryHashTable,
                               const u32 *activePrimaryIdx,
                               const u32 *activeLaneIndex, u32 activeCount,
                               u32 *activeEncoded, u32 *encodedByLane,
                               u32 laneCapacity) {
    u32 i;

    if (!primaryHashTable || !activePrimaryIdx || !activeLaneIndex ||
        !activeEncoded || !encodedByLane) {
        return;
    }

    for (i = 0; i < laneCapacity; i++) {
        encodedByLane[i] = 0;
    }

    for (i = 0; i < activeCount; i++) {
        const u32 encoded = primaryHashTable[activePrimaryIdx[i]];
        activeEncoded[i] = encoded;
        if (activeLaneIndex[i] < laneCapacity) {
            encodedByLane[activeLaneIndex[i]] = encoded;
        }
    }
}

static really_inline
void pbeFillBatchEncodedValues(const u8 *primaryBitmap, u32 bitmapSize,
                               const u32 *primaryHashTable,
                               struct PBEPositionContext *ctxs, u32 laneCount,
                               u32 laneCapacity) {
    u32 primaryIdx[PBE_BATCH_MAX_WIDTH] = {0};
    u32 activePrimaryIdx[PBE_BATCH_MAX_WIDTH] = {0};
    u32 activeLaneIndex[PBE_BATCH_MAX_WIDTH] = {0};
    u32 activeEncoded[PBE_BATCH_MAX_WIDTH] = {0};
    u32 encodedByLane[PBE_BATCH_MAX_WIDTH] = {0};
    u32 activeMask;
    u32 activeCount;
    u32 lane;

    if (!ctxs || laneCount > laneCapacity || laneCapacity > PBE_BATCH_MAX_WIDTH) {
        return;
    }

    for (lane = 0; lane < laneCount; lane++) {
        primaryIdx[lane] = ctxs[lane].primaryIdx;
    }

    activeMask = pbeBatchBitmapMask(primaryBitmap, bitmapSize, primaryIdx,
                                    laneCount);
    activeCount = pbeCompactPrimaryLanes(primaryIdx, laneCount, activeMask,
                                         activePrimaryIdx, activeLaneIndex);
    pbeBatchLoadPrimaryValues(primaryHashTable, activePrimaryIdx,
                              activeLaneIndex, activeCount, activeEncoded,
                              encodedByLane, laneCapacity);

    for (lane = 0; lane < laneCount; lane++) {
        ctxs[lane].exactEncoded = encodedByLane[lane];
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
