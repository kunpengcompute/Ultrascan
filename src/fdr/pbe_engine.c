/*
 * Copyright (c) 2026, Huawei Technologies Co., Ltd.
 */

#include "fdr_enhanced.h"
#include "fdr_internal.h"
#include "pbe_runtime.h"
#include "pbe_runtime_inline.h"
#include "util/bitutils.h"
#include "util/simd_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct HAORuntimeStats g_haoStats;
static int g_haoStatsForceEnabled;
static int g_haoStatsEnvEnabled = -1;
static int g_haoStatsAtexitRegistered;

struct HAOPositionContext {
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

struct HAOPrimaryProbeState {
    u32 laneCount;
    u32 byteIndex[PBE_BATCH_MAX_WIDTH];
    u8 bitMask[PBE_BATCH_MAX_WIDTH];
    u8 gatheredBytes[PBE_BATCH_MAX_WIDTH];
    u32 activeMask;
    u32 activeLaneIndex[PBE_BATCH_MAX_WIDTH];
    u32 activePrimaryIdx[PBE_BATCH_MAX_WIDTH];
    u32 activeEncoded[PBE_BATCH_MAX_WIDTH];
};

static void haoDumpRuntimeStats(void);

static int haoStatsEnabled(void) {
    if (g_haoStatsEnvEnabled < 0) {
        const char *env = getenv("HS_HAO_STATS");
        g_haoStatsEnvEnabled = env && *env && *env != '0';
        if (g_haoStatsEnvEnabled && !g_haoStatsAtexitRegistered) {
            atexit(haoDumpRuntimeStats);
            g_haoStatsAtexitRegistered = 1;
        }
    }
    return g_haoStatsForceEnabled || g_haoStatsEnvEnabled;
}

static really_inline
void haoStatsAdd(u64a *counter, u64a delta) {
    if (!haoStatsEnabled() || !counter) {
        return;
    }
    *counter += delta;
}

static really_inline
void haoBuildPositionContext(const struct PBERuntimeHeader *hdr,
                             const struct PBERuntimeBitSelector *selectors,
                             const struct FDR_Runtime_Args *a, size_t endPos,
                             struct HAOPositionContext *ctx, int fillKey) {
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
void haoEnsureLaneWindowContext(struct HAOPositionContext *ctx) {
    if (!ctx || ctx->laneWindowReady) {
        return;
    }

    pbeBuildLaneWindow32FromWindow(ctx->window64, ctx->validMask8,
                                   ctx->laneWindow32, &ctx->validMask32);
    ctx->laneWindowReady = 1;
}

static really_inline
int haoPrimaryBitmapHasValue(const u8 *bitmap, u32 bitmapSize, u32 idx) {
    if (!bitmap || idx / 8U >= bitmapSize) {
        return 0;
    }
    return !!(bitmap[idx / 8U] & (1U << (idx % 8U)));
}

static really_inline
void haoPreparePrimaryProbeStateFromPrimaryIdx(
    const u32 *primaryIdx, u32 laneCount, struct HAOPrimaryProbeState *state) {
    u32 lane;

    if (!primaryIdx || !state || laneCount > PBE_BATCH_MAX_WIDTH) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->laneCount = laneCount;
    for (lane = 0; lane < laneCount; lane++) {
        state->activePrimaryIdx[lane] = primaryIdx[lane];
        state->byteIndex[lane] = primaryIdx[lane] >> 3;
        state->bitMask[lane] = (u8)(1U << (primaryIdx[lane] & 7U));
    }
}

static really_inline
int haoProbePrimaryBitmapGrouped(const u8 *bitmap, u32 bitmapSize,
                                 const struct HAOPrimaryProbeState *state,
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
u32 haoProbePrimaryBitmapPacked(const u8 *bitmap, u32 bitmapSize,
                                struct HAOPrimaryProbeState *state) {
    u32 activeMask = 0;
    u32 lane;

    if (!state) {
        return 0;
    }

    if (haoProbePrimaryBitmapGrouped(bitmap, bitmapSize, state, &activeMask)) {
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
u32 haoCompactAndLoadPrimary(const u32 *primaryHashTable,
                             struct HAOPrimaryProbeState *state) {
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

static void haoDumpRuntimeStats(void) {
    if (!haoStatsEnabled()) {
        return;
    }

    fprintf(stderr,
            "[HAO][Runtime] blockCalls=%llu blockLanes=%llu primaryProbeLanes=%llu primaryActiveLanes=%llu encodedRangeCalls=%llu encodedEntriesVisited=%llu verifierCalls=%llu verifierEntryHits=%llu verifierSlotHits=%llu directReports=%llu encodedConfirmCalls=%llu encodedConfirmMatches=%llu residualPosCalls=%llu residualRuleChecks=%llu residualConfirmCalls=%llu residualConfirmMatches=%llu\n",
            (unsigned long long)g_haoStats.blockCalls,
            (unsigned long long)g_haoStats.blockLanes,
            (unsigned long long)g_haoStats.primaryProbeLanes,
            (unsigned long long)g_haoStats.primaryActiveLanes,
            (unsigned long long)g_haoStats.encodedRangeCalls,
            (unsigned long long)g_haoStats.encodedEntriesVisited,
            (unsigned long long)g_haoStats.verifierCalls,
            (unsigned long long)g_haoStats.verifierEntryHits,
            (unsigned long long)g_haoStats.verifierSlotHits,
            (unsigned long long)g_haoStats.directReports,
            (unsigned long long)g_haoStats.encodedConfirmCalls,
            (unsigned long long)g_haoStats.encodedConfirmMatches,
            (unsigned long long)g_haoStats.residualPosCalls,
            (unsigned long long)g_haoStats.residualRuleChecks,
            (unsigned long long)g_haoStats.residualConfirmCalls,
            (unsigned long long)g_haoStats.residualConfirmMatches);
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
    if (!hdr->selectorCount || !hdr->classCount || !hdr->secondaryCount) {
        return 0;
    }
    if (hdr->classCount > PBE_RUNTIME_MAX_MASK_CLASSES) {
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
    if ((u64a)hdr->classTableOffset + (u64a)hdr->classCount *
            sizeof(struct PBERuntimeMaskClass) >
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
    {
        const struct PBERuntimeMaskClass *classes =
            (const struct PBERuntimeMaskClass *)((const u8 *)hdr +
                                                 hdr->classTableOffset);
        u32 i;
        for (i = 0; i < hdr->classCount; i++) {
            const struct PBERuntimeMaskClass *klass = &classes[i];
            if (klass->classKeyBits > hdr->keyBits) {
                return 0;
            }
            if ((u64a)klass->primaryBitmapOffset + (u64a)klass->primaryBitmapSize >
                (u64a)fdr->pbeSize) {
                return 0;
            }
            if ((u64a)klass->primaryOffset + (u64a)klass->primaryCount * sizeof(u32) >
                (u64a)fdr->pbeSize) {
                return 0;
            }
            if (klass->secondaryOffset >= hdr->secondaryCount) {
                return 0;
            }
            if ((u64a)klass->secondaryOffset + (u64a)klass->secondaryCount >
                (u64a)hdr->secondaryCount) {
                return 0;
            }
        }
    }
    return 1;
}

/* HAO v2 当前还未切到正式执行路径，这里先提供 runtime 侧的只读布局校验。
 * 后续切换执行主路径时，可以直接复用这层边界检查。 */
static int haoValidateLayout(const void *blob, u32 blobSize,
                             const struct HAORuntimeHeader **outHdr) {
    const struct HAORuntimeHeader *hdr;

    if (!blob || blobSize < sizeof(struct HAORuntimeHeader)) {
        return 0;
    }

    hdr = (const struct HAORuntimeHeader *)blob;
    if (hdr->magic != HAO_RUNTIME_MAGIC ||
        hdr->version != HAO_RUNTIME_VERSION) {
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
            sizeof(struct HAORuntimeBitSelector) >
        (u64a)blobSize) {
        return 0;
    }
    if ((u64a)hdr->primaryBitmapOffset + (u64a)hdr->primaryBitmapSize >
        (u64a)blobSize) {
        return 0;
    }
    if ((u64a)hdr->primaryOffset + (u64a)hdr->primaryCount * sizeof(u32) >
        (u64a)blobSize) {
        return 0;
    }
    if ((u64a)hdr->secondaryOffset + (u64a)hdr->secondaryCount *
            sizeof(struct HAORuntimeSecondaryHashEntry) >
        (u64a)blobSize) {
        return 0;
    }
    if ((u64a)hdr->ruleMetaOffset + (u64a)hdr->ruleMetaCount *
            sizeof(struct HAORuntimeRuleMeta) >
        (u64a)blobSize) {
        return 0;
    }
    if ((u64a)hdr->literalBlobOffset + (u64a)hdr->literalBlobSize >
        (u64a)blobSize) {
        return 0;
    }
    if ((u64a)hdr->residualRuleIndexOffset +
            (u64a)hdr->residualRuleCount * sizeof(u32) >
        (u64a)blobSize) {
        return 0;
    }

    if (outHdr) {
        *outHdr = hdr;
    }
    return 1;
}

/* 先提供一个 runtime 侧摘要统计，帮助我们在不切执行链的前提下校验 HAO v2 blob。 */
static void haoInspectLayout(const struct HAORuntimeHeader *hdr,
                             struct HAORuntimeInspectSummary *summary) {
    const u32 *primary;
    const struct HAORuntimeSecondaryHashEntry *secondary;
    u32 i;

    if (!hdr || !summary) {
        return;
    }

    memset(summary, 0, sizeof(*summary));
    summary->selectorCount = hdr->selectorCount;
    summary->primaryCount = hdr->primaryCount;
    summary->primaryBitmapSize = hdr->primaryBitmapSize;
    summary->secondaryCount = hdr->secondaryCount;
    summary->ruleMetaCount = hdr->ruleMetaCount;
    summary->residualRuleCount = hdr->residualRuleCount;

    primary = (const u32 *)((const u8 *)hdr + hdr->primaryOffset);
    secondary = (const struct HAORuntimeSecondaryHashEntry *)(
        (const u8 *)hdr + hdr->secondaryOffset);

    for (i = 0; i < hdr->primaryCount; i++) {
        if (!primary[i]) {
            continue;
        }
        {
            const u32 entryCount = primary[i] >> PBE_RUNTIME_L1_COUNT_SHIFT;
            summary->nonEmptyPrimary++;
            if (entryCount > 1) {
                summary->multiEntryBucketCount++;
            }
            if (summary->maxEntriesPerKey < entryCount) {
                summary->maxEntriesPerKey = entryCount;
            }
        }
    }

    for (i = 0; i < hdr->secondaryCount; i++) {
        summary->totalRulesInL2 +=
            popcount32(secondary[i].slotMask &
                       ((1U << PBE_RUNTIME_RULE_SLOTS_PER_ENTRY) - 1U));
    }
}

/* HAO v2 运行期当前先复用 PBE 的位置上下文与 L2 预筛 helper，
 * 通过一个轻量 shim 头把全局单表 layout 接入现有公共小函数。 */
static void haoBuildShimHeader(const struct HAORuntimeHeader *hdr,
                               struct PBERuntimeHeader *shim) {
    memset(shim, 0, sizeof(*shim));
    shim->keyBits = hdr->keyBits;
    shim->selectorCount = hdr->selectorCount;
    shim->primaryCount = hdr->primaryCount;
    shim->primaryBitmapSize = hdr->primaryBitmapSize;
    shim->secondaryCount = hdr->secondaryCount;
    shim->ruleMetaCount = hdr->ruleMetaCount;
    shim->literalBlobSize = hdr->literalBlobSize;
    shim->extractMode = hdr->extractMode;
    shim->windowBytes = hdr->windowBytes;
    shim->bextMask = hdr->bextMask;
}

/* HAO v2 的 confirm 当前仍然沿用原始 literal + mask/cmp 做最终确认。
 * 后续真正的 HAO confirm 优化会在 block kernel 稳定后再推进。 */
static int haoRuleExactMatch(const struct HAORuntimeRuleMeta *rm,
                             const struct FDR_Runtime_Args *a, size_t endPos,
                             const u8 *literalBlob, u32 literalBlobSize) {
    if (!rm || !a || !literalBlob) {
        return 0;
    }
    if (!rm->len) {
        return 0;
    }
    if (rm->litOffset > literalBlobSize ||
        (u64a)rm->litOffset + rm->len > literalBlobSize) {
        return 0;
    }
    if ((u64a)rm->len > (u64a)endPos + 1 + a->len_history) {
        return 0;
    }

    {
        const s64a startPos = (s64a)endPos - (s64a)rm->len + 1;
        const u8 *pat = literalBlob + rm->litOffset;
        const int nocase = (rm->flags & PBE_RULE_FLAG_NOCASE) ? 1 : 0;
        u16 i;

        for (i = 0; i < rm->len; i++) {
            u8 got;
            if (!pbeGetByteAt(a, startPos + i, &got)) {
                return 0;
            }
            if (nocase) {
                if ((u8)mytoupper((char)got) != pat[i]) {
                    return 0;
                }
            } else if (got != pat[i]) {
                return 0;
            }
        }

        if ((rm->flags & PBE_RULE_FLAG_HAS_MASK) && rm->maskLen) {
            const u8 mlen = rm->maskLen;
            const s64a maskStart = startPos + (s64a)rm->len - (s64a)mlen;
            for (i = 0; i < mlen && i < sizeof(rm->msk); i++) {
                u8 got;
                if (!pbeGetByteAt(a, maskStart + i, &got)) {
                    return 0;
                }
                if ((got & rm->msk[i]) != rm->cmp[i]) {
                    return 0;
                }
            }
        }
    }

    return 1;
}

static really_inline
u32 haoBuildShuffledValidMask(const struct HAORuntimeSecondaryHashEntry *entry,
                              const struct HAOPositionContext *ctx) {
    u32 shuffledValidMask = 0;
    u32 i;

    if (!entry || !ctx) {
        return 0;
    }

    for (i = 0; i < PBE_RUNTIME_RULE_VECTOR_BYTES; i++) {
        const u8 srcCtl = entry->tableControl[i];
        const u32 chunkBase = i & 0x10U;
        u32 srcIndex;
        u32 srcBit;

        if (srcCtl & 0x80U) {
            continue;
        }
        srcIndex = chunkBase + srcCtl;
        srcBit = 1U << srcIndex;
        if (ctx->validMask32 & srcBit) {
            shuffledValidMask |= (1U << i);
        }
    }

    return shuffledValidMask;
}

static really_inline
int haoRuleCanReportFromVerifier(const struct HAORuntimeRuleMeta *rm) {
    if (!rm) {
        return 0;
    }
    return (rm->planFlags & HAO_RUNTIME_PLAN_FLAG_DIRECT_REPORT_SAFE) != 0;
}

static really_inline
u32 haoEntrySlotMask(const struct HAORuntimeSecondaryHashEntry *entry) {
    if (!entry) {
        return 0;
    }
    return entry->slotMask & ((1U << PBE_RUNTIME_RULE_SLOTS_PER_ENTRY) - 1U);
}

static really_inline
u32 haoEntryLaneMaskFromByteMatches(
    const struct HAORuntimeSecondaryHashEntry *entry, u32 byteMatchMask) {
    u32 laneMask = 0;
    u32 slotMask = haoEntrySlotMask(entry);
    u32 slot;

    for (slot = 0; slot < PBE_RUNTIME_RULE_SLOTS_PER_ENTRY; slot++) {
        const u32 laneBase = slot * PBE_RUNTIME_BYTES_PER_RULE_SLOT;
        const u32 laneBits = 0xffU << laneBase;
        const u32 laneTail = (entry->tailMask & ~entry->headMask) & laneBits;
        const u32 laneHead = entry->headMask & laneBits;

        if (!(slotMask & (1U << slot))) {
            continue;
        }
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
u32 haoEntrySingleSlotMatchMaskFromContext(
    const struct HAORuntimeSecondaryHashEntry *entry,
    struct HAOPositionContext *ctx, u32 shuffledValidMask) {
    u32 mask = 0;

    if (!entry || !ctx || !haoEntrySlotMask(entry)) {
        return 0;
    }

    haoEnsureLaneWindowContext(ctx);

    mask = shuffledValidMask & entry->tailMask;
    while (mask) {
        const u32 idx = ctz32(mask);
        const u8 srcCtl = entry->tableControl[idx];
        const u32 chunkBase = idx & 0x10U;
        const u32 srcIndex = chunkBase + srcCtl;

        if (srcCtl & 0x80U ||
            ctx->laneWindow32[srcIndex] != entry->ruleVector[idx]) {
            return 0;
        }
        mask &= mask - 1;
    }

    return haoEntrySlotMask(entry);
}

static u32 haoEntryMatchMaskFromContext(
    const struct HAORuntimeSecondaryHashEntry *entry,
    struct HAOPositionContext *ctx) {
    u32 shuffledValidMask;
    const u32 slotMask = haoEntrySlotMask(entry);

    if (!entry || !slotMask || !ctx) {
        return 0;
    }

    haoStatsAdd(&g_haoStats.verifierCalls, 1);
    haoEnsureLaneWindowContext(ctx);
    shuffledValidMask = haoBuildShuffledValidMask(entry, ctx);

    if (popcount32(slotMask) == 1) {
        const u32 laneMask =
            haoEntrySingleSlotMatchMaskFromContext(entry, ctx,
                                                   shuffledValidMask);
        if (laneMask) {
            haoStatsAdd(&g_haoStats.verifierEntryHits, 1);
            haoStatsAdd(&g_haoStats.verifierSlotHits, 1);
        }
        return laneMask;
    }

#if defined(HAVE_NEON) || defined(HAVE_SSE2)
    {
        {
            const m128 inputLo = loadu128(ctx->laneWindow32);
            const m128 inputHi = loadu128(ctx->laneWindow32 + 16);
            const m128 ctrlLo = loadu128(entry->tableControl);
            const m128 ctrlHi = loadu128(entry->tableControl + 16);
            const m128 shufLo = pshufb_m128(inputLo, ctrlLo);
            const m128 shufHi = pshufb_m128(inputHi, ctrlHi);
            const m128 ruleLo = loadu128(entry->ruleVector);
            const m128 ruleHi = loadu128(entry->ruleVector + 16);
            const u32 eqLo = movemask128(eq128(shufLo, ruleLo));
            const u32 eqHi = movemask128(eq128(shufHi, ruleHi));
            const u32 byteMatchMask = (eqLo | (eqHi << 16)) &
                                      shuffledValidMask & entry->tailMask;
            const u32 laneMask =
                haoEntryLaneMaskFromByteMatches(entry, byteMatchMask);

            if (laneMask) {
                haoStatsAdd(&g_haoStats.verifierEntryHits, 1);
                haoStatsAdd(&g_haoStats.verifierSlotHits, popcount32(laneMask));
            }
            return laneMask;
        }
    }
#else
    {
        u8 shuffledWindow32[PBE_RUNTIME_RULE_VECTOR_BYTES] = {0};
        u32 i;

        for (i = 0; i < PBE_RUNTIME_RULE_VECTOR_BYTES; i++) {
            const u8 srcCtl = entry->tableControl[i];
            const u32 chunkBase = i & 0x10U;
            const u32 srcIndex = chunkBase + srcCtl;

            if (!(shuffledValidMask & (1U << i))) {
                continue;
            }
            shuffledWindow32[i] = ctx->laneWindow32[srcIndex];
        }

        {
            u32 byteMatchMask = 0;

            for (i = 0; i < PBE_RUNTIME_RULE_VECTOR_BYTES; i++) {
                const u32 bit = 1U << i;
                if (!(shuffledValidMask & bit)) {
                    continue;
                }
                if (shuffledWindow32[i] == entry->ruleVector[i]) {
                    byteMatchMask |= bit;
                }
            }

            {
                const u32 laneMask =
                    haoEntryLaneMaskFromByteMatches(entry, byteMatchMask);
                if (laneMask) {
                    haoStatsAdd(&g_haoStats.verifierEntryHits, 1);
                    haoStatsAdd(&g_haoStats.verifierSlotHits,
                                popcount32(laneMask));
                }
                return laneMask;
            }
        }
    }
#endif
}

static int haoProcessEncodedRange(
    const struct HAORuntimeHeader *hdr,
    const struct HAORuntimeSecondaryHashEntry *secondaryHashTable,
    const struct HAORuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct HAOPositionContext *ctx, u32 encoded) {
    u32 offset = 0;
    u32 count = 0;
    u32 n;

    if (!encoded || !ctx) {
        return HWLM_SUCCESS;
    }

    haoStatsAdd(&g_haoStats.encodedRangeCalls, 1);
    pbeDecodePrimaryValue(encoded, &offset, &count);
    for (n = 0; n < count; n++) {
        const u32 off = offset + n;
        const struct HAORuntimeSecondaryHashEntry *entry;
        u32 laneMask;
        u32 r;

        if (!off || off >= hdr->secondaryCount) {
            break;
        }

        entry = &secondaryHashTable[off];
        haoStatsAdd(&g_haoStats.encodedEntriesVisited, 1);
        laneMask = haoEntryMatchMaskFromContext(entry, ctx);
        if (!laneMask) {
            continue;
        }

        for (r = 0; r < PBE_RUNTIME_RULE_SLOTS_PER_ENTRY; r++) {
            const u16 ridx = entry->ruleIndex[r];
            const struct HAORuntimeRuleMeta *rm;

            if (!(haoEntrySlotMask(entry) & (1U << r))) {
                continue;
            }
            if (ridx >= hdr->ruleMetaCount) {
                continue;
            }
            if (!(laneMask & (1U << r))) {
                continue;
            }

            rm = &ruleMeta[ridx];
            if (!(rm->groups & *control)) {
                continue;
            }
            if (haoRuleCanReportFromVerifier(rm)) {
                haoStatsAdd(&g_haoStats.directReports, 1);
            } else {
                haoStatsAdd(&g_haoStats.encodedConfirmCalls, 1);
                if (!haoRuleExactMatch(rm, a, ctx->endPos, literalBlob,
                                       literalBlobSize)) {
                    continue;
                }
                haoStatsAdd(&g_haoStats.encodedConfirmMatches, 1);
            }

            *control = a->cb(ctx->endPos, rm->id, a->scratch);
            if (*control == HWLM_TERMINATE_MATCHING) {
                return HWLM_TERMINATED;
            }
        }
    }

    return HWLM_SUCCESS;
}

static int haoProcessResidualRulesAtPos(
    const struct HAORuntimeHeader *hdr,
    const u32 *residualRuleIndexes,
    const struct HAORuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, size_t endPos) {
    u32 i;

    if (!hdr || !ruleMeta || !literalBlob || !a || !control) {
        return HWLM_SUCCESS;
    }
    if (!hdr->residualRuleCount) {
        return HWLM_SUCCESS;
    }

    haoStatsAdd(&g_haoStats.residualPosCalls, 1);
    for (i = 0; i < hdr->residualRuleCount; i++) {
        const u32 ridx = residualRuleIndexes[i];
        const struct HAORuntimeRuleMeta *rm;

        if (ridx >= hdr->ruleMetaCount) {
            continue;
        }

        rm = &ruleMeta[ridx];
        if (!(rm->groups & *control)) {
            continue;
        }
        haoStatsAdd(&g_haoStats.residualRuleChecks, 1);
        haoStatsAdd(&g_haoStats.residualConfirmCalls, 1);
        if (!haoRuleExactMatch(rm, a, endPos, literalBlob, literalBlobSize)) {
            continue;
        }
        haoStatsAdd(&g_haoStats.residualConfirmMatches, 1);

        *control = a->cb(endPos, rm->id, a->scratch);
        if (*control == HWLM_TERMINATE_MATCHING) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

/* HAO v2 执行骨架先走全局单表 naive 路径。
 * 这里的目标是先把新 layout 真正跑起来，后续再替换成 32B block kernel。 */
static int haoRunNaiveBlob(const struct HAORuntimeHeader *hdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t *control) {
    struct PBERuntimeHeader shim;
    const struct PBERuntimeBitSelector *selectors;
    const u8 *primaryBitmap;
    const u32 *primaryHashTable;
    const struct HAORuntimeSecondaryHashEntry *secondaryHashTable;
    const struct HAORuntimeRuleMeta *ruleMeta;
    const u8 *literalBlob;
    const u32 *residualRuleIndexes;
    size_t i;

    if (!hdr || !a || !a->buf || !a->len || a->start_offset >= a->len) {
        return HWLM_SUCCESS;
    }

    haoBuildShimHeader(hdr, &shim);
    selectors = (const struct PBERuntimeBitSelector *)((const u8 *)hdr +
                                                       hdr->selectorsOffset);
    primaryBitmap = (const u8 *)hdr + hdr->primaryBitmapOffset;
    primaryHashTable = (const u32 *)((const u8 *)hdr + hdr->primaryOffset);
    secondaryHashTable = (const struct HAORuntimeSecondaryHashEntry *)(
        (const u8 *)hdr + hdr->secondaryOffset);
    ruleMeta = (const struct HAORuntimeRuleMeta *)((const u8 *)hdr +
                                                   hdr->ruleMetaOffset);
    literalBlob = (const u8 *)hdr + hdr->literalBlobOffset;
    residualRuleIndexes = (const u32 *)((const u8 *)hdr +
                                        hdr->residualRuleIndexOffset);

    for (i = a->start_offset; i < a->len; i++) {
        struct HAOPositionContext ctx;

        haoBuildPositionContext(&shim, selectors, a, i, &ctx, 1);
        if (ctx.key < hdr->primaryCount &&
            haoPrimaryBitmapHasValue(primaryBitmap, hdr->primaryBitmapSize,
                                     ctx.key)) {
            if (haoProcessEncodedRange(hdr, secondaryHashTable, ruleMeta,
                                       literalBlob, hdr->literalBlobSize, a,
                                       control, &ctx,
                                       primaryHashTable[ctx.key]) ==
                HWLM_TERMINATED) {
                return HWLM_TERMINATED;
            }
        }
        if (hdr->residualRuleCount) {
            if (haoProcessResidualRulesAtPos(hdr, residualRuleIndexes, ruleMeta,
                                             literalBlob,
                                             hdr->literalBlobSize, a, control,
                                             ctx.endPos) ==
                HWLM_TERMINATED) {
                return HWLM_TERMINATED;
            }
        }
    }

    return HWLM_SUCCESS;
}

/* HAO v2 第二步先接一个“全局单表 batch 前端骨架”。
 * 当前仍然复用 PBE 的 batch context/bitmap/compact helper，但不再走 Mask-Class，
 * 而是直接对全局 key 空间做 bitmap probe 与 primary gather。 */
struct HAOBlockState {
    u32 laneCount;
    size_t endPos[PBE_BATCH_MAX_WIDTH];
    u8 byteLanes[PBE_RUNTIME_BYTES_PER_RULE_SLOT][PBE_BATCH_MAX_WIDTH];
    u32 keys[PBE_BATCH_MAX_WIDTH];
    u32 validMask8[PBE_BATCH_MAX_WIDTH];
};

static void haoBuildBlockByteView(const struct FDR_Runtime_Args *a,
                                  size_t blockStart, u32 laneCount,
                                  u8 *blockBytes) {
    const u32 prefix = PBE_RUNTIME_BYTES_PER_RULE_SLOT - 1U;
    u32 i;

    memset(blockBytes, 0, prefix + laneCount);
    for (i = 0; i < prefix; i++) {
        u8 b = 0;
        pbeGetByteAt(a, (s64a)blockStart - (s64a)prefix + i, &b);
        blockBytes[i] = b;
    }

    memcpy(blockBytes + prefix, a->buf + blockStart, laneCount);
}

static void haoBuildByteLanesFromBlockBytes(
    const u8 *blockBytes, u32 laneCount,
    u8 byteLanes[PBE_RUNTIME_BYTES_PER_RULE_SLOT][PBE_BATCH_MAX_WIDTH]) {
    u32 byteIdx;
    u32 lane;

    if (!blockBytes || !byteLanes || !laneCount) {
        return;
    }

    memset(byteLanes, 0,
           sizeof(u8) * PBE_RUNTIME_BYTES_PER_RULE_SLOT * PBE_BATCH_MAX_WIDTH);

#if defined(__aarch64__) || defined(__x86_64__)
    {
        m128 lo;
        m128 hi;
        m128 src0 = loadu128(blockBytes);
        m128 src1 = loadu128(blockBytes + 16);
        m128 src2 = loadu128(blockBytes + 32);

        for (byteIdx = 0; byteIdx < PBE_RUNTIME_BYTES_PER_RULE_SLOT; byteIdx++) {
#if defined(__aarch64__)
            lo = extbyte_m128(src0, src1, (int)byteIdx);
            hi = extbyte_m128(src1, src2, (int)byteIdx);
#else
            switch (byteIdx) {
            case 0:
                lo = palignr(src1, src0, 0);
                hi = palignr(src2, src1, 0);
                break;
            case 1:
                lo = palignr(src1, src0, 1);
                hi = palignr(src2, src1, 1);
                break;
            case 2:
                lo = palignr(src1, src0, 2);
                hi = palignr(src2, src1, 2);
                break;
            case 3:
                lo = palignr(src1, src0, 3);
                hi = palignr(src2, src1, 3);
                break;
            case 4:
                lo = palignr(src1, src0, 4);
                hi = palignr(src2, src1, 4);
                break;
            case 5:
                lo = palignr(src1, src0, 5);
                hi = palignr(src2, src1, 5);
                break;
            case 6:
                lo = palignr(src1, src0, 6);
                hi = palignr(src2, src1, 6);
                break;
            default:
                lo = palignr(src1, src0, 7);
                hi = palignr(src2, src1, 7);
                break;
            }
#endif
            storeu128(byteLanes[byteIdx], lo);
            storeu128(byteLanes[byteIdx] + 16, hi);
        }

        if (laneCount < PBE_BATCH_MAX_WIDTH) {
            for (byteIdx = 0; byteIdx < PBE_RUNTIME_BYTES_PER_RULE_SLOT;
                 byteIdx++) {
                memset(byteLanes[byteIdx] + laneCount, 0,
                       PBE_BATCH_MAX_WIDTH - laneCount);
            }
        }
        return;
    }
#endif

    for (byteIdx = 0; byteIdx < PBE_RUNTIME_BYTES_PER_RULE_SLOT; byteIdx++) {
        for (lane = 0; lane < laneCount; lane++) {
            byteLanes[byteIdx][lane] = blockBytes[lane + byteIdx];
        }
    }
}

static u64a haoLoadWindow64FromByteLanes(
    const u8 byteLanes[PBE_RUNTIME_BYTES_PER_RULE_SLOT][PBE_BATCH_MAX_WIDTH],
    u32 lane, u32 windowBytes) {
    u64a window = 0;
    u32 i;

    if (!windowBytes || windowBytes > PBE_RUNTIME_BYTES_PER_RULE_SLOT) {
        windowBytes = PBE_RUNTIME_BYTES_PER_RULE_SLOT;
    }

    for (i = 0; i < windowBytes; i++) {
        window |= ((u64a)byteLanes[PBE_RUNTIME_BYTES_PER_RULE_SLOT - 1U - i][lane])
                  << (i * 8U);
    }

    return window;
}

static void haoBuildWindowsFromByteLanes(
    const u8 byteLanes[PBE_RUNTIME_BYTES_PER_RULE_SLOT][PBE_BATCH_MAX_WIDTH],
    u32 laneCount, u32 windowBytes, u64a *windows) {
    u32 lane;
    u32 i;

    if (!byteLanes || !windows || !laneCount) {
        return;
    }

    if (!windowBytes || windowBytes > PBE_RUNTIME_BYTES_PER_RULE_SLOT) {
        windowBytes = PBE_RUNTIME_BYTES_PER_RULE_SLOT;
    }

    memset(windows, 0, sizeof(u64a) * PBE_BATCH_MAX_WIDTH);
    for (i = 0; i < windowBytes; i++) {
        const u32 srcByte = PBE_RUNTIME_BYTES_PER_RULE_SLOT - 1U - i;
        const u32 shift = i * 8U;
        for (lane = 0; lane < laneCount; lane++) {
            windows[lane] |= ((u64a)byteLanes[srcByte][lane]) << shift;
        }
    }
}

static void haoExtractKeysFromByteLanes(
    const struct PBERuntimeHeader *shim,
    const struct PBERuntimeBitSelector *selectors,
    struct HAOBlockState *state) {
    u32 sel;
    const u32 laneMaskLimit = state && state->laneCount < 32U
                                  ? ((1U << state->laneCount) - 1U)
                                  : 0xffffffffU;

    if (!shim || !selectors || !state || !state->laneCount) {
        return;
    }

    memset(state->keys, 0, sizeof(state->keys));

    if (shim->extractMode == PBE_RUNTIME_EXTRACT_MODE_BEXT) {
        u64a windows[PBE_BATCH_MAX_WIDTH] = {0};
        haoBuildWindowsFromByteLanes(state->byteLanes, state->laneCount,
                                     shim->windowBytes, windows);
        pbeExtractKeysFromWindows(shim, selectors, windows, state->laneCount,
                                  state->keys);
        return;
    }

    for (sel = 0; sel < shim->selectorCount &&
                  sel < PBE_RUNTIME_MAX_SELECTORS; sel++) {
        const u32 srcByte =
            PBE_RUNTIME_BYTES_PER_RULE_SLOT - 1U - selectors[sel].byteOffset;
        const u8 bit = (u8)(1U << selectors[sel].bitOffset);

#if defined(__aarch64__) || defined(__x86_64__)
        {
            const m128 zero = zeroes128();
            const m128 bitMask = set16x8(bit);
            const m128 loBytes = loadu128(state->byteLanes[srcByte]);
            const m128 hiBytes = loadu128(state->byteLanes[srcByte] + 16);
            const u32 loActive =
                (~movemask128(eq128(and128(loBytes, bitMask), zero))) & 0xffffU;
            const u32 hiActive =
                (~movemask128(eq128(and128(hiBytes, bitMask), zero))) & 0xffffU;
            u32 activeMask = (loActive | (hiActive << 16)) & laneMaskLimit;

            while (activeMask) {
                const u32 lane = ctz32(activeMask);
                state->keys[lane] |= (1U << sel);
                activeMask &= activeMask - 1U;
            }
            continue;
        }
#endif

        for (u32 lane = 0; lane < state->laneCount; lane++) {
            if (state->byteLanes[srcByte][lane] & bit) {
                state->keys[lane] |= (1U << sel);
            }
        }
    }
}

static int haoBuildBlockState(const struct PBERuntimeHeader *shim,
                              const struct PBERuntimeBitSelector *selectors,
                              const struct FDR_Runtime_Args *a,
                              size_t blockStart, u32 blockLaneCount,
                              struct HAOBlockState *state) {
    u8 blockBytes[HAO_RUNTIME_BLOCK_BYTES + 16U];
    u32 laneCount = 0;

    if (!shim || !selectors || !a || !state || !blockLaneCount ||
        blockLaneCount > PBE_BATCH_MAX_WIDTH) {
        return 0;
    }

    memset(state, 0, sizeof(*state));
    memset(blockBytes, 0, sizeof(blockBytes));

    while (laneCount < blockLaneCount && blockStart + laneCount < a->len) {
        const size_t pos = blockStart + laneCount;
        state->endPos[laneCount] = pos;
        state->validMask8[laneCount] = pbeComputeValidMask8(a, pos);
        laneCount++;
    }

    if (!laneCount) {
        return 0;
    }

    haoBuildBlockByteView(a, blockStart, laneCount, blockBytes);
    haoBuildByteLanesFromBlockBytes(blockBytes, laneCount, state->byteLanes);
    state->laneCount = laneCount;
    haoExtractKeysFromByteLanes(shim, selectors, state);
    return laneCount;
}

static void haoBuildContextFromBlockState(const struct HAOBlockState *state,
                                          u32 lane, u32 windowBytes,
                                          struct HAOPositionContext *ctx) {
    if (!state || !ctx || lane >= state->laneCount) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->endPos = state->endPos[lane];
    ctx->window64 = haoLoadWindow64FromByteLanes(state->byteLanes, lane,
                                                 windowBytes);
    ctx->validMask8 = state->validMask8[lane];
    ctx->key = state->keys[lane];
}

static int haoProcessBlockBatch(const struct HAORuntimeHeader *hdr,
                                const struct PBERuntimeHeader *shim,
                                const struct PBERuntimeBitSelector *selectors,
                                const u8 *primaryBitmap,
                                const u32 *primaryHashTable,
                                const struct HAORuntimeSecondaryHashEntry *secondaryHashTable,
                                const struct HAORuntimeRuleMeta *ruleMeta,
                                const u8 *literalBlob,
                                const u32 *residualRuleIndexes,
                                const struct FDR_Runtime_Args *a,
                                hwlm_group_t *control, size_t blockStart,
                                u32 blockLaneCount) {
    struct HAOBlockState block;
    struct HAOPrimaryProbeState probe;
    u32 primaryIdx[PBE_BATCH_MAX_WIDTH] = {0};
    u32 encodedByLane[PBE_BATCH_MAX_WIDTH] = {0};
    u32 activeCount = 0;
    u32 lane;

    if (!hdr || !shim || !selectors || !primaryBitmap || !primaryHashTable ||
        !secondaryHashTable || !ruleMeta || !literalBlob || !a || !control ||
        !blockLaneCount || blockLaneCount > PBE_BATCH_MAX_WIDTH) {
        return HWLM_SUCCESS;
    }

    if (!haoBuildBlockState(shim, selectors, a, blockStart, blockLaneCount,
                            &block)) {
        return HWLM_SUCCESS;
    }

    haoStatsAdd(&g_haoStats.blockCalls, 1);
    haoStatsAdd(&g_haoStats.blockLanes, block.laneCount);

    for (u32 lane = 0; lane < block.laneCount; lane++) {
        primaryIdx[lane] = block.keys[lane];
    }

    haoStatsAdd(&g_haoStats.primaryProbeLanes, block.laneCount);
    haoPreparePrimaryProbeStateFromPrimaryIdx(primaryIdx, block.laneCount,
                                              &probe);
    probe.activeMask = haoProbePrimaryBitmapPacked(primaryBitmap,
                                                   hdr->primaryBitmapSize,
                                                   &probe);
    activeCount = haoCompactAndLoadPrimary(primaryHashTable, &probe);
    haoStatsAdd(&g_haoStats.primaryActiveLanes, activeCount);

    for (lane = 0; lane < activeCount; lane++) {
        encodedByLane[probe.activeLaneIndex[lane]] = probe.activeEncoded[lane];
    }

    for (lane = 0; lane < block.laneCount; lane++) {
        if (encodedByLane[lane]) {
            struct HAOPositionContext ctx;

            haoBuildContextFromBlockState(&block, lane, shim->windowBytes, &ctx);
            if (haoProcessEncodedRange(hdr, secondaryHashTable, ruleMeta,
                                       literalBlob, hdr->literalBlobSize, a,
                                       control, &ctx,
                                       encodedByLane[lane]) == HWLM_TERMINATED) {
                return HWLM_TERMINATED;
            }
        }

        if (hdr->residualRuleCount) {
            if (haoProcessResidualRulesAtPos(hdr, residualRuleIndexes, ruleMeta,
                                             literalBlob,
                                             hdr->literalBlobSize, a, control,
                                             block.endPos[lane]) ==
                HWLM_TERMINATED) {
                return HWLM_TERMINATED;
            }
        }
    }

    return HWLM_SUCCESS;
}

static int haoRunBatchBlob(const struct HAORuntimeHeader *hdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t *control) {
    struct PBERuntimeHeader shim;
    const struct PBERuntimeBitSelector *selectors;
    const u8 *primaryBitmap;
    const u32 *primaryHashTable;
    const struct HAORuntimeSecondaryHashEntry *secondaryHashTable;
    const struct HAORuntimeRuleMeta *ruleMeta;
    const u8 *literalBlob;
    const u32 *residualRuleIndexes;
    size_t i;

    if (!hdr || !a || !a->buf || !a->len || a->start_offset >= a->len) {
        return HWLM_SUCCESS;
    }

    haoBuildShimHeader(hdr, &shim);
    selectors = (const struct PBERuntimeBitSelector *)((const u8 *)hdr +
                                                       hdr->selectorsOffset);
    primaryBitmap = (const u8 *)hdr + hdr->primaryBitmapOffset;
    primaryHashTable = (const u32 *)((const u8 *)hdr + hdr->primaryOffset);
    secondaryHashTable = (const struct HAORuntimeSecondaryHashEntry *)(
        (const u8 *)hdr + hdr->secondaryOffset);
    ruleMeta = (const struct HAORuntimeRuleMeta *)((const u8 *)hdr +
                                                   hdr->ruleMetaOffset);
    literalBlob = (const u8 *)hdr + hdr->literalBlobOffset;
    residualRuleIndexes = (const u32 *)((const u8 *)hdr +
                                        hdr->residualRuleIndexOffset);
    for (i = a->start_offset; i < a->len; i += HAO_RUNTIME_BLOCK_BYTES) {
        const size_t remaining = a->len - i;
        const u32 blockLaneCount = remaining > HAO_RUNTIME_BLOCK_BYTES
                                       ? HAO_RUNTIME_BLOCK_BYTES
                                       : (u32)remaining;

        if (haoProcessBlockBatch(hdr, &shim, selectors, primaryBitmap,
                                 primaryHashTable, secondaryHashTable,
                                 ruleMeta, literalBlob, residualRuleIndexes,
                                 a, control, i,
                                 blockLaneCount) == HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static hwlm_error_t haoExecBlobWithPath(const void *blob, u32 blobSize,
                                        const struct FDR_Runtime_Args *a,
                                        hwlm_group_t control, int useBatch4) {
    const struct HAORuntimeHeader *hdr = NULL;

    (void)useBatch4;
    if (!haoValidateLayout(blob, blobSize, &hdr)) {
        return HWLM_SUCCESS;
    }
    if (!a) {
        return HWLM_SUCCESS;
    }

    return useBatch4 ? haoRunBatchBlob(hdr, a, &control)
                     : haoRunNaiveBlob(hdr, a, &control);
}

/* 统一用安全拷贝读取 blob magic，避免直接解引用未对齐地址。 */
static int pbeReadBlobMagic(const void *blob, u32 blobSize, u32 *magic) {
    if (!blob || !magic || blobSize < sizeof(*magic)) {
        return 0;
    }

    memcpy(magic, blob, sizeof(*magic));
    return 1;
}

static int pbeProcessEncodedRange(
    const struct PBERuntimeHeader *hdr,
    const struct PBERuntimeSecondaryHashEntry *secondaryHashTable,
    const struct PBERuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct PBEPositionContext *ctx, u32 classKey,
    u32 encoded) {
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
            if (((classKey ^ kv) & km) != 0) {
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

static int pbeProcessMaskClassesForContext(
    const struct PBERuntimeHeader *hdr,
    const struct PBERuntimeMaskClass *classes,
    const struct PBERuntimeSecondaryHashEntry *secondaryHashTable,
    const struct PBERuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct PBEPositionContext *ctx) {
    u32 classIdx;

    for (classIdx = 0; classIdx < hdr->classCount; classIdx++) {
        const struct PBERuntimeMaskClass *klass = &classes[classIdx];
        const u8 *primaryBitmap =
            (const u8 *)hdr + klass->primaryBitmapOffset;
        const u32 *primaryHashTable =
            (const u32 *)((const u8 *)hdr + klass->primaryOffset);
        const u32 classKey = pbeProjectKeyToClass(ctx->key, klass->classMask);

        if (classKey >= klass->primaryCount) {
            continue;
        }
        if (!pbePrimaryBitmapHasValue(primaryBitmap, klass->primaryBitmapSize,
                                      classKey)) {
            continue;
        }
        if (pbeProcessEncodedRange(hdr, secondaryHashTable, ruleMeta,
                                   literalBlob, literalBlobSize, a, control,
                                   ctx, classKey,
                                   primaryHashTable[classKey]) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static int pbeProcessMaskClassesForLaneWithBatchState(
    const struct PBERuntimeHeader *hdr,
    const struct PBERuntimeMaskClass *classes,
    const struct PBERuntimeSecondaryHashEntry *secondaryHashTable,
    const struct PBERuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct PBEPositionContext *ctx,
    const struct PBEMaskClassBatchState *batchState, u32 lane) {
    u32 classIdx;

    for (classIdx = 0; classIdx < hdr->classCount; classIdx++) {
        const struct PBERuntimeMaskClass *klass = &classes[classIdx];
        u32 classKey = 0;
        u32 encoded = 0;

        if (batchState && lane < batchState->laneCount && pbeMaskClassIsHot(klass)) {
            encoded = batchState->classEncoded[classIdx][lane];
            if (!encoded) {
                continue;
            }
            classKey = batchState->classKeys[classIdx][lane];
        } else {
            const u8 *primaryBitmap =
                (const u8 *)hdr + klass->primaryBitmapOffset;
            const u32 *primaryHashTable =
                (const u32 *)((const u8 *)hdr + klass->primaryOffset);

            classKey = pbeProjectKeyToClass(ctx->key, klass->classMask);

            if (classKey >= klass->primaryCount) {
                continue;
            }
            if (!pbePrimaryBitmapHasValue(primaryBitmap, klass->primaryBitmapSize,
                                          classKey)) {
                continue;
            }
            encoded = primaryHashTable[classKey];
        }

        if (pbeProcessEncodedRange(hdr, secondaryHashTable, ruleMeta,
                                   literalBlob, literalBlobSize, a, control,
                                   ctx, classKey,
                                   encoded) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
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
    const struct PBERuntimeMaskClass *classes =
        (const struct PBERuntimeMaskClass *)((const u8 *)hdr +
                                             hdr->classTableOffset);
    const struct PBERuntimeSecondaryHashEntry *secondaryHashTable =
        (const struct PBERuntimeSecondaryHashEntry *)((const u8 *)hdr +
                                                      hdr->secondaryOffset);
    const struct PBERuntimeRuleMeta *ruleMeta =
        (const struct PBERuntimeRuleMeta *)((const u8 *)hdr +
                                            hdr->ruleMetaOffset);
    const u8 *literalBlob = (const u8 *)hdr + hdr->literalBlobOffset;
    const u32 literalBlobSize = hdr->literalBlobSize;

    size_t i;
    for (i = a->start_offset; i < a->len; i++) {
        struct PBEPositionContext ctx;

        pbeBuildPositionContext(hdr, selectors, a, i, &ctx, 1);
        if (pbeProcessMaskClassesForContext(hdr, classes, secondaryHashTable,
                                            ruleMeta, literalBlob,
                                            literalBlobSize, a, control,
                                            &ctx) == HWLM_TERMINATED) {
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
    const struct PBERuntimeMaskClass *classes =
        (const struct PBERuntimeMaskClass *)((const u8 *)hdr +
                                             hdr->classTableOffset);
    const struct PBERuntimeSecondaryHashEntry *secondaryHashTable =
        (const struct PBERuntimeSecondaryHashEntry *)((const u8 *)hdr +
                                                      hdr->secondaryOffset);
    const struct PBERuntimeRuleMeta *ruleMeta =
        (const struct PBERuntimeRuleMeta *)((const u8 *)hdr +
                                            hdr->ruleMetaOffset);
    const u8 *literalBlob = (const u8 *)hdr + hdr->literalBlobOffset;
    const u32 literalBlobSize = hdr->literalBlobSize;

    const u32 batchWidth = pbeSuggestedBatchWidth(hdr);
    size_t i;
    for (i = a->start_offset; i < a->len; i += batchWidth) {
        struct PBEPositionContext ctxs[PBE_BATCH_MAX_WIDTH];
        u32 laneCount = 0;

        laneCount = pbeBuildBatchContexts(hdr, selectors, a, i, batchWidth,
                                          ctxs);

        if (hdr->classCount <= 2) {
            u32 lane;
            for (lane = 0; lane < laneCount; lane++) {
                if (pbeProcessMaskClassesForContext(hdr, classes,
                                                    secondaryHashTable,
                                                    ruleMeta, literalBlob,
                                                    literalBlobSize, a, control,
                                                    &ctxs[lane]) ==
                    HWLM_TERMINATED) {
                    return HWLM_TERMINATED;
                }
            }
        } else {
            struct PBEMaskClassBatchState batchState;
            u32 lane;
            pbeBuildMaskClassBatchState(hdr, classes, ctxs, laneCount,
                                        &batchState);

            for (lane = 0; lane < laneCount; lane++) {
                if (pbeProcessMaskClassesForLaneWithBatchState(
                        hdr, classes, secondaryHashTable, ruleMeta,
                        literalBlob, literalBlobSize, a, control, &ctxs[lane],
                        &batchState, lane) == HWLM_TERMINATED) {
                    return HWLM_TERMINATED;
                }
            }
        }
    }

    return HWLM_SUCCESS;
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
    if (!fdr || !fdr->pbeOffset || !fdr->pbeSize) {
        return HWLM_SUCCESS;
    }

    {
        const u8 *base = (const u8 *)fdr;
        const void *blob = base + fdr->pbeOffset;
        u32 magic = 0;

        if (pbeReadBlobMagic(blob, fdr->pbeSize, &magic) &&
            magic == HAO_RUNTIME_MAGIC) {
            return haoExecBlobWithPath(blob, fdr->pbeSize, a, control, 0);
        }
    }

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
    ctx.validMask8 = pbeComputeValidMask8(a, endPos);

    return useVector ? pbeEntryMatchMaskFromContextVector(entry, &ctx)
                     : pbeEntryMatchMaskFromContextScalar(entry, &ctx);
}

u32 PbeRuntimeBitmapProbeMaskForTest(const u8 *bitmap, u32 bitmapSize,
                                     const u32 *primaryIdx, u32 laneCount,
                                     int usePacked) {
    struct PBEBitmapProbeState probe;

    memset(&probe, 0, sizeof(probe));
    pbePrepareBitmapProbeStateFromPrimaryIdx(primaryIdx, laneCount, &probe);

    return usePacked ? pbeProbeBitmapPacked(bitmap, bitmapSize, &probe)
                     : pbeProbeBitmapScalar(bitmap, bitmapSize, &probe);
}

int HaoRuntimeValidateLayoutForTest(const void *blob, u32 blobSize) {
    return haoValidateLayout(blob, blobSize, NULL);
}

int HaoRuntimeInspectBlobForTest(const void *blob, u32 blobSize,
                                 struct HAORuntimeInspectSummary *summary) {
    const struct HAORuntimeHeader *hdr = NULL;

    if (!summary) {
        return 0;
    }
    if (!haoValidateLayout(blob, blobSize, &hdr)) {
        return 0;
    }
    haoInspectLayout(hdr, summary);
    return 1;
}

void HaoRuntimeResetStatsForTest(void) {
    memset(&g_haoStats, 0, sizeof(g_haoStats));
    g_haoStatsForceEnabled = 1;
}

void HaoRuntimeGetStatsForTest(struct HAORuntimeStats *summary) {
    if (!summary) {
        return;
    }
    *summary = g_haoStats;
}

hwlm_error_t HaoEngineExecBlobNaiveForTest(const void *blob, u32 blobSize,
                                           const struct FDR_Runtime_Args *a,
                                           hwlm_group_t control) {
    return haoExecBlobWithPath(blob, blobSize, a, control, 0);
}

hwlm_error_t HaoEngineExecBlobBatchForTest(const void *blob, u32 blobSize,
                                           const struct FDR_Runtime_Args *a,
                                           hwlm_group_t control) {
    return haoExecBlobWithPath(blob, blobSize, a, control, 1);
}

hwlm_error_t PbeEngineExec(const struct FDR *fdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t control) {
    if (!fdr || !fdr->pbeOffset || !fdr->pbeSize) {
        return HWLM_SUCCESS;
    }

    {
        const u8 *base = (const u8 *)fdr;
        const void *blob = base + fdr->pbeOffset;
        u32 magic = 0;

        if (pbeReadBlobMagic(blob, fdr->pbeSize, &magic) &&
            magic == HAO_RUNTIME_MAGIC) {
            return haoExecBlobWithPath(blob, fdr->pbeSize, a,
                                       control, 1);
        }
    }

    return pbeExecWithPath(fdr, a, control, 1);
}
