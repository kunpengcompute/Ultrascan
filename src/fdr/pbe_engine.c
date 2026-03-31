/*
 * Copyright (c) 2026, Huawei Technologies Co., Ltd.
 */

#include "fdr_enhanced.h"
#include "fdr_internal.h"
#include "pbe_runtime.h"
#include "hs_compile.h"
#include "util/bitutils.h"
#include "util/compare.h"
#include "util/cpuid_flags.h"
#include "util/simd_utils.h"

#include <string.h>

struct PBEPositionContext;

static u32 pbeExtractKeyFromWindow(
    const struct PBERuntimeHeader *hdr,
    const struct PBERuntimeBitSelector *selectors, u64a window);

static void pbeDecodePrimaryValue(u32 encoded, u32 *offset, u32 *count);
static int pbePrimaryBitmapHasValue(const u8 *bitmap, u32 bitmapSize, u32 idx);
static int pbeRuntimeCanUseBextFastPath(void);
static u64a pbeLoadWindow64Normalized(const struct FDR_Runtime_Args *a,
                                      size_t endPos, u32 windowBytes);
static u32 pbeExtractKeyScalarFromWindow(
    const struct PBERuntimeBitSelector *selectors, u32 selectorCount,
    u64a window);
static u64a pbeExtractPackedBitsFallback(u64a window, u64a mask);
static u32 pbeRemapPackedBits(u64a packed, const u8 *bitOrder,
                              u32 selectorCount);
static u32 pbeExtractKeyBext(const struct PBERuntimeHeader *hdr, u64a window);
static u32 pbeComputeValidMask8(const struct FDR_Runtime_Args *a, size_t endPos);
static void pbeBuildLaneWindow32FromWindow(u64a window, u32 validMask8,
                                           u8 *window32, u32 *validMask32);
static void pbeBuildPositionContext(
    const struct PBERuntimeHeader *hdr,
    const struct PBERuntimeBitSelector *selectors,
    const struct FDR_Runtime_Args *a, size_t endPos, u32 wildcardEncoded,
    struct PBEPositionContext *ctx);
static int pbeProcessEncodedRange(
    const struct PBERuntimeHeader *hdr,
    const struct PBERuntimeSecondaryHashEntry *secondaryHashTable,
    const struct PBERuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, const struct PBEPositionContext *ctx, u32 encoded);
static u32 pbeBuildBatch4Contexts(const struct PBERuntimeHeader *hdr,
                                  const struct PBERuntimeBitSelector *selectors,
                                  const struct FDR_Runtime_Args *a,
                                  size_t startPos, u32 wildcardEncoded,
                                  struct PBEPositionContext *ctxs);
static u32 pbeBatchBitmapMask(const u8 *bitmap, u32 bitmapSize,
                              const u32 *primaryIdx, u32 laneCount);
static u32 pbeCompactPrimaryLanes(const u32 *primaryIdx, u32 laneCount,
                                  u32 activeMask, u32 *activePrimaryIdx,
                                  u32 *activeLaneIndex);
static void pbeBatchLoadPrimaryValues(const u32 *primaryHashTable,
                                      const u32 *activePrimaryIdx,
                                      const u32 *activeLaneIndex,
                                      u32 activeCount, u32 *activeEncoded,
                                      u32 *encodedByLane, u32 laneCapacity);
static void pbeFillBatch4EncodedValues(const u8 *primaryBitmap,
                                       u32 bitmapSize,
                                       const u32 *primaryHashTable,
                                       struct PBEPositionContext *ctxs,
                                       u32 laneCount);

static u32 pbeEntryMatchMaskFromContextScalar(
    const struct PBERuntimeSecondaryHashEntry *entry,
    const struct PBEPositionContext *ctx);
static u32 pbeEntryMatchMaskFromContextVector(
    const struct PBERuntimeSecondaryHashEntry *entry,
    const struct PBEPositionContext *ctx);
static u32 pbeEntrySingleSlotMatchMaskFromContext(
    const struct PBERuntimeSecondaryHashEntry *entry,
    const struct PBEPositionContext *ctx);

static int pbeRunNaive(const struct PBERuntimeHeader *hdr,
                       const struct FDR_Runtime_Args *a,
                       hwlm_group_t *control);
static int pbeRunBatch4(const struct PBERuntimeHeader *hdr,
                        const struct FDR_Runtime_Args *a,
                        hwlm_group_t *control);

#define PBE_BATCH_WIDTH 4U

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

static int pbeGetByteAt(const struct FDR_Runtime_Args *a, s64a pos, u8 *out) {
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

static u8 pbeNormalizeByte(u8 c) {
    return ourisalpha(c) ? (u8)mytoupper((char)c) : c;
}

static int pbeRuntimeCanUseBextFastPath(void) {
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

static u64a pbeLoadWindow64Normalized(const struct FDR_Runtime_Args *a,
                                      size_t endPos, u32 windowBytes) {
    u64a window = 0;
    u32 i;

    if (!a) {
        return 0;
    }

    if (!windowBytes || windowBytes > PBE_RUNTIME_BYTES_PER_RULE_SLOT) {
        windowBytes = PBE_RUNTIME_BYTES_PER_RULE_SLOT;
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

static u32 pbeComputeValidMask8(const struct FDR_Runtime_Args *a, size_t endPos) {
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

static void pbeBuildLaneWindow32FromWindow(u64a window, u32 validMask8,
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

static u32 pbeExtractKeyScalarFromWindow(
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

static u64a pbeExtractPackedBitsFallback(u64a window, u64a mask) {
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

static u32 pbeRemapPackedBits(u64a packed, const u8 *bitOrder,
                              u32 selectorCount) {
    u32 key = 0;
    u32 i;

    if (!bitOrder) {
        return 0;
    }

    for (i = 0; i < selectorCount && i < PBE_RUNTIME_MAX_SELECTORS; i++) {
        const u8 keyBit = bitOrder[i];
        if (keyBit >= PBE_RUNTIME_MAX_SELECTORS) {
            continue;
        }
        if (packed & ((u64a)1 << i)) {
            key |= (1U << keyBit);
        }
    }

    return key;
}

static u32 pbeExtractKeyBext(const struct PBERuntimeHeader *hdr, u64a window) {
    const u64a packed = pbeRuntimeCanUseBextFastPath()
                            ? pbeExtractPackedBitsSveBitPerm(window,
                                                             hdr->bextMask)
                            : pbeExtractPackedBitsFallback(window,
                                                           hdr->bextMask);
    return pbeRemapPackedBits(packed, hdr->bextToKeyBit, hdr->selectorCount);
}

static u32 pbeExtractKeyFromWindow(
    const struct PBERuntimeHeader *hdr,
    const struct PBERuntimeBitSelector *selectors, u64a window) {
    if (hdr->extractMode == PBE_RUNTIME_EXTRACT_MODE_BEXT) {
        return pbeExtractKeyBext(hdr, window);
    }

    return pbeExtractKeyScalarFromWindow(selectors, hdr->selectorCount, window);
}

static void pbeBuildPositionContext(
    const struct PBERuntimeHeader *hdr,
    const struct PBERuntimeBitSelector *selectors,
    const struct FDR_Runtime_Args *a, size_t endPos, u32 wildcardEncoded,
    struct PBEPositionContext *ctx) {
    const u64a window64 =
        pbeLoadWindow64Normalized(a, endPos, PBE_RUNTIME_BYTES_PER_RULE_SLOT);
    const u32 key = pbeExtractKeyFromWindow(hdr, selectors, window64);
    const u32 validMask8 = pbeComputeValidMask8(a, endPos);

    assert(ctx);
    memset(ctx, 0, sizeof(*ctx));
    ctx->endPos = endPos;
    ctx->key = key;
    ctx->primaryIdx = (key < hdr->primaryCount) ? key : 0;
    ctx->wildcardEncoded = wildcardEncoded;
    ctx->window64 = window64;
    pbeBuildLaneWindow32FromWindow(window64, validMask8, ctx->laneWindow32,
                                   &ctx->validMask32);
}

static int pbeRuleExactMatch(const struct PBERuntimeRuleMeta *rm,
                             const struct FDR_Runtime_Args *a, size_t endPos,
                             const u8 *literalBlob, u32 literalBlobSize) {
    if (!rm || !a || !literalBlob) {
        return 0;
    }
    const u16 len = rm->len;
    if (!len) {
        return 0;
    }
    if (rm->litOffset > literalBlobSize ||
        (u64a)rm->litOffset + len > literalBlobSize) {
        return 0;
    }
    if ((u64a)len > (u64a)endPos + 1 + a->len_history) {
        return 0;
    }

    const s64a startPos = (s64a)endPos - (s64a)len + 1;
    const u8 *pat = literalBlob + rm->litOffset;
    const int nocase = (rm->flags & PBE_RULE_FLAG_NOCASE) ? 1 : 0;
    u16 i;
    for (i = 0; i < len; i++) {
        u8 got;
        if (!pbeGetByteAt(a, startPos + i, &got)) {
            return 0;
        }
        const u8 expect = pat[i];
        if (nocase) {
            if ((u8)mytoupper((char)got) != expect) {
                return 0;
            }
        } else if (got != expect) {
            return 0;
        }
    }

    if ((rm->flags & PBE_RULE_FLAG_HAS_MASK) && rm->maskLen) {
        const u8 mlen = rm->maskLen;
        if (mlen > sizeof(rm->msk)) {
            return 0;
        }
        const s64a maskStart = startPos + (s64a)len - (s64a)mlen;
        for (i = 0; i < mlen; i++) {
            u8 got;
            if (!pbeGetByteAt(a, maskStart + i, &got)) {
                return 0;
            }
            if ((got & rm->msk[i]) != rm->cmp[i]) {
                return 0;
            }
        }
    }

    return 1;
}

static int pbeValidateLayout(const struct FDR *fdr,
                             const struct PBERuntimeHeader *hdr) {
    if (!fdr || !hdr) {
        return 0;
    }
    if (hdr->magic != PBE_RUNTIME_MAGIC ||
        hdr->version != PBE_RUNTIME_VERSION) {
        return 0;
    }
    if (!hdr->selectorCount || !hdr->primaryCount || !hdr->secondaryCount) {
        return 0;
    }
    if (hdr->selectorCount > PBE_RUNTIME_MAX_SELECTORS) {
        return 0;
    }
    if (!hdr->windowBytes || hdr->windowBytes > PBE_RUNTIME_BYTES_PER_RULE_SLOT) {
        return 0;
    }
    if (hdr->extractMode > PBE_RUNTIME_EXTRACT_MODE_BEXT) {
        return 0;
    }
    if ((u64a)hdr->selectorsOffset + (u64a)hdr->selectorCount *
            sizeof(struct PBERuntimeBitSelector) >
        (u64a)fdr->pbeSize) {
        return 0;
    }
    if ((u64a)hdr->primaryBitmapOffset + (u64a)hdr->primaryBitmapSize >
        (u64a)fdr->pbeSize) {
        return 0;
    }
    if ((u64a)hdr->primaryOffset + (u64a)hdr->primaryCount * sizeof(u32) >
        (u64a)fdr->pbeSize) {
        return 0;
    }
    if ((u64a)hdr->secondaryOffset + (u64a)hdr->secondaryCount *
            sizeof(struct PBERuntimeSecondaryHashEntry) >
        (u64a)fdr->pbeSize) {
        return 0;
    }
    if ((u64a)hdr->ruleMetaOffset + (u64a)hdr->ruleMetaCount *
            sizeof(struct PBERuntimeRuleMeta) >
        (u64a)fdr->pbeSize) {
        return 0;
    }
    if ((u64a)hdr->literalBlobOffset + (u64a)hdr->literalBlobSize >
        (u64a)fdr->pbeSize) {
        return 0;
    }
    return 1;
}

static int pbePrimaryBitmapHasValue(const u8 *bitmap, u32 bitmapSize, u32 idx) {
    if (!bitmap || idx / 8U >= bitmapSize) {
        return 0;
    }
    return !!(bitmap[idx / 8U] & (1U << (idx % 8U)));
}

static u32 pbeEntryLaneMaskFromByteMatches(
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

static u32 pbeEntryMatchMaskFromContextScalar(
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

static u32 pbeEntrySingleSlotMatchMaskFromContext(
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

static u32 pbeEntryMatchMaskFromContextVector(
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

static int pbeProcessEncodedRange(
    const struct PBERuntimeHeader *hdr,
    const struct PBERuntimeSecondaryHashEntry *secondaryHashTable,
    const struct PBERuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, const struct PBEPositionContext *ctx, u32 encoded) {
    u32 offset = 0;
    u32 count = 0;
    u32 n;

    if (!encoded || !ctx) {
        return HWLM_SUCCESS;
    }

    pbeDecodePrimaryValue(encoded, &offset, &count);
    for (n = 0; n < count; n++) {
        const u32 off = offset + n;
        const struct PBERuntimeSecondaryHashEntry *entry;
        u32 laneMask;
        u32 r;

        if (!off || off >= hdr->secondaryCount) {
            break;
        }

        entry = &secondaryHashTable[off];
        laneMask = pbeEntryMatchMaskFromContextVector(entry, ctx);
        if (!laneMask) {
            continue;
        }

        for (r = 0; r < entry->ruleCount &&
                    r < PBE_RUNTIME_RULE_SLOTS_PER_ENTRY; r++) {
            const u16 ridx = entry->ruleIndex[r];
            const u32 kv = entry->keyValue[r];
            const u32 km = entry->keyMask[r];
            const struct PBERuntimeRuleMeta *rm;

            if (ridx >= hdr->ruleMetaCount) {
                continue;
            }
            if (!(laneMask & (1U << r))) {
                continue;
            }
            if (((ctx->key ^ kv) & km) != 0) {
                continue;
            }

            rm = &ruleMeta[ridx];
            if (!(rm->groups & *control)) {
                continue;
            }
            if (!pbeRuleExactMatch(rm, a, ctx->endPos, literalBlob,
                                   literalBlobSize)) {
                continue;
            }

            *control = a->cb(ctx->endPos, rm->id, a->scratch);
            if (*control == HWLM_TERMINATE_MATCHING) {
                return HWLM_TERMINATED;
            }
        }
    }

    return HWLM_SUCCESS;
}

static u32 pbeBuildBatch4Contexts(const struct PBERuntimeHeader *hdr,
                                  const struct PBERuntimeBitSelector *selectors,
                                  const struct FDR_Runtime_Args *a,
                                  size_t startPos, u32 wildcardEncoded,
                                  struct PBEPositionContext *ctxs) {
    u32 laneCount = 0;

    while (laneCount < PBE_BATCH_WIDTH && startPos + laneCount < a->len) {
        const size_t pos = startPos + laneCount;
        pbeBuildPositionContext(hdr, selectors, a, pos, wildcardEncoded,
                                &ctxs[laneCount]);
        laneCount++;
    }

    return laneCount;
}

static u32 pbeBatchBitmapMask(const u8 *bitmap, u32 bitmapSize,
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

static u32 pbeCompactPrimaryLanes(const u32 *primaryIdx, u32 laneCount,
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

static void pbeBatchLoadPrimaryValues(const u32 *primaryHashTable,
                                      const u32 *activePrimaryIdx,
                                      const u32 *activeLaneIndex,
                                      u32 activeCount, u32 *activeEncoded,
                                      u32 *encodedByLane,
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

static void pbeFillBatch4EncodedValues(const u8 *primaryBitmap,
                                       u32 bitmapSize,
                                       const u32 *primaryHashTable,
                                       struct PBEPositionContext *ctxs,
                                       u32 laneCount) {
    u32 primaryIdx[PBE_BATCH_WIDTH] = {0};
    u32 activePrimaryIdx[PBE_BATCH_WIDTH] = {0};
    u32 activeLaneIndex[PBE_BATCH_WIDTH] = {0};
    u32 activeEncoded[PBE_BATCH_WIDTH] = {0};
    u32 encodedByLane[PBE_BATCH_WIDTH] = {0};
    u32 activeMask;
    u32 activeCount;
    u32 lane;

    if (!ctxs) {
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
                              encodedByLane, PBE_BATCH_WIDTH);

    for (lane = 0; lane < laneCount; lane++) {
        ctxs[lane].exactEncoded = encodedByLane[lane];
    }
}

static UNUSED
int pbeRunNaive(const struct PBERuntimeHeader *hdr,
                       const struct FDR_Runtime_Args *a,
                       hwlm_group_t *control) {
    if (!hdr || !a || !a->buf || !a->len || a->start_offset >= a->len) {
        return HWLM_SUCCESS;
    }

    const struct PBERuntimeBitSelector *selectors =
        (const struct PBERuntimeBitSelector *)((const u8 *)hdr +
                                               hdr->selectorsOffset);
    const u8 *primaryBitmap = (const u8 *)hdr + hdr->primaryBitmapOffset;
    const u32 *primaryHashTable =
        (const u32 *)((const u8 *)hdr + hdr->primaryOffset);
    const struct PBERuntimeSecondaryHashEntry *secondaryHashTable =
        (const struct PBERuntimeSecondaryHashEntry *)((const u8 *)hdr +
                                                      hdr->secondaryOffset);
    const struct PBERuntimeRuleMeta *ruleMeta =
        (const struct PBERuntimeRuleMeta *)((const u8 *)hdr +
                                            hdr->ruleMetaOffset);
    const u8 *literalBlob = (const u8 *)hdr + hdr->literalBlobOffset;
    const u32 literalBlobSize = hdr->literalBlobSize;
    const u32 wildcardIdx = 0;
    const u32 wildcardValue =
        (wildcardIdx < hdr->primaryCount &&
         pbePrimaryBitmapHasValue(primaryBitmap, hdr->primaryBitmapSize,
                                  wildcardIdx))
            ? primaryHashTable[wildcardIdx]
            : 0;

    size_t i;
    for (i = a->start_offset; i < a->len; i++) {
        struct PBEPositionContext ctx;

        pbeBuildPositionContext(hdr, selectors, a, i, wildcardValue, &ctx);
        ctx.exactEncoded =
            pbePrimaryBitmapHasValue(primaryBitmap, hdr->primaryBitmapSize,
                                     ctx.primaryIdx)
                ? primaryHashTable[ctx.primaryIdx]
                : 0;

        if (pbeProcessEncodedRange(hdr, secondaryHashTable, ruleMeta,
                                   literalBlob, literalBlobSize, a, control,
                                   &ctx, ctx.exactEncoded) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
        if (ctx.wildcardEncoded != ctx.exactEncoded &&
            pbeProcessEncodedRange(hdr, secondaryHashTable, ruleMeta,
                                   literalBlob, literalBlobSize, a, control,
                                   &ctx, ctx.wildcardEncoded) ==
                HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }
    return HWLM_SUCCESS;
}

static int pbeRunBatch4(const struct PBERuntimeHeader *hdr,
                        const struct FDR_Runtime_Args *a,
                        hwlm_group_t *control) {
    const struct PBERuntimeBitSelector *selectors =
        (const struct PBERuntimeBitSelector *)((const u8 *)hdr +
                                               hdr->selectorsOffset);
    const u8 *primaryBitmap = (const u8 *)hdr + hdr->primaryBitmapOffset;
    const u32 *primaryHashTable =
        (const u32 *)((const u8 *)hdr + hdr->primaryOffset);
    const struct PBERuntimeSecondaryHashEntry *secondaryHashTable =
        (const struct PBERuntimeSecondaryHashEntry *)((const u8 *)hdr +
                                                      hdr->secondaryOffset);
    const struct PBERuntimeRuleMeta *ruleMeta =
        (const struct PBERuntimeRuleMeta *)((const u8 *)hdr +
                                            hdr->ruleMetaOffset);
    const u8 *literalBlob = (const u8 *)hdr + hdr->literalBlobOffset;
    const u32 literalBlobSize = hdr->literalBlobSize;
    const u32 wildcardIdx = 0;
    const u32 wildcardValue =
        (wildcardIdx < hdr->primaryCount &&
         pbePrimaryBitmapHasValue(primaryBitmap, hdr->primaryBitmapSize,
                                  wildcardIdx))
            ? primaryHashTable[wildcardIdx]
            : 0;

    size_t i;
    for (i = a->start_offset; i < a->len; i += PBE_BATCH_WIDTH) {
        struct PBEPositionContext ctxs[PBE_BATCH_WIDTH];
        u32 laneCount = 0;
        u32 lane;

        memset(ctxs, 0, sizeof(ctxs));
        laneCount = pbeBuildBatch4Contexts(hdr, selectors, a, i, wildcardValue,
                                           ctxs);
        pbeFillBatch4EncodedValues(primaryBitmap, hdr->primaryBitmapSize,
                                   primaryHashTable, ctxs, laneCount);

        for (lane = 0; lane < laneCount; lane++) {
            if (ctxs[lane].exactEncoded &&
                pbeProcessEncodedRange(hdr, secondaryHashTable, ruleMeta,
                                       literalBlob, literalBlobSize, a, control,
                                       &ctxs[lane], ctxs[lane].exactEncoded) ==
                    HWLM_TERMINATED) {
                return HWLM_TERMINATED;
            }

            if (!ctxs[lane].wildcardEncoded ||
                ctxs[lane].wildcardEncoded == ctxs[lane].exactEncoded) {
                continue;
            }
            if (pbeProcessEncodedRange(hdr, secondaryHashTable, ruleMeta,
                                       literalBlob, literalBlobSize, a, control,
                                       &ctxs[lane], ctxs[lane].wildcardEncoded) ==
                HWLM_TERMINATED) {
                return HWLM_TERMINATED;
            }
        }
    }

    return HWLM_SUCCESS;
}

static void pbeDecodePrimaryValue(u32 encoded, u32 *offset, u32 *count) {
    if (offset) {
        *offset = encoded & PBE_RUNTIME_L1_OFFSET_MASK;
    }
    if (count) {
        *count = encoded >> PBE_RUNTIME_L1_COUNT_SHIFT;
    }
}
static
hwlm_error_t pbeExecWithPath(const struct FDR *fdr,
                             const struct FDR_Runtime_Args *a,
                             hwlm_group_t control, int useBatch4) {
    if (!fdr || !fdr->pbeOffset || !fdr->pbeSize) {
        return HWLM_SUCCESS;
    }

    const u8 *base = (const u8 *)fdr;
    const struct PBERuntimeHeader *hdr =
        (const struct PBERuntimeHeader *)(base + fdr->pbeOffset);

    if (!pbeValidateLayout(fdr, hdr)) {
        return HWLM_SUCCESS;
    }

    if (!a || (hdr->flags & PBE_RUNTIME_FLAG_PARTIAL_COVERAGE)) {
        return HWLM_SUCCESS;
    }

    return useBatch4 ? pbeRunBatch4(hdr, a, &control)
                     : pbeRunNaive(hdr, a, &control);
}

hwlm_error_t PbeEngineExecNaiveForTest(const struct FDR *fdr,
                                       const struct FDR_Runtime_Args *a,
                                       hwlm_group_t control) {
    return pbeExecWithPath(fdr, a, control, 0);
}

u32 PbeRuntimeEntryMatchMaskForTest(
    const struct PBERuntimeSecondaryHashEntry *entry,
    const struct FDR_Runtime_Args *a, size_t endPos, int useVector) {
    struct PBEPositionContext ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.endPos = endPos;
    ctx.window64 = pbeLoadWindow64Normalized(a, endPos,
                                             PBE_RUNTIME_BYTES_PER_RULE_SLOT);
    pbeBuildLaneWindow32FromWindow(ctx.window64, pbeComputeValidMask8(a, endPos),
                                   ctx.laneWindow32, &ctx.validMask32);

    return useVector ? pbeEntryMatchMaskFromContextVector(entry, &ctx)
                     : pbeEntryMatchMaskFromContextScalar(entry, &ctx);
}

hwlm_error_t PbeEngineExec(const struct FDR *fdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t control) {
    return pbeExecWithPath(fdr, a, control, 1);
}
