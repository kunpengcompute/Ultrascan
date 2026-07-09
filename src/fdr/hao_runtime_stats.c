/*
 * Copyright (c) 2026, Huawei Technologies Co., Ltd.
 */

#include "hao_runtime_stats.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if HAO_ENABLE_RUNTIME_STATS

struct HAORuntimeStats g_haoStats;
static int g_haoStatsForceEnabled;
static int g_haoStatsEnvEnabled = -1;
static int g_haoStatsAtexitRegistered;
int g_haoStatsActive;

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

/* Labels contain mixed ASCII and CJK text. strlen() counts bytes rather than
 * display columns, so we compensate to keep the colon column aligned. */
#define HAO_STAT_FMT(label, fmt, val)                                    \
    do {                                                                  \
        int _blen = (int)strlen(label);                                   \
        /* Count each 3-byte UTF-8 CJK code point as display width 2. */   \
        int _cjk  = 0;                                                    \
        for (const char *_p = (label); *_p; ) {                           \
            unsigned char _c = (unsigned char)*_p;                        \
            if (_c >= 0xE0) { _cjk++; _p += 3; }                          \
            else if (_c >= 0xC0) { _p += 2; }                             \
            else { _p += 1; }                                             \
        }                                                                 \
        int _pad = 42 - (_blen - _cjk);                                   \
        if (_pad < 1) _pad = 1;                                           \
        fprintf(stderr, "  %s%*s: " fmt "\n", label, _pad, "", val);   \
    } while (0)

static void haoDumpRuntimeStats(void);

void haoRefreshStatsMode(void) {
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

void haoStatsObserveL2Entry(
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

void haoStatsObserveL2Bucket(
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

void haoStatsObserveRangeShape(u32 entryCount, u32 ruleCount) {
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
