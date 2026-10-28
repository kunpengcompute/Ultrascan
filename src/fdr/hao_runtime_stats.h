#ifndef HAO_RUNTIME_STATS_H
#define HAO_RUNTIME_STATS_H

#include "hao_runtime.h"
#include "util/bitutils.h"

#ifndef HAO_ENABLE_RUNTIME_STATS
#define HAO_ENABLE_RUNTIME_STATS 0
#endif

#if HAO_ENABLE_RUNTIME_STATS
struct HAORuntimeStats {
    u64a scanCalls;
    u64a scanInputBytes;
    u64a blockCalls;
    u64a blockLanes;
    u64a primaryProbeLanes;
    u64a primaryActiveLanes;
    u64a encodedRangeCalls;
    u64a encodedRangeReportCalls;
    u64a encodedEntriesVisited;
    u64a verifierCalls;
    u64a verifierEntryHits;
    u64a verifierSlotHits;
    u64a encodedGroupRejects;
    u64a callbackReports;
    u64a l2RangeEntryBucketsEq1;
    u64a l2RangeEntryBuckets2To4;
    u64a l2RangeEntryBucketsGt4;
    u64a l2RangeRuleBucketsEq1;
    u64a l2RangeRuleBuckets2To4;
    u64a l2RangeRuleBucketsGt4;
    u64a l2RangeCollisionBuckets;
    u64a l2RangeTotalEntries;
    u64a l2RangeTotalRules;
    u64a l2RangeMinEntries;
    u64a l2RangeMaxEntries;
    u64a l2RangeMinRules;
    u64a l2RangeMaxRules;
};

extern struct HAORuntimeStats g_haoStats;
extern int g_haoStatsActive;

void haoRefreshStatsMode(void);
void haoStatsObserveL2Entry(
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, u32 offset, u32 matchMask);
void haoStatsObserveL2Bucket(
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, u32 offset, u32 count,
    u32 visitedCount, int anyReport);
void haoStatsObserveRangeShape(u32 entryCount, u32 ruleCount);

#define HAO_STATS_IF_ENABLED(stmt) \
    do {                           \
        if (g_haoStatsActive) {    \
            stmt;                  \
        }                          \
    } while (0)

#define HAO_STATS_ADD(field, delta) \
    HAO_STATS_IF_ENABLED(g_haoStats.field += (u64a)(delta))
#else
static really_inline
void haoRefreshStatsMode(void) {
}

static really_inline
void haoStatsObserveRangeShape(u32 entryCount, u32 ruleCount) {
    (void)entryCount;
    (void)ruleCount;
}

#define HAO_STATS_IF_ENABLED(stmt) \
    do {                           \
    } while (0)

#define HAO_STATS_ADD(field, delta) \
    do {                            \
    } while (0)
#endif

#endif /* HAO_RUNTIME_STATS_H */
