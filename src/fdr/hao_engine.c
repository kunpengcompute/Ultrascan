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

#ifndef HAO_L2_FP_TOPN
#define HAO_L2_FP_TOPN 20U
#endif

#define HAO_L2_BUCKET_RULE_ID_LIMIT 16U

struct HAOL2EntryHotStat {
    const void *table;
    u32 offset;
    u32 ruleCount;
    u32 ruleIds[HAO_RUNTIME_RULE_SLOTS_PER_ENTRY];
    u64a visits;
    u64a misses;
    u64a entryHits;
    u64a slotHits;
};

struct HAOL2BucketHotStat {
    const void *table;
    u32 offset;
    u32 count;
    u32 ruleIdCount;
    u32 ruleIds[HAO_L2_BUCKET_RULE_ID_LIMIT];
    u32 ruleOverflow;
    u64a visits;
    u64a noReports;
    u64a reports;
    u64a entriesVisited;
};

static struct HAOL2EntryHotStat *g_haoL2EntryStats;
static u32 g_haoL2EntryStatsCap;
static u32 g_haoL2EntryStatsCount;
static struct HAOL2BucketHotStat *g_haoL2BucketStats;
static u32 g_haoL2BucketStatsCap;
static u32 g_haoL2BucketStatsCount;
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

struct HAOHashRuntime {
    u32 mode;
    u32 keyMask;
    u64a bextMask;
    const u16 *l15Tags;
    const u64a *l15TagMasks;
    u32 l15TagCount;
    u32 l15MaskCount;
    u16 dotVector[HAO_RUNTIME_DOT_VECTOR_LANES];
};

struct HAODotGroupRuntime {
    struct HAOHashRuntime hash;
    u32 l2EntryCount;
    u32 knownBytes;
    const u8 *primaryBitmap;
    const u32 *primaryHashTable;
    const struct HAORuntimeL2Check *l2CheckTable;
    const struct HAORuntimeL2Meta *l2MetaTable;
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

static u32 haoStatsHashKey(const void *table, u32 offset) {
    uintptr_t v = (uintptr_t)table;

    v ^= (uintptr_t)offset * (uintptr_t)0x9e3779b97f4a7c15ULL;
    v ^= v >> 33;
    v *= (uintptr_t)0xff51afd7ed558ccdULL;
    v ^= v >> 33;
    return (u32)v;
}

static int haoStatsGrowEntries(void) {
    u32 oldCap = g_haoL2EntryStatsCap;
    u32 newCap = oldCap ? oldCap << 1 : 65536U;
    struct HAOL2EntryHotStat *oldStats = g_haoL2EntryStats;
    struct HAOL2EntryHotStat *newStats;
    u32 i;

    newStats = (struct HAOL2EntryHotStat *)calloc(
        newCap, sizeof(struct HAOL2EntryHotStat));
    if (!newStats) {
        return 0;
    }

    g_haoL2EntryStats = newStats;
    g_haoL2EntryStatsCap = newCap;
    g_haoL2EntryStatsCount = 0;
    for (i = 0; i < oldCap; i++) {
        struct HAOL2EntryHotStat item;
        u32 pos;

        if (!oldStats[i].table) {
            continue;
        }
        item = oldStats[i];
        pos = haoStatsHashKey(item.table, item.offset) & (newCap - 1U);
        while (newStats[pos].table) {
            pos = (pos + 1U) & (newCap - 1U);
        }
        newStats[pos] = item;
        g_haoL2EntryStatsCount++;
    }
    free(oldStats);
    return 1;
}

static int haoStatsGrowBuckets(void) {
    u32 oldCap = g_haoL2BucketStatsCap;
    u32 newCap = oldCap ? oldCap << 1 : 32768U;
    struct HAOL2BucketHotStat *oldStats = g_haoL2BucketStats;
    struct HAOL2BucketHotStat *newStats;
    u32 i;

    newStats = (struct HAOL2BucketHotStat *)calloc(
        newCap, sizeof(struct HAOL2BucketHotStat));
    if (!newStats) {
        return 0;
    }

    g_haoL2BucketStats = newStats;
    g_haoL2BucketStatsCap = newCap;
    g_haoL2BucketStatsCount = 0;
    for (i = 0; i < oldCap; i++) {
        struct HAOL2BucketHotStat item;
        u32 pos;

        if (!oldStats[i].table) {
            continue;
        }
        item = oldStats[i];
        pos = haoStatsHashKey(item.table, item.offset) & (newCap - 1U);
        while (newStats[pos].table) {
            pos = (pos + 1U) & (newCap - 1U);
        }
        newStats[pos] = item;
        g_haoL2BucketStatsCount++;
    }
    free(oldStats);
    return 1;
}

static struct HAOL2EntryHotStat *haoStatsEntryFor(
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, u32 offset) {
    u32 pos;
    struct HAOL2EntryHotStat *rec;

    if (!g_haoStatsActive || !l2CheckTable || !l2MetaTable || !ruleMeta ||
        !offset) {
        return NULL;
    }
    if (!g_haoL2EntryStatsCap ||
        (g_haoL2EntryStatsCount + 1U) * 10U >=
            g_haoL2EntryStatsCap * 7U) {
        if (!haoStatsGrowEntries()) {
            return NULL;
        }
    }

    pos = haoStatsHashKey(l2CheckTable, offset) &
          (g_haoL2EntryStatsCap - 1U);
    while (g_haoL2EntryStats[pos].table &&
           (g_haoL2EntryStats[pos].table != l2CheckTable ||
            g_haoL2EntryStats[pos].offset != offset)) {
        pos = (pos + 1U) & (g_haoL2EntryStatsCap - 1U);
    }

    rec = &g_haoL2EntryStats[pos];
    if (!rec->table) {
        u32 slot;

        rec->table = l2CheckTable;
        rec->offset = offset;
        for (slot = 0; slot < HAO_RUNTIME_RULE_SLOTS_PER_ENTRY; slot++) {
            const u32 ridx = l2MetaTable[offset].ruleIndex[slot];

            if (ridx == HAO_RUNTIME_INVALID_RULE_INDEX) {
                continue;
            }
            rec->ruleIds[rec->ruleCount++] = ruleMeta[ridx].id;
        }
        g_haoL2EntryStatsCount++;
    }
    return rec;
}

static struct HAOL2BucketHotStat *haoStatsBucketFor(
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, u32 offset, u32 count) {
    u32 pos;
    struct HAOL2BucketHotStat *rec;

    if (!g_haoStatsActive || !l2CheckTable || !l2MetaTable || !ruleMeta ||
        !offset || !count) {
        return NULL;
    }
    if (!g_haoL2BucketStatsCap ||
        (g_haoL2BucketStatsCount + 1U) * 10U >=
            g_haoL2BucketStatsCap * 7U) {
        if (!haoStatsGrowBuckets()) {
            return NULL;
        }
    }

    pos = haoStatsHashKey(l2CheckTable, offset) &
          (g_haoL2BucketStatsCap - 1U);
    while (g_haoL2BucketStats[pos].table &&
           (g_haoL2BucketStats[pos].table != l2CheckTable ||
            g_haoL2BucketStats[pos].offset != offset)) {
        pos = (pos + 1U) & (g_haoL2BucketStatsCap - 1U);
    }

    rec = &g_haoL2BucketStats[pos];
    if (!rec->table) {
        u32 n;

        rec->table = l2CheckTable;
        rec->offset = offset;
        rec->count = count;
        for (n = 0; n < count; n++) {
            u32 slot;
            const struct HAORuntimeL2Meta *meta = &l2MetaTable[offset + n];

            for (slot = 0; slot < HAO_RUNTIME_RULE_SLOTS_PER_ENTRY; slot++) {
                const u32 ridx = meta->ruleIndex[slot];

                if (ridx == HAO_RUNTIME_INVALID_RULE_INDEX) {
                    continue;
                }
                if (rec->ruleIdCount < HAO_L2_BUCKET_RULE_ID_LIMIT) {
                    rec->ruleIds[rec->ruleIdCount++] = ruleMeta[ridx].id;
                } else {
                    rec->ruleOverflow = 1;
                }
            }
        }
        g_haoL2BucketStatsCount++;
    }
    return rec;
}

static void haoStatsObserveL2Entry(
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, u32 offset, u32 matchMask) {
    HAO_STATS_IF_ENABLED({
        struct HAOL2EntryHotStat *rec =
            haoStatsEntryFor(l2CheckTable, l2MetaTable, ruleMeta, offset);

        if (rec) {
            rec->visits++;
            if (matchMask) {
                rec->entryHits++;
                rec->slotHits += popcount32(matchMask);
            } else {
                rec->misses++;
            }
        }
    });
}

static void haoStatsObserveL2Bucket(
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, u32 offset, u32 count,
    u32 visitedCount, int anyReport) {
    HAO_STATS_IF_ENABLED({
        struct HAOL2BucketHotStat *rec =
            haoStatsBucketFor(l2CheckTable, l2MetaTable, ruleMeta,
                              offset, count);

        if (rec) {
            rec->visits++;
            rec->entriesVisited += visitedCount;
            if (anyReport) {
                rec->reports++;
            } else {
                rec->noReports++;
            }
        }
    });
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

static int haoCmpEntryMissDesc(const void *a, const void *b) {
    const struct HAOL2EntryHotStat * const *pa =
        (const struct HAOL2EntryHotStat * const *)a;
    const struct HAOL2EntryHotStat * const *pb =
        (const struct HAOL2EntryHotStat * const *)b;
    const struct HAOL2EntryHotStat *ra = *pa;
    const struct HAOL2EntryHotStat *rb = *pb;

    if (ra->misses != rb->misses) {
        return ra->misses < rb->misses ? 1 : -1;
    }
    if (ra->visits != rb->visits) {
        return ra->visits < rb->visits ? 1 : -1;
    }
    return ra->offset < rb->offset ? -1 : (ra->offset > rb->offset);
}

static int haoCmpBucketNoReportDesc(const void *a, const void *b) {
    const struct HAOL2BucketHotStat * const *pa =
        (const struct HAOL2BucketHotStat * const *)a;
    const struct HAOL2BucketHotStat * const *pb =
        (const struct HAOL2BucketHotStat * const *)b;
    const struct HAOL2BucketHotStat *ra = *pa;
    const struct HAOL2BucketHotStat *rb = *pb;

    if (ra->noReports != rb->noReports) {
        return ra->noReports < rb->noReports ? 1 : -1;
    }
    if (ra->visits != rb->visits) {
        return ra->visits < rb->visits ? 1 : -1;
    }
    return ra->offset < rb->offset ? -1 : (ra->offset > rb->offset);
}

static void haoStatsPrintRuleIds(const u32 *ruleIds, u32 count,
                                 u32 overflow) {
    u32 i;

    fputc('[', stderr);
    for (i = 0; i < count; i++) {
        if (i) {
            fputc(',', stderr);
        }
        fprintf(stderr, "%u", ruleIds[i]);
    }
    if (overflow) {
        if (count) {
            fputc(',', stderr);
        }
        fputs("...", stderr);
    }
    fputc(']', stderr);
}

static void haoDumpL2EntryTopN(u64a totalMisses) {
    struct HAOL2EntryHotStat **items;
    u32 i;
    u32 count = 0;
    u32 limit;

    if (!g_haoL2EntryStats || !g_haoL2EntryStatsCount) {
        return;
    }

    items = (struct HAOL2EntryHotStat **)malloc(
        sizeof(*items) * g_haoL2EntryStatsCount);
    if (!items) {
        return;
    }

    for (i = 0; i < g_haoL2EntryStatsCap; i++) {
        if (g_haoL2EntryStats[i].table && g_haoL2EntryStats[i].misses) {
            items[count++] = &g_haoL2EntryStats[i];
        }
    }
    if (!count) {
        free(items);
        return;
    }

    qsort(items, count, sizeof(*items), haoCmpEntryMissDesc);
    limit = count < HAO_L2_FP_TOPN ? count : HAO_L2_FP_TOPN;

    fprintf(stderr, "[HAO][L2-Entry-FP-TopN]\n");
    fprintf(stderr,
            "  rank table offset visits misses fpRate fpShare entryHits slotHits ruleIds\n");
    for (i = 0; i < limit; i++) {
        const struct HAOL2EntryHotStat *rec = items[i];

        fprintf(stderr,
                "  %u %p %u %llu %llu %.5f %.5f %llu %llu ",
                i + 1U, rec->table, rec->offset,
                (unsigned long long)rec->visits,
                (unsigned long long)rec->misses,
                haoStatsPct(rec->misses, rec->visits),
                haoStatsPct(rec->misses, totalMisses),
                (unsigned long long)rec->entryHits,
                (unsigned long long)rec->slotHits);
        haoStatsPrintRuleIds(rec->ruleIds, rec->ruleCount, 0);
        fputc('\n', stderr);
    }
    free(items);
}

static void haoDumpL2BucketTopN(u64a totalNoReports) {
    struct HAOL2BucketHotStat **items;
    u32 i;
    u32 count = 0;
    u32 limit;

    if (!g_haoL2BucketStats || !g_haoL2BucketStatsCount) {
        return;
    }

    items = (struct HAOL2BucketHotStat **)malloc(
        sizeof(*items) * g_haoL2BucketStatsCount);
    if (!items) {
        return;
    }

    for (i = 0; i < g_haoL2BucketStatsCap; i++) {
        if (g_haoL2BucketStats[i].table &&
            g_haoL2BucketStats[i].noReports) {
            items[count++] = &g_haoL2BucketStats[i];
        }
    }
    if (!count) {
        free(items);
        return;
    }

    qsort(items, count, sizeof(*items), haoCmpBucketNoReportDesc);
    limit = count < HAO_L2_FP_TOPN ? count : HAO_L2_FP_TOPN;

    fprintf(stderr, "[HAO][L2-Bucket-NoReport-TopN]\n");
    fprintf(stderr,
            "  rank table offset count visits noReports noReportRate noReportShare entriesVisited ruleIds\n");
    for (i = 0; i < limit; i++) {
        const struct HAOL2BucketHotStat *rec = items[i];

        fprintf(stderr,
                "  %u %p %u %u %llu %llu %.5f %.5f %llu ",
                i + 1U, rec->table, rec->offset, rec->count,
                (unsigned long long)rec->visits,
                (unsigned long long)rec->noReports,
                haoStatsPct(rec->noReports, rec->visits),
                haoStatsPct(rec->noReports, totalNoReports),
                (unsigned long long)rec->entriesVisited);
        haoStatsPrintRuleIds(rec->ruleIds, rec->ruleIdCount,
                             rec->ruleOverflow);
        fputc('\n', stderr);
    }
    free(items);
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
    HAO_STAT_FMT("primaryBitmapHitLanes(bitmap命中lane数)",   "%llu", (unsigned long long)g_haoStats.primaryBitmapHitLanes);
    HAO_STAT_FMT("bitmapHitPct(bitmap命中率)",                "%.5f",
        haoStatsPct(g_haoStats.primaryBitmapHitLanes, g_haoStats.primaryProbeLanes));
    HAO_STAT_FMT("primaryAliasRejects(bitmap别名拒绝数)",     "%llu", (unsigned long long)g_haoStats.primaryAliasRejects);
    HAO_STAT_FMT("primaryAliasRejectPct(bitmap别名拒绝率)",   "%.5f",
        haoStatsPct(g_haoStats.primaryAliasRejects, g_haoStats.primaryBitmapHitLanes));
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
    HAO_STAT_FMT("l15TagChecks",                             "%llu", (unsigned long long)g_haoStats.l15TagChecks);
    HAO_STAT_FMT("l15TagRejects",                            "%llu", (unsigned long long)g_haoStats.l15TagRejects);
    HAO_STAT_FMT("l15TagRejectPct",                          "%.5f",
        haoStatsPct(g_haoStats.l15TagRejects, g_haoStats.l15TagChecks));

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
    HAO_STAT_FMT("l2EntriesPerMiB(每MiB访问L2表项数)",        "%.5f",
        haoStatsPerMiB(g_haoStats.encodedEntriesVisited, g_haoStats.scanInputBytes));
    HAO_STAT_FMT("reportsPerMiB(每MiB报告次数)",              "%.5f",
        haoStatsPerMiB(g_haoStats.callbackReports, g_haoStats.scanInputBytes));

    haoDumpL2EntryTopN(l2EntryRejects);
    haoDumpL2BucketTopN(l2LaneNoReport);
}
#endif

/* Validate the HAO blob layout before entering execution. */
static int haoValidateLayout(const void *blob, u32 blobSize,
                             const struct HAORuntimeHeader **outHdr) {
    const struct HAORuntimeHeader *hdr;
    u32 keyBits;
    u32 hashMode;

    if (!blob || blobSize < sizeof(struct HAORuntimeHeader)) {
        return 0;
    }

    hdr = (const struct HAORuntimeHeader *)blob;
    if (hdr->magic != HAO_RUNTIME_MAGIC ||
        hdr->version != HAO_RUNTIME_VERSION) {
        return 0;
    }
    keyBits = haoRuntimeHeaderKeyBits(hdr);
    hashMode = haoRuntimeHeaderHashMode(hdr);

    if (!keyBits || !hdr->primaryCount || !hdr->l2EntryCount) {
        return 0;
    }
    if (keyBits > HAO_RUNTIME_MAX_SELECTORS) {
        return 0;
    }
    if (hashMode != HAO_RUNTIME_HASH_BEXT &&
        hashMode != HAO_RUNTIME_HASH_DOT &&
        hashMode != HAO_RUNTIME_HASH_DOT_GROUP) {
        return 0;
    }
    if (hashMode == HAO_RUNTIME_HASH_BEXT && !hdr->bextMask) {
        return 0;
    }
    if (hashMode == HAO_RUNTIME_HASH_DOT_GROUP &&
        (hdr->l15TagOffset || hdr->l15TagCount ||
         hdr->l15TagBits || hdr->l15TagOverlapBits ||
         hdr->l15MaskTableOffset || hdr->l15MaskCount)) {
        return 0;
    }
    if ((u64a)hdr->ruleMetaOffset + (u64a)hdr->ruleMetaCount *
            sizeof(struct HAORuntimeRuleMeta) >
        (u64a)blobSize) {
        return 0;
    }
    if (hashMode == HAO_RUNTIME_HASH_DOT_GROUP) {
        const struct HAORuntimeDotGroupDesc *groups;
        u32 i;

        if (!hdr->primaryBitmapOffset ||
            hdr->primaryCount > HAO_RUNTIME_DOT_GROUP_COUNT ||
            (u64a)hdr->primaryBitmapOffset +
                (u64a)hdr->primaryCount *
                    sizeof(struct HAORuntimeDotGroupDesc) >
                (u64a)blobSize) {
            return 0;
        }
        groups = (const struct HAORuntimeDotGroupDesc *)((const u8 *)hdr +
                                                         hdr->primaryBitmapOffset);
        for (i = 0; i < hdr->primaryCount; i++) {
            const struct HAORuntimeDotGroupDesc *g = &groups[i];
            if (!g->keyBits || g->keyBits > HAO_RUNTIME_MAX_SELECTORS ||
                !g->primaryCount || !g->l2EntryCount || !g->dotVector) {
                return 0;
            }
            if ((u64a)g->primaryBitmapOffset +
                    (u64a)g->primaryBitmapSize >
                (u64a)blobSize) {
                return 0;
            }
            if ((u64a)g->primaryOffset +
                    (u64a)g->primaryCount * sizeof(u32) >
                (u64a)blobSize) {
                return 0;
            }
            if (!g->l2CheckOffset || !g->l2MetaOffset ||
                (g->l2CheckOffset & (HAO_RUNTIME_L2_CHECK_ALIGN - 1U))) {
                return 0;
            }
            if ((u64a)g->l2CheckOffset +
                    (u64a)g->l2EntryCount *
                        sizeof(struct HAORuntimeL2Check) >
                (u64a)blobSize) {
                return 0;
            }
            if ((u64a)g->l2MetaOffset +
                    (u64a)g->l2EntryCount *
                        sizeof(struct HAORuntimeL2Meta) >
                (u64a)blobSize) {
                return 0;
            }
        }
        if (outHdr) {
            *outHdr = hdr;
        }
        return 1;
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
#if HAO_L15_TAG
    if (hdr->l15TagBits || hdr->l15TagOffset ||
        hdr->l15TagCount || hdr->l15TagOverlapBits ||
        hdr->l15MaskTableOffset || hdr->l15MaskCount) {
        const u64a *masks;

        if (hashMode != HAO_RUNTIME_HASH_BEXT ||
            hdr->l15TagBits != HAO_L15_TAG_BITS ||
            hdr->l15TagOverlapBits > HAO_L15_TAG_MAX_OVERLAP_BITS ||
            hdr->l15TagCount != hdr->l2EntryCount ||
            !hdr->l15MaskCount ||
            hdr->l15MaskCount > HAO_L15_TAG_MAX_MASKS ||
            !hdr->l15TagOffset ||
            !hdr->l15MaskTableOffset ||
            (hdr->l15TagOffset & (sizeof(u16) - 1U)) ||
            (hdr->l15MaskTableOffset & (sizeof(u64a) - 1U)) ||
            (u64a)hdr->l15TagOffset +
                    (u64a)hdr->l15TagCount * sizeof(u16) >
                (u64a)blobSize ||
            (u64a)hdr->l15MaskTableOffset +
                    (u64a)hdr->l15MaskCount * sizeof(u64a) >
                (u64a)blobSize) {
            return 0;
        }
        masks = (const u64a *)((const u8 *)hdr + hdr->l15MaskTableOffset);
        for (u32 i = 0; i < hdr->l15MaskCount; i++) {
            const u32 overlap = popcount64(masks[i] & hdr->bextMask);
            if (popcount64(masks[i]) != HAO_L15_TAG_BITS ||
                overlap > HAO_L15_TAG_MAX_OVERLAP_BITS ||
                HAO_L15_TAG_BITS - overlap < HAO_L15_TAG_MIN_NEW_BITS) {
                return 0;
            }
        }
    }
#else
    if (hdr->l15TagBits || hdr->l15TagOffset ||
        hdr->l15TagCount || hdr->l15TagOverlapBits ||
        hdr->l15MaskTableOffset || hdr->l15MaskCount) {
        return 0;
    }
#endif
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

static really_inline
void haoBuildHashRuntime(const struct HAORuntimeHeader *hdr,
                         struct HAOHashRuntime *hash) {
    u32 i;

    hash->mode = haoRuntimeHeaderHashMode(hdr);
    hash->keyMask = haoPackedKeyMask(haoRuntimeHeaderKeyBits(hdr));
    hash->bextMask = hdr->bextMask;
    hash->l15Tags = NULL;
    hash->l15TagMasks = NULL;
    hash->l15TagCount = hdr->l15TagCount;
    hash->l15MaskCount = hdr->l15MaskCount;
    for (i = 0; i < HAO_RUNTIME_DOT_VECTOR_LANES; i++) {
        hash->dotVector[i] = haoRuntimeHeaderDotVectorLane(hdr, i);
    }
}

static really_inline
void haoAttachL15Tags(const struct HAORuntimeHeader *hdr,
                      struct HAOHashRuntime *hash) {
#if HAO_L15_TAG
    if (hdr->l15TagOffset && hdr->l15MaskTableOffset &&
        hdr->l15TagCount && hdr->l15MaskCount) {
        hash->l15Tags = (const u16 *)((const u8 *)hdr + hdr->l15TagOffset);
        hash->l15TagMasks =
            (const u64a *)((const u8 *)hdr + hdr->l15MaskTableOffset);
    }
#else
    (void)hdr;
    (void)hash;
#endif
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
    summary->keyBits = haoRuntimeHeaderKeyBits(hdr);
    summary->primaryCount = hdr->primaryCount;
    summary->primaryBitmapSize = hdr->primaryBitmapSize;
    summary->l2EntryCount = hdr->l2EntryCount;
    summary->ruleMetaCount = hdr->ruleMetaCount;

    if (haoRuntimeHeaderHashMode(hdr) == HAO_RUNTIME_HASH_DOT_GROUP) {
        const struct HAORuntimeDotGroupDesc *groups =
            (const struct HAORuntimeDotGroupDesc *)((const u8 *)hdr +
                                                    hdr->primaryBitmapOffset);
        u32 g;

        summary->primaryCount = 0;
        summary->primaryBitmapSize = 0;
        summary->l2EntryCount = 0;
        for (g = 0; g < hdr->primaryCount; g++) {
            const u32 *groupPrimary =
                (const u32 *)((const u8 *)hdr + groups[g].primaryOffset);
            const struct HAORuntimeL2Meta *groupL2Meta =
                (const struct HAORuntimeL2Meta *)((const u8 *)hdr +
                                                  groups[g].l2MetaOffset);
            u32 n;

            summary->primaryCount += groups[g].primaryCount;
            summary->primaryBitmapSize += groups[g].primaryBitmapSize;
            summary->l2EntryCount += groups[g].l2EntryCount;
            for (n = 0; n < groups[g].primaryCount; n++) {
                const u32 encoded = groupPrimary[n];
                if (!encoded) {
                    continue;
                }
                {
                    const u32 entryCount =
                        encoded >> HAO_RUNTIME_L1_COUNT_SHIFT;
                    summary->nonEmptyPrimary++;
                    if (entryCount > 1) {
                        summary->multiEntryBucketCount++;
                    }
                    if (summary->maxEntriesPerKey < entryCount) {
                        summary->maxEntriesPerKey = entryCount;
                    }
                }
            }
            for (n = 0; n < groups[g].l2EntryCount; n++) {
                summary->totalRulesInL2 +=
                    haoL2MetaRuleCount(&groupL2Meta[n]);
            }
        }
        return;
    }

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
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable, u32 l2Offset,
    int *anyReport
#endif
    ) {
    HAO_STATS_ADD(verifierCalls, 1);
    u32 matchMask = haoL2MatchSve(check, meta, ctx, laneData, vslotBits);
#if HAO_ENABLE_RUNTIME_STATS
    haoStatsObserveL2Entry(l2CheckTable, l2MetaTable, ruleMeta,
                           l2Offset, matchMask);
#endif
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
int haoL15TagRejectsEntry(const struct HAOHashRuntime *hash, u32 offset,
                          svuint64_t laneData) {
#if HAO_L15_TAG
    u16 stored;
    u32 maskId;
    u64a mask;
    u32 tag;

    if (unlikely(!hash->l15Tags || !hash->l15TagMasks ||
                 offset >= hash->l15TagCount)) {
        return 0;
    }

    stored = hash->l15Tags[offset];
    if (likely(!(stored & HAO_L15_TAG_VALID))) {
        return 0;
    }

    maskId = (stored & HAO_L15_TAG_MASK_ID_MASK) >>
             HAO_L15_TAG_MASK_ID_SHIFT;

    mask = hash->l15TagMasks[maskId];

    HAO_STATS_ADD(l15TagChecks, 1);
    const svbool_t pgOne64 = svptrue_pat_b64(SV_VL1);
    const svuint64_t vtag =
        svbext_n_u64(laneData, (uint64_t)mask);
    tag = (u32)svlastb_u64(pgOne64, vtag) & HAO_L15_TAG_VALUE_MASK;
    if (tag == (stored & HAO_L15_TAG_VALUE_MASK)) {
        return 0;
    }

    HAO_STATS_ADD(l15TagRejects, 1);
    return 1;
#else
    (void)hash;
    (void)offset;
    (void)laneData;
    return 0;
#endif
}

static really_inline
int haoRunL2Range(
    const struct HAOHashRuntime *hash,
    u32 l2EntryCount,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta,
    const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct HAOPositionContext *ctx,
    svuint64_t laneData, svuint64_t vslotBits, u32 encoded) {
    u32 offset = encoded & HAO_RUNTIME_L1_OFFSET_MASK;
    u32 count = encoded >> HAO_RUNTIME_L1_COUNT_SHIFT;
    u32 lastMatchId = HAO_RUNTIME_INVALID_RULE_INDEX;

#if !HAO_L2_COUNT1_FAST
    (void)hash;
#endif
#if !HAO_ENABLE_RUNTIME_STATS
    (void)l2EntryCount;
#if HAO_L2_COUNT1_FAST
    if (likely(count == 1U)) {
        if (unlikely(haoL15TagRejectsEntry(hash, offset, laneData))) {
            return HWLM_SUCCESS;
        }
        HAO_STATS_ADD(encodedRangeCalls, 1);
        return haoProcessL2Entry(
            &l2CheckTable[offset], &l2MetaTable[offset], ruleMeta, a, control,
            ctx, laneData, vslotBits, &lastMatchId);
    }
#endif

    HAO_STATS_ADD(encodedRangeCalls, 1);
    for (u32 n = 0; n < count; n++) {
        const u32 off = offset + n;
        int rv;

        if (unlikely(haoL15TagRejectsEntry(hash, off, laneData))) {
            continue;
        }
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
    int rangeCounted = 0;
#if HAO_L2_COUNT1_FAST
    if (count == 1U) {
        int rv;

        if (!offset || offset >= l2EntryCount) {
            haoStatsObserveRangeShape(0, 0);
            return HWLM_SUCCESS;
        }

        if (haoL15TagRejectsEntry(hash, offset, laneData)) {
            return HWLM_SUCCESS;
        }

        HAO_STATS_ADD(encodedRangeCalls, 1);
        HAO_STATS_IF_ENABLED({
            visitedCount = 1;
            bucketRuleCount = haoL2MetaRuleCount(&l2MetaTable[offset]);
        });
        HAO_STATS_ADD(encodedEntriesVisited, 1);

        rv = haoProcessL2Entry(
            &l2CheckTable[offset], &l2MetaTable[offset], ruleMeta,
            a, control, ctx, laneData, vslotBits, &lastMatchId,
            l2CheckTable, l2MetaTable, offset,
            &anyReport);
        haoStatsObserveL2Bucket(l2CheckTable, l2MetaTable, ruleMeta,
                                offset, count, visitedCount, anyReport);
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

        if (haoL15TagRejectsEntry(hash, off, laneData)) {
            continue;
        }

        if (!rangeCounted) {
            HAO_STATS_ADD(encodedRangeCalls, 1);
            rangeCounted = 1;
        }

        HAO_STATS_IF_ENABLED({
            visitedCount++;
            bucketRuleCount += haoL2MetaRuleCount(&l2MetaTable[off]);
        });
        HAO_STATS_ADD(encodedEntriesVisited, 1);

        rv = haoProcessL2Entry(
            &l2CheckTable[off], &l2MetaTable[off], ruleMeta, a, control, ctx,
            laneData, vslotBits, &lastMatchId,
            l2CheckTable, l2MetaTable, off, &anyReport);
        if (rv == HWLM_TERMINATED) {
            haoStatsObserveL2Bucket(l2CheckTable, l2MetaTable, ruleMeta,
                                    offset, count, visitedCount, 1);
            haoStatsObserveRangeShape(visitedCount, bucketRuleCount);
            HAO_STATS_ADD(encodedRangeReportCalls, 1);
            return HWLM_TERMINATED;
        }
    }

    if (rangeCounted) {
        haoStatsObserveL2Bucket(l2CheckTable, l2MetaTable, ruleMeta,
                                offset, count, visitedCount, anyReport);
        haoStatsObserveRangeShape(visitedCount, bucketRuleCount);
        HAO_STATS_ADD(encodedRangeReportCalls, anyReport ? 1 : 0);
    }
#endif

    return HWLM_SUCCESS;
}

#if defined(HS_BUILD_HAVE_SVEBITPERM) && defined(__ARM_FEATURE_SVE2_BITPERM)
static really_inline
int haoRunRawTailScalar(
    const struct HAOHashRuntime *hash, u32 l2EntryCount,
    const u8 *primaryBitmap,
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
    struct HAOHashRuntime hash;
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
    haoBuildHashRuntime(hdr, &hash);
    haoAttachL15Tags(hdr, &hash);

#if defined(HS_BUILD_HAVE_SVEBITPERM) && defined(__ARM_FEATURE_SVE2_BITPERM)
    for (i = a->start_offset; i < a->len; i++) {
        HAO_STATS_ADD(primaryProbeLanes, 1);
        if (haoRunRawTailScalar(&hash, hdr->l2EntryCount, primaryBitmap,
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
    (void)hash;
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
svuint16_t haoLoadDotVector(const struct HAOHashRuntime *hash) {
    const svbool_t pg16 = svptrue_b16();
    u16 coeffs[HAO_BATCH_MAX_WIDTH / 2U];
    u32 i;

    for (i = 0; i < HAO_BATCH_MAX_WIDTH / 2U; i++) {
        coeffs[i] = hash->dotVector[i & (HAO_RUNTIME_DOT_VECTOR_LANES - 1U)];
    }
    return svld1_u16(pg16, coeffs);
}

static really_inline
void haoBuildDotGroupRuntime(const struct HAORuntimeHeader *hdr,
                             const struct HAORuntimeDotGroupDesc *desc,
                             struct HAODotGroupRuntime *group) {
    u32 i;

    group->hash.mode = HAO_RUNTIME_HASH_DOT;
    group->hash.keyMask = haoPackedKeyMask(desc->keyBits);
    group->hash.bextMask = 0;
    group->hash.l15Tags = NULL;
    group->hash.l15TagMasks = NULL;
    group->hash.l15TagCount = 0;
    group->hash.l15MaskCount = 0;
    for (i = 0; i < HAO_RUNTIME_DOT_VECTOR_LANES; i++) {
        group->hash.dotVector[i] = (u16)(desc->dotVector >> (i * 16U));
    }
    group->l2EntryCount = desc->l2EntryCount;
    group->knownBytes = desc->knownBytes;
    group->primaryBitmap = (const u8 *)hdr + desc->primaryBitmapOffset;
    group->primaryHashTable =
        (const u32 *)((const u8 *)hdr + desc->primaryOffset);
    group->l2CheckTable =
        (const struct HAORuntimeL2Check *)((const u8 *)hdr +
                                           desc->l2CheckOffset);
    group->l2MetaTable =
        (const struct HAORuntimeL2Meta *)((const u8 *)hdr +
                                          desc->l2MetaOffset);
}

static really_inline
int haoDotGroupKnownBytesToIndex(u32 knownBytes) {
    switch (knownBytes) {
    case 2U:
        return 0;
    case 4U:
        return 1;
    case 6U:
        return 2;
    case 8U:
        return 3;
    default:
        return -1;
    }
}

static
int haoDotGroupBuildCombinedCoeffs(const struct HAODotGroupRuntime *groups,
                                   u32 groupCount,
                                   u16 coeffs[HAO_RUNTIME_DOT_VECTOR_LANES]) {
    u32 g;

    if (!groups || !coeffs || !groupCount ||
        groupCount > HAO_RUNTIME_DOT_GROUP_COUNT) {
        return 0;
    }

    memset(coeffs, 0, sizeof(u16) * HAO_RUNTIME_DOT_VECTOR_LANES);
    for (g = 0; g < groupCount; g++) {
        const int groupIndex =
            haoDotGroupKnownBytesToIndex(groups[g].knownBytes);
        const u32 firstKnownByte =
            HAO_RUNTIME_BYTES_PER_RULE_SLOT - groups[g].knownBytes;
        u32 lane;

        if (groupIndex < 0) {
            return 0;
        }

        for (lane = 0; lane < HAO_RUNTIME_DOT_VECTOR_LANES; lane++) {
            const u32 byteBase = lane * 2U;
            const int laneKnown = byteBase >= firstKnownByte;
            const u16 coeff = groups[g].hash.dotVector[lane];

            if (!laneKnown) {
                if (coeff) {
                    return 0;
                }
                continue;
            }

            if (!coeff) {
                return 0;
            }
            if (coeffs[lane] && coeffs[lane] != coeff) {
                return 0;
            }
            coeffs[lane] = coeff;
        }
    }

    return 1;
}

static
int haoDotGroupCombinedDisabled(void) {
    static int disabled = -1;
    const char *env;

    if (disabled >= 0) {
        return disabled;
    }

    env = getenv("HS_HAO_DOT_GROUP_COMBINED");
    disabled = env && *env == '0';
    return disabled;
}

static really_inline
void haoPrepRawKeys(const u8 *primaryBitmap, svuint32_t vkeys,
                    svuint32_t *vbitPos, svuint32_t *vbitmapBytes);

static really_inline
void haoPrepRawKeysPred(const u8 *primaryBitmap, svbool_t pg,
                        svuint32_t vkeys, svuint32_t *vbitPos,
                        svuint32_t *vbitmapBytes);

static really_inline
u32 haoRetireEncodedPair(const u32 *primaryHashTable, svuint32_t vlaneBits,
                         svuint32_t vkeys, svuint32_t vbitPos,
                         svuint32_t vbitmapBytes, u32 *encodedPair);

static really_inline
u32 haoRetireEncodedPairPred(const u32 *primaryHashTable, svbool_t pvalid,
                             svuint32_t vlaneBits, svuint32_t vkeys,
                             svuint32_t vbitPos, svuint32_t vbitmapBytes,
                             u32 *encodedPair);

static really_inline
svuint64_t haoDotGroupMulWord(svuint64_t word, u16 coeff) {
    const svbool_t pg64 = svptrue_b64();

    return svmul_u64_x(pg64, word, svdup_n_u64(coeff));
}

static really_inline
svuint32_t haoDotGroupPackPairKeys(svuint64_t key0, svuint64_t key1,
                                   u32 keyMask) {
    const svbool_t pg64 = svptrue_b64();
    const svuint64_t vkeyMask = svdup_n_u64(keyMask);

    key0 = svand_u64_x(pg64, key0, vkeyMask);
    key1 = svand_u64_x(pg64, key1, vkeyMask);
    return svqxtnt_u64(svqxtnb_u64(key0), key1);
}

static really_inline
void haoCollectDotGroupPairCombined(
    const struct HAODotGroupRuntime *groups, u32 groupCount,
    const u16 coeffs[HAO_RUNTIME_DOT_VECTOR_LANES], u32 pair,
    svuint8_t vrow0, svuint8_t vrow1, svuint32_t vlaneBits,
    u32 encodedByGroup[HAO_RUNTIME_DOT_GROUP_COUNT][4][8],
    u32 laneMasks[HAO_RUNTIME_DOT_GROUP_COUNT]) {
    const svbool_t pg64 = svptrue_b64();
    const svuint64_t raw0 = svreinterpret_u64_u8(vrow0);
    const svuint64_t raw1 = svreinterpret_u64_u8(vrow1);
    const svuint64_t word00 = svand_n_u64_x(pg64, raw0, 0xffffU);
    const svuint64_t word01 =
        svand_n_u64_x(pg64, svlsr_n_u64_x(pg64, raw0, 16), 0xffffU);
    const svuint64_t word02 =
        svand_n_u64_x(pg64, svlsr_n_u64_x(pg64, raw0, 32), 0xffffU);
    const svuint64_t word03 =
        svand_n_u64_x(pg64, svlsr_n_u64_x(pg64, raw0, 48), 0xffffU);
    const svuint64_t word10 = svand_n_u64_x(pg64, raw1, 0xffffU);
    const svuint64_t word11 =
        svand_n_u64_x(pg64, svlsr_n_u64_x(pg64, raw1, 16), 0xffffU);
    const svuint64_t word12 =
        svand_n_u64_x(pg64, svlsr_n_u64_x(pg64, raw1, 32), 0xffffU);
    const svuint64_t word13 =
        svand_n_u64_x(pg64, svlsr_n_u64_x(pg64, raw1, 48), 0xffffU);
    const svuint64_t term00 = haoDotGroupMulWord(word00, coeffs[0]);
    const svuint64_t term01 = haoDotGroupMulWord(word01, coeffs[1]);
    const svuint64_t term02 = haoDotGroupMulWord(word02, coeffs[2]);
    const svuint64_t term03 = haoDotGroupMulWord(word03, coeffs[3]);
    const svuint64_t term10 = haoDotGroupMulWord(word10, coeffs[0]);
    const svuint64_t term11 = haoDotGroupMulWord(word11, coeffs[1]);
    const svuint64_t term12 = haoDotGroupMulWord(word12, coeffs[2]);
    const svuint64_t term13 = haoDotGroupMulWord(word13, coeffs[3]);
    const svuint64_t sum02 = term03;
    const svuint64_t sum12 = term13;
    const svuint64_t sum04 = svadd_u64_x(pg64, term02, sum02);
    const svuint64_t sum14 = svadd_u64_x(pg64, term12, sum12);
    const svuint64_t sum06 = svadd_u64_x(pg64, term01, sum04);
    const svuint64_t sum16 = svadd_u64_x(pg64, term11, sum14);
    const svuint64_t sum08 = svadd_u64_x(pg64, term00, sum06);
    const svuint64_t sum18 = svadd_u64_x(pg64, term10, sum16);
    u32 g;

    for (g = 0; g < groupCount; g++) {
        svuint32_t vkeys = svdup_n_u32(0U);
        svuint32_t vbitPos;
        svuint32_t vbitmapBytes;
        u32 groupLaneMask;

        switch (groups[g].knownBytes) {
        case 2U:
            vkeys = haoDotGroupPackPairKeys(sum02, sum12,
                                            groups[g].hash.keyMask);
            break;
        case 4U:
            vkeys = haoDotGroupPackPairKeys(sum04, sum14,
                                            groups[g].hash.keyMask);
            break;
        case 6U:
            vkeys = haoDotGroupPackPairKeys(sum06, sum16,
                                            groups[g].hash.keyMask);
            break;
        case 8U:
            vkeys = haoDotGroupPackPairKeys(sum08, sum18,
                                            groups[g].hash.keyMask);
            break;
        default:
            continue;
        }

        haoPrepRawKeys(groups[g].primaryBitmap, vkeys, &vbitPos,
                       &vbitmapBytes);
        groupLaneMask = haoRetireEncodedPair(
            groups[g].primaryHashTable, vlaneBits, vkeys, vbitPos,
            vbitmapBytes, encodedByGroup[g][pair]);
        laneMasks[g] |= groupLaneMask;
        HAO_STATS_ADD(primaryActiveLanes,
                      (u32)__builtin_popcount(groupLaneMask));
    }
}

static really_inline
void haoCollectDotGroupPairCombinedPred(
    const struct HAODotGroupRuntime *groups, u32 groupCount,
    const u16 coeffs[HAO_RUNTIME_DOT_VECTOR_LANES], u32 pair,
    svuint8_t vrow0, svuint8_t vrow1, svbool_t pvalid,
    svuint32_t vlaneBits,
    u32 encodedByGroup[HAO_RUNTIME_DOT_GROUP_COUNT][4][8],
    u32 laneMasks[HAO_RUNTIME_DOT_GROUP_COUNT]) {
    const svbool_t pg64 = svptrue_b64();
    const svuint64_t raw0 = svreinterpret_u64_u8(vrow0);
    const svuint64_t raw1 = svreinterpret_u64_u8(vrow1);
    const svuint64_t word00 = svand_n_u64_x(pg64, raw0, 0xffffU);
    const svuint64_t word01 =
        svand_n_u64_x(pg64, svlsr_n_u64_x(pg64, raw0, 16), 0xffffU);
    const svuint64_t word02 =
        svand_n_u64_x(pg64, svlsr_n_u64_x(pg64, raw0, 32), 0xffffU);
    const svuint64_t word03 =
        svand_n_u64_x(pg64, svlsr_n_u64_x(pg64, raw0, 48), 0xffffU);
    const svuint64_t word10 = svand_n_u64_x(pg64, raw1, 0xffffU);
    const svuint64_t word11 =
        svand_n_u64_x(pg64, svlsr_n_u64_x(pg64, raw1, 16), 0xffffU);
    const svuint64_t word12 =
        svand_n_u64_x(pg64, svlsr_n_u64_x(pg64, raw1, 32), 0xffffU);
    const svuint64_t word13 =
        svand_n_u64_x(pg64, svlsr_n_u64_x(pg64, raw1, 48), 0xffffU);
    const svuint64_t term00 = haoDotGroupMulWord(word00, coeffs[0]);
    const svuint64_t term01 = haoDotGroupMulWord(word01, coeffs[1]);
    const svuint64_t term02 = haoDotGroupMulWord(word02, coeffs[2]);
    const svuint64_t term03 = haoDotGroupMulWord(word03, coeffs[3]);
    const svuint64_t term10 = haoDotGroupMulWord(word10, coeffs[0]);
    const svuint64_t term11 = haoDotGroupMulWord(word11, coeffs[1]);
    const svuint64_t term12 = haoDotGroupMulWord(word12, coeffs[2]);
    const svuint64_t term13 = haoDotGroupMulWord(word13, coeffs[3]);
    const svuint64_t sum02 = term03;
    const svuint64_t sum12 = term13;
    const svuint64_t sum04 = svadd_u64_x(pg64, term02, sum02);
    const svuint64_t sum14 = svadd_u64_x(pg64, term12, sum12);
    const svuint64_t sum06 = svadd_u64_x(pg64, term01, sum04);
    const svuint64_t sum16 = svadd_u64_x(pg64, term11, sum14);
    const svuint64_t sum08 = svadd_u64_x(pg64, term00, sum06);
    const svuint64_t sum18 = svadd_u64_x(pg64, term10, sum16);
    u32 g;

    for (g = 0; g < groupCount; g++) {
        svuint32_t vkeys = svdup_n_u32(0U);
        svuint32_t vbitPos;
        svuint32_t vbitmapBytes;
        u32 groupLaneMask;

        switch (groups[g].knownBytes) {
        case 2U:
            vkeys = haoDotGroupPackPairKeys(sum02, sum12,
                                            groups[g].hash.keyMask);
            break;
        case 4U:
            vkeys = haoDotGroupPackPairKeys(sum04, sum14,
                                            groups[g].hash.keyMask);
            break;
        case 6U:
            vkeys = haoDotGroupPackPairKeys(sum06, sum16,
                                            groups[g].hash.keyMask);
            break;
        case 8U:
            vkeys = haoDotGroupPackPairKeys(sum08, sum18,
                                            groups[g].hash.keyMask);
            break;
        default:
            continue;
        }

        haoPrepRawKeysPred(groups[g].primaryBitmap, pvalid, vkeys, &vbitPos,
                           &vbitmapBytes);
        groupLaneMask = haoRetireEncodedPairPred(
            groups[g].primaryHashTable, pvalid, vlaneBits, vkeys, vbitPos,
            vbitmapBytes, encodedByGroup[g][pair]);
        laneMasks[g] |= groupLaneMask;
        HAO_STATS_ADD(primaryActiveLanes,
                      (u32)__builtin_popcount(groupLaneMask));
    }
}

static really_inline
svuint32_t haoRawKeyPairBext(svuint8_t vrow0, svuint8_t vrow1,
                             u64a bextMask) {
    const svuint64_t keys0 =
        svbext_n_u64(svreinterpret_u64_u8(vrow0), (uint64_t)bextMask);
    const svuint64_t keys1 =
        svbext_n_u64(svreinterpret_u64_u8(vrow1), (uint64_t)bextMask);
    // const svuint64_t paired = svzip1_u64(keys0, keys1);
    return svqxtnt_u64(svqxtnb_u64(keys0), keys1);
}

static really_inline
svuint32_t haoRawKeyPairDot(svuint8_t vrow0, svuint8_t vrow1,
                            svuint16_t vdot, u32 keyMask) {
    const svbool_t pg64 = svptrue_b64();
    const svuint64_t zero = svdup_n_u64(0U);
    const svuint64_t keys0 = svand_n_u64_x(pg64, 
        svdot_u64(zero, svreinterpret_u16_u8(vrow0), vdot), keyMask);
    const svuint64_t keys1 = svand_n_u64_x(pg64, 
        svdot_u64(zero, svreinterpret_u16_u8(vrow1), vdot), keyMask);
    return svqxtnt_u64(svqxtnb_u64(keys0), keys1);
}

static really_inline
svuint32_t haoRawKeyPair(svuint8_t vrow0, svuint8_t vrow1,
                         const struct HAOHashRuntime *hash,
                         svuint16_t vdot) {
    if (hash->mode == HAO_RUNTIME_HASH_DOT) {
        return haoRawKeyPairDot(vrow0, vrow1, vdot, hash->keyMask);
    }
    return haoRawKeyPairBext(vrow0, vrow1, hash->bextMask);
}

static really_inline
svuint32_t haoBitmapKeyVec(svbool_t pg, svuint32_t vkeys) {
#if HAO_COMPRESSED_BITMAP
    return svlsr_n_u32_x(pg, vkeys, HAO_COMPRESSED_BITMAP_SHIFT);
#else
    return vkeys;
#endif
}

static really_inline
u32 haoBitmapKeyScalar(u32 key) {
#if HAO_COMPRESSED_BITMAP
    return key >> HAO_COMPRESSED_BITMAP_SHIFT;
#else
    return key;
#endif
}

static really_inline
void haoPrepRawKeys(const u8 *primaryBitmap, svuint32_t vkeys,
                    svuint32_t *vbitPos, svuint32_t *vbitmapBytes) {
    const svbool_t pg32 = svptrue_b32();
    const u32 *primaryBitmapWords = (const u32 *)primaryBitmap;
    const svuint32_t vbitmapKeys = haoBitmapKeyVec(pg32, vkeys);
    const svuint32_t vwordIdx = svlsr_n_u32_x(pg32, vbitmapKeys, 5);

    *vbitPos = svand_n_u32_x(pg32, vbitmapKeys, 31U);
    *vbitmapBytes = svld1_gather_u32index_u32(pg32, primaryBitmapWords,
                                              vwordIdx);
}

static really_inline
void haoPrepRawKeysPred(const u8 *primaryBitmap, svbool_t pg,
                              svuint32_t vkeys, svuint32_t *vbitPos,
                              svuint32_t *vbitmapBytes) {
    const u32 *primaryBitmapWords = (const u32 *)primaryBitmap;
    const svuint32_t vbitmapKeys = haoBitmapKeyVec(pg, vkeys);
    const svuint32_t vwordIdx = svlsr_n_u32_x(pg, vbitmapKeys, 5);

    *vbitPos = svand_n_u32_x(pg, vbitmapKeys, 31U);
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
#if HAO_COMPRESSED_BITMAP
    const svbool_t pprimary = svcmpne_n_u32(phit, vencoded, 0U);
    const svuint32_t vhitBits = svsel_u32(pprimary, vlaneBits, vzero);
    const u32 laneMask = svorv_u32(pg32, vhitBits);

    svst1_u32(pprimary, encodedPair, vencoded);
#else
    const svuint32_t vhitBits = svsel_u32(phit, vlaneBits, vzero);
    const u32 laneMask = svorv_u32(pg32, vhitBits);

    svst1_u32(phit, encodedPair, vencoded);
#endif

#if HAO_ENABLE_RUNTIME_STATS
    const svuint32_t vbitmapHitBits = svsel_u32(phit, vlaneBits, vzero);
    const u32 bitmapLaneMask = svorv_u32(pg32, vbitmapHitBits);
    HAO_STATS_ADD(primaryBitmapHitLanes,
                  (u32)__builtin_popcount(bitmapLaneMask));
    HAO_STATS_ADD(primaryAliasRejects,
                  (u32)__builtin_popcount(bitmapLaneMask & ~laneMask));
#endif
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
#if HAO_COMPRESSED_BITMAP
    const svbool_t pprimary = svcmpne_n_u32(phit, vencoded, 0U);
    const svuint32_t vhitBits = svsel_u32(pprimary, vlaneBits, vzero);
    const u32 laneMask = svorv_u32(pg32, vhitBits);

    svst1_u32(pprimary, encodedPair, vencoded);
#else
    const svuint32_t vhitBits = svsel_u32(phit, vlaneBits, vzero);
    const u32 laneMask = svorv_u32(pg32, vhitBits);

    svst1_u32(phit, encodedPair, vencoded);
#endif

#if HAO_ENABLE_RUNTIME_STATS
    const svuint32_t vbitmapHitBits = svsel_u32(phit, vlaneBits, vzero);
    const u32 bitmapLaneMask = svorv_u32(pg32, vbitmapHitBits);
    HAO_STATS_ADD(primaryBitmapHitLanes,
                  (u32)__builtin_popcount(bitmapLaneMask));
    HAO_STATS_ADD(primaryAliasRejects,
                  (u32)__builtin_popcount(bitmapLaneMask & ~laneMask));
#endif
    return laneMask;
}

static really_inline
int haoRunEncodedLanes(
    const struct HAOHashRuntime *hash, u32 l2EntryCount,
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
        if (likely(haoRunL2Range(hash, l2EntryCount, l2CheckTable,
                l2MetaTable, ruleMeta, a, control, &ctx, laneData, vslotBits,
                encoded) == HWLM_TERMINATED)) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static really_inline
u32 haoCollectRawEncoded(const u8 *primaryBitmap,
                         const u32 *primaryHashTable,
                         const struct HAOHashRuntime *hash,
                         svuint16_t vdot,
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

    const svuint32_t vkeys01 = haoRawKeyPair(vrow0, vrow1, hash, vdot);
    const svuint32_t vkeys23 = haoRawKeyPair(vrow2, vrow3, hash, vdot);
    const svuint32_t vkeys45 = haoRawKeyPair(vrow4, vrow5, hash, vdot);
    const svuint32_t vkeys67 = haoRawKeyPair(vrow6, vrow7, hash, vdot);

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
    const struct HAOHashRuntime *hash, svuint16_t vdot,
    u32 l2EntryCount, const u8 *primaryBitmap,
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
        primaryBitmap, primaryHashTable, hash, vdot, vlo, vhi,
        vlaneBits01, vlaneBits23, vlaneBits45, vlaneBits67,
        encodedByPair);
    const u32 fullValidBlock =
        blockStart + a->len_history >= HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U;

    if (likely(laneMask)) {
        if (haoRunEncodedLanes(
                hash, l2EntryCount, l2CheckTable, l2MetaTable, ruleMeta, a,
                control, blockStart, fullValidBlock, encodedByPair, laneMask,
                vlo, vhi) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static really_inline
u32 haoCollectRawTailEncoded(const u8 *primaryBitmap,
                             const u32 *primaryHashTable,
                             const struct HAOHashRuntime *hash,
                             svuint16_t vdot,
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

    const svuint32_t vkeys01 = haoRawKeyPair(vrow0, vrow1, hash, vdot);
    const svuint32_t vkeys23 = haoRawKeyPair(vrow2, vrow3, hash, vdot);
    const svuint32_t vkeys45 = haoRawKeyPair(vrow4, vrow5, hash, vdot);
    const svuint32_t vkeys67 = haoRawKeyPair(vrow6, vrow7, hash, vdot);

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
    const struct HAOHashRuntime *hash, svuint16_t vdot,
    u32 l2EntryCount, const u8 *primaryBitmap,
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
        primaryBitmap, primaryHashTable, hash, vdot, blockLaneCount,
        vlo, vhi, vlaneIds01, vlaneIds23, vlaneIds45, vlaneIds67,
        vlaneBits01, vlaneBits23, vlaneBits45, vlaneBits67,
        encodedByPair);
    const u32 fullValidBlock =
        blockStart + a->len_history >= HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U;

    if (likely(laneMask)) {
        if (haoRunEncodedLanes(
                hash, l2EntryCount, l2CheckTable, l2MetaTable, ruleMeta, a,
                control, blockStart, fullValidBlock, encodedByPair, laneMask,
                vlo, vhi) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static really_inline
void haoCollectDotGroupEncodedCombined(
    const struct HAODotGroupRuntime *groups, u32 groupCount,
    const u16 coeffs[HAO_RUNTIME_DOT_VECTOR_LANES],
    svuint8_t vlo, svuint8_t vhi,
    svuint32_t vlaneBits01, svuint32_t vlaneBits23,
    svuint32_t vlaneBits45, svuint32_t vlaneBits67,
    u32 encodedByGroup[HAO_RUNTIME_DOT_GROUP_COUNT][4][8],
    u32 laneMasks[HAO_RUNTIME_DOT_GROUP_COUNT]) {
    const svuint8_t vrow0 = svext_u8(vlo, vhi, 25);
    const svuint8_t vrow1 = svext_u8(vlo, vhi, 26);
    const svuint8_t vrow2 = svext_u8(vlo, vhi, 27);
    const svuint8_t vrow3 = svext_u8(vlo, vhi, 28);
    const svuint8_t vrow4 = svext_u8(vlo, vhi, 29);
    const svuint8_t vrow5 = svext_u8(vlo, vhi, 30);
    const svuint8_t vrow6 = svext_u8(vlo, vhi, 31);
    const svuint8_t vrow7 = vhi;

    HAO_STATS_ADD(primaryProbeLanes, HAO_BATCH_MAX_WIDTH * groupCount);
    haoCollectDotGroupPairCombined(groups, groupCount, coeffs, 0U, vrow0,
                                   vrow1, vlaneBits01, encodedByGroup,
                                   laneMasks);
    haoCollectDotGroupPairCombined(groups, groupCount, coeffs, 1U, vrow2,
                                   vrow3, vlaneBits23, encodedByGroup,
                                   laneMasks);
    haoCollectDotGroupPairCombined(groups, groupCount, coeffs, 2U, vrow4,
                                   vrow5, vlaneBits45, encodedByGroup,
                                   laneMasks);
    haoCollectDotGroupPairCombined(groups, groupCount, coeffs, 3U, vrow6,
                                   vrow7, vlaneBits67, encodedByGroup,
                                   laneMasks);
}

static really_inline
void haoCollectDotGroupTailEncodedCombined(
    const struct HAODotGroupRuntime *groups, u32 groupCount,
    const u16 coeffs[HAO_RUNTIME_DOT_VECTOR_LANES],
    u32 blockLaneCount, svuint8_t vlo, svuint8_t vhi,
    svuint32_t vlaneIds01, svuint32_t vlaneIds23,
    svuint32_t vlaneIds45, svuint32_t vlaneIds67,
    svuint32_t vlaneBits01, svuint32_t vlaneBits23,
    svuint32_t vlaneBits45, svuint32_t vlaneBits67,
    u32 encodedByGroup[HAO_RUNTIME_DOT_GROUP_COUNT][4][8],
    u32 laneMasks[HAO_RUNTIME_DOT_GROUP_COUNT]) {
    const svbool_t pg32 = svptrue_b32();
    const svuint8_t vrow0 = svext_u8(vlo, vhi, 25);
    const svuint8_t vrow1 = svext_u8(vlo, vhi, 26);
    const svuint8_t vrow2 = svext_u8(vlo, vhi, 27);
    const svuint8_t vrow3 = svext_u8(vlo, vhi, 28);
    const svuint8_t vrow4 = svext_u8(vlo, vhi, 29);
    const svuint8_t vrow5 = svext_u8(vlo, vhi, 30);
    const svuint8_t vrow6 = svext_u8(vlo, vhi, 31);
    const svuint8_t vrow7 = vhi;
    const svbool_t pvalid01 =
        svcmplt_n_u32(pg32, vlaneIds01, blockLaneCount);
    const svbool_t pvalid23 =
        svcmplt_n_u32(pg32, vlaneIds23, blockLaneCount);
    const svbool_t pvalid45 =
        svcmplt_n_u32(pg32, vlaneIds45, blockLaneCount);
    const svbool_t pvalid67 =
        svcmplt_n_u32(pg32, vlaneIds67, blockLaneCount);

    HAO_STATS_ADD(primaryProbeLanes, blockLaneCount * groupCount);
    haoCollectDotGroupPairCombinedPred(groups, groupCount, coeffs, 0U, vrow0,
                                       vrow1, pvalid01, vlaneBits01,
                                       encodedByGroup, laneMasks);
    haoCollectDotGroupPairCombinedPred(groups, groupCount, coeffs, 1U, vrow2,
                                       vrow3, pvalid23, vlaneBits23,
                                       encodedByGroup, laneMasks);
    haoCollectDotGroupPairCombinedPred(groups, groupCount, coeffs, 2U, vrow4,
                                       vrow5, pvalid45, vlaneBits45,
                                       encodedByGroup, laneMasks);
    haoCollectDotGroupPairCombinedPred(groups, groupCount, coeffs, 3U, vrow6,
                                       vrow7, pvalid67, vlaneBits67,
                                       encodedByGroup, laneMasks);
}

static really_inline
int haoRunDotGroupEncodedLaneMajor(
    const struct HAODotGroupRuntime *groups, u32 groupCount,
    const struct HAORuntimeRuleMeta *ruleMeta,
    const struct FDR_Runtime_Args *a, hwlm_group_t *control,
    size_t blockStart, u32 fullValidBlock,
    const u32 encodedByGroup[HAO_RUNTIME_DOT_GROUP_COUNT][4][8],
    const u32 laneMasks[HAO_RUNTIME_DOT_GROUP_COUNT],
    u32 blockLaneCount, svuint8_t vlo, svuint8_t vhi) {
    const svbool_t pgb = svptrue_b8();
    const svuint8x2_t rawTbl = svcreate2_u8(vlo, vhi);
    const svuint8_t baseIdx =
        svadd_n_u8_x(pgb, svand_n_u8_x(pgb, svindex_u8(0, 1), 7U), 25U);
    const svbool_t pg64 =
        svwhilelt_b64((u32)0, HAO_RUNTIME_RULE_SLOTS_PER_ENTRY);
    const svuint64_t vslotBits =
        svlsl_u64_x(pg64, svdup_n_u64(1U), svindex_u64(0, 1));
    u32 pendingLanes = 0;
    u32 lane;
    u32 g;

    (void)blockLaneCount;
    for (g = 0; g < groupCount; g++) {
        pendingLanes |= laneMasks[g];
    }

    while (pendingLanes) {
        lane = (u32)__builtin_ctz(pendingLanes);
        pendingLanes &= pendingLanes - 1U;

        const u32 laneBit = 1U << lane;
        const u32 pair = (lane & 7U) >> 1U;
        const u32 pairIdx = ((lane >> 3U) << 1U) | (lane & 1U);
        struct HAOPositionContext ctx;
        size_t endPos;
        u32 validMask32 = 0xffffffffU;
        svuint8_t laneIdx;
        svuint8_t laneBytes;
        svuint64_t laneData;

        endPos = blockStart + lane;
        if (unlikely(!fullValidBlock)) {
            validMask32 = haoComputeValidMask8(a, endPos) * 0x01010101U;
        }
        ctx.endPos = endPos;
        ctx.validMask32 = validMask32;
        laneIdx = svadd_n_u8_x(pgb, baseIdx, (uint8_t)lane);
        laneBytes = svtbl2_u8(rawTbl, laneIdx);
        laneData = svreinterpret_u64_u8(laneBytes);

        for (g = 0; g < groupCount; g++) {
            if (!(laneMasks[g] & laneBit)) {
                continue;
            }
            const u32 encoded = encodedByGroup[g][pair][pairIdx];

            if (unlikely(haoRunL2Range(
                    &groups[g].hash, groups[g].l2EntryCount,
                    groups[g].l2CheckTable,
                    groups[g].l2MetaTable, ruleMeta, a, control, &ctx,
                    laneData, vslotBits, encoded) == HWLM_TERMINATED)) {
                return HWLM_TERMINATED;
            }
        }
    }

    return HWLM_SUCCESS;
}

static really_inline
int haoRunDotGroupRaw32(
    const struct HAODotGroupRuntime *groups, u32 groupCount,
    const u16 combinedCoeffs[HAO_RUNTIME_DOT_VECTOR_LANES],
    const struct HAORuntimeRuleMeta *ruleMeta,
    const struct FDR_Runtime_Args *a, hwlm_group_t *control,
    size_t blockStart, svuint8_t vlo, svuint8_t vhi,
    svuint32_t vlaneBits01, svuint32_t vlaneBits23,
    svuint32_t vlaneBits45, svuint32_t vlaneBits67) {
    u32 encodedByGroup[HAO_RUNTIME_DOT_GROUP_COUNT][4][8];
    u32 laneMasks[HAO_RUNTIME_DOT_GROUP_COUNT] = {0};
    const u32 fullValidBlock =
        blockStart + a->len_history >= HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U;
    u32 g;

    if (likely(combinedCoeffs)) {
        haoCollectDotGroupEncodedCombined(
            groups, groupCount, combinedCoeffs, vlo, vhi, vlaneBits01,
            vlaneBits23, vlaneBits45, vlaneBits67, encodedByGroup,
            laneMasks);
    } else {
        for (g = 0; g < groupCount; g++) {
            const svuint16_t vdot = haoLoadDotVector(&groups[g].hash);
            HAO_STATS_ADD(primaryProbeLanes, HAO_BATCH_MAX_WIDTH);
            laneMasks[g] = haoCollectRawEncoded(
                groups[g].primaryBitmap, groups[g].primaryHashTable,
                &groups[g].hash, vdot, vlo, vhi, vlaneBits01,
                vlaneBits23, vlaneBits45, vlaneBits67, encodedByGroup[g]);
        }
    }

    return haoRunDotGroupEncodedLaneMajor(
        groups, groupCount, ruleMeta, a, control, blockStart, fullValidBlock,
        encodedByGroup, laneMasks, HAO_BATCH_MAX_WIDTH, vlo, vhi);
}

static really_inline
int haoRunDotGroupRawTailVec(
    const struct HAODotGroupRuntime *groups, u32 groupCount,
    const u16 combinedCoeffs[HAO_RUNTIME_DOT_VECTOR_LANES],
    const struct HAORuntimeRuleMeta *ruleMeta,
    const struct FDR_Runtime_Args *a, hwlm_group_t *control,
    size_t blockStart, u32 blockLaneCount, svuint8_t vlo, svuint8_t vhi,
    svuint32_t vlaneIds01, svuint32_t vlaneIds23,
    svuint32_t vlaneIds45, svuint32_t vlaneIds67,
    svuint32_t vlaneBits01, svuint32_t vlaneBits23,
    svuint32_t vlaneBits45, svuint32_t vlaneBits67) {
    u32 encodedByGroup[HAO_RUNTIME_DOT_GROUP_COUNT][4][8];
    u32 laneMasks[HAO_RUNTIME_DOT_GROUP_COUNT] = {0};
    const u32 fullValidBlock =
        blockStart + a->len_history >= HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U;
    u32 g;

    if (likely(combinedCoeffs)) {
        haoCollectDotGroupTailEncodedCombined(
            groups, groupCount, combinedCoeffs, blockLaneCount, vlo, vhi,
            vlaneIds01, vlaneIds23, vlaneIds45, vlaneIds67, vlaneBits01,
            vlaneBits23, vlaneBits45, vlaneBits67, encodedByGroup,
            laneMasks);
    } else {
        for (g = 0; g < groupCount; g++) {
            const svuint16_t vdot = haoLoadDotVector(&groups[g].hash);
            HAO_STATS_ADD(primaryProbeLanes, blockLaneCount);
            laneMasks[g] = haoCollectRawTailEncoded(
                groups[g].primaryBitmap, groups[g].primaryHashTable,
                &groups[g].hash, vdot, blockLaneCount, vlo, vhi,
                vlaneIds01, vlaneIds23, vlaneIds45, vlaneIds67, vlaneBits01,
                vlaneBits23, vlaneBits45, vlaneBits67, encodedByGroup[g]);
        }
    }

    return haoRunDotGroupEncodedLaneMajor(
        groups, groupCount, ruleMeta, a, control, blockStart, fullValidBlock,
        encodedByGroup, laneMasks, blockLaneCount, vlo, vhi);
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
    const u32 bitmapKey = haoBitmapKeyScalar(key);
    const u32 word = primaryBitmapWords[bitmapKey >> 5U];
    return word & (1U << (bitmapKey & 31U));
}

static really_inline
u32 haoHashRuntimeScalar(const struct HAOHashRuntime *hash, u64a rawWord) {
    if (hash->mode == HAO_RUNTIME_HASH_DOT) {
        u64a dot = 0;
        u32 i;

        for (i = 0; i < HAO_RUNTIME_DOT_VECTOR_LANES; i++) {
            const u64a word = (rawWord >> (i * 16U)) & 0xffffU;
            dot += word * hash->dotVector[i];
        }
        return (u32)(dot & hash->keyMask);
    }

    return (u32)pext64(rawWord, hash->bextMask);
}

static really_inline
int haoRunRawTailScalar(
    const struct HAOHashRuntime *hash, u32 l2EntryCount,
    const u8 *primaryBitmap,
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
        const u32 key = haoHashRuntimeScalar(hash, rawWord);

        if (haoRawBitmapHitScalar(primaryBitmap, key)) {
            const u32 encoded = primaryHashTable[key];
            const u32 validMask8 = haoComputeValidMask8(a, endPos);
            const svuint64_t laneData = svdup_n_u64(rawWord);

            HAO_STATS_ADD(primaryBitmapHitLanes, 1);
            if (encoded) {
                activeCount++;
                ctx.endPos = endPos;
                ctx.validMask32 = validMask8 * 0x01010101U;
                if (haoRunL2Range(
                        hash, l2EntryCount, l2CheckTable, l2MetaTable,
                        ruleMeta, a, control, &ctx, laneData, vslotBits,
                        encoded) ==
                    HWLM_TERMINATED) {
                    HAO_STATS_ADD(primaryActiveLanes, activeCount);
                    return HWLM_TERMINATED;
                }
            } else {
                HAO_STATS_ADD(primaryAliasRejects, 1);
            }
        }

    }

    HAO_STATS_ADD(primaryActiveLanes, activeCount);
    return HWLM_SUCCESS;
}

#endif

static int haoRunBatchDotGroupBlob(const struct HAORuntimeHeader *hdr,
                                   const struct FDR_Runtime_Args *a,
                                   hwlm_group_t *control) {
    const struct HAORuntimeDotGroupDesc *descs;
    const struct HAORuntimeRuleMeta *ruleMeta;
    struct HAODotGroupRuntime groups[HAO_RUNTIME_DOT_GROUP_COUNT];
    u16 combinedCoeffs[HAO_RUNTIME_DOT_VECTOR_LANES];
    const u16 *combinedCoeffPtr = NULL;
    u32 groupCount;
    u32 g;
    size_t i = a->start_offset;

    haoRefreshStatsMode();
    HAO_STATS_ADD(scanCalls, 1);
    HAO_STATS_ADD(scanInputBytes, a->len - a->start_offset);

    groupCount = hdr->primaryCount;
    descs = (const struct HAORuntimeDotGroupDesc *)((const u8 *)hdr +
                                                    hdr->primaryBitmapOffset);
    ruleMeta = (const struct HAORuntimeRuleMeta *)((const u8 *)hdr +
                                                   hdr->ruleMetaOffset);
    for (g = 0; g < groupCount; g++) {
        haoBuildDotGroupRuntime(hdr, &descs[g], &groups[g]);
    }
    if (!haoDotGroupCombinedDisabled() &&
        haoDotGroupBuildCombinedCoeffs(groups, groupCount, combinedCoeffs)) {
        combinedCoeffPtr = combinedCoeffs;
    }

    const svuint32_t vlaneBits01 = haoRawLaneBits(0U);
    const svuint32_t vlaneBits23 = haoRawLaneBits(2U);
    const svuint32_t vlaneBits45 = haoRawLaneBits(4U);
    const svuint32_t vlaneBits67 = haoRawLaneBits(6U);

    svuint8_t rawPrev = svdup_n_u8(0);
    haoLoadRawPrev32(a, i, &rawPrev);
    for ( ; i + HAO_RUNTIME_BLOCK_BYTES <= a->len;
          i += HAO_RUNTIME_BLOCK_BYTES) {
        svuint8_t rawCurr;
        int rt;

        HAO_PREFETCH_R(a->buf + i + HAO_PREFETCH_INPUT_DISTANCE);
        haoLoadRawCurr32(a, i, HAO_RUNTIME_BLOCK_BYTES, &rawCurr);

        HAO_STATS_ADD(blockCalls, 1);
        HAO_STATS_ADD(blockLanes, HAO_BATCH_MAX_WIDTH);
        rt = haoRunDotGroupRaw32(groups, groupCount, combinedCoeffPtr,
                                 ruleMeta, a, control, i, rawPrev, rawCurr,
                                 vlaneBits01, vlaneBits23, vlaneBits45,
                                 vlaneBits67);
        if (likely(rt == HWLM_TERMINATED)) {
            return HWLM_TERMINATED;
        }
        rawPrev = rawCurr;
    }

    if (i < a->len) {
        svuint8_t rawCurr;
        const u32 blockLaneCount = (u32)(a->len - i);
        const svuint32_t vlaneIds01 = haoRawLaneIds(0U);
        const svuint32_t vlaneIds23 = haoRawLaneIds(2U);
        const svuint32_t vlaneIds45 = haoRawLaneIds(4U);
        const svuint32_t vlaneIds67 = haoRawLaneIds(6U);
        int rt;

        haoLoadRawPrev32(a, i, &rawPrev);
        haoLoadRawCurr32(a, i, blockLaneCount, &rawCurr);

        HAO_STATS_ADD(blockCalls, 1);
        HAO_STATS_ADD(blockLanes, blockLaneCount);
        rt = haoRunDotGroupRawTailVec(
            groups, groupCount, combinedCoeffPtr, ruleMeta, a, control, i,
            blockLaneCount, rawPrev, rawCurr, vlaneIds01, vlaneIds23,
            vlaneIds45, vlaneIds67, vlaneBits01, vlaneBits23, vlaneBits45,
            vlaneBits67);
        if (likely(rt == HWLM_TERMINATED)) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static int haoRunBatchBlob(const struct HAORuntimeHeader *hdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t *control) {
    const u8 *primaryBitmap;
    const u32 *primaryHashTable;
    const struct HAORuntimeL2Check *l2CheckTable;
    const struct HAORuntimeL2Meta *l2MetaTable;
    const struct HAORuntimeRuleMeta *ruleMeta;
    struct HAOHashRuntime hash;
    svuint16_t vdot;
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
    haoBuildHashRuntime(hdr, &hash);
    haoAttachL15Tags(hdr, &hash);
    vdot = haoLoadDotVector(&hash);
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
        int rt = haoRunRaw32(&hash, vdot, l2EntryCount, primaryBitmap,
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
        int rt = haoRunRawTailVec(&hash, vdot, l2EntryCount, primaryBitmap,
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

    if (haoRuntimeHeaderHashMode(hdr) == HAO_RUNTIME_HASH_DOT_GROUP) {
        return haoRunBatchDotGroupBlob(hdr, a, &control);
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
