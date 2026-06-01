/*
 * Copyright (c) 2026, Huawei Technologies Co., Ltd.
 */

#include "fdr_enhanced.h"
#include "fdr_internal.h"
#include "hao_runtime.h"
#include "hao_runtime_test.h"
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

#ifndef HAO_PREFETCH_INPUT_DISTANCE
#define HAO_PREFETCH_INPUT_DISTANCE 32U
#endif

#define HAO_PREFETCH_R(addr) __builtin_prefetch((addr), 0, 3)


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
void haoPrepProbe(
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
int haoProbeBits(const u8 *bitmap, u32 bitmapSize,
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

/* Validate the HAO blob layout before entering execution. */
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
    if (!hdr->keyBits || !hdr->primaryCount || !hdr->l2EntryCount) {
        return 0;
    }
    if (hdr->keyBits > HAO_RUNTIME_MAX_SELECTORS) {
        return 0;
    }
    if (!hdr->bextMask) {
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
    if (!hdr->l2CheckOffset || !hdr->l2MetaOffset ||
        (hdr->l2CheckOffset & (HAO_RUNTIME_L2_CHECK_ALIGN - 1U))) {
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
    if (outHdr) {
        *outHdr = hdr;
    }
    return 1;
}

static really_inline
u32 haoL2MetaRuleCount(const struct HAORuntimeL2Meta *meta) {
    u32 slot;
    u32 count = 0;

    if (!meta) {
        return 0;
    }
    for (slot = 0; slot < HAO_RUNTIME_RULE_SLOTS_PER_ENTRY; slot++) {
        count += meta->ruleIndex[slot] != HAO_RUNTIME_INVALID_RULE_INDEX;
    }
    return count;
}

/* Collect a read-only summary for inspecting HAO blobs in tests. */
static void haoInspectLayout(const struct HAORuntimeHeader *hdr,
                             struct HAORuntimeInspectSummary *summary) {
    const u32 *primary;
    const struct HAORuntimeL2Meta *l2MetaTable;
    u32 i;

    if (!hdr || !summary) {
        return;
    }

    memset(summary, 0, sizeof(*summary));
    summary->keyBits = hdr->keyBits;
    summary->primaryCount = hdr->primaryCount;
    summary->primaryBitmapSize = hdr->primaryBitmapSize;
    summary->l2EntryCount = hdr->l2EntryCount;
    summary->ruleMetaCount = hdr->ruleMetaCount;

    primary = (const u32 *)((const u8 *)hdr + hdr->primaryOffset);
    l2MetaTable = (const struct HAORuntimeL2Meta *)((const u8 *)hdr +
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
        summary->totalRulesInL2 += haoL2MetaRuleCount(&l2MetaTable[i]);
    }
}

/* HAO receives Rose short literals (<= 8 bytes). L2 already verifies the
 * literal bytes, so the remaining confirm work is only the supplementary
 * mask/cmp payload carried by masked or mixed-sensitivity literals. */
static really_inline
int haoRuleMaskMatch(const struct HAORuntimeRuleMeta *rm,
                     const struct FDR_Runtime_Args *a,
                     const struct HAOPositionContext *ctx,
                     svuint64_t laneData) {
    const u8 mlen = rm->maskLen;
    if (unlikely((u64a)mlen > (u64a)ctx->endPos + 1 + a->len_history)) {
        return 0;
    }

    const u64a laneWord = (u64a)svlastb_u64(svptrue_b64(), laneData);
    return (laneWord & rm->maskWord) == rm->cmpWord;
}

static really_inline
u32 haoL2ValidSlots(u32 careBits, u32 validMask32,
                    u32 matchMask) {
    u32 slot;
    u32 out = matchMask;
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
                  const struct HAOPositionContext *ctx,
                  svuint64_t laneData, svuint64_t vslotBits) {
    const svbool_t pg = svwhilelt_b64((u32)0, HAO_RUNTIME_RULE_SLOTS_PER_ENTRY);
    const svuint64_t rule = svld1_u64(pg, (const uint64_t *)check->rule);
    const svuint64_t mask = svld1_u64(pg, (const uint64_t *)check->mask);
    const svbool_t hit =
        svcmpeq_u64(pg, svand_u64_x(pg, laneData, mask), rule);
    u32 laneMask = (u32)svorv_u64(
        pg, svsel_u64(hit, vslotBits, svdup_n_u64(0U)));

    if (unlikely(ctx->validMask32 != 0xffffffffU)) {
        laneMask = haoL2ValidSlots(
            meta->careBits, ctx->validMask32, laneMask);
    }
    return laneMask;
}

static really_inline
int haoProcessL2Entry(
    const struct HAORuntimeL2Check *check, const struct HAORuntimeL2Meta *meta,
    const struct HAORuntimeRuleMeta *ruleMeta,
    const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct HAOPositionContext *ctx,
    svuint64_t laneData, svuint64_t vslotBits, u32 *lastMatchId
#if HAO_ENABLE_RUNTIME_STATS
    ,
    int *anyReport
#endif
    ) {
    HAO_STATS_ADD(verifierCalls, 1);
    u32 matchMask = haoL2MatchSve(check, meta, ctx, laneData, vslotBits);
    if (likely(!matchMask)) {
        return HWLM_SUCCESS;
    }

    HAO_STATS_ADD(verifierEntryHits, 1);
    HAO_STATS_ADD(verifierSlotHits, popcount32(matchMask));

    while (matchMask) {
        const u32 r = ctz32(matchMask);
        const u32 ridx = meta->ruleIndex[r];
        const struct HAORuntimeRuleMeta *rm;

        rm = &ruleMeta[ridx];
        if (!(rm->groups & *control)) {
            HAO_STATS_ADD(encodedGroupRejects, 1);
            matchMask &= matchMask - 1U;
            continue;
        }
        if (rm->flags & HAO_RULE_FLAG_HAS_MASK) {
            HAO_STATS_ADD(encodedConfirmCalls, 1);
            if (!haoRuleMaskMatch(rm, a, ctx, laneData)) {
                HAO_STATS_ADD(encodedConfirmRejects, 1);
                matchMask &= matchMask - 1U;
                continue;
            }
            HAO_STATS_ADD(encodedConfirmMatches, 1);
        } else {
            HAO_STATS_ADD(directReports, 1);
        }

        if ((rm->flags & HAO_RULE_FLAG_NORUNS) && *lastMatchId == rm->id) {
            matchMask &= matchMask - 1U;
            continue;
        }
        *lastMatchId = rm->id;

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
int haoRunL2Range(
    u32 l2EntryCount,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta,
    const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct HAOPositionContext *ctx,
    svuint64_t laneData, svuint64_t vslotBits, u32 encoded) {
    HAO_STATS_ADD(encodedRangeCalls, 1);
    u32 offset = encoded & HAO_RUNTIME_L1_OFFSET_MASK;
    u32 count = encoded >> HAO_RUNTIME_L1_COUNT_SHIFT;
    u32 lastMatchId = HAO_RUNTIME_INVALID_RULE_INDEX;

#if !HAO_ENABLE_RUNTIME_STATS
    (void)l2EntryCount;
#if HAO_L2_COUNT1_FAST
    if (likely(count == 1U)) {
        return haoProcessL2Entry(
            &l2CheckTable[offset], &l2MetaTable[offset], ruleMeta, a, control,
            ctx, laneData, vslotBits, &lastMatchId);
    }
#endif

    for (u32 n = 0; n < count; n++) {
        const u32 off = offset + n;
        int rv;

        rv = haoProcessL2Entry(
            &l2CheckTable[off], &l2MetaTable[off], ruleMeta, a, control, ctx,
            laneData, vslotBits, &lastMatchId);
        if (rv == HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }
#else
    u32 visitedCount = 0;
    u32 bucketRuleCount = 0;
    int anyReport = 0;
#if HAO_L2_COUNT1_FAST
    if (count == 1U) {
        int rv;

        if (!offset || offset >= l2EntryCount) {
            haoStatsObserveRangeShape(0, 0);
            return HWLM_SUCCESS;
        }

        HAO_STATS_IF_ENABLED({
            visitedCount = 1;
            bucketRuleCount = haoL2MetaRuleCount(&l2MetaTable[offset]);
        });
        HAO_STATS_ADD(encodedEntriesVisited, 1);

        rv = haoProcessL2Entry(
            &l2CheckTable[offset], &l2MetaTable[offset], ruleMeta,
            a, control, ctx, laneData, vslotBits, &lastMatchId,
            &anyReport);
        haoStatsObserveRangeShape(visitedCount, bucketRuleCount);
        HAO_STATS_ADD(encodedRangeReportCalls, anyReport ? 1 : 0);
        return rv;
    }
#endif

    for (u32 n = 0; n < count; n++) {
        const u32 off = offset + n;
        int rv;

        if (!off || off >= l2EntryCount) {
            break;
        }

        HAO_STATS_IF_ENABLED({
            visitedCount++;
            bucketRuleCount += haoL2MetaRuleCount(&l2MetaTable[off]);
        });
        HAO_STATS_ADD(encodedEntriesVisited, 1);

        rv = haoProcessL2Entry(
            &l2CheckTable[off], &l2MetaTable[off], ruleMeta, a, control, ctx,
            laneData, vslotBits, &lastMatchId, &anyReport);
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

#if defined(HS_BUILD_HAVE_SVEBITPERM) && defined(__ARM_FEATURE_SVE2_BITPERM)
static really_inline
int haoRunRawTailScalar(
    u64a bextMask, u32 l2EntryCount, const u8 *primaryBitmap,
    const u32 *primaryHashTable,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, size_t blockStart, u32 blockLaneCount);
#endif

/* Naive HAO execution path used both as a correctness baseline and as a
 * fallback when the batch kernel is not selected. */
static int haoRunNaiveBlob(const struct HAORuntimeHeader *hdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t *control) {
    const u8 *primaryBitmap;
    const u32 *primaryHashTable;
    const struct HAORuntimeL2Check *l2CheckTable;
    const struct HAORuntimeL2Meta *l2MetaTable;
    const struct HAORuntimeRuleMeta *ruleMeta;
    u64a bextMask;
    size_t i;

    if (!hdr || !a || !a->buf || !a->len || a->start_offset >= a->len) {
        return HWLM_SUCCESS;
    }

    haoRefreshStatsMode();
    HAO_STATS_ADD(scanCalls, 1);
    HAO_STATS_ADD(scanInputBytes, a->len - a->start_offset);

    primaryBitmap = (const u8 *)hdr + hdr->primaryBitmapOffset;
    primaryHashTable =
        (const u32 *)((const u8 *)hdr + hdr->primaryOffset);
    l2CheckTable = (const struct HAORuntimeL2Check *)((const u8 *)hdr +
                                                      hdr->l2CheckOffset);
    l2MetaTable = (const struct HAORuntimeL2Meta *)((const u8 *)hdr +
                                                    hdr->l2MetaOffset);
    ruleMeta = (const struct HAORuntimeRuleMeta *)((const u8 *)hdr +
                                                   hdr->ruleMetaOffset);
    bextMask = hdr->bextMask;

#if defined(HS_BUILD_HAVE_SVEBITPERM) && defined(__ARM_FEATURE_SVE2_BITPERM)
    for (i = a->start_offset; i < a->len; i++) {
        HAO_STATS_ADD(primaryProbeLanes, 1);
        if (haoRunRawTailScalar(bextMask, hdr->l2EntryCount, primaryBitmap,
                                primaryHashTable,
                                l2CheckTable, l2MetaTable, ruleMeta, a,
                                control, i, 1U) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }
#else
    (void)primaryBitmap;
    (void)primaryHashTable;
    (void)l2CheckTable;
    (void)l2MetaTable;
    (void)ruleMeta;
    (void)bextMask;
    (void)i;
#endif

    return HWLM_SUCCESS;
}

#if defined(HS_BUILD_HAVE_SVEBITPERM) && defined(__ARM_FEATURE_SVE2_BITPERM)

static really_inline
void haoLoadRawPrev32(const struct FDR_Runtime_Args *a, size_t blockStart,
                      svuint8_t *vlo) {
    const svbool_t pgb = svptrue_b8();
    u8 prevBytes[HAO_RUNTIME_BLOCK_BYTES] = {0};
    u32 i;

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
svuint32_t haoRawKeyPair(svuint8_t vrow0, svuint8_t vrow1, u64a bextMask) {
    const svuint64_t keys0 =
        svbext_n_u64(svreinterpret_u64_u8(vrow0), (uint64_t)bextMask);
    const svuint64_t keys1 =
        svbext_n_u64(svreinterpret_u64_u8(vrow1), (uint64_t)bextMask);
    // const svuint64_t paired = svzip1_u64(keys0, keys1);
    return svqxtnt_u64(svqxtnb_u64(keys0), keys1);
}

static really_inline
void haoPrepRawKeys(const u8 *primaryBitmap, svuint32_t vkeys,
                    svuint32_t *vbitPos, svuint32_t *vbitmapBytes) {
    const svbool_t pg32 = svptrue_b32();
    const u32 *primaryBitmapWords = (const u32 *)primaryBitmap;
    const svuint32_t vwordIdx = svlsr_n_u32_x(pg32, vkeys, 5);

    *vbitPos = svand_n_u32_x(pg32, vkeys, 31U);
    *vbitmapBytes = svld1_gather_u32index_u32(pg32, primaryBitmapWords,
                                              vwordIdx);
}

static really_inline
void haoPrepRawKeysPred(const u8 *primaryBitmap, svbool_t pg,
                              svuint32_t vkeys, svuint32_t *vbitPos,
                              svuint32_t *vbitmapBytes) {
    const u32 *primaryBitmapWords = (const u32 *)primaryBitmap;
    const svuint32_t vwordIdx = svlsr_n_u32_x(pg, vkeys, 5);

    *vbitPos = svand_n_u32_x(pg, vkeys, 31U);
    *vbitmapBytes = svld1_gather_u32index_u32(pg, primaryBitmapWords,
                                              vwordIdx);
}

static really_inline
svuint32_t haoRawLaneIds(u32 pairBase) {
    return svzip1_u32(svindex_u32(pairBase, 8U),
                      svindex_u32(pairBase + 1U, 8U));
}

static really_inline
svuint32_t haoRawLaneBits(u32 pairBase) {
    const svbool_t pg32 = svptrue_b32();

    return svlsl_u32_x(pg32, svdup_n_u32(1U), haoRawLaneIds(pairBase));
}

static really_inline
u32 haoRetireEncodedPair(const u32 *primaryHashTable, svuint32_t vlaneBits,
                         svuint32_t vkeys, svuint32_t vbitPos,
                         svuint32_t vbitmapBytes, u32 *encodedPair) {
    const svbool_t pg32 = svptrue_b32();
    const svuint32_t vone = svdup_n_u32(1U);
    const svuint32_t vbitMask = svlsl_u32_x(pg32, vone, vbitPos);
    const svuint32_t vhit = svand_u32_x(pg32, vbitmapBytes, vbitMask);
    const svbool_t phit = svcmpne_n_u32(pg32, vhit, 0U);
    const svuint32_t vzero = svdup_n_u32(0U);

    if (likely(!svptest_any(pg32, phit))) {
        return 0;
    }

    const svuint32_t vencoded =
        svld1_gather_u32index_u32(phit, primaryHashTable, vkeys);
    const svuint32_t vhitBits = svsel_u32(phit, vlaneBits, vzero);
    const u32 laneMask = svorv_u32(pg32, vhitBits);

    svst1_u32(phit, encodedPair, vencoded);
    return laneMask;
}

static really_inline
u32 haoRetireEncodedPairPred(const u32 *primaryHashTable, svbool_t pvalid,
                             svuint32_t vlaneBits, svuint32_t vkeys,
                             svuint32_t vbitPos, svuint32_t vbitmapBytes,
                             u32 *encodedPair) {
    const svbool_t pg32 = svptrue_b32();
    const svuint32_t vone = svdup_n_u32(1U);
    const svuint32_t vbitMask = svlsl_u32_x(pg32, vone, vbitPos);
    const svuint32_t vrawHit = svand_u32_x(pg32, vbitmapBytes, vbitMask);
    const svuint32_t vhit = svsel_u32(pvalid, vrawHit, svdup_n_u32(0U));
    const svbool_t phit = svcmpne_n_u32(pg32, vhit, 0U);
    const svuint32_t vzero = svdup_n_u32(0U);

    if (likely(!svptest_any(pg32, phit))) {
        return 0;
    }

    const svuint32_t vencoded =
        svld1_gather_u32index_u32(phit, primaryHashTable, vkeys);
    const svuint32_t vhitBits = svsel_u32(phit, vlaneBits, vzero);
    const u32 laneMask = svorv_u32(pg32, vhitBits);

    svst1_u32(phit, encodedPair, vencoded);
    return laneMask;
}

static really_inline
int haoRunEncodedLanes(
    u32 l2EntryCount,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta,
    const struct FDR_Runtime_Args *a, hwlm_group_t *control,
    size_t blockStart, u32 fullValidBlock, const u32 encodedByPair[4][8],
    u32 laneMask, svuint8_t vlo, svuint8_t vhi) {
    const svbool_t pgb = svptrue_b8();
    const svuint8x2_t rawTbl = svcreate2_u8(vlo, vhi);
    const svuint8_t baseIdx =
        svadd_n_u8_x(pgb, svand_n_u8_x(pgb, svindex_u8(0, 1), 7U), 25U);
    const svbool_t pg64 =
        svwhilelt_b64((u32)0, HAO_RUNTIME_RULE_SLOTS_PER_ENTRY);
    const svuint64_t vslotBits =
        svlsl_u64_x(pg64, svdup_n_u64(1U), svindex_u64(0, 1));

    while (laneMask) {
        const u32 lane = (u32)__builtin_ctz(laneMask);
        const u32 pair = (lane & 7U) >> 1U;
        const u32 pairIdx = ((lane >> 3U) << 1U) | (lane & 1U);
        const u32 encoded = encodedByPair[pair][pairIdx];

        laneMask &= laneMask - 1U;
        struct HAOPositionContext ctx;
        size_t endPos = blockStart + lane;
        u32 validMask32 = 0xffffffffU;
        svuint8_t laneIdx = svadd_n_u8_x(pgb, baseIdx, (uint8_t)lane);
        svuint8_t laneBytes = svtbl2_u8(rawTbl, laneIdx);
        svuint64_t laneData = svreinterpret_u64_u8(laneBytes);

        if (unlikely(!fullValidBlock)) {
            validMask32 = haoComputeValidMask8(a, endPos) * 0x01010101U;
        }
        ctx.endPos = endPos;
        ctx.validMask32 = validMask32;
        if (likely(haoRunL2Range(l2EntryCount, l2CheckTable, l2MetaTable,
                ruleMeta, a, control, &ctx, laneData, vslotBits, encoded) ==
            HWLM_TERMINATED)) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static really_inline
u32 haoCollectRawEncoded(const u8 *primaryBitmap,
                         const u32 *primaryHashTable, u64a bextMask,
                         svuint8_t vlo, svuint8_t vhi,
                         svuint32_t vlaneBits01,
                         svuint32_t vlaneBits23,
                         svuint32_t vlaneBits45,
                         svuint32_t vlaneBits67,
                         u32 encodedByPair[4][8]) {
    u32 laneMask = 0;
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

    const svuint32_t vkeys01 = haoRawKeyPair(vrow0, vrow1, bextMask);
    const svuint32_t vkeys23 = haoRawKeyPair(vrow2, vrow3, bextMask);
    const svuint32_t vkeys45 = haoRawKeyPair(vrow4, vrow5, bextMask);
    const svuint32_t vkeys67 = haoRawKeyPair(vrow6, vrow7, bextMask);

    haoPrepRawKeys(primaryBitmap, vkeys01, &vbitPos01, &vbitmapBytes01);
    haoPrepRawKeys(primaryBitmap, vkeys23, &vbitPos23, &vbitmapBytes23);
    haoPrepRawKeys(primaryBitmap, vkeys45, &vbitPos45, &vbitmapBytes45);
    haoPrepRawKeys(primaryBitmap, vkeys67, &vbitPos67, &vbitmapBytes67);

    laneMask |= haoRetireEncodedPair(
        primaryHashTable, vlaneBits01, vkeys01, vbitPos01, vbitmapBytes01, encodedByPair[0]);
    laneMask |= haoRetireEncodedPair(
        primaryHashTable, vlaneBits23, vkeys23, vbitPos23, vbitmapBytes23, encodedByPair[1]);
    laneMask |= haoRetireEncodedPair(
        primaryHashTable, vlaneBits45, vkeys45, vbitPos45, vbitmapBytes45, encodedByPair[2]);
    laneMask |= haoRetireEncodedPair(
        primaryHashTable, vlaneBits67, vkeys67, vbitPos67, vbitmapBytes67, encodedByPair[3]);

    HAO_STATS_ADD(primaryActiveLanes, (u32)__builtin_popcount(laneMask));
    return laneMask;
}

static really_inline
int haoRunRaw32(
    u64a bextMask, u32 l2EntryCount, const u8 *primaryBitmap,
    const u32 *primaryHashTable,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control,
    size_t blockStart, svuint8_t vlo, svuint8_t vhi,
    svuint32_t vlaneBits01, svuint32_t vlaneBits23,
    svuint32_t vlaneBits45, svuint32_t vlaneBits67) {
    u32 encodedByPair[4][8];
    const u32 laneMask = haoCollectRawEncoded(
        primaryBitmap, primaryHashTable, bextMask, vlo, vhi,
        vlaneBits01, vlaneBits23, vlaneBits45, vlaneBits67,
        encodedByPair);
    const u32 fullValidBlock =
        blockStart + a->len_history >= HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U;

    if (likely(laneMask)) {
        if (haoRunEncodedLanes(
                l2EntryCount, l2CheckTable, l2MetaTable, ruleMeta, a, control,
                blockStart, fullValidBlock, encodedByPair, laneMask, vlo, vhi) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static really_inline
u32 haoCollectRawTailEncoded(const u8 *primaryBitmap,
                             const u32 *primaryHashTable, u64a bextMask,
                             u32 blockLaneCount, svuint8_t vlo, svuint8_t vhi,
                             svuint32_t vlaneIds01,
                             svuint32_t vlaneIds23,
                             svuint32_t vlaneIds45,
                             svuint32_t vlaneIds67,
                             svuint32_t vlaneBits01,
                             svuint32_t vlaneBits23,
                             svuint32_t vlaneBits45,
                             svuint32_t vlaneBits67,
                             u32 encodedByPair[4][8]) {
    u32 laneMask = 0;
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

    const svuint32_t vkeys01 = haoRawKeyPair(vrow0, vrow1, bextMask);
    const svuint32_t vkeys23 = haoRawKeyPair(vrow2, vrow3, bextMask);
    const svuint32_t vkeys45 = haoRawKeyPair(vrow4, vrow5, bextMask);
    const svuint32_t vkeys67 = haoRawKeyPair(vrow6, vrow7, bextMask);

    {
        const svbool_t pg32 = svptrue_b32();
        const svbool_t pvalid01 =
            svcmplt_n_u32(pg32, vlaneIds01, blockLaneCount);
        const svbool_t pvalid23 =
            svcmplt_n_u32(pg32, vlaneIds23, blockLaneCount);
        const svbool_t pvalid45 =
            svcmplt_n_u32(pg32, vlaneIds45, blockLaneCount);
        const svbool_t pvalid67 =
            svcmplt_n_u32(pg32, vlaneIds67, blockLaneCount);

        haoPrepRawKeysPred(primaryBitmap, pvalid01, vkeys01,
                                 &vbitPos01, &vbitmapBytes01);
        haoPrepRawKeysPred(primaryBitmap, pvalid23, vkeys23,
                                 &vbitPos23, &vbitmapBytes23);
        haoPrepRawKeysPred(primaryBitmap, pvalid45, vkeys45,
                                 &vbitPos45, &vbitmapBytes45);
        haoPrepRawKeysPred(primaryBitmap, pvalid67, vkeys67,
                                 &vbitPos67, &vbitmapBytes67);

        laneMask |= haoRetireEncodedPairPred(
            primaryHashTable, pvalid01, vlaneBits01, vkeys01, vbitPos01,
            vbitmapBytes01, encodedByPair[0]);
        laneMask |= haoRetireEncodedPairPred(
            primaryHashTable, pvalid23, vlaneBits23, vkeys23, vbitPos23,
            vbitmapBytes23, encodedByPair[1]);
        laneMask |= haoRetireEncodedPairPred(
            primaryHashTable, pvalid45, vlaneBits45, vkeys45, vbitPos45,
            vbitmapBytes45, encodedByPair[2]);
        laneMask |= haoRetireEncodedPairPred(
            primaryHashTable, pvalid67, vlaneBits67, vkeys67, vbitPos67,
            vbitmapBytes67, encodedByPair[3]);
    }

    HAO_STATS_ADD(primaryActiveLanes, (u32)__builtin_popcount(laneMask));
    return laneMask;
}

static really_inline
int haoRunRawTailVec(
    u64a bextMask, u32 l2EntryCount, const u8 *primaryBitmap,
    const u32 *primaryHashTable,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control,
    size_t blockStart, u32 blockLaneCount, svuint8_t vlo, svuint8_t vhi,
    svuint32_t vlaneIds01, svuint32_t vlaneIds23,
    svuint32_t vlaneIds45, svuint32_t vlaneIds67,
    svuint32_t vlaneBits01, svuint32_t vlaneBits23,
    svuint32_t vlaneBits45, svuint32_t vlaneBits67) {
    u32 encodedByPair[4][8];
    const u32 laneMask = haoCollectRawTailEncoded(
        primaryBitmap, primaryHashTable, bextMask, blockLaneCount,
        vlo, vhi, vlaneIds01, vlaneIds23, vlaneIds45, vlaneIds67,
        vlaneBits01, vlaneBits23, vlaneBits45, vlaneBits67,
        encodedByPair);
    const u32 fullValidBlock =
        blockStart + a->len_history >= HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U;

    if (likely(laneMask)) {
        if (haoRunEncodedLanes(
                l2EntryCount, l2CheckTable, l2MetaTable, ruleMeta, a, control,
                blockStart, fullValidBlock, encodedByPair, laneMask, vlo, vhi) ==
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
    u64a bextMask, u32 l2EntryCount, const u8 *primaryBitmap,
    const u32 *primaryHashTable,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, size_t blockStart, u32 blockLaneCount) {
    u32 lane;
    u32 activeCount = 0;
    const svbool_t pg64 =
        svwhilelt_b64((u32)0, HAO_RUNTIME_RULE_SLOTS_PER_ENTRY);
    const svuint64_t vslotBits =
        svlsl_u64_x(pg64, svdup_n_u64(1U), svindex_u64(0, 1));

    for (lane = 0; lane < blockLaneCount; lane++) {
        struct HAOPositionContext ctx;
        const size_t endPos = blockStart + lane;
        const u64a rawWord = haoBuildRawWordScalar(a, endPos);
        const u32 key = (u32)pext64(rawWord, bextMask);

        if (haoRawBitmapHitScalar(primaryBitmap, key)) {
            const u32 encoded = primaryHashTable[key];
            const u32 validMask8 = haoComputeValidMask8(a, endPos);
            const svuint64_t laneData = svdup_n_u64(rawWord);

            if (encoded) {
                activeCount++;
                ctx.endPos = endPos;
                ctx.validMask32 = validMask8 * 0x01010101U;
                if (haoRunL2Range(
                        l2EntryCount, l2CheckTable, l2MetaTable, ruleMeta, a,
                        control, &ctx, laneData, vslotBits, encoded) ==
                    HWLM_TERMINATED) {
                    HAO_STATS_ADD(primaryActiveLanes, activeCount);
                    return HWLM_TERMINATED;
                }
            }
        }

    }

    HAO_STATS_ADD(primaryActiveLanes, activeCount);
    return HWLM_SUCCESS;
}

#endif

static int haoRunBatchBlob(const struct HAORuntimeHeader *hdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t *control) {
    const u8 *primaryBitmap;
    const u32 *primaryHashTable;
    const struct HAORuntimeL2Check *l2CheckTable;
    const struct HAORuntimeL2Meta *l2MetaTable;
    const struct HAORuntimeRuleMeta *ruleMeta;
    u64a bextMask;
    u32 l2EntryCount;
    size_t i = a->start_offset;

    haoRefreshStatsMode();
    HAO_STATS_ADD(scanCalls, 1);
    HAO_STATS_ADD(scanInputBytes, a->len - a->start_offset);

    primaryBitmap = (const u8 *)hdr + hdr->primaryBitmapOffset;
    primaryHashTable =
        (const u32 *)((const u8 *)hdr + hdr->primaryOffset);
    l2CheckTable = (const struct HAORuntimeL2Check *)((const u8 *)hdr +
                                                      hdr->l2CheckOffset);
    l2MetaTable = (const struct HAORuntimeL2Meta *)((const u8 *)hdr +
                                                    hdr->l2MetaOffset);
    ruleMeta = (const struct HAORuntimeRuleMeta *)((const u8 *)hdr +
                                                   hdr->ruleMetaOffset);
    bextMask = hdr->bextMask;
    l2EntryCount = hdr->l2EntryCount;

    const svuint32_t vlaneBits01 = haoRawLaneBits(0U);
    const svuint32_t vlaneBits23 = haoRawLaneBits(2U);
    const svuint32_t vlaneBits45 = haoRawLaneBits(4U);
    const svuint32_t vlaneBits67 = haoRawLaneBits(6U);

    svuint8_t rawPrev = svdup_n_u8(0);
    haoLoadRawPrev32(a, i, &rawPrev);
    for ( ; i + HAO_RUNTIME_BLOCK_BYTES <= a->len; i += HAO_RUNTIME_BLOCK_BYTES) {
        svuint8_t rawCurr;

        HAO_PREFETCH_R(a->buf + i + HAO_PREFETCH_INPUT_DISTANCE);

        haoLoadRawCurr32(a, i, HAO_RUNTIME_BLOCK_BYTES, &rawCurr);

        HAO_STATS_ADD(blockCalls, 1);
        HAO_STATS_ADD(blockLanes, HAO_BATCH_MAX_WIDTH);
        HAO_STATS_ADD(primaryProbeLanes, HAO_BATCH_MAX_WIDTH);
        int rt = haoRunRaw32(bextMask, l2EntryCount, primaryBitmap,
                                primaryHashTable, l2CheckTable,
                                l2MetaTable, ruleMeta, a, control, i,
                                rawPrev, rawCurr, vlaneBits01, vlaneBits23,
                                vlaneBits45, vlaneBits67);
        if (likely(rt == HWLM_TERMINATED)) {
            return HWLM_TERMINATED;
        }
        rawPrev = rawCurr;
    }

    if (i < a->len) {
        svuint8_t rawCurr;
        const u32 blockLaneCount = (u32)(a->len - i);
        haoLoadRawPrev32(a, i, &rawPrev);
        haoLoadRawCurr32(a, i, blockLaneCount, &rawCurr);

        HAO_STATS_ADD(blockCalls, 1);
        HAO_STATS_ADD(blockLanes, blockLaneCount);
        HAO_STATS_ADD(primaryProbeLanes, blockLaneCount);
        const svuint32_t vlaneIds01 = haoRawLaneIds(0U);
        const svuint32_t vlaneIds23 = haoRawLaneIds(2U);
        const svuint32_t vlaneIds45 = haoRawLaneIds(4U);
        const svuint32_t vlaneIds67 = haoRawLaneIds(6U);
        int rt = haoRunRawTailVec(bextMask, l2EntryCount, primaryBitmap,
                                    primaryHashTable, l2CheckTable,
                                    l2MetaTable, ruleMeta, a, control, i,
                                    blockLaneCount, rawPrev, rawCurr,
                                    vlaneIds01, vlaneIds23, vlaneIds45,
                                    vlaneIds67, vlaneBits01, vlaneBits23,
                                    vlaneBits45, vlaneBits67);
        if (likely(rt == HWLM_TERMINATED)) {
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
        haoPrepProbe(primaryIdx, laneCount,
                                                  &probe);

        {
            u32 groupedActiveMask = 0;
            if (haoProbeBits(bitmap, bitmapSize, &probe,
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
