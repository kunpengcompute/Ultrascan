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
#include <stdint.h>
#include <arm_sve.h>

#ifndef HAO_ENABLE_RUNTIME_STATS
#define HAO_ENABLE_RUNTIME_STATS 0
#endif

#ifndef HAO_L2_COUNT1_FAST
#define HAO_L2_COUNT1_FAST 1
#endif

static struct HAORuntimeStats g_haoStats;
static int g_haoStatsForceEnabled;
#if HAO_ENABLE_RUNTIME_STATS
static int g_haoStatsEnvEnabled = -1;
static int g_haoStatsAtexitRegistered;
static int g_haoStatsActive;
#endif

/* Labels contain mixed ASCII and CJK text. strlen() counts bytes rather than
 * display columns, so we compensate to keep the colon column aligned. */
#define HAO_STAT_FMT(label, fmt, val)                                    \
    do {                                                                  \
        int _blen = (int)strlen(label);                                   \
        /* Count each 3-byte UTF-8 CJK code point as display width 2. */          \
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

#if HAO_ENABLE_RUNTIME_STATS
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
#else
static really_inline
void haoRefreshStatsMode(void) {
}
#endif

#if HAO_ENABLE_RUNTIME_STATS
#define HAO_STATS_IF_ENABLED(stmt) \
    do {                           \
        if (g_haoStatsActive) {    \
            stmt;                  \
        }                          \
    } while (0)

#define HAO_STATS_ADD(field, delta) \
    HAO_STATS_IF_ENABLED(g_haoStats.field += (u64a)(delta))
#else
#define HAO_STATS_IF_ENABLED(stmt) \
    do {                           \
    } while (0)

#define HAO_STATS_ADD(field, delta) \
    do {                            \
    } while (0)
#endif

struct HAOPositionContext {
    size_t endPos;
    u64a laneWord;
    u32 validMask32;
};

struct HAOPrimaryProbeState {
    u32 laneCount;
    const u32 *primaryIdx;
    u32 byteIndex[HAO_BATCH_MAX_WIDTH];
    u8 bitMask[HAO_BATCH_MAX_WIDTH];
    u32 groupedBaseByte;
    u8 groupedSpan;
    u8 groupedReady;
    u16 reserved0;
    u32 groupedLaneMaskByByteBit[HAO_BITMAP_GROUPED_BYTES][8];
};

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
    u32 minByte = 0;
    u32 maxByte = 0;

    if (!primaryIdx || !state || laneCount > HAO_BATCH_MAX_WIDTH) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->laneCount = laneCount;
    state->primaryIdx = primaryIdx;
    for (lane = 0; lane < laneCount; lane++) {
        const u32 idx = primaryIdx[lane];
        const u32 byteIdx = idx >> 3;

        state->byteIndex[lane] = byteIdx;
        state->bitMask[lane] = (u8)(1U << (idx & 7U));
        if (!lane) {
            minByte = byteIdx;
            maxByte = byteIdx;
        } else {
            if (byteIdx < minByte) {
                minByte = byteIdx;
            }
            if (byteIdx > maxByte) {
                maxByte = byteIdx;
            }
        }
    }

    if (laneCount) {
        const u32 span = maxByte - minByte + 1U;

        if (span <= HAO_BITMAP_GROUPED_BYTES) {
            state->groupedBaseByte = minByte;
            state->groupedSpan = (u8)span;
            state->groupedReady = 1;

            for (lane = 0; lane < laneCount; lane++) {
                const u32 relByte = state->byteIndex[lane] - minByte;
                const u32 bit = primaryIdx[lane] & 7U;

                state->groupedLaneMaskByByteBit[relByte][bit] |= 1U << lane;
            }
        }
    }
}

static really_inline
int haoProbePrimaryBitmapGrouped(const u8 *bitmap, u32 bitmapSize,
                                 const struct HAOPrimaryProbeState *state,
                                 u32 *activeMaskOut) {
    u32 relByte;
    u32 activeMask = 0;

    if (!bitmap || !state || !state->laneCount || !activeMaskOut) {
        return 0;
    }

    if (!state->groupedReady || !state->groupedSpan) {
        return 0;
    }
    if (state->groupedBaseByte >= bitmapSize ||
        state->groupedBaseByte + state->groupedSpan > bitmapSize) {
        return 0;
    }

    for (relByte = 0; relByte < state->groupedSpan; relByte++) {
        u32 bits = bitmap[state->groupedBaseByte + relByte];

        while (bits) {
            const u32 bit = ctz32(bits);
            activeMask |= state->groupedLaneMaskByByteBit[relByte][bit];
            bits &= bits - 1U;
        }
    }

    *activeMaskOut = activeMask;
    return 1;
}

#if HAO_ENABLE_RUNTIME_STATS
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

static void haoStatsObserveRangeShape(u32 entryCount, u32 ruleCount) {
    HAO_STATS_IF_ENABLED({
        g_haoStats.l2RangeTotalEntries += entryCount;
        g_haoStats.l2RangeTotalRules += ruleCount;
        if (!g_haoStats.l2RangeMinEntries || entryCount < g_haoStats.l2RangeMinEntries) {
            g_haoStats.l2RangeMinEntries = entryCount;
        }
        if (entryCount > g_haoStats.l2RangeMaxEntries) {
            g_haoStats.l2RangeMaxEntries = entryCount;
        }
        if (!g_haoStats.l2RangeMinRules || ruleCount < g_haoStats.l2RangeMinRules) {
            g_haoStats.l2RangeMinRules = ruleCount;
        }
        if (ruleCount > g_haoStats.l2RangeMaxRules) {
            g_haoStats.l2RangeMaxRules = ruleCount;
        }

        if (entryCount == 1) {
            g_haoStats.l2RangeEntryBucketsEq1++;
        } else if (entryCount <= 4) {
            g_haoStats.l2RangeEntryBuckets2To4++;
        } else {
            g_haoStats.l2RangeEntryBucketsGt4++;
        }

        if (ruleCount == 1) {
            g_haoStats.l2RangeRuleBucketsEq1++;
        } else if (ruleCount <= 4) {
            g_haoStats.l2RangeRuleBuckets2To4++;
        } else {
            g_haoStats.l2RangeRuleBucketsGt4++;
        }

        if (ruleCount > 1) {
            g_haoStats.l2RangeCollisionBuckets++;
        }
    });
}
#else
static really_inline
void haoStatsObserveRangeShape(u32 entryCount, u32 ruleCount) {
    (void)entryCount;
    (void)ruleCount;
}
#endif

#if HAO_ENABLE_RUNTIME_STATS
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
    const double avgEntriesPerRange = g_haoStats.encodedRangeCalls
        ? (double)g_haoStats.l2RangeTotalEntries /
              (double)g_haoStats.encodedRangeCalls
        : 0.0;
    const double avgRulesPerRange = g_haoStats.encodedRangeCalls
        ? (double)g_haoStats.l2RangeTotalRules /
              (double)g_haoStats.encodedRangeCalls
        : 0.0;

    if (!g_haoStatsActive) {
        return;
    }
    fprintf(stderr, "[HAO][Runtime/运行时]\n");
    HAO_STAT_FMT("scans(扫描次数)",                           "%llu", (unsigned long long)g_haoStats.scanCalls);
    HAO_STAT_FMT("inputBytes(输入字节数)",                    "%llu", (unsigned long long)g_haoStats.scanInputBytes);
    HAO_STAT_FMT("callbackReports(回调上报次数)",             "%llu", (unsigned long long)g_haoStats.callbackReports);

    fprintf(stderr, "[HAO][L1/一级过滤]\n");
    HAO_STAT_FMT("blockCalls(分块处理次数)",                  "%llu", (unsigned long long)g_haoStats.blockCalls);
    HAO_STAT_FMT("blockLanes(分块总lane数)",                  "%llu", (unsigned long long)g_haoStats.blockLanes);
    HAO_STAT_FMT("primaryProbeLanes(L1探测lane数)",           "%llu", (unsigned long long)g_haoStats.primaryProbeLanes);
    HAO_STAT_FMT("primaryActiveLanes(L1命中lane数)",          "%llu", (unsigned long long)g_haoStats.primaryActiveLanes);
    HAO_STAT_FMT("activePct(L1命中率)",                       "%.5f",
        haoStatsPct(g_haoStats.primaryActiveLanes, g_haoStats.primaryProbeLanes));

    fprintf(stderr, "[HAO][L2/二级哈希]\n");
    HAO_STAT_FMT("rangeCalls(L2范围处理次数)",                "%llu", (unsigned long long)g_haoStats.encodedRangeCalls);
    HAO_STAT_FMT("rangeReportCalls(L2产生报告的lane数)",      "%llu", (unsigned long long)g_haoStats.encodedRangeReportCalls);
    HAO_STAT_FMT("entriesVisited(L2访问entry数)",             "%llu", (unsigned long long)g_haoStats.encodedEntriesVisited);
    HAO_STAT_FMT("verifierCalls(verifier调用次数)",           "%llu", (unsigned long long)g_haoStats.verifierCalls);
    HAO_STAT_FMT("verifierEntryHits(verifier命中的entry数)",  "%llu", (unsigned long long)g_haoStats.verifierEntryHits);
    HAO_STAT_FMT("verifierSlotHits(verifier命中的slot数)",    "%llu", (unsigned long long)g_haoStats.verifierSlotHits);
    HAO_STAT_FMT("groupRejects(group过滤拒绝数)",             "%llu", (unsigned long long)g_haoStats.encodedGroupRejects);
    HAO_STAT_FMT("directReports(直接上报次数)",               "%llu", (unsigned long long)g_haoStats.directReports);
    HAO_STAT_FMT("confirmCalls(精确确认次数)",                "%llu", (unsigned long long)g_haoStats.encodedConfirmCalls);
    HAO_STAT_FMT("confirmMatches(精确确认命中数)",            "%llu", (unsigned long long)g_haoStats.encodedConfirmMatches);
    HAO_STAT_FMT("confirmRejects(精确确认拒绝数)",            "%llu", (unsigned long long)g_haoStats.encodedConfirmRejects);

    fprintf(stderr, "[HAO][L2-Buckets/二级桶分布]\n");
    HAO_STAT_FMT("avgEntriesPerRange(每次L2平均entry数)",     "%.5f", avgEntriesPerRange);
    HAO_STAT_FMT("minEntriesPerRange(每次L2最少entry数)",     "%llu", (unsigned long long)g_haoStats.l2RangeMinEntries);
    HAO_STAT_FMT("maxEntriesPerRange(每次L2最多entry数)",     "%llu", (unsigned long long)g_haoStats.l2RangeMaxEntries);
    HAO_STAT_FMT("rangeEntryBucketsEq1(L2命中1-entry桶次数)", "%llu", (unsigned long long)g_haoStats.l2RangeEntryBucketsEq1);
    HAO_STAT_FMT("rangeEntryBucketsEq1Pct(L2命中1-entry桶占比)", "%.5f",
        haoStatsPct(g_haoStats.l2RangeEntryBucketsEq1, g_haoStats.encodedRangeCalls));
    HAO_STAT_FMT("rangeEntryBuckets2To4(L2命中2~4-entry桶次数)", "%llu", (unsigned long long)g_haoStats.l2RangeEntryBuckets2To4);
    HAO_STAT_FMT("rangeEntryBuckets2To4Pct(L2命中2~4-entry桶占比)", "%.5f",
        haoStatsPct(g_haoStats.l2RangeEntryBuckets2To4, g_haoStats.encodedRangeCalls));
    HAO_STAT_FMT("rangeEntryBucketsGt4(L2命中>4-entry桶次数)", "%llu", (unsigned long long)g_haoStats.l2RangeEntryBucketsGt4);
    HAO_STAT_FMT("rangeEntryBucketsGt4Pct(L2命中>4-entry桶占比)", "%.5f",
        haoStatsPct(g_haoStats.l2RangeEntryBucketsGt4, g_haoStats.encodedRangeCalls));
    HAO_STAT_FMT("avgRulesPerRange(每次L2平均规则数)",          "%.5f", avgRulesPerRange);
    HAO_STAT_FMT("minRulesPerRange(每次L2最少规则数)",        "%llu", (unsigned long long)g_haoStats.l2RangeMinRules);
    HAO_STAT_FMT("maxRulesPerRange(每次L2最多规则数)",        "%llu", (unsigned long long)g_haoStats.l2RangeMaxRules);
    HAO_STAT_FMT("rangeRuleBucketsEq1(L2命中1规则桶次数)",    "%llu", (unsigned long long)g_haoStats.l2RangeRuleBucketsEq1);
    HAO_STAT_FMT("rangeRuleBucketsEq1Pct(L2命中1规则桶占比)", "%.5f",
        haoStatsPct(g_haoStats.l2RangeRuleBucketsEq1, g_haoStats.encodedRangeCalls));
    HAO_STAT_FMT("rangeRuleBuckets2To4(L2命中2~4规则桶次数)", "%llu", (unsigned long long)g_haoStats.l2RangeRuleBuckets2To4);
    HAO_STAT_FMT("rangeRuleBuckets2To4Pct(L2命中2~4规则桶占比)", "%.5f",
        haoStatsPct(g_haoStats.l2RangeRuleBuckets2To4, g_haoStats.encodedRangeCalls));
    HAO_STAT_FMT("rangeRuleBucketsGt4(L2命中>4规则桶次数)",   "%llu", (unsigned long long)g_haoStats.l2RangeRuleBucketsGt4);
    HAO_STAT_FMT("rangeRuleBucketsGt4Pct(L2命中>4规则桶占比)", "%.5f",
        haoStatsPct(g_haoStats.l2RangeRuleBucketsGt4, g_haoStats.encodedRangeCalls));
    HAO_STAT_FMT("rangeCollisionPct(L2命中冲突桶占比)",       "%.5f",
        haoStatsPct(g_haoStats.l2RangeCollisionBuckets, g_haoStats.encodedRangeCalls));

    // fprintf(stderr, "[HAO][Residual/兜底路径]\n");
    // HAO_STAT_FMT("posCalls(兜底位置次数)",                    "%llu", (unsigned long long)g_haoStats.residualPosCalls);
    // HAO_STAT_FMT("ruleChecks(兜底规则检查数)",                "%llu", (unsigned long long)g_haoStats.residualRuleChecks);
    // HAO_STAT_FMT("groupRejects(兜底group拒绝数)",             "%llu", (unsigned long long)g_haoStats.residualGroupRejects);
    // HAO_STAT_FMT("confirmCalls(兜底确认次数)",                "%llu", (unsigned long long)g_haoStats.residualConfirmCalls);
    // HAO_STAT_FMT("confirmMatches(兜底确认命中数)",            "%llu", (unsigned long long)g_haoStats.residualConfirmMatches);
    // HAO_STAT_FMT("confirmRejects(兜底确认拒绝数)",            "%llu", (unsigned long long)g_haoStats.residualConfirmRejects);

    fprintf(stderr, "[HAO][Rates/关键比率]\n");
    HAO_STAT_FMT("l2EntryFalsePositivePct(L2表项假阳性率)",   "%.5f",
        haoStatsPct(l2EntryRejects, g_haoStats.encodedEntriesVisited));
    HAO_STAT_FMT("l2LaneNoReportPct(L2无报告lane占比)",       "%.5f",
        haoStatsPct(l2LaneNoReport, g_haoStats.encodedRangeCalls));
    HAO_STAT_FMT("l2ConfirmFalsePositivePct(L2确认假阳性率)", "%.5f",
        haoStatsPct(g_haoStats.encodedConfirmRejects, g_haoStats.encodedConfirmCalls));
    HAO_STAT_FMT("l2SlotFalsePositivePct(L2槽位假阳性率)",    "%.5f",
        haoStatsPct(l2SlotFalsePos, l2SlotEligible));
    HAO_STAT_FMT("l2EntriesPerMiB(每MiB访问L2表项数)",        "%.5f",
        haoStatsPerMiB(g_haoStats.encodedEntriesVisited, g_haoStats.scanInputBytes));
    HAO_STAT_FMT("l2ConfirmCallsPerMiB(每MiB精确确认次数)",   "%.5f",
        haoStatsPerMiB(g_haoStats.encodedConfirmCalls, g_haoStats.scanInputBytes));
    HAO_STAT_FMT("reportsPerMiB(每MiB报告次数)",              "%.5f",
        haoStatsPerMiB(g_haoStats.callbackReports, g_haoStats.scanInputBytes));
}
#endif

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
    if (!hdr->selectorCount || !hdr->primaryCount || !hdr->l2EntryCount) {
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
    if ((u64a)hdr->primaryBitmapCoarseOffset +
            (u64a)hdr->primaryCoarseBitmapSize >
        (u64a)blobSize) {
        return 0;
    }
    if ((u64a)hdr->primaryOffset + (u64a)hdr->primaryCount * sizeof(u32) >
        (u64a)blobSize) {
        return 0;
    }
    if ((u64a)hdr->primaryBitmapRawOffset + (u64a)hdr->primaryBitmapSize >
        (u64a)blobSize) {
        return 0;
    }
    if ((u64a)hdr->primaryBitmapRawCoarseOffset +
            (u64a)hdr->primaryCoarseBitmapSize >
        (u64a)blobSize) {
        return 0;
    }
    if ((u64a)hdr->primaryRawOffset + (u64a)hdr->primaryCount * sizeof(u32) >
        (u64a)blobSize) {
        return 0;
    }
    if (!hdr->l2CheckOffset || !hdr->l2MetaOffset ||
        (hdr->l2CheckOffset & 63U)) {
        return 0;
    }
    if ((u64a)hdr->l2CheckOffset + (u64a)hdr->l2EntryCount *
            sizeof(struct HAORuntimeL2Check) >
        (u64a)blobSize) {
        return 0;
    }
    if ((u64a)hdr->l2MetaOffset + (u64a)hdr->l2EntryCount *
            sizeof(struct HAORuntimeL2Meta) >
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
    const struct HAORuntimeL2Meta *l2Meta;
    u32 i;

    if (!hdr || !summary) {
        return;
    }

    memset(summary, 0, sizeof(*summary));
    summary->selectorCount = hdr->selectorCount;
    summary->primaryCount = hdr->primaryCount;
    summary->primaryBitmapSize = hdr->primaryBitmapSize;
    summary->primaryCoarseBitmapSize = hdr->primaryCoarseBitmapSize;
    summary->l2EntryCount = hdr->l2EntryCount;
    summary->ruleMetaCount = hdr->ruleMetaCount;
    summary->residualRuleCount = hdr->residualRuleCount;

    primary = (const u32 *)((const u8 *)hdr + hdr->primaryOffset);
    l2Meta = (const struct HAORuntimeL2Meta *)((const u8 *)hdr +
                                               hdr->l2MetaOffset);

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

    for (i = 0; i < hdr->l2EntryCount; i++) {
        summary->totalRulesInL2 +=
            popcount32(l2Meta[i].slotMask &
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
        const int nocase = (rm->flags & HAO_RULE_FLAG_NOCASE) ? 1 : 0;
        u16 i;

        for (i = 0; i < rm->len; i++) {
            u8 got;
            if (!haoGetByteAt(a, startPos + i, &got)) {
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

        if ((rm->flags & HAO_RULE_FLAG_HAS_MASK) && rm->maskLen) {
            const u8 mlen = rm->maskLen;
            const s64a maskStart = startPos + (s64a)rm->len - (s64a)mlen;
            for (i = 0; i < mlen && i < sizeof(rm->msk); i++) {
                u8 got;
                if (!haoGetByteAt(a, maskStart + i, &got)) {
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
int haoRuleCanReportFromVerifier(const struct HAORuntimeRuleMeta *rm) {
    if (!rm) {
        return 0;
    }
    return (rm->planFlags & HAO_RUNTIME_PLAN_FLAG_DIRECT_REPORT_SAFE) != 0;
}

static really_inline
u32 haoL2ValidSlotMaskFromCareBits(u32 careBits, u32 validMask32,
                                   u32 slotMask) {
    u32 slot;
    u32 out = slotMask;
    const u32 invalidCare = careBits & ~validMask32;

    if (!invalidCare) {
        return out;
    }

    for (slot = 0; slot < HAO_RUNTIME_RULE_SLOTS_PER_ENTRY; slot++) {
        const u32 slotBits =
            0xffU << (slot * HAO_RUNTIME_BYTES_PER_RULE_SLOT);

        if (invalidCare & slotBits) {
            out &= ~(1U << slot);
        }
    }
    return out;
}

static really_inline
u32 haoL2MatchSve(const struct HAORuntimeL2Check *check,
                  const struct HAORuntimeL2Meta *meta,
                  const struct HAOPositionContext *ctx) {
    const svbool_t pg = svwhilelt_b64((u32)0, HAO_RUNTIME_RULE_SLOTS_PER_ENTRY);
    const svuint64_t data = svdup_n_u64(ctx->laneWord);
    const svuint64_t rule = svld1_u64(pg, (const uint64_t *)check->rule);
    const svuint64_t mask = svld1_u64(pg, (const uint64_t *)check->mask);
    const svuint64_t bits =
        svlsl_u64_x(pg, svdup_n_u64(1U), svindex_u64(0, 1));
    const svbool_t hit =
        svcmpeq_u64(pg, svand_u64_x(pg, data, mask), rule);
    u32 laneMask = (u32)svorv_u64(
        pg, svsel_u64(hit, bits, svdup_n_u64(0U)));

    laneMask &= meta->slotMask &
                ((1U << HAO_RUNTIME_RULE_SLOTS_PER_ENTRY) - 1U);
    if (unlikely(ctx->validMask32 != 0xffffffffU)) {
        laneMask = haoL2ValidSlotMaskFromCareBits(
            meta->careBits, ctx->validMask32, laneMask);
    }
    return laneMask;
}

static really_inline
int haoProcessPreparedEntryMatchesSve(
    const struct HAORuntimeHeader *hdr, const struct HAORuntimeL2Check *check,
    const struct HAORuntimeL2Meta *meta,
    const struct HAORuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct HAOPositionContext *ctx
#if HAO_ENABLE_RUNTIME_STATS
    ,
    int *anyReport
#endif
    ) {
    u32 matchMask;

    if (!check || !meta || !meta->slotMask || !ctx) {
        return HWLM_SUCCESS;
    }

    HAO_STATS_ADD(verifierCalls, 1);
    matchMask = haoL2MatchSve(check, meta, ctx);
    if (!matchMask) {
        return HWLM_SUCCESS;
    }

    HAO_STATS_ADD(verifierEntryHits, 1);
    HAO_STATS_ADD(verifierSlotHits, popcount32(matchMask));

    while (matchMask) {
        const u32 r = ctz32(matchMask);
        const u32 ridx = meta->ruleIndex[r];
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
#if HAO_ENABLE_RUNTIME_STATS
        *anyReport = 1;
#endif
        *control = a->cb(ctx->endPos, rm->id, a->scratch);
        if (*control == HWLM_TERMINATE_MATCHING) {
            return HWLM_TERMINATED;
        }
        matchMask &= matchMask - 1U;
    }

    return HWLM_SUCCESS;
}

static really_inline
u32 haoL2MetaSlotCount(const struct HAORuntimeL2Meta *meta) {
    if (!meta) {
        return 0;
    }
    if (meta->slotCount) {
        return MIN(meta->slotCount, (u8)HAO_RUNTIME_RULE_SLOTS_PER_ENTRY);
    }
    return popcount32(meta->slotMask &
                      ((1U << HAO_RUNTIME_RULE_SLOTS_PER_ENTRY) - 1U));
}

#if !HAO_ENABLE_RUNTIME_STATS
static really_inline
int haoProcessSingleL2Entry(
    const struct HAORuntimeHeader *hdr,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct HAOPositionContext *ctx, u32 offset) {
    if (unlikely(!offset || offset >= hdr->l2EntryCount)) {
        return HWLM_SUCCESS;
    }

    return haoProcessPreparedEntryMatchesSve(
        hdr, &l2CheckTable[offset], &l2MetaTable[offset], ruleMeta,
        literalBlob, literalBlobSize, a, control, ctx);
}
#endif

static really_inline
int haoProcessEncodedRangePrepared(
    const struct HAORuntimeHeader *hdr,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct HAOPositionContext *ctx, u32 encoded) {
    u32 offset = 0;
    u32 count = 0;
    u32 n;
#if HAO_ENABLE_RUNTIME_STATS
    u32 visitedCount = 0;
    u32 bucketRuleCount = 0;
    int anyReport = 0;
#endif

    if (!encoded || !ctx || !l2CheckTable || !l2MetaTable) {
        return HWLM_SUCCESS;
    }

    HAO_STATS_ADD(encodedRangeCalls, 1);
    haoDecodePrimaryValue(encoded, &offset, &count);

#if !HAO_ENABLE_RUNTIME_STATS
#if HAO_L2_COUNT1_FAST
    if (likely(count == 1U)) {
        return haoProcessSingleL2Entry(
            hdr, l2CheckTable, l2MetaTable, ruleMeta, literalBlob,
            literalBlobSize, a, control, ctx, offset);
    }
#endif

    for (n = 0; n < count; n++) {
        const u32 off = offset + n;
        int rv;

        if (!off || off >= hdr->l2EntryCount) {
            break;
        }

        rv = haoProcessPreparedEntryMatchesSve(
            hdr, &l2CheckTable[off], &l2MetaTable[off], ruleMeta,
            literalBlob, literalBlobSize, a, control, ctx);
        if (rv == HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }
#else
#if HAO_L2_COUNT1_FAST
    if (count == 1U) {
        int rv;

        if (!offset || offset >= hdr->l2EntryCount) {
            haoStatsObserveRangeShape(0, 0);
            return HWLM_SUCCESS;
        }

        HAO_STATS_IF_ENABLED({
            visitedCount = 1;
            bucketRuleCount = haoL2MetaSlotCount(&l2MetaTable[offset]);
        });
        HAO_STATS_ADD(encodedEntriesVisited, 1);

        rv = haoProcessPreparedEntryMatchesSve(
            hdr, &l2CheckTable[offset], &l2MetaTable[offset], ruleMeta,
            literalBlob, literalBlobSize, a, control, ctx, &anyReport);
        haoStatsObserveRangeShape(visitedCount, bucketRuleCount);
        HAO_STATS_ADD(encodedRangeReportCalls, anyReport ? 1 : 0);
        return rv;
    }
#endif

    for (n = 0; n < count; n++) {
        const u32 off = offset + n;
        int rv;

        if (!off || off >= hdr->l2EntryCount) {
            break;
        }

        HAO_STATS_IF_ENABLED({
            visitedCount++;
            bucketRuleCount += haoL2MetaSlotCount(&l2MetaTable[off]);
        });
        HAO_STATS_ADD(encodedEntriesVisited, 1);

        rv = haoProcessPreparedEntryMatchesSve(
            hdr, &l2CheckTable[off], &l2MetaTable[off], ruleMeta,
            literalBlob, literalBlobSize, a, control, ctx, &anyReport);
        if (rv == HWLM_TERMINATED) {
            haoStatsObserveRangeShape(visitedCount, bucketRuleCount);
            HAO_STATS_ADD(encodedRangeReportCalls, 1);
            return HWLM_TERMINATED;
        }
    }

    haoStatsObserveRangeShape(visitedCount, bucketRuleCount);
    HAO_STATS_ADD(encodedRangeReportCalls, anyReport ? 1 : 0);
#endif

    return HWLM_SUCCESS;
}

static int haoProcessResidualRulesAtPos(
    const struct HAORuntimeHeader *hdr,
    const u32 *residualRuleIndexes,
    const struct HAORuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, size_t endPos) {
    u32 i;

    if (!hdr || !residualRuleIndexes || !ruleMeta || !literalBlob || !a ||
        !control || !hdr->residualRuleCount) {
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

#if defined(HS_BUILD_HAVE_SVEBITPERM) && defined(__ARM_FEATURE_SVE2_BITPERM)
static really_inline
int haoRunRawTailScalar(
    const struct HAORuntimeHeader *hdr, const u8 *primaryBitmap,
    const u32 *primaryHashTable,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    const u32 *residualRuleIndexes, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, size_t blockStart, u32 blockLaneCount);
#endif

/* Naive HAO v2 execution path used both as a correctness baseline and as a
 * fallback when the batch kernel is not selected. */
static int haoRunNaiveBlob(const struct HAORuntimeHeader *hdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t *control) {
    const u8 *primaryBitmapRaw;
    const u32 *primaryHashTableRaw;
    const struct HAORuntimeL2Check *l2CheckTable;
    const struct HAORuntimeL2Meta *l2MetaTable;
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

    primaryBitmapRaw = (const u8 *)hdr + hdr->primaryBitmapRawOffset;
    primaryHashTableRaw =
        (const u32 *)((const u8 *)hdr + hdr->primaryRawOffset);
    l2CheckTable = (const struct HAORuntimeL2Check *)((const u8 *)hdr +
                                                      hdr->l2CheckOffset);
    l2MetaTable = (const struct HAORuntimeL2Meta *)((const u8 *)hdr +
                                                    hdr->l2MetaOffset);
    ruleMeta = (const struct HAORuntimeRuleMeta *)((const u8 *)hdr +
                                                   hdr->ruleMetaOffset);
    literalBlob = (const u8 *)hdr + hdr->literalBlobOffset;
    residualRuleIndexes = (const u32 *)((const u8 *)hdr +
                                        hdr->residualRuleIndexOffset);

#if defined(HS_BUILD_HAVE_SVEBITPERM) && defined(__ARM_FEATURE_SVE2_BITPERM)
    for (i = a->start_offset; i < a->len; i++) {
        HAO_STATS_ADD(primaryProbeLanes, 1);
        if (haoRunRawTailScalar(hdr, primaryBitmapRaw, primaryHashTableRaw,
                                l2CheckTable, l2MetaTable, ruleMeta,
                                literalBlob, residualRuleIndexes, a, control,
                                i, 1U) == HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }
#else
    (void)primaryBitmapRaw;
    (void)primaryHashTableRaw;
    (void)l2CheckTable;
    (void)l2MetaTable;
    (void)ruleMeta;
    (void)literalBlob;
    (void)residualRuleIndexes;
    (void)i;
#endif

    return HWLM_SUCCESS;
}

static really_inline
void haoStoreActivePrimaryVector(const u32 *primaryHashTable, svbool_t phit,
                                 svuint32_t vidx, svuint32_t vlaneBase,
                                 u32 hitCount, u32 *activeLaneIndex,
                                 u32 *activeEncoded) {
    const svuint32_t vencoded =
        svld1_gather_u32index_u32(phit, primaryHashTable, vidx);
    const svuint32_t vactiveLane = svcompact_u32(phit, vlaneBase);
    const svuint32_t vactiveEnc = svcompact_u32(phit, vencoded);
    const svbool_t pgw = svwhilelt_b32((u32)0, hitCount);

    svst1_u32(pgw, activeLaneIndex, vactiveLane);
    svst1_u32(pgw, activeEncoded, vactiveEnc);
}

#if defined(HS_BUILD_HAVE_SVEBITPERM) && defined(__ARM_FEATURE_SVE2_BITPERM)
static really_inline
int haoCanRunRaw32(const struct HAORuntimeHeader *hdr,
                   const struct FDR_Runtime_Args *a, size_t blockStart,
                   u32 blockLaneCount) {
    if (!hdr || !a || !a->buf) {
        return 0;
    }
    if (hdr->extractMode != HAO_RUNTIME_EXTRACT_MODE_BEXT ||
        hdr->windowBytes != HAO_RUNTIME_BYTES_PER_RULE_SLOT ||
        hdr->residualRuleCount || blockLaneCount != HAO_BATCH_MAX_WIDTH ||
        svcntw() != 8U || !hdr->primaryBitmapRawOffset ||
        !hdr->primaryRawOffset) {
        return 0;
    }
    return blockStart < a->len &&
           blockStart + HAO_BATCH_MAX_WIDTH <= a->len;
}

static really_inline
int haoCanRunRaw32V2(const struct HAORuntimeHeader *hdr,
                     const struct FDR_Runtime_Args *a, size_t blockStart,
                     u32 blockLaneCount) {
    return haoCanRunRaw32(hdr, a, blockStart, blockLaneCount);
}

static really_inline
int haoCanRunRawTailVec(const struct HAORuntimeHeader *hdr,
                        const struct FDR_Runtime_Args *a, size_t blockStart,
                        u32 blockLaneCount) {
    if (!hdr || !a || !a->buf || !blockLaneCount ||
        blockLaneCount >= HAO_BATCH_MAX_WIDTH) {
        return 0;
    }
    if (hdr->extractMode != HAO_RUNTIME_EXTRACT_MODE_BEXT ||
        hdr->windowBytes != HAO_RUNTIME_BYTES_PER_RULE_SLOT ||
        hdr->residualRuleCount || svcntw() != 8U ||
        !hdr->primaryBitmapRawOffset || !hdr->primaryRawOffset) {
        return 0;
    }
    return blockStart < a->len && blockStart + blockLaneCount <= a->len;
}

static really_inline
void haoLoadRawPrev32(const struct FDR_Runtime_Args *a, size_t blockStart,
                      svuint8_t *vlo) {
    const svbool_t pgb = svptrue_b8();
    u8 prevBytes[HAO_RUNTIME_BLOCK_BYTES] = {0};
    u32 i;

    assert(a);
    assert(vlo);

    if (blockStart >= HAO_RUNTIME_BLOCK_BYTES) {
        *vlo = svld1_u8(
            pgb, (const uint8_t *)(a->buf + blockStart -
                                    HAO_RUNTIME_BLOCK_BYTES));
        return;
    }
    if (!blockStart && (!a->buf_history || !a->len_history)) {
        *vlo = svdup_n_u8(0);
        return;
    }

    for (i = 0; i < HAO_RUNTIME_BLOCK_BYTES; i++) {
        u8 b = 0;
        haoGetByteAt(a,
                     (s64a)blockStart - (s64a)HAO_RUNTIME_BLOCK_BYTES + i,
                     &b);
        prevBytes[i] = b;
    }

    *vlo = svld1_u8(pgb, prevBytes);
}

static really_inline
void haoLoadRawCurr32(const struct FDR_Runtime_Args *a, size_t blockStart,
                      u32 blockLaneCount, svuint8_t *vhi) {
    const svbool_t pgb = svptrue_b8();
    u8 currBytes[HAO_RUNTIME_BLOCK_BYTES] = {0};
    u32 i;

    assert(a);
    assert(vhi);

    if (blockLaneCount == HAO_RUNTIME_BLOCK_BYTES) {
        *vhi = svld1_u8(pgb, (const uint8_t *)(a->buf + blockStart));
        return;
    }

    for (i = 0; i < blockLaneCount; i++) {
        if (blockStart + i < a->len) {
            currBytes[i] = a->buf[blockStart + i];
        }
    }

    *vhi = svld1_u8(pgb, currBytes);
}

static really_inline
svuint32_t haoExtractRawKeys4(svuint8_t vrow, u64a bextMaskRaw) {
    const svuint64_t packed =
        svbext_n_u64(svreinterpret_u64_u8(vrow), (uint64_t)bextMaskRaw);
    const svuint32_t as32 = svreinterpret_u32_u64(packed);

    return svuzp1_u32(as32, as32);
}

static really_inline
void haoFillRawCtxValid32(struct HAOPositionContext *ctx, size_t endPos,
                          u32 validMask32, u64a laneWord) {
    assert(ctx);
    ctx->endPos = endPos;
    ctx->laneWord = laneWord;
    ctx->validMask32 = validMask32;
}

static really_inline
void haoPrepareRawKeysVec(const u8 *primaryBitmap, svuint32_t vkeys,
                          svuint32_t *vbitPos,
                          svuint32_t *vbitmapBytes) {
    const svbool_t pg32 = svptrue_b32();
    const u32 *primaryBitmapWords = (const u32 *)primaryBitmap;
    const svuint32_t vwordIdx = svlsr_n_u32_x(pg32, vkeys, 5);

    assert(vbitPos);
    assert(vbitmapBytes);

    *vbitPos = svand_n_u32_x(pg32, vkeys, 31U);
    *vbitmapBytes = svld1_gather_u32index_u32(pg32, primaryBitmapWords,
                                              vwordIdx);
}

static really_inline
void haoPrepareRawKeysVecPred(const u8 *primaryBitmap, svbool_t pg,
                              svuint32_t vkeys, svuint32_t *vbitPos,
                              svuint32_t *vbitmapBytes) {
    const u32 *primaryBitmapWords = (const u32 *)primaryBitmap;
    const svuint32_t vwordIdx = svlsr_n_u32_x(pg, vkeys, 5);

    assert(vbitPos);
    assert(vbitmapBytes);

    *vbitPos = svand_n_u32_x(pg, vkeys, 31U);
    *vbitmapBytes = svld1_gather_u32index_u32(pg, primaryBitmapWords,
                                              vwordIdx);
}

static really_inline
svuint32_t haoRawPairLaneIds(u32 pairBase) {
    return svzip1_u32(svindex_u32(pairBase, 8U),
                      svindex_u32(pairBase + 1U, 8U));
}

static really_inline
void haoMergeRawPairActive(const u32 laneByPair[4][8],
                           const u32 encodedByPair[4][8],
                           const u32 countByPair[4],
                           u32 *activeLaneIndex, u32 *activeEncoded,
                           u32 *activeCount) {
    u32 pos[4] = {0};
    u32 out = 0;
    u32 total = countByPair[0] + countByPair[1] + countByPair[2] + countByPair[3];

    while (out < total) {
        u32 l0 = (pos[0] < countByPair[0]) ? laneByPair[0][pos[0]] : 0xffffffffU;
        u32 l1 = (pos[1] < countByPair[1]) ? laneByPair[1][pos[1]] : 0xffffffffU;
        u32 l2 = (pos[2] < countByPair[2]) ? laneByPair[2][pos[2]] : 0xffffffffU;
        u32 l3 = (pos[3] < countByPair[3]) ? laneByPair[3][pos[3]] : 0xffffffffU;
        u32 bestLane = l0;
        u32 bestPair = 0;

        if (l1 < bestLane) { bestLane = l1; bestPair = 1; }
        if (l2 < bestLane) { bestLane = l2; bestPair = 2; }
        if (l3 < bestLane) { bestLane = l3; bestPair = 3; }

        activeLaneIndex[out] = bestLane;
        activeEncoded[out] = encodedByPair[bestPair][pos[bestPair]];
        pos[bestPair]++;
        out++;
    }

    *activeCount = out;
}

static really_inline
u32 haoRetireRawKeysVecLocalWithIds(const u32 *primaryHashTable,
                                    svuint32_t vkeys, svuint32_t vlaneIds,
                                    svuint32_t vbitPos, svuint32_t vbitmapBytes,
                                    u32 *activeLaneIndex, u32 *activeEncoded) {
    const svbool_t pg32 = svptrue_b32();
    const svuint32_t vone = svdup_n_u32(1U);
    const svuint32_t vbitMask = svlsl_u32_x(pg32, vone, vbitPos);
    const svuint32_t vhit = svand_u32_x(pg32, vbitmapBytes, vbitMask);
    const svbool_t phit = svcmpne_n_u32(pg32, vhit, 0U);
    const u32 hitCount = (u32)svcntp_b32(pg32, phit);
    u32 activeCount = 0;

    assert(activeLaneIndex);
    assert(activeEncoded);

    if (hitCount) {
        haoStoreActivePrimaryVector(primaryHashTable, phit, vkeys, vlaneIds,
                                    hitCount, activeLaneIndex, activeEncoded);
        activeCount = hitCount;
    }

    HAO_STATS_ADD(primaryActiveLanes, activeCount);
    return activeCount;
}

static really_inline
u32 haoRetireRawKeysVecTailWithIds(const u32 *primaryHashTable,
                                   svuint32_t vkeys, svuint32_t vlaneIds,
                                   svuint32_t vbitPos,
                                   svuint32_t vbitmapBytes,
                                   u32 blockLaneCount,
                                   u32 *activeLaneIndex,
                                   u32 *activeEncoded) {
    const svbool_t pg32 = svptrue_b32();
    const svbool_t pvalid = svcmplt_n_u32(pg32, vlaneIds, blockLaneCount);
    const svuint32_t vone = svdup_n_u32(1U);
    const svuint32_t vbitMask = svlsl_u32_x(pg32, vone, vbitPos);
    const svuint32_t vrawHit = svand_u32_x(pg32, vbitmapBytes, vbitMask);
    const svuint32_t vhit = svsel_u32(pvalid, vrawHit, svdup_n_u32(0U));
    const svbool_t phit = svcmpne_n_u32(pg32, vhit, 0U);
    const u32 hitCount = (u32)svcntp_b32(pg32, phit);
    u32 activeCount = 0;

    assert(activeLaneIndex);
    assert(activeEncoded);

    if (hitCount) {
        haoStoreActivePrimaryVector(primaryHashTable, phit, vkeys, vlaneIds,
                                    hitCount, activeLaneIndex, activeEncoded);
        activeCount = hitCount;
    }

    HAO_STATS_ADD(primaryActiveLanes, activeCount);
    return activeCount;
}

static really_inline
int haoProcessRawActiveLanes(
    const struct HAORuntimeHeader *hdr,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    const struct FDR_Runtime_Args *a, hwlm_group_t *control,
    size_t blockStart, u32 fullValidBlock, const u32 *activeLaneIndex,
    const u32 *activeEncoded, u32 activeCount, svuint8_t vlo,
    svuint8_t vhi) {
    const svbool_t pgb = svptrue_b8();
    const svbool_t pg64 = svptrue_pat_b64(SV_VL1);
    const svuint8x2_t rawTbl = svcreate2_u8(vlo, vhi);
    const svuint8_t baseIdx = svindex_u8(25, 1);
    u32 i;

    for (i = 0; i < activeCount; i++) {
        struct HAOPositionContext ctx;
        const u32 lane = activeLaneIndex[i];
        const u32 encoded = activeEncoded[i];
        const size_t endPos = blockStart + lane;
        u32 validMask32 = 0xffffffffU;
        const svuint8_t laneIdx =
            svadd_n_u8_x(pgb, baseIdx, (uint8_t)lane);
        const svuint8_t laneBytes = svtbl2_u8(rawTbl, laneIdx);
        const u64a laneWord =
            (u64a)svlastb_u64(pg64, svreinterpret_u64_u8(laneBytes));

        if (unlikely(!fullValidBlock)) {
            validMask32 = haoComputeValidMask8(a, endPos) * 0x01010101U;
        }
        haoFillRawCtxValid32(&ctx, endPos, validMask32, laneWord);
        if (haoProcessEncodedRangePrepared(
                hdr, l2CheckTable, l2MetaTable, ruleMeta, literalBlob,
                hdr->literalBlobSize, a, control, &ctx, encoded) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static really_inline
u32 haoCollectRaw32Active(const u8 *primaryBitmap,
                          const u32 *primaryHashTable, u64a bextMask,
                          svuint8_t vlo, svuint8_t vhi,
                          u32 *blockActiveLaneIndex,
                          u32 *blockActiveEncoded) {
    u32 blockActiveCount = 0;
    svuint32_t vbitPos01;
    svuint32_t vbitPos23;
    svuint32_t vbitPos45;
    svuint32_t vbitPos67;
    svuint32_t vbitmapBytes01;
    svuint32_t vbitmapBytes23;
    svuint32_t vbitmapBytes45;
    svuint32_t vbitmapBytes67;
    const svuint8_t vrow0 = svext_u8(vlo, vhi, 25);
    const svuint8_t vrow1 = svext_u8(vlo, vhi, 26);
    const svuint8_t vrow2 = svext_u8(vlo, vhi, 27);
    const svuint8_t vrow3 = svext_u8(vlo, vhi, 28);
    const svuint8_t vrow4 = svext_u8(vlo, vhi, 29);
    const svuint8_t vrow5 = svext_u8(vlo, vhi, 30);
    const svuint8_t vrow6 = svext_u8(vlo, vhi, 31);
    const svuint8_t vrow7 = vhi;

    const svuint32_t vkeys0 = haoExtractRawKeys4(vrow0, bextMask);
    const svuint32_t vkeys1 = haoExtractRawKeys4(vrow1, bextMask);
    const svuint32_t vkeys2 = haoExtractRawKeys4(vrow2, bextMask);
    const svuint32_t vkeys3 = haoExtractRawKeys4(vrow3, bextMask);
    const svuint32_t vkeys4 = haoExtractRawKeys4(vrow4, bextMask);
    const svuint32_t vkeys5 = haoExtractRawKeys4(vrow5, bextMask);
    const svuint32_t vkeys6 = haoExtractRawKeys4(vrow6, bextMask);
    const svuint32_t vkeys7 = haoExtractRawKeys4(vrow7, bextMask);

    const svuint32_t vkeys01  = svzip1_u32(vkeys0, vkeys1);
    const svuint32_t vkeys23  = svzip1_u32(vkeys2, vkeys3);
    const svuint32_t vkeys45  = svzip1_u32(vkeys4, vkeys5);
    const svuint32_t vkeys67  = svzip1_u32(vkeys6, vkeys7);

    haoPrepareRawKeysVec(primaryBitmap, vkeys01, &vbitPos01, &vbitmapBytes01);
    haoPrepareRawKeysVec(primaryBitmap, vkeys23, &vbitPos23, &vbitmapBytes23);
    haoPrepareRawKeysVec(primaryBitmap, vkeys45, &vbitPos45, &vbitmapBytes45);
    haoPrepareRawKeysVec(primaryBitmap, vkeys67, &vbitPos67, &vbitmapBytes67);

    {
        u32 laneByPair[4][8];
        u32 encodedByPair[4][8];
        u32 countByPair[4];
        const svuint32_t vlaneIds01 = haoRawPairLaneIds(0U);
        const svuint32_t vlaneIds23 = haoRawPairLaneIds(2U);
        const svuint32_t vlaneIds45 = haoRawPairLaneIds(4U);
        const svuint32_t vlaneIds67 = haoRawPairLaneIds(6U);

        countByPair[0] = haoRetireRawKeysVecLocalWithIds(
            primaryHashTable, vkeys01, vlaneIds01, vbitPos01, vbitmapBytes01,
            laneByPair[0], encodedByPair[0]);
        countByPair[1] = haoRetireRawKeysVecLocalWithIds(
            primaryHashTable, vkeys23, vlaneIds23, vbitPos23, vbitmapBytes23,
            laneByPair[1], encodedByPair[1]);
        countByPair[2] = haoRetireRawKeysVecLocalWithIds(
            primaryHashTable, vkeys45, vlaneIds45, vbitPos45, vbitmapBytes45,
            laneByPair[2], encodedByPair[2]);
        countByPair[3] = haoRetireRawKeysVecLocalWithIds(
            primaryHashTable, vkeys67, vlaneIds67, vbitPos67, vbitmapBytes67,
            laneByPair[3], encodedByPair[3]);

        haoMergeRawPairActive((const u32 (*)[8])laneByPair,
                              (const u32 (*)[8])encodedByPair,
                              countByPair, blockActiveLaneIndex,
                              blockActiveEncoded, &blockActiveCount);
    }

    return blockActiveCount;
}

static really_inline
int haoRunRaw32V2(
    const struct HAORuntimeHeader *hdr, const u8 *primaryBitmap,
    const u32 *primaryHashTable,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    const struct FDR_Runtime_Args *a, hwlm_group_t *control,
    size_t blockStart, svuint8_t vlo, svuint8_t vhi) {
    u32 blockActiveLaneIndex[HAO_BATCH_MAX_WIDTH];
    u32 blockActiveEncoded[HAO_BATCH_MAX_WIDTH];
    const u32 blockActiveCount = haoCollectRaw32Active(
        primaryBitmap, primaryHashTable, hdr->bextMaskRaw, vlo, vhi,
        blockActiveLaneIndex, blockActiveEncoded);
    const u32 fullValidBlock =
        blockStart + a->len_history >= HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U;

    if (blockActiveCount) {
        if (haoProcessRawActiveLanes(
                hdr, l2CheckTable, l2MetaTable, ruleMeta, literalBlob, a,
                control, blockStart, fullValidBlock, blockActiveLaneIndex,
                blockActiveEncoded, blockActiveCount, vlo, vhi) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static really_inline
u32 haoCollectRawTailActive(const u8 *primaryBitmap,
                            const u32 *primaryHashTable, u64a bextMask,
                            u32 blockLaneCount, svuint8_t vlo, svuint8_t vhi,
                            u32 *blockActiveLaneIndex,
                            u32 *blockActiveEncoded) {
    u32 blockActiveCount = 0;
    svuint32_t vbitPos01;
    svuint32_t vbitPos23;
    svuint32_t vbitPos45;
    svuint32_t vbitPos67;
    svuint32_t vbitmapBytes01;
    svuint32_t vbitmapBytes23;
    svuint32_t vbitmapBytes45;
    svuint32_t vbitmapBytes67;

    const svuint8_t vrow0 = svext_u8(vlo, vhi, 25);
    const svuint8_t vrow1 = svext_u8(vlo, vhi, 26);
    const svuint8_t vrow2 = svext_u8(vlo, vhi, 27);
    const svuint8_t vrow3 = svext_u8(vlo, vhi, 28);
    const svuint8_t vrow4 = svext_u8(vlo, vhi, 29);
    const svuint8_t vrow5 = svext_u8(vlo, vhi, 30);
    const svuint8_t vrow6 = svext_u8(vlo, vhi, 31);
    const svuint8_t vrow7 = vhi;

    const svuint32_t vkeys0 = haoExtractRawKeys4(vrow0, bextMask);
    const svuint32_t vkeys1 = haoExtractRawKeys4(vrow1, bextMask);
    const svuint32_t vkeys2 = haoExtractRawKeys4(vrow2, bextMask);
    const svuint32_t vkeys3 = haoExtractRawKeys4(vrow3, bextMask);
    const svuint32_t vkeys4 = haoExtractRawKeys4(vrow4, bextMask);
    const svuint32_t vkeys5 = haoExtractRawKeys4(vrow5, bextMask);
    const svuint32_t vkeys6 = haoExtractRawKeys4(vrow6, bextMask);
    const svuint32_t vkeys7 = haoExtractRawKeys4(vrow7, bextMask);

    const svuint32_t vkeys01 = svzip1_u32(vkeys0, vkeys1);
    const svuint32_t vkeys23 = svzip1_u32(vkeys2, vkeys3);
    const svuint32_t vkeys45 = svzip1_u32(vkeys4, vkeys5);
    const svuint32_t vkeys67 = svzip1_u32(vkeys6, vkeys7);

    {
        u32 laneByPair[4][8];
        u32 encodedByPair[4][8];
        u32 countByPair[4];
        const svbool_t pg32 = svptrue_b32();
        const svuint32_t vlaneIds01 = haoRawPairLaneIds(0U);
        const svuint32_t vlaneIds23 = haoRawPairLaneIds(2U);
        const svuint32_t vlaneIds45 = haoRawPairLaneIds(4U);
        const svuint32_t vlaneIds67 = haoRawPairLaneIds(6U);
        const svbool_t pvalid01 =
            svcmplt_n_u32(pg32, vlaneIds01, blockLaneCount);
        const svbool_t pvalid23 =
            svcmplt_n_u32(pg32, vlaneIds23, blockLaneCount);
        const svbool_t pvalid45 =
            svcmplt_n_u32(pg32, vlaneIds45, blockLaneCount);
        const svbool_t pvalid67 =
            svcmplt_n_u32(pg32, vlaneIds67, blockLaneCount);

        haoPrepareRawKeysVecPred(primaryBitmap, pvalid01, vkeys01,
                                 &vbitPos01, &vbitmapBytes01);
        haoPrepareRawKeysVecPred(primaryBitmap, pvalid23, vkeys23,
                                 &vbitPos23, &vbitmapBytes23);
        haoPrepareRawKeysVecPred(primaryBitmap, pvalid45, vkeys45,
                                 &vbitPos45, &vbitmapBytes45);
        haoPrepareRawKeysVecPred(primaryBitmap, pvalid67, vkeys67,
                                 &vbitPos67, &vbitmapBytes67);

        countByPair[0] = haoRetireRawKeysVecTailWithIds(
            primaryHashTable, vkeys01, vlaneIds01, vbitPos01, vbitmapBytes01,
            blockLaneCount, laneByPair[0], encodedByPair[0]);
        countByPair[1] = haoRetireRawKeysVecTailWithIds(
            primaryHashTable, vkeys23, vlaneIds23, vbitPos23, vbitmapBytes23,
            blockLaneCount, laneByPair[1], encodedByPair[1]);
        countByPair[2] = haoRetireRawKeysVecTailWithIds(
            primaryHashTable, vkeys45, vlaneIds45, vbitPos45, vbitmapBytes45,
            blockLaneCount, laneByPair[2], encodedByPair[2]);
        countByPair[3] = haoRetireRawKeysVecTailWithIds(
            primaryHashTable, vkeys67, vlaneIds67, vbitPos67, vbitmapBytes67,
            blockLaneCount, laneByPair[3], encodedByPair[3]);

        haoMergeRawPairActive((const u32 (*)[8])laneByPair,
                              (const u32 (*)[8])encodedByPair,
                              countByPair, blockActiveLaneIndex,
                              blockActiveEncoded, &blockActiveCount);
    }

    return blockActiveCount;
}

static really_inline
int haoRunRawTailVec(
    const struct HAORuntimeHeader *hdr, const u8 *primaryBitmap,
    const u32 *primaryHashTable,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    const struct FDR_Runtime_Args *a, hwlm_group_t *control,
    size_t blockStart, u32 blockLaneCount, svuint8_t vlo, svuint8_t vhi) {
    u32 blockActiveLaneIndex[HAO_BATCH_MAX_WIDTH];
    u32 blockActiveEncoded[HAO_BATCH_MAX_WIDTH];
    const u32 blockActiveCount = haoCollectRawTailActive(
        primaryBitmap, primaryHashTable, hdr->bextMaskRaw, blockLaneCount,
        vlo, vhi, blockActiveLaneIndex, blockActiveEncoded);
    const u32 fullValidBlock =
        blockStart + a->len_history >= HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U;

    if (blockActiveCount) {
        if (haoProcessRawActiveLanes(
                hdr, l2CheckTable, l2MetaTable, ruleMeta, literalBlob, a,
                control, blockStart, fullValidBlock, blockActiveLaneIndex,
                blockActiveEncoded, blockActiveCount, vlo, vhi) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static really_inline
u64a haoBuildRawWordScalar(const struct FDR_Runtime_Args *a, size_t endPos) {
    u8 bytes[HAO_RUNTIME_BYTES_PER_RULE_SLOT];
    u32 i;

    assert(a);

    for (i = 0; i < HAO_RUNTIME_BYTES_PER_RULE_SLOT; i++) {
        u8 b = 0;
        const s64a pos = (s64a)endPos -
                         (s64a)(HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U) +
                         (s64a)i;

        haoGetByteAt(a, pos, &b);
        bytes[i] = b;
    }

    return unaligned_load_u64a(bytes);
}

static really_inline
int haoRawBitmapHitScalar(const u8 *primaryBitmap, u32 key) {
    const u32 *primaryBitmapWords = (const u32 *)primaryBitmap;
    const u32 word = primaryBitmapWords[key >> 5U];
    return word & (1U << (key & 31U));
}

static really_inline
int haoRunRawTailScalar(
    const struct HAORuntimeHeader *hdr, const u8 *primaryBitmap,
    const u32 *primaryHashTable,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    const u32 *residualRuleIndexes, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, size_t blockStart, u32 blockLaneCount) {
    u32 lane;
    u32 activeCount = 0;

    assert(hdr);
    assert(primaryBitmap);
    assert(primaryHashTable);
    assert(l2CheckTable);
    assert(l2MetaTable);
    assert(ruleMeta);
    assert(literalBlob);
    assert(a);
    assert(control);

    for (lane = 0; lane < blockLaneCount; lane++) {
        struct HAOPositionContext ctx;
        const size_t endPos = blockStart + lane;
        const u64a rawWord = haoBuildRawWordScalar(a, endPos);
        const u32 key = (u32)pext64(rawWord, hdr->bextMaskRaw);

        if (haoRawBitmapHitScalar(primaryBitmap, key)) {
            const u32 encoded = primaryHashTable[key];
            const u64a laneWord = haoBuildRawWordScalar(a, endPos);
            const u32 validMask8 = haoComputeValidMask8(a, endPos);

            if (encoded) {
                activeCount++;
                haoFillRawCtxValid32(&ctx, endPos,
                                      validMask8 * 0x01010101U, laneWord);
                if (haoProcessEncodedRangePrepared(
                        hdr, l2CheckTable, l2MetaTable, ruleMeta, literalBlob,
                        hdr->literalBlobSize, a, control, &ctx, encoded) ==
                    HWLM_TERMINATED) {
                    HAO_STATS_ADD(primaryActiveLanes, activeCount);
                    return HWLM_TERMINATED;
                }
            }
        }

        if (hdr->residualRuleCount &&
            haoProcessResidualRulesAtPos(hdr, residualRuleIndexes, ruleMeta,
                                         literalBlob, hdr->literalBlobSize, a,
                                         control, endPos) ==
                HWLM_TERMINATED) {
            HAO_STATS_ADD(primaryActiveLanes, activeCount);
            return HWLM_TERMINATED;
        }
    }

    HAO_STATS_ADD(primaryActiveLanes, activeCount);
    return HWLM_SUCCESS;
}

#endif


static int haoProcessBlockBatch(const struct HAORuntimeHeader *hdr,
                                const u8 *primaryBitmapRaw,
                                const u32 *primaryHashTableRaw,
                                const struct HAORuntimeL2Check *l2CheckTable,
                                const struct HAORuntimeL2Meta *l2MetaTable,
                                const struct HAORuntimeRuleMeta *ruleMeta,
                                const u8 *literalBlob,
                                const u32 *residualRuleIndexes,
                                const struct FDR_Runtime_Args *a,
                                hwlm_group_t *control, size_t blockStart,
                                u32 blockLaneCount) {
    if (!hdr || !primaryBitmapRaw || !primaryHashTableRaw ||
        !l2CheckTable || !l2MetaTable || !ruleMeta || !literalBlob || !a || !control ||
        !blockLaneCount || blockLaneCount > HAO_BATCH_MAX_WIDTH ||
        hdr->extractMode != HAO_RUNTIME_EXTRACT_MODE_BEXT ||
        hdr->windowBytes != HAO_RUNTIME_BYTES_PER_RULE_SLOT ||
        !hdr->primaryBitmapRawOffset ||
        !hdr->primaryRawOffset) {
        return HWLM_SUCCESS;
    }

    HAO_STATS_ADD(blockCalls, 1);
    HAO_STATS_ADD(blockLanes, blockLaneCount);
    HAO_STATS_ADD(primaryProbeLanes, blockLaneCount);
#if defined(HS_BUILD_HAVE_SVEBITPERM) && defined(__ARM_FEATURE_SVE2_BITPERM)
    return haoRunRawTailScalar(hdr, primaryBitmapRaw, primaryHashTableRaw,
                               l2CheckTable, l2MetaTable, ruleMeta,
                               literalBlob, residualRuleIndexes, a, control,
                               blockStart, blockLaneCount);
#else
    (void)l2CheckTable;
    (void)l2MetaTable;
    return HWLM_SUCCESS;
#endif
}

static int haoRunBatchBlob(const struct HAORuntimeHeader *hdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t *control) {
    const u8 *primaryBitmapRaw;
    const u32 *primaryHashTableRaw;
    const struct HAORuntimeL2Check *l2CheckTable;
    const struct HAORuntimeL2Meta *l2MetaTable;
    const struct HAORuntimeRuleMeta *ruleMeta;
    const u8 *literalBlob;
    const u32 *residualRuleIndexes;
    size_t i;
#if defined(__ARM_FEATURE_SVE) && defined(HS_BUILD_HAVE_SVEBITPERM) && \
    defined(__ARM_FEATURE_SVE2_BITPERM)
    svuint8_t rawPrev = svdup_n_u8(0);
    int rawPrevReady = 0;
#endif

    if (unlikely(!hdr || !a || !a->buf || !a->len ||
                 a->start_offset >= a->len)) {
        return HWLM_SUCCESS;
    }

    haoRefreshStatsMode();
    HAO_STATS_ADD(scanCalls, 1);
    HAO_STATS_ADD(scanInputBytes, a->len - a->start_offset);

    primaryBitmapRaw = (const u8 *)hdr + hdr->primaryBitmapRawOffset;
    primaryHashTableRaw =
        (const u32 *)((const u8 *)hdr + hdr->primaryRawOffset);
    l2CheckTable = (const struct HAORuntimeL2Check *)((const u8 *)hdr +
                                                      hdr->l2CheckOffset);
    l2MetaTable = (const struct HAORuntimeL2Meta *)((const u8 *)hdr +
                                                    hdr->l2MetaOffset);
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
        if (likely(haoCanRunRaw32V2(hdr, a, i, blockLaneCount))) {
            svuint8_t rawCurr;

            if (unlikely(!rawPrevReady)) {
                haoLoadRawPrev32(a, i, &rawPrev);
                rawPrevReady = 1;
            }
            haoLoadRawCurr32(a, i, HAO_RUNTIME_BLOCK_BYTES, &rawCurr);

            HAO_STATS_ADD(blockCalls, 1);
            HAO_STATS_ADD(blockLanes, HAO_BATCH_MAX_WIDTH);
            HAO_STATS_ADD(primaryProbeLanes, HAO_BATCH_MAX_WIDTH);

            if (likely(haoRunRaw32V2(hdr, primaryBitmapRaw, primaryHashTableRaw,
                                       l2CheckTable, l2MetaTable, ruleMeta,
                                       literalBlob, a, control, i, rawPrev,
                                       rawCurr) == HWLM_TERMINATED)) {
                return HWLM_TERMINATED;
            }
            rawPrev = rawCurr;
            continue;
        }
        if (unlikely(haoCanRunRawTailVec(hdr, a, i, blockLaneCount))) {
            svuint8_t rawCurr;

            if (unlikely(!rawPrevReady)) {
                haoLoadRawPrev32(a, i, &rawPrev);
                rawPrevReady = 1;
            }
            haoLoadRawCurr32(a, i, blockLaneCount, &rawCurr);

            HAO_STATS_ADD(blockCalls, 1);
            HAO_STATS_ADD(blockLanes, blockLaneCount);
            HAO_STATS_ADD(primaryProbeLanes, blockLaneCount);

            if (unlikely(haoRunRawTailVec(hdr, primaryBitmapRaw,
                                          primaryHashTableRaw, l2CheckTable,
                                          l2MetaTable, ruleMeta, literalBlob,
                                          a, control, i, blockLaneCount,
                                          rawPrev, rawCurr) ==
                         HWLM_TERMINATED)) {
                return HWLM_TERMINATED;
            }
            rawPrev = rawCurr;
            continue;
        }
        rawPrevReady = 0;

        if (unlikely(haoProcessBlockBatch(hdr, primaryBitmapRaw,
                                          primaryHashTableRaw, l2CheckTable,
                                          l2MetaTable, ruleMeta, literalBlob,
                                          residualRuleIndexes, a, control, i,
                                          blockLaneCount) ==
                     HWLM_TERMINATED)) {
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

hwlm_error_t HaoEngineExecNaiveForTest(const struct FDR *fdr,
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

u32 HaoRuntimeBitmapProbeMaskForTest(const u8 *bitmap, u32 bitmapSize,
                                     const u32 *primaryIdx, u32 laneCount,
                                     int usePacked) {
    u32 activeMask = 0;
    u32 lane;

    if (!bitmap || !primaryIdx || laneCount > HAO_BATCH_MAX_WIDTH) {
        return 0;
    }

    if (!usePacked) {
        for (lane = 0; lane < laneCount; lane++) {
            if (haoPrimaryBitmapHasValue(bitmap, bitmapSize, primaryIdx[lane])) {
                activeMask |= 1U << lane;
            }
        }
        return activeMask;
    }

    {
        struct HAOPrimaryProbeState probe;
        memset(&probe, 0, sizeof(probe));
        haoPreparePrimaryProbeStateFromPrimaryIdx(primaryIdx, laneCount,
                                                  &probe);

        {
            u32 groupedActiveMask = 0;
            if (haoProbePrimaryBitmapGrouped(bitmap, bitmapSize, &probe,
                                             &groupedActiveMask)) {
                return groupedActiveMask;
            }
        }

#if defined(HAVE_NEON) || defined(HAVE_SSE2)
        {
            u8 gatheredBytes[HAO_BATCH_MAX_WIDTH] = {0};

            for (lane = 0; lane < probe.laneCount; lane++) {
                const u32 idx = probe.byteIndex[lane];
                gatheredBytes[lane] = idx < bitmapSize ? bitmap[idx] : 0;
            }

            for (lane = 0; lane < probe.laneCount; lane += 16U) {
                const u32 lanesThisRound = MIN(16U, probe.laneCount - lane);
                const u32 laneMask = lanesThisRound == 16U
                                         ? 0xffffU
                                         : ((1U << lanesThisRound) - 1U);
                const m128 gathered = loadu128(gatheredBytes + lane);
                const m128 masks = loadu128(probe.bitMask + lane);
                const m128 masked = and128(gathered, masks);
                const u32 roundMask =
                    (~movemask128(eq128(masked, zeroes128()))) & laneMask;

                activeMask |= roundMask << lane;
            }
            return activeMask;
        }
#else
        for (lane = 0; lane < probe.laneCount; lane++) {
            const u32 idx = probe.byteIndex[lane];
            if (idx < bitmapSize && (bitmap[idx] & probe.bitMask[lane])) {
                activeMask |= 1U << lane;
            }
        }
        return activeMask;
#endif
    }
}

u64a HaoRuntimeRawLaneWordForTest(const u8 *prev32, const u8 *curr32,
                                  u32 lane) {
    if (!prev32 || !curr32 || lane >= HAO_RUNTIME_BLOCK_BYTES) {
        return 0;
    }

#if defined(HAVE_SVE)
    {
        const svbool_t pgb = svptrue_b8();
        const svbool_t pg64 = svptrue_pat_b64(SV_VL1);
        const svuint8_t vlo = svld1_u8(pgb, prev32);
        const svuint8_t vhi = svld1_u8(pgb, curr32);
        const svuint8x2_t rawTbl = svcreate2_u8(vlo, vhi);
        const svuint8_t baseIdx = svindex_u8(25, 1);
        const svuint8_t laneIdx =
            svadd_n_u8_x(pgb, baseIdx, (uint8_t)lane);
        const svuint8_t laneBytes = svtbl2_u8(rawTbl, laneIdx);

        return (u64a)svlastb_u64(pg64, svreinterpret_u64_u8(laneBytes));
    }
#else
    u8 bytes[HAO_RUNTIME_BYTES_PER_RULE_SLOT];
    u32 i;

    for (i = 0; i < HAO_RUNTIME_BYTES_PER_RULE_SLOT; i++) {
        const u32 idx = lane + 25U + i;
        bytes[i] = idx < HAO_RUNTIME_BLOCK_BYTES
                     ? prev32[idx]
                     : curr32[idx - HAO_RUNTIME_BLOCK_BYTES];
    }
    return unaligned_load_u64a(bytes);
#endif
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

int HaoRuntimeStatsEnabledForTest(void) {
#if HAO_ENABLE_RUNTIME_STATS
    return 1;
#else
    return 0;
#endif
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
hwlm_error_t haoExec(const struct FDR *fdr,
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
    return haoExec(fdr, a, control);
}
