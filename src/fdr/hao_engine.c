/*
 * Copyright (c) 2026, Huawei Technologies Co., Ltd.
 */

#include "fdr_enhanced.h"
#include "fdr_internal.h"
#include "hao_runtime.h"
#include "hao_runtime_inline.h"
#include "util/bitutils.h"
#include "util/simd_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct HAORuntimeStats g_haoStats;
static int g_haoStatsForceEnabled;
static int g_haoStatsEnvEnabled = -1;
static int g_haoStatsAtexitRegistered;
static int g_haoStatsActive;

/* 中文字符显示宽度=2，但 strlen 算3字节，需要补偿
 * 补偿量 = (strlen(s) - display_width) / 2 * 1
 * 用固定总显示列宽 = 42，右对齐冒号 */
#define HAO_STAT_FMT(label, fmt, val)                                    \
    do {                                                                  \
        int _blen = (int)strlen(label);                                   \
        /* 每个中文字符占3字节显示2列，多出1字节需从pad中扣除 */          \
        int _cjk  = 0;                                                    \
        for (const char *_p = (label); *_p; ) {                          \
            unsigned char _c = (unsigned char)*_p;                        \
            if (_c >= 0xE0) { _cjk++; _p += 3; }                        \
            else if (_c >= 0xC0) { _p += 2; }                            \
            else { _p += 1; }                                             \
        }                                                                 \
        int _pad = 42 - (_blen - _cjk);                                  \
        if (_pad < 1) _pad = 1;                                           \
        fprintf(stderr, "  %s%*s: " fmt "\n", label, _pad, "", val);     \
    } while(0)

static void haoDumpRuntimeStats(void);

static void haoRefreshStatsMode(void) {
    if (g_haoStatsEnvEnabled < 0) {
        const char *env = getenv("HS_HAO_STATS");
        g_haoStatsEnvEnabled = env && *env && *env != '0';
        if (g_haoStatsEnvEnabled && !g_haoStatsAtexitRegistered) {
            atexit(haoDumpRuntimeStats);
            g_haoStatsAtexitRegistered = 1;
        }
    }
    g_haoStatsActive = g_haoStatsForceEnabled || g_haoStatsEnvEnabled;
}

#define HAO_STATS_IF_ENABLED(stmt) \
    do {                           \
        if (g_haoStatsActive) {    \
            stmt;                  \
        }                          \
    } while (0)

#define HAO_STATS_ADD(field, delta) \
    HAO_STATS_IF_ENABLED(g_haoStats.field += (u64a)(delta))

struct HAOPositionContext {
    size_t endPos;
    u32 key;
    u32 validMask8;
    u8 laneWindowReady;
    u8 reserved0;
    u16 reserved1;
    u64a window64;
    u8 laneWindow32[HAO_RUNTIME_RULE_VECTOR_BYTES];
    u32 validMask32;
};

struct HAOPrimaryProbeState {
    u32 laneCount;
    u32 byteIndex[HAO_COMPAT_BATCH_MAX_WIDTH];
    u8 bitMask[HAO_COMPAT_BATCH_MAX_WIDTH];
    u32 activeLaneIndex[HAO_COMPAT_BATCH_MAX_WIDTH];
    u32 activePrimaryIdx[HAO_COMPAT_BATCH_MAX_WIDTH];
    u32 activeEncoded[HAO_COMPAT_BATCH_MAX_WIDTH];
};

static really_inline
void haoBuildPositionContext(const struct HAORuntimeHeader *hdr,
                             const struct HAORuntimeBitSelector *selectors,
                             const struct FDR_Runtime_Args *a, size_t endPos,
                             struct HAOPositionContext *ctx, int fillKey) {
    const u64a window64 =
        haoLoadWindow64Normalized(a, endPos, hdr->windowBytes);
    const u32 validMask8 = haoComputeValidMask8(a, endPos);

    assert(ctx);
    memset(ctx, 0, sizeof(*ctx));
    ctx->endPos = endPos;
    ctx->window64 = window64;
    ctx->validMask8 = validMask8;
    if (fillKey) {
        ctx->key = haoExtractKeyFromWindow(hdr, selectors, window64);
    }
}

static really_inline
void haoEnsureLaneWindowContext(struct HAOPositionContext *ctx) {
    if (!ctx || ctx->laneWindowReady) {
        return;
    }

    haoBuildLaneWindow32FromWindow(ctx->window64, ctx->validMask8,
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

    if (!primaryIdx || !state || laneCount > HAO_COMPAT_BATCH_MAX_WIDTH) {
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

    if (maxByte - minByte + 1 > HAO_COMPAT_BITMAP_GROUPED_BYTES) {
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
u32 haoProbeCompactAndLoadPrimary(const u8 *bitmap, u32 bitmapSize,
                                  const u32 *primaryHashTable,
                                  struct HAOPrimaryProbeState *state) {
    u32 activeCount = 0;
    u32 lane = 0;

    if (!bitmap || !primaryHashTable || !state) {
        return 0;
    }

    {
        u32 groupedActiveMask = 0;
        if (haoProbePrimaryBitmapGrouped(bitmap, bitmapSize, state,
                                         &groupedActiveMask)) {
            while (groupedActiveMask) {
                const u32 activeLane = ctz32(groupedActiveMask);
                const u32 primaryIdx = state->activePrimaryIdx[activeLane];
                state->activePrimaryIdx[activeCount] = primaryIdx;
                state->activeLaneIndex[activeCount] = activeLane;
                state->activeEncoded[activeCount] =
                    primaryHashTable[primaryIdx];
                activeCount++;
                groupedActiveMask &= groupedActiveMask - 1U;
            }
            return activeCount;
        }
    }

#if defined(HAVE_NEON) || defined(HAVE_SSE2)
    {
        u8 gatheredBytes[HAO_COMPAT_BATCH_MAX_WIDTH];

        for (lane = 0; lane < state->laneCount; lane++) {
            const u32 idx = state->byteIndex[lane];
            gatheredBytes[lane] = idx < bitmapSize ? bitmap[idx] : 0;
        }

        for (lane = 0; lane < state->laneCount; lane += 16U) {
            const u32 lanesThisRound = MIN(16U, state->laneCount - lane);
            const u32 laneMask = lanesThisRound == 16U
                                     ? 0xffffU
                                     : ((1U << lanesThisRound) - 1U);
            const m128 gathered = loadu128(gatheredBytes + lane);
            const m128 masks = loadu128(state->bitMask + lane);
            const m128 masked = and128(gathered, masks);
            u32 activeMask =
                (~movemask128(eq128(masked, zeroes128()))) & laneMask;

            while (activeMask) {
                const u32 activeLane = lane + ctz32(activeMask);
                const u32 primaryIdx = state->activePrimaryIdx[activeLane];
                state->activePrimaryIdx[activeCount] = primaryIdx;
                state->activeLaneIndex[activeCount] = activeLane;
                state->activeEncoded[activeCount] =
                    primaryHashTable[primaryIdx];
                activeCount++;
                activeMask &= activeMask - 1U;
            }
        }

        return activeCount;
    }
#else
    for (lane = 0; lane < state->laneCount; lane++) {
        const u32 idx = state->byteIndex[lane];
        const u8 gathered = idx < bitmapSize ? bitmap[idx] : 0;

        if (!(gathered & state->bitMask[lane])) {
            continue;
        }
        state->activeLaneIndex[activeCount] = lane;
        state->activeEncoded[activeCount] =
            primaryHashTable[state->activePrimaryIdx[lane]];
        state->activePrimaryIdx[activeCount] = state->activePrimaryIdx[lane];
        activeCount++;
    }

    return activeCount;
#endif
}

static double haoStatsPct(u64a num, u64a den) {
    if (!den) {
        return 0.0;
    }
    return (100.0 * (double)num) / (double)den;
}

static double haoStatsPerMiB(u64a num, u64a bytes) {
    if (!bytes) {
        return 0.0;
    }
    return ((double)num * 1048576.0) / (double)bytes;
}

static void haoDumpRuntimeStats(void) {
    const u64a l2EntryRejects =
        g_haoStats.encodedEntriesVisited >= g_haoStats.verifierEntryHits
            ? g_haoStats.encodedEntriesVisited - g_haoStats.verifierEntryHits
            : 0;
    const u64a l2LaneNoReport =
        g_haoStats.encodedRangeCalls >= g_haoStats.encodedRangeReportCalls
            ? g_haoStats.encodedRangeCalls - g_haoStats.encodedRangeReportCalls
            : 0;
    const u64a l2SlotEligible =
        g_haoStats.verifierSlotHits >= g_haoStats.encodedGroupRejects
            ? g_haoStats.verifierSlotHits - g_haoStats.encodedGroupRejects
            : 0;
    const u64a l2SlotFalsePos =
        l2SlotEligible >= (g_haoStats.directReports +
                           g_haoStats.encodedConfirmMatches)
            ? l2SlotEligible - (g_haoStats.directReports +
                                g_haoStats.encodedConfirmMatches)
            : 0;

    if (!g_haoStatsActive) {
        return;
    }
    fprintf(stderr, "[HAO][Runtime/运行时]\n");
    HAO_STAT_FMT("scans(扫描次数)",           "%llu", (unsigned long long)g_haoStats.scanCalls);
    HAO_STAT_FMT("inputBytes(输入字节数)",    "%llu", (unsigned long long)g_haoStats.scanInputBytes);
    HAO_STAT_FMT("callbackReports(回调上报次数)", "%llu", (unsigned long long)g_haoStats.callbackReports);

    fprintf(stderr, "[HAO][L1/一级过滤]\n");
    HAO_STAT_FMT("blockCalls(分块处理次数)",      "%llu", (unsigned long long)g_haoStats.blockCalls);
    HAO_STAT_FMT("blockLanes(分块总lane数)",       "%llu", (unsigned long long)g_haoStats.blockLanes);
    HAO_STAT_FMT("primaryProbeLanes(L1探测lane数)", "%llu", (unsigned long long)g_haoStats.primaryProbeLanes);
    HAO_STAT_FMT("primaryActiveLanes(L1命中lane数)", "%llu", (unsigned long long)g_haoStats.primaryActiveLanes);
    HAO_STAT_FMT("activePct(L1命中率)",            "%.5f",
        haoStatsPct(g_haoStats.primaryActiveLanes, g_haoStats.primaryProbeLanes));

    fprintf(stderr, "[HAO][L2/二级过滤]\n");
    HAO_STAT_FMT("rangeCalls(L2范围处理次数)",         "%llu", (unsigned long long)g_haoStats.encodedRangeCalls);
    HAO_STAT_FMT("rangeReportCalls(L2产生报告的lane数)", "%llu", (unsigned long long)g_haoStats.encodedRangeReportCalls);
    HAO_STAT_FMT("entriesVisited(L2访问entry数)",       "%llu", (unsigned long long)g_haoStats.encodedEntriesVisited);
    HAO_STAT_FMT("verifierCalls(verifier调用次数)",     "%llu", (unsigned long long)g_haoStats.verifierCalls);
    HAO_STAT_FMT("verifierEntryHits(verifier命中的entry数)", "%llu", (unsigned long long)g_haoStats.verifierEntryHits);
    HAO_STAT_FMT("verifierSlotHits(verifier命中的slot数)",   "%llu", (unsigned long long)g_haoStats.verifierSlotHits);
    HAO_STAT_FMT("groupRejects(group过滤拒绝数)",       "%llu", (unsigned long long)g_haoStats.encodedGroupRejects);
    HAO_STAT_FMT("directReports(直接上报次数)",         "%llu", (unsigned long long)g_haoStats.directReports);
    HAO_STAT_FMT("confirmCalls(精确确认次数)",          "%llu", (unsigned long long)g_haoStats.encodedConfirmCalls);
    HAO_STAT_FMT("confirmMatches(精确确认命中数)",      "%llu", (unsigned long long)g_haoStats.encodedConfirmMatches);
    HAO_STAT_FMT("confirmRejects(精确确认拒绝数)",      "%llu", (unsigned long long)g_haoStats.encodedConfirmRejects);

    fprintf(stderr, "[HAO][Residual/兜底路径]\n");
    HAO_STAT_FMT("posCalls(兜底位置次数)",      "%llu", (unsigned long long)g_haoStats.residualPosCalls);
    HAO_STAT_FMT("ruleChecks(兜底规则检查数)",  "%llu", (unsigned long long)g_haoStats.residualRuleChecks);
    HAO_STAT_FMT("groupRejects(兜底group拒绝数)", "%llu", (unsigned long long)g_haoStats.residualGroupRejects);
    HAO_STAT_FMT("confirmCalls(兜底确认次数)",  "%llu", (unsigned long long)g_haoStats.residualConfirmCalls);
    HAO_STAT_FMT("confirmMatches(兜底确认命中数)", "%llu", (unsigned long long)g_haoStats.residualConfirmMatches);
    HAO_STAT_FMT("confirmRejects(兜底确认拒绝数)", "%llu", (unsigned long long)g_haoStats.residualConfirmRejects);

    fprintf(stderr, "[HAO][Rates/关键比率]\n");
    HAO_STAT_FMT("l2EntryFalsePositivePct(L2表项假阳性率)",   "%.5f",
        haoStatsPct(l2EntryRejects, g_haoStats.encodedEntriesVisited));
    HAO_STAT_FMT("l2LaneNoReportPct(L2无报告lane占比)",        "%.5f",
        haoStatsPct(l2LaneNoReport, g_haoStats.encodedRangeCalls));
    HAO_STAT_FMT("l2ConfirmFalsePositivePct(L2确认假阳性率)", "%.5f",
        haoStatsPct(g_haoStats.encodedConfirmRejects, g_haoStats.encodedConfirmCalls));
    HAO_STAT_FMT("l2SlotFalsePositivePct(L2槽位假阳性率)",    "%.5f",
        haoStatsPct(l2SlotFalsePos, l2SlotEligible));
    HAO_STAT_FMT("l2EntriesPerMiB(每MiB访问L2表项数)",        "%.5f",
        haoStatsPerMiB(g_haoStats.encodedEntriesVisited, g_haoStats.scanInputBytes));
    HAO_STAT_FMT("l2ConfirmCallsPerMiB(每MiB精确确认次数)",   "%.5f",
        haoStatsPerMiB(g_haoStats.encodedConfirmCalls, g_haoStats.scanInputBytes));
    HAO_STAT_FMT("reportsPerMiB(每MiB报告次数)",               "%.5f",
        haoStatsPerMiB(g_haoStats.callbackReports, g_haoStats.scanInputBytes));
}

static int haoCompatRuleExactMatch(const HAOCompatRuntimeRuleMeta *rm,
                                   const struct FDR_Runtime_Args *a,
                                   size_t endPos, const u8 *literalBlob,
                                   u32 literalBlobSize) {
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
    const int nocase = (rm->flags & HAO_COMPAT_RULE_FLAG_NOCASE) ? 1 : 0;
    u16 i;
    for (i = 0; i < len; i++) {
        u8 got;
        if (!haoCompatGetByteAt(a, startPos + i, &got)) {
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

    if ((rm->flags & HAO_COMPAT_RULE_FLAG_HAS_MASK) && rm->maskLen) {
        const u8 mlen = rm->maskLen;
        if (mlen > sizeof(rm->msk)) {
            return 0;
        }
        const s64a maskStart = startPos + (s64a)len - (s64a)mlen;
        for (i = 0; i < mlen; i++) {
            u8 got;
            if (!haoCompatGetByteAt(a, maskStart + i, &got)) {
                return 0;
            }
            if ((got & rm->msk[i]) != rm->cmp[i]) {
                return 0;
            }
        }
    }

    return 1;
}

static int haoCompatValidateLayout(const struct FDR *fdr,
                                   const HAOCompatRuntimeHeader *hdr) {
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
            sizeof(HAOCompatRuntimeBitSelector) >
        (u64a)fdrMatcherBlobSize(fdr)) {
        return 0;
    }
    if ((u64a)hdr->classTableOffset + (u64a)hdr->classCount *
            sizeof(HAOCompatRuntimeMaskClass) >
        (u64a)fdrMatcherBlobSize(fdr)) {
        return 0;
    }
    if ((u64a)hdr->secondaryOffset + (u64a)hdr->secondaryCount *
            sizeof(HAOCompatRuntimeSecondaryHashEntry) >
        (u64a)fdrMatcherBlobSize(fdr)) {
        return 0;
    }
    if ((u64a)hdr->ruleMetaOffset + (u64a)hdr->ruleMetaCount *
            sizeof(HAOCompatRuntimeRuleMeta) >
        (u64a)fdrMatcherBlobSize(fdr)) {
        return 0;
    }
    if ((u64a)hdr->literalBlobOffset + (u64a)hdr->literalBlobSize >
        (u64a)fdrMatcherBlobSize(fdr)) {
        return 0;
    }
    {
        const HAOCompatRuntimeMaskClass *classes =
            (const HAOCompatRuntimeMaskClass *)((const u8 *)hdr +
                                                 hdr->classTableOffset);
        u32 i;
        for (i = 0; i < hdr->classCount; i++) {
            const HAOCompatRuntimeMaskClass *klass = &classes[i];
            if (klass->classKeyBits > hdr->keyBits) {
                return 0;
            }
            if ((u64a)klass->primaryBitmapOffset + (u64a)klass->primaryBitmapSize >
                (u64a)fdrMatcherBlobSize(fdr)) {
                return 0;
            }
            if ((u64a)klass->primaryOffset + (u64a)klass->primaryCount * sizeof(u32) >
                (u64a)fdrMatcherBlobSize(fdr)) {
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

/* Validate the HAO v2 blob layout before entering execution. */
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
    if (hdr->selectorCount > HAO_RUNTIME_MAX_SELECTORS) {
        return 0;
    }
    if (!hdr->windowBytes || hdr->windowBytes > HAO_RUNTIME_BYTES_PER_RULE_SLOT) {
        return 0;
    }
    if (hdr->extractMode > HAO_RUNTIME_EXTRACT_MODE_BEXT) {
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

/* Collect a read-only summary for inspecting HAO v2 blobs in tests. */
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
            const u32 entryCount = primary[i] >> HAO_RUNTIME_L1_COUNT_SHIFT;
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
                       ((1U << HAO_RUNTIME_RULE_SLOTS_PER_ENTRY) - 1U));
    }
}

/* HAO v2 still confirms matches against the original literal and mask/cmp
 * payload. This remains the correctness boundary until a cheaper HAO-native
 * confirm path is ready. */
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
        const int nocase = (rm->flags & HAO_COMPAT_RULE_FLAG_NOCASE) ? 1 : 0;
        u16 i;

        for (i = 0; i < rm->len; i++) {
            u8 got;
            if (!haoCompatGetByteAt(a, startPos + i, &got)) {
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

        if ((rm->flags & HAO_COMPAT_RULE_FLAG_HAS_MASK) && rm->maskLen) {
            const u8 mlen = rm->maskLen;
            const s64a maskStart = startPos + (s64a)rm->len - (s64a)mlen;
            for (i = 0; i < mlen && i < sizeof(rm->msk); i++) {
                u8 got;
                if (!haoCompatGetByteAt(a, maskStart + i, &got)) {
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
    
    for (i = 0; i < HAO_RUNTIME_RULE_VECTOR_BYTES; i++) {
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
    return entry->slotMask & ((1U << HAO_RUNTIME_RULE_SLOTS_PER_ENTRY) - 1U);
}

static really_inline
u32 haoEntrySlotCount(const struct HAORuntimeSecondaryHashEntry *entry) {
    const u32 slotMask = haoEntrySlotMask(entry);

    if (!slotMask) {
        return 0;
    }
    if (entry && entry->slotCount) {
        return MIN(entry->slotCount,
                   (u8)HAO_RUNTIME_RULE_SLOTS_PER_ENTRY);
    }
    return popcount32(slotMask);
}

static really_inline
int haoEntryHasIdentityTableControl(
    const struct HAORuntimeSecondaryHashEntry *entry) {
    return entry &&
           (entry->flags & HAO_RUNTIME_SECONDARY_ENTRY_FLAG_IDENTITY_TBL) != 0;
}

static really_inline
u32 haoEntryLaneMaskFromByteMatches(
    const struct HAORuntimeSecondaryHashEntry *entry, u32 byteMatchMask) {
    u32 laneMask = 0;
    u32 slotMask = haoEntrySlotMask(entry);
    while (slotMask) {
        const u32 slot = ctz32(slotMask);
        const u32 laneBase = slot * HAO_RUNTIME_BYTES_PER_RULE_SLOT;
        const u32 laneRequired = entry->tailMask &
                                 (0xffU << laneBase);

        if (laneRequired &&
            ((byteMatchMask & laneRequired) == laneRequired)) {
            laneMask |= (1U << slot);
        }
        slotMask &= slotMask - 1U;
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

    if (haoEntryHasIdentityTableControl(entry)) {
        mask = entry->tailMask;
        while (mask) {
            const u32 bit = mask & (0U - mask);
            const u32 idx = ctz32(mask);
            if (!(ctx->validMask32 & bit) ||
                ctx->laneWindow32[idx] != entry->ruleVector[idx]) {
                return 0;
            }
            mask &= mask - 1;
        }
        return haoEntrySlotMask(entry);
    }

    mask = entry->tailMask;
    while (mask) {
        const u32 bit = mask & (0U - mask);
        const u32 idx = ctz32(mask);
        const u8 srcCtl = entry->tableControl[idx];
        const u32 chunkBase = idx & 0x10U;
        const u32 srcIndex = chunkBase + srcCtl;

        if (!(shuffledValidMask & bit) ||
            srcCtl & 0x80U ||
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
    u32 shuffledValidMask = 0;
    const u32 slotMask = haoEntrySlotMask(entry);
    const u32 slotCount = haoEntrySlotCount(entry);
    const int identityTableControl = haoEntryHasIdentityTableControl(entry);

    if (!entry || !slotMask || !ctx) {
        return 0;
    }

    HAO_STATS_ADD(verifierCalls, 1);
    haoEnsureLaneWindowContext(ctx);
    if (identityTableControl) {
        shuffledValidMask = ctx->validMask32 & entry->tailMask;
    } else {
        shuffledValidMask = haoBuildShuffledValidMask(entry, ctx);
    }

    if (slotCount == 1) {
        const u32 laneMask =
            haoEntrySingleSlotMatchMaskFromContext(entry, ctx,
                                                   shuffledValidMask);
        if (laneMask) {
            HAO_STATS_ADD(verifierEntryHits, 1);
            HAO_STATS_ADD(verifierSlotHits, 1);
        }
        return laneMask;
    }

#if defined(HAVE_NEON) || defined(HAVE_SSE2)
    {
        {
            const m128 inputLo = loadu128(ctx->laneWindow32);
            const m128 inputHi = loadu128(ctx->laneWindow32 + 16);
            const m128 ruleLo = loadu128(entry->ruleVector);
            const m128 ruleHi = loadu128(entry->ruleVector + 16);
            u32 eqLo;
            u32 eqHi;

            if (identityTableControl) {
                eqLo = movemask128(eq128(inputLo, ruleLo));
                eqHi = movemask128(eq128(inputHi, ruleHi));
            } else {
                const m128 ctrlLo = loadu128(entry->tableControl);
                const m128 ctrlHi = loadu128(entry->tableControl + 16);
                const m128 shufLo = pshufb_m128(inputLo, ctrlLo);
                const m128 shufHi = pshufb_m128(inputHi, ctrlHi);
                eqLo = movemask128(eq128(shufLo, ruleLo));
                eqHi = movemask128(eq128(shufHi, ruleHi));
            }
            const u32 byteMatchMask = (eqLo | (eqHi << 16)) &
                                      shuffledValidMask & entry->tailMask;
            const u32 laneMask =
                haoEntryLaneMaskFromByteMatches(entry, byteMatchMask);

            if (laneMask) {
                HAO_STATS_ADD(verifierEntryHits, 1);
                HAO_STATS_ADD(verifierSlotHits, popcount32(laneMask));
            }
            return laneMask;
        }
    } 
#else
    {
        {
            u32 byteMatchMask = 0;
            u32 i;

            for (i = 0; i < HAO_RUNTIME_RULE_VECTOR_BYTES; i++) {
                const u32 bit = 1U << i;
                u8 got;
                if (!(shuffledValidMask & bit)) {
                    continue;
                }
                if (identityTableControl) {
                    got = ctx->laneWindow32[i];
                } else {
                    const u8 srcCtl = entry->tableControl[i];
                    const u32 chunkBase = i & 0x10U;
                    const u32 srcIndex = chunkBase + srcCtl;
                    got = ctx->laneWindow32[srcIndex];
                }
                if (got == entry->ruleVector[i]) {
                    byteMatchMask |= bit;
                }
            }

            {
                const u32 laneMask =
                    haoEntryLaneMaskFromByteMatches(entry, byteMatchMask);
                if (laneMask) {
                    HAO_STATS_ADD(verifierEntryHits, 1);
                    HAO_STATS_ADD(verifierSlotHits, popcount32(laneMask));
                }
                return laneMask;
            }
        }
    }
#endif
}

static u32 haoEntryMatchMaskFromContextScalarForTest(
    const struct HAORuntimeSecondaryHashEntry *entry,
    struct HAOPositionContext *ctx) {
    u32 shuffledValidMask = 0;
    const u32 slotMask = haoEntrySlotMask(entry);
    const u32 slotCount = haoEntrySlotCount(entry);
    const int identityTableControl = haoEntryHasIdentityTableControl(entry);

    if (!entry || !slotMask || !ctx) {
        return 0;
    }

    haoEnsureLaneWindowContext(ctx);
    if (identityTableControl) {
        shuffledValidMask = ctx->validMask32 & entry->tailMask;
    } else {
        shuffledValidMask = haoBuildShuffledValidMask(entry, ctx);
    }

    if (slotCount == 1) {
        return haoEntrySingleSlotMatchMaskFromContext(entry, ctx,
                                                      shuffledValidMask);
    }

    {
        u32 byteMatchMask = 0;
        u32 i;

        for (i = 0; i < HAO_RUNTIME_RULE_VECTOR_BYTES; i++) {
            const u32 bit = 1U << i;
            u8 got;

            if (!(shuffledValidMask & bit)) {
                continue;
            }
            if (identityTableControl) {
                got = ctx->laneWindow32[i];
            } else {
                const u8 srcCtl = entry->tableControl[i];
                const u32 chunkBase = i & 0x10U;
                const u32 srcIndex = chunkBase + srcCtl;
                got = ctx->laneWindow32[srcIndex];
            }
            if (got == entry->ruleVector[i]) {
                byteMatchMask |= bit;
            }
        }

        return haoEntryLaneMaskFromByteMatches(entry, byteMatchMask);
    }
}

static u32 haoEntryMatchMaskFromContextVectorForTest(
    const struct HAORuntimeSecondaryHashEntry *entry,
    struct HAOPositionContext *ctx) {
    u32 shuffledValidMask = 0;
    const u32 slotMask = haoEntrySlotMask(entry);
    const u32 slotCount = haoEntrySlotCount(entry);
    const int identityTableControl = haoEntryHasIdentityTableControl(entry);

    if (!entry || !slotMask || !ctx) {
        return 0;
    }

    haoEnsureLaneWindowContext(ctx);
    if (identityTableControl) {
        shuffledValidMask = ctx->validMask32 & entry->tailMask;
    } else {
        shuffledValidMask = haoBuildShuffledValidMask(entry, ctx);
    }

    if (slotCount == 1) {
        return haoEntrySingleSlotMatchMaskFromContext(entry, ctx,
                                                      shuffledValidMask);
    }

#if defined(HAVE_NEON) || defined(HAVE_SSE2)
    {
        const m128 inputLo = loadu128(ctx->laneWindow32);
        const m128 inputHi = loadu128(ctx->laneWindow32 + 16);
        const m128 ruleLo = loadu128(entry->ruleVector);
        const m128 ruleHi = loadu128(entry->ruleVector + 16);
        u32 eqLo;
        u32 eqHi;

        if (identityTableControl) {
            eqLo = movemask128(eq128(inputLo, ruleLo));
            eqHi = movemask128(eq128(inputHi, ruleHi));
        } else {
            const m128 ctrlLo = loadu128(entry->tableControl);
            const m128 ctrlHi = loadu128(entry->tableControl + 16);
            const m128 shufLo = pshufb_m128(inputLo, ctrlLo);
            const m128 shufHi = pshufb_m128(inputHi, ctrlHi);
            eqLo = movemask128(eq128(shufLo, ruleLo));
            eqHi = movemask128(eq128(shufHi, ruleHi));
        }

        return haoEntryLaneMaskFromByteMatches(
            entry, (eqLo | (eqHi << 16)) & shuffledValidMask & entry->tailMask);
    }
#else
    return haoEntryMatchMaskFromContextScalarForTest(entry, ctx);
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
    int anyReport = 0;

    if (!encoded || !ctx) {
        return HWLM_SUCCESS;
    }

    HAO_STATS_ADD(encodedRangeCalls, 1);
    haoCompatDecodePrimaryValue(encoded, &offset, &count);
    for (n = 0; n < count; n++) {
        const u32 off = offset + n;
        const struct HAORuntimeSecondaryHashEntry *entry;
        u32 laneMask;
        u32 matchMask;

        if (!off || off >= hdr->secondaryCount) {
            break;
        }

        entry = &secondaryHashTable[off];
        HAO_STATS_ADD(encodedEntriesVisited, 1);
        laneMask = haoEntryMatchMaskFromContext(entry, ctx);
        if (!laneMask) {
            continue;
        }

        matchMask = laneMask & haoEntrySlotMask(entry);
        while (matchMask) {
            const u32 r = ctz32(matchMask);
            const u16 ridx = entry->ruleIndex[r];
            const struct HAORuntimeRuleMeta *rm;

            if (ridx >= hdr->ruleMetaCount) {
                matchMask &= matchMask - 1U;
                continue;
            }

            rm = &ruleMeta[ridx];
            if (!(rm->groups & *control)) {
                HAO_STATS_ADD(encodedGroupRejects, 1);
                matchMask &= matchMask - 1U;
                continue;
            }
            if (haoRuleCanReportFromVerifier(rm)) {
                HAO_STATS_ADD(directReports, 1);
            } else {
                HAO_STATS_ADD(encodedConfirmCalls, 1);
                if (!haoRuleExactMatch(rm, a, ctx->endPos, literalBlob,
                                       literalBlobSize)) {
                    HAO_STATS_ADD(encodedConfirmRejects, 1);
                    matchMask &= matchMask - 1U;
                    continue;
                }
                HAO_STATS_ADD(encodedConfirmMatches, 1);
            }

            HAO_STATS_ADD(callbackReports, 1);
            anyReport = 1;
            *control = a->cb(ctx->endPos, rm->id, a->scratch);
            if (*control == HWLM_TERMINATE_MATCHING) {
                HAO_STATS_ADD(encodedRangeReportCalls, 1);
                return HWLM_TERMINATED;
            }
            matchMask &= matchMask - 1U;
        }
    }

    HAO_STATS_ADD(encodedRangeReportCalls, anyReport ? 1 : 0);

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

    HAO_STATS_ADD(residualPosCalls, 1);
    for (i = 0; i < hdr->residualRuleCount; i++) {
        const u32 ridx = residualRuleIndexes[i];
        const struct HAORuntimeRuleMeta *rm;

        if (ridx >= hdr->ruleMetaCount) {
            continue;
        }

        rm = &ruleMeta[ridx];
        if (!(rm->groups & *control)) {
            HAO_STATS_ADD(residualGroupRejects, 1);
            continue;
        }
        HAO_STATS_ADD(residualRuleChecks, 1);
        HAO_STATS_ADD(residualConfirmCalls, 1);
        if (!haoRuleExactMatch(rm, a, endPos, literalBlob, literalBlobSize)) {
            HAO_STATS_ADD(residualConfirmRejects, 1);
            continue;
        }
        HAO_STATS_ADD(residualConfirmMatches, 1);

        HAO_STATS_ADD(callbackReports, 1);
        *control = a->cb(endPos, rm->id, a->scratch);
        if (*control == HWLM_TERMINATE_MATCHING) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

/* Naive HAO v2 execution path used both as a correctness baseline and as a
 * fallback when the batch kernel is not selected. */
static int haoRunNaiveBlob(const struct HAORuntimeHeader *hdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t *control) {
    const struct HAORuntimeBitSelector *selectors;
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

    haoRefreshStatsMode();
    HAO_STATS_ADD(scanCalls, 1);
    HAO_STATS_ADD(scanInputBytes, a->len - a->start_offset);

    selectors = (const struct HAORuntimeBitSelector *)((const u8 *)hdr +
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

        haoBuildPositionContext(hdr, selectors, a, i, &ctx, 1);
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

/* Batch block state for the HAO v2 global single-table front-end. */
struct HAOBlockState {
    u32 laneCount;
    size_t endPos[HAO_COMPAT_BATCH_MAX_WIDTH];
    u64a windows[HAO_COMPAT_BATCH_MAX_WIDTH];
    u32 keys[HAO_COMPAT_BATCH_MAX_WIDTH];
    u32 validMask8[HAO_COMPAT_BATCH_MAX_WIDTH];
};

static void haoBuildBlockByteView(const struct FDR_Runtime_Args *a,
                                  size_t blockStart, u32 laneCount,
                                  u8 *blockBytes) {
    const u32 prefix = HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U;
    u32 i;

    memset(blockBytes, 0, prefix + laneCount);
    for (i = 0; i < prefix; i++) {
        u8 b = 0;
        haoCompatGetByteAt(a, (s64a)blockStart - (s64a)prefix + i, &b);
        blockBytes[i] = b;
    }

    memcpy(blockBytes + prefix, a->buf + blockStart, laneCount);
}

static void haoBuildByteLanesFromBlockBytes(
    const u8 *blockBytes, u32 laneCount,
    u8 byteLanes[HAO_RUNTIME_BYTES_PER_RULE_SLOT][HAO_COMPAT_BATCH_MAX_WIDTH]) {
    u32 byteIdx;
    u32 lane;

    if (!blockBytes || !byteLanes || !laneCount) {
        return;
    }

    memset(byteLanes, 0,
           sizeof(u8) * HAO_RUNTIME_BYTES_PER_RULE_SLOT *
               HAO_COMPAT_BATCH_MAX_WIDTH);

#if defined(__aarch64__) || defined(__x86_64__)
    {
        m128 lo;
        m128 hi;
        m128 src0 = loadu128(blockBytes);
        m128 src1 = loadu128(blockBytes + 16);
        m128 src2 = loadu128(blockBytes + 32);

        for (byteIdx = 0; byteIdx < HAO_RUNTIME_BYTES_PER_RULE_SLOT; byteIdx++) {
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

        if (laneCount < HAO_COMPAT_BATCH_MAX_WIDTH) {
            for (byteIdx = 0; byteIdx < HAO_RUNTIME_BYTES_PER_RULE_SLOT;
                 byteIdx++) {
                memset(byteLanes[byteIdx] + laneCount, 0,
                       HAO_COMPAT_BATCH_MAX_WIDTH - laneCount);
            }
        }
        return;
    }
#endif

    for (byteIdx = 0; byteIdx < HAO_RUNTIME_BYTES_PER_RULE_SLOT; byteIdx++) {
        for (lane = 0; lane < laneCount; lane++) {
            byteLanes[byteIdx][lane] = blockBytes[lane + byteIdx];
        }
    }
}

// TODO: 待优化：使用向量化的方式来处理
static void haoBuildWindowsFromBlockBytes(const u8 *blockBytes, u32 laneCount,
                                          u32 windowBytes, u64a *windows) {
    u32 lane;
    u32 i;

    if (!blockBytes || !windows || !laneCount) {
        return;
    }

    if (!windowBytes || windowBytes > HAO_RUNTIME_BYTES_PER_RULE_SLOT) {
        windowBytes = HAO_RUNTIME_BYTES_PER_RULE_SLOT;
    }

    memset(windows, 0, sizeof(u64a) * laneCount);
    for (lane = 0; lane < laneCount; lane++) {
        if (windowBytes == HAO_RUNTIME_BYTES_PER_RULE_SLOT) {
            windows[lane] =
                haoByteReverse64(unaligned_load_u64a(blockBytes + lane));
            continue;
        }

        for (i = 0; i < windowBytes; i++) {
            const u32 shift = i * 8U;
            windows[lane] |=
                ((u64a)blockBytes[lane + windowBytes - 1U - i]) << shift;
        }
    }
}

static void haoExtractKeysFromBlockBytes(
    const struct HAORuntimeHeader *hdr,
    const struct HAORuntimeBitSelector *selectors, const u8 *blockBytes,
    struct HAOBlockState *state) {
    u32 sel;
    const u32 laneMaskLimit = state && state->laneCount < 32U
                                  ? ((1U << state->laneCount) - 1U)
                                  : 0xffffffffU;

    if (!hdr || !selectors || !blockBytes || !state || !state->laneCount) {
        return;
    }

    memset(state->keys, 0, sizeof(state->keys[0]) * state->laneCount);

    if (hdr->extractMode == HAO_RUNTIME_EXTRACT_MODE_BEXT) {
        haoExtractKeysFromWindows(hdr, selectors, state->windows,
                                  state->laneCount, state->keys);
        return;
    }

    {
        u8 byteLanes[HAO_RUNTIME_BYTES_PER_RULE_SLOT][HAO_COMPAT_BATCH_MAX_WIDTH];

        haoBuildByteLanesFromBlockBytes(blockBytes, state->laneCount, byteLanes);

        for (sel = 0; sel < hdr->selectorCount &&
                      sel < HAO_RUNTIME_MAX_SELECTORS; sel++) {
            const u32 srcByte =
                HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U - selectors[sel].byteOffset;
            const u8 bit = (u8)(1U << selectors[sel].bitOffset);

#if defined(__aarch64__) || defined(__x86_64__)
            {
                const m128 zero = zeroes128();
                const m128 bitMask = set16x8(bit);
                const m128 loBytes = loadu128(byteLanes[srcByte]);
                const m128 hiBytes = loadu128(byteLanes[srcByte] + 16);
                const u32 loActive =
                    (~movemask128(eq128(and128(loBytes, bitMask), zero))) &
                    0xffffU;
                const u32 hiActive =
                    (~movemask128(eq128(and128(hiBytes, bitMask), zero))) &
                    0xffffU;
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
                if (byteLanes[srcByte][lane] & bit) {
                    state->keys[lane] |= (1U << sel);
                }
            }
        }
    }
}

/* Build one HAO batch block state. The block byte view is materialized once,
 * then reused to build per-lane windows and derive keys for the whole block. */
static int haoBuildBlockState(const struct HAORuntimeHeader *hdr,
                              const struct HAORuntimeBitSelector *selectors,
                              const struct FDR_Runtime_Args *a,
                              size_t blockStart, u32 blockLaneCount,
                              struct HAOBlockState *state) {
    static const u32 validMaskLut[HAO_RUNTIME_BYTES_PER_RULE_SLOT + 1U] = {
        0x00U, 0x80U, 0xc0U, 0xe0U, 0xf0U, 0xf8U, 0xfcU, 0xfeU, 0xffU};
    u8 blockBytes[HAO_RUNTIME_BLOCK_BYTES + 16U];
    u32 laneCount = 0;
    int fullyValidBlock;

    if (!hdr || !selectors || !a || !state || !blockLaneCount ||
        blockLaneCount > HAO_COMPAT_BATCH_MAX_WIDTH) {
        return 0;
    }

    fullyValidBlock =
        ((u64a)blockStart + a->len_history) >=
        (HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U);

    if (fullyValidBlock) {
        while (laneCount < blockLaneCount && blockStart + laneCount < a->len) {
            const size_t pos = blockStart + laneCount;
            state->endPos[laneCount] = pos;
            state->validMask8[laneCount] = 0xffU;
            laneCount++;
        }
    } else {
        while (laneCount < blockLaneCount && blockStart + laneCount < a->len) {
            const size_t pos = blockStart + laneCount;
            const u64a available = (u64a)pos + 1U + a->len_history;

            state->endPos[laneCount] = pos;
            state->validMask8[laneCount] =
                available >= HAO_RUNTIME_BYTES_PER_RULE_SLOT
                    ? 0xffU
                    : validMaskLut[available];
            laneCount++;
        }
    }

    if (!laneCount) {
        return 0;
    }

    haoBuildBlockByteView(a, blockStart, laneCount, blockBytes);
    state->laneCount = laneCount;
    haoBuildWindowsFromBlockBytes(blockBytes, laneCount, hdr->windowBytes,
                                  state->windows);
    haoExtractKeysFromBlockBytes(hdr, selectors, blockBytes, state);
    return laneCount;
}

static void haoBuildContextFromBlockState(const struct HAOBlockState *state,
                                          u32 lane,
                                          struct HAOPositionContext *ctx) {
    if (!state || !ctx || lane >= state->laneCount) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->endPos = state->endPos[lane];
    ctx->window64 = state->windows[lane];
    ctx->validMask8 = state->validMask8[lane];
    ctx->key = state->keys[lane];
}

static int haoProcessBlockBatch(const struct HAORuntimeHeader *hdr,
                                const struct HAORuntimeBitSelector *selectors,
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
    u32 primaryIdx[HAO_COMPAT_BATCH_MAX_WIDTH] = {0};
    u32 encodedByLane[HAO_COMPAT_BATCH_MAX_WIDTH] = {0};
    u32 activeCount = 0;
    u32 lane;

    if (!hdr || !selectors || !primaryBitmap || !primaryHashTable ||
        !secondaryHashTable || !ruleMeta || !literalBlob || !a || !control ||
        !blockLaneCount || blockLaneCount > HAO_COMPAT_BATCH_MAX_WIDTH) {
        return HWLM_SUCCESS;
    }

    if (!haoBuildBlockState(hdr, selectors, a, blockStart, blockLaneCount,
                            &block)) {
        return HWLM_SUCCESS;
    }

    HAO_STATS_ADD(blockCalls, 1);
    HAO_STATS_ADD(blockLanes, block.laneCount);

    for (lane = 0; lane < block.laneCount; lane++) {
        primaryIdx[lane] = block.keys[lane];
    }

    HAO_STATS_ADD(primaryProbeLanes, block.laneCount);
    haoPreparePrimaryProbeStateFromPrimaryIdx(primaryIdx, block.laneCount,
                                              &probe);
    activeCount = haoProbeCompactAndLoadPrimary(primaryBitmap,
                                                hdr->primaryBitmapSize,
                                                primaryHashTable, &probe);
    HAO_STATS_ADD(primaryActiveLanes, activeCount);

    for (lane = 0; lane < activeCount; lane++) {
        encodedByLane[probe.activeLaneIndex[lane]] = probe.activeEncoded[lane];
    }

    for (lane = 0; lane < block.laneCount; lane++) {
        if (encodedByLane[lane]) {
            struct HAOPositionContext ctx;

            haoBuildContextFromBlockState(&block, lane, &ctx);
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
    const struct HAORuntimeBitSelector *selectors;
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

    haoRefreshStatsMode();
    HAO_STATS_ADD(scanCalls, 1);
    HAO_STATS_ADD(scanInputBytes, a->len - a->start_offset);

    selectors = (const struct HAORuntimeBitSelector *)((const u8 *)hdr +
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

        if (haoProcessBlockBatch(hdr, selectors, primaryBitmap,
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

/* Read blob magic through memcpy to avoid alignment issues. */
static int haoReadBlobMagic(const void *blob, u32 blobSize, u32 *magic) {
    if (!blob || !magic || blobSize < sizeof(*magic)) {
        return 0;
    }

    memcpy(magic, blob, sizeof(*magic));
    return 1;
}

static int haoCompatProcessEncodedRange(
    const HAOCompatRuntimeHeader *hdr,
    const HAOCompatRuntimeSecondaryHashEntry *secondaryHashTable,
    const HAOCompatRuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct HAOCompatPositionContext *ctx, u32 classKey,
    u32 encoded) {
    u32 offset = 0;
    u32 count = 0;
    u32 n;

    if (!encoded || !ctx) {
        return HWLM_SUCCESS;
    }

    haoCompatDecodePrimaryValue(encoded, &offset, &count);
    for (n = 0; n < count; n++) {
        const u32 off = offset + n;
        const HAOCompatRuntimeSecondaryHashEntry *entry;
        u32 laneMask;
        u32 r;

        if (!off || off >= hdr->secondaryCount) {
            break;
        }

        entry = &secondaryHashTable[off];
        laneMask = haoCompatEntryMatchMaskFromContextVector(entry, ctx);
        if (!laneMask) {
            continue;
        }

        for (r = 0; r < entry->ruleCount &&
                    r < PBE_RUNTIME_RULE_SLOTS_PER_ENTRY; r++) {
            const u16 ridx = entry->ruleIndex[r];
            const u32 kv = entry->keyValue[r];
            const u32 km = entry->keyMask[r];
            const HAOCompatRuntimeRuleMeta *rm;

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
            if (!haoCompatRuleExactMatch(rm, a, ctx->endPos, literalBlob,
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

static int haoCompatProcessMaskClassesForContext(
    const HAOCompatRuntimeHeader *hdr,
    const HAOCompatRuntimeMaskClass *classes,
    const HAOCompatRuntimeSecondaryHashEntry *secondaryHashTable,
    const HAOCompatRuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct HAOCompatPositionContext *ctx) {
    u32 classIdx;

    for (classIdx = 0; classIdx < hdr->classCount; classIdx++) {
        const HAOCompatRuntimeMaskClass *klass = &classes[classIdx];
        const u8 *primaryBitmap =
            (const u8 *)hdr + klass->primaryBitmapOffset;
        const u32 *primaryHashTable =
            (const u32 *)((const u8 *)hdr + klass->primaryOffset);
        const u32 classKey = haoCompatProjectKeyToClass(ctx->key, klass->classMask);

        if (classKey >= klass->primaryCount) {
            continue;
        }
        if (!haoCompatPrimaryBitmapHasValue(primaryBitmap, klass->primaryBitmapSize,
                                      classKey)) {
            continue;
        }
        if (haoCompatProcessEncodedRange(hdr, secondaryHashTable, ruleMeta,
                                   literalBlob, literalBlobSize, a, control,
                                   ctx, classKey,
                                   primaryHashTable[classKey]) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static int haoCompatProcessMaskClassesForLaneWithBatchState(
    const HAOCompatRuntimeHeader *hdr,
    const HAOCompatRuntimeMaskClass *classes,
    const HAOCompatRuntimeSecondaryHashEntry *secondaryHashTable,
    const HAOCompatRuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct HAOCompatPositionContext *ctx,
    const struct HAOCompatMaskClassBatchState *batchState, u32 lane) {
    u32 classIdx;

    for (classIdx = 0; classIdx < hdr->classCount; classIdx++) {
        const HAOCompatRuntimeMaskClass *klass = &classes[classIdx];
        u32 classKey = 0;
        u32 encoded = 0;

        if (batchState && lane < batchState->laneCount && haoCompatMaskClassIsHot(klass)) {
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

            classKey = haoCompatProjectKeyToClass(ctx->key, klass->classMask);

            if (classKey >= klass->primaryCount) {
                continue;
            }
            if (!haoCompatPrimaryBitmapHasValue(primaryBitmap, klass->primaryBitmapSize,
                                          classKey)) {
                continue;
            }
            encoded = primaryHashTable[classKey];
        }

        if (haoCompatProcessEncodedRange(hdr, secondaryHashTable, ruleMeta,
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
int haoCompatRunNaive(const HAOCompatRuntimeHeader *hdr,
                       const struct FDR_Runtime_Args *a,
                       hwlm_group_t *control) {
    if (!hdr || !a || !a->buf || !a->len || a->start_offset >= a->len) {
        return HWLM_SUCCESS;
    }

    const HAOCompatRuntimeBitSelector *selectors =
        (const HAOCompatRuntimeBitSelector *)((const u8 *)hdr +
                                               hdr->selectorsOffset);
    const HAOCompatRuntimeMaskClass *classes =
        (const HAOCompatRuntimeMaskClass *)((const u8 *)hdr +
                                             hdr->classTableOffset);
    const HAOCompatRuntimeSecondaryHashEntry *secondaryHashTable =
        (const HAOCompatRuntimeSecondaryHashEntry *)((const u8 *)hdr +
                                                      hdr->secondaryOffset);
    const HAOCompatRuntimeRuleMeta *ruleMeta =
        (const HAOCompatRuntimeRuleMeta *)((const u8 *)hdr +
                                            hdr->ruleMetaOffset);
    const u8 *literalBlob = (const u8 *)hdr + hdr->literalBlobOffset;
    const u32 literalBlobSize = hdr->literalBlobSize;

    size_t i;
    for (i = a->start_offset; i < a->len; i++) {
        struct HAOCompatPositionContext ctx;

        haoCompatBuildPositionContext(hdr, selectors, a, i, &ctx, 1);
        if (haoCompatProcessMaskClassesForContext(hdr, classes, secondaryHashTable,
                                            ruleMeta, literalBlob,
                                            literalBlobSize, a, control,
                                            &ctx) == HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }
    return HWLM_SUCCESS;
}

static int haoCompatRunBatch(const HAOCompatRuntimeHeader *hdr,
                        const struct FDR_Runtime_Args *a,
                        hwlm_group_t *control) {
    const HAOCompatRuntimeBitSelector *selectors =
        (const HAOCompatRuntimeBitSelector *)((const u8 *)hdr +
                                               hdr->selectorsOffset);
    const HAOCompatRuntimeMaskClass *classes =
        (const HAOCompatRuntimeMaskClass *)((const u8 *)hdr +
                                             hdr->classTableOffset);
    const HAOCompatRuntimeSecondaryHashEntry *secondaryHashTable =
        (const HAOCompatRuntimeSecondaryHashEntry *)((const u8 *)hdr +
                                                      hdr->secondaryOffset);
    const HAOCompatRuntimeRuleMeta *ruleMeta =
        (const HAOCompatRuntimeRuleMeta *)((const u8 *)hdr +
                                            hdr->ruleMetaOffset);
    const u8 *literalBlob = (const u8 *)hdr + hdr->literalBlobOffset;
    const u32 literalBlobSize = hdr->literalBlobSize;

    const u32 batchWidth = haoCompatSuggestedBatchWidth(hdr);
    size_t i;
    for (i = a->start_offset; i < a->len; i += batchWidth) {
        struct HAOCompatPositionContext ctxs[HAO_COMPAT_BATCH_MAX_WIDTH];
        u32 laneCount = 0;

        laneCount = haoCompatBuildBatchContexts(hdr, selectors, a, i, batchWidth,
                                          ctxs);

        if (hdr->classCount <= 2) {
            u32 lane;
            for (lane = 0; lane < laneCount; lane++) {
                if (haoCompatProcessMaskClassesForContext(hdr, classes,
                                                    secondaryHashTable,
                                                    ruleMeta, literalBlob,
                                                    literalBlobSize, a, control,
                                                    &ctxs[lane]) ==
                    HWLM_TERMINATED) {
                    return HWLM_TERMINATED;
                }
            }
        } else {
            struct HAOCompatMaskClassBatchState batchState;
            u32 lane;
            haoCompatBuildMaskClassBatchState(hdr, classes, ctxs, laneCount,
                                        &batchState);

            for (lane = 0; lane < laneCount; lane++) {
                if (haoCompatProcessMaskClassesForLaneWithBatchState(
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
hwlm_error_t haoCompatExecWithPath(const struct FDR *fdr,
                             const struct FDR_Runtime_Args *a,
                             hwlm_group_t control, int useBatch4) {
    if (!fdr || !fdrMatcherBlobOffset(fdr) || !fdrMatcherBlobSize(fdr)) {
        return HWLM_SUCCESS;
    }

    const u8 *base = (const u8 *)fdr;
    const HAOCompatRuntimeHeader *hdr =
        (const HAOCompatRuntimeHeader *)(base + fdrMatcherBlobOffset(fdr));

    if (!haoCompatValidateLayout(fdr, hdr)) {
        return HWLM_SUCCESS;
    }

    if (!a || (hdr->flags & PBE_RUNTIME_FLAG_PARTIAL_COVERAGE)) {
        return HWLM_SUCCESS;
    }

    return useBatch4 ? haoCompatRunBatch(hdr, a, &control)
                     : haoCompatRunNaive(hdr, a, &control);
}

hwlm_error_t HaoCompatEngineExecNaiveForTest(const struct FDR *fdr,
                                             const struct FDR_Runtime_Args *a,
                                             hwlm_group_t control) {
    if (!fdr || !fdrMatcherBlobOffset(fdr) || !fdrMatcherBlobSize(fdr)) {
        return HWLM_SUCCESS;
    }

    {
        const u8 *base = (const u8 *)fdr;
        const void *blob = base + fdrMatcherBlobOffset(fdr);
        return haoExecBlobWithPath(blob, fdrMatcherBlobSize(fdr), a,
                                   control, 0);
    }
}

u32 HaoCompatRuntimeEntryMatchMaskForTest(
    const HAOCompatRuntimeSecondaryHashEntry *entry,
    const struct FDR_Runtime_Args *a, size_t endPos, int useVector) {
    struct HAOCompatPositionContext ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.endPos = endPos;
    ctx.window64 = haoCompatLoadWindow64Normalized(a, endPos,
                                             PBE_RUNTIME_BYTES_PER_RULE_SLOT);
    ctx.validMask8 = haoCompatComputeValidMask8(a, endPos);

    return useVector ? haoCompatEntryMatchMaskFromContextVector(entry, &ctx)
                     : haoCompatEntryMatchMaskFromContextScalar(entry, &ctx);
}

u32 HaoRuntimeEntryMatchMaskForTest(
    const struct HAORuntimeSecondaryHashEntry *entry,
    const struct FDR_Runtime_Args *a, size_t endPos, int useVector) {
    struct HAOPositionContext ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.endPos = endPos;
    ctx.window64 =
        haoLoadWindow64Normalized(a, endPos, HAO_RUNTIME_BYTES_PER_RULE_SLOT);
    ctx.validMask8 = haoComputeValidMask8(a, endPos);

    return useVector ? haoEntryMatchMaskFromContextVectorForTest(entry, &ctx)
                     : haoEntryMatchMaskFromContextScalarForTest(entry, &ctx);
}

u32 HaoCompatRuntimeBitmapProbeMaskForTest(const u8 *bitmap, u32 bitmapSize,
                                           const u32 *primaryIdx,
                                           u32 laneCount, int usePacked) {
    struct HAOCompatBitmapProbeState probe;

    memset(&probe, 0, sizeof(probe));
    haoCompatPrepareBitmapProbeStateFromPrimaryIdx(primaryIdx, laneCount, &probe);

    return usePacked ? haoCompatProbeBitmapPacked(bitmap, bitmapSize, &probe)
                     : haoCompatProbeBitmapScalar(bitmap, bitmapSize, &probe);
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
    haoRefreshStatsMode();
}

void HaoRuntimeGetStatsForTest(struct HAORuntimeStats *summary) {
    if (!summary) {
        return;
    }
    *summary = g_haoStats;
    g_haoStatsForceEnabled = 0;
    haoRefreshStatsMode();
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

static
hwlm_error_t haoFamilyExec(const struct FDR *fdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t control) {
    if (!fdr || !fdrMatcherBlobOffset(fdr) || !fdrMatcherBlobSize(fdr)) {
        return HWLM_SUCCESS;
    }

    {
        const u8 *base = (const u8 *)fdr;
        const void *blob = base + fdrMatcherBlobOffset(fdr);
        return haoExecBlobWithPath(blob, fdrMatcherBlobSize(fdr), a,
                                   control, 1);
    }
}

hwlm_error_t HaoEngineExec(const struct FDR *fdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t control) {
    return haoFamilyExec(fdr, a, control);
}




