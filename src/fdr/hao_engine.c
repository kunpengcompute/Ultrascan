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
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <arm_sve.h>

// #define HAO_BITMAP_CACHE_LOCK /* 取消注释开启L0 cache */

#ifndef HAO_SVE_WORD_LEVEL_BITMAP_PROBE
#define HAO_SVE_WORD_LEVEL_BITMAP_PROBE 0
#endif

#ifndef HAO_SVE_BYTE_RETIRE_SCALAR_PRIMARY_LOAD
#define HAO_SVE_BYTE_RETIRE_SCALAR_PRIMARY_LOAD 0
#endif

#ifndef HAO_SVE_BYTE_RETIRE_HYBRID_PRIMARY_LOAD
#define HAO_SVE_BYTE_RETIRE_HYBRID_PRIMARY_LOAD 0
#endif

#ifndef HAO_SVE_BYTE_RETIRE_HYBRID_THRESHOLD
#define HAO_SVE_BYTE_RETIRE_HYBRID_THRESHOLD 2
#endif

#ifndef HAO_SVE_BYTE_RETIRE_DIRECT_VHIT_SCALAR
#define HAO_SVE_BYTE_RETIRE_DIRECT_VHIT_SCALAR 0
#endif

#ifndef HAO_SVE_STREAM32_BYTE
#define HAO_SVE_STREAM32_BYTE 1
#endif

#ifndef HAO_SVE_STREAM32_BYTE_V2
#define HAO_SVE_STREAM32_BYTE_V2 1
#endif

#ifdef HAO_BITMAP_CACHE_LOCK
#define DEV       "/dev/hisi_soc_cache_mgmt"
#define ALIGN_1MB (1UL * 1024 * 1024)
#define MAP_SIZE  (1UL * 1024 * 1024)
#endif

#ifdef HAO_BITMAP_CACHE_LOCK

typedef struct {
    void *addr;
    int fd;
} CacheMem;

static CacheMem cache_alloc(size_t size)
{
    CacheMem cm = {NULL, -1};
    int retry = 5;

    cm.fd = open(DEV, O_RDWR);
    if (cm.fd < 0) {
        perror("open " DEV);
        return cm;
    }

    while (retry-- > 0) {
        void *probe = mmap(NULL, MAP_SIZE + ALIGN_1MB,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (probe == MAP_FAILED) {
            perror("probe mmap");
            break;
        }

        void *aligned = ((uintptr_t)probe % ALIGN_1MB == 0)
            ? probe
            : (void *)((uintptr_t)probe + ALIGN_1MB
                       - (uintptr_t)probe % ALIGN_1MB);

        munmap(probe, MAP_SIZE + ALIGN_1MB);

        {
            void *addr = mmap(aligned, MAP_SIZE,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, cm.fd, 0);
            if (addr == MAP_FAILED) {
                if (errno == EINVAL) {
                    continue;
                }
                perror("device mmap");
                break;
            }

            memset(addr, 0, MAP_SIZE);
            cm.addr = addr;
            return cm;
        }
    }

    close(cm.fd);
    cm.fd = -1;
    return cm;
}

static void cache_free(CacheMem *cm)
{
    if (cm->addr) {
        munmap(cm->addr, MAP_SIZE);
        cm->addr = NULL;
    }
    if (cm->fd >= 0) {
        close(cm->fd);
        cm->fd = -1;
    }
}

typedef struct {
    CacheMem cm;
    u8 *bitmap;
    u32 bitmapSize;
} HaoBitmapCtx;

typedef struct {
    const struct HAORuntimeHeader *hdr;
    const u8 *srcBitmap;
    u32 srcSize;
    HaoBitmapCtx ctx;
    u8 initialized;
    u8 destroyRegistered;
} HaoBitmapCache;

static int hao_bitmap_ctx_init(HaoBitmapCtx *ctx,
                               const u8 *src_bitmap, u32 src_size)
{
    if (src_size > MAP_SIZE) {
        fprintf(stderr, "[cache] bitmap too large: %u > %lu\n",
                src_size, MAP_SIZE);
        return -1;
    }
    ctx->cm = cache_alloc(src_size);
    if (!ctx->cm.addr) {
        return -1;
    }

    memcpy(ctx->cm.addr, src_bitmap, src_size);
    ctx->bitmap = (u8 *)ctx->cm.addr;
    ctx->bitmapSize = src_size;
    return 0;
}

static void hao_bitmap_ctx_destroy(HaoBitmapCtx *ctx)
{
    cache_free(&ctx->cm);
    ctx->bitmap = NULL;
    ctx->bitmapSize = 0;
}

static HaoBitmapCache g_haoBitmapCache = {0};

static void hao_bitmap_cache_destroy(void)
{
    if (!g_haoBitmapCache.initialized) {
        return;
    }

    hao_bitmap_ctx_destroy(&g_haoBitmapCache.ctx);
    g_haoBitmapCache.hdr = NULL;
    g_haoBitmapCache.srcBitmap = NULL;
    g_haoBitmapCache.srcSize = 0;
    g_haoBitmapCache.initialized = 0;
}

static const u8 *hao_bitmap_cache_get(const struct HAORuntimeHeader *hdr,
                                      const u8 *src_bitmap, u32 src_size)
{
    if (!hdr || !src_bitmap || !src_size) {
        return src_bitmap;
    }

    if (g_haoBitmapCache.initialized &&
        g_haoBitmapCache.hdr == hdr &&
        g_haoBitmapCache.srcBitmap == src_bitmap &&
        g_haoBitmapCache.srcSize == src_size &&
        g_haoBitmapCache.ctx.bitmap) {
        return g_haoBitmapCache.ctx.bitmap;
    }

    if (g_haoBitmapCache.initialized) {
        hao_bitmap_cache_destroy();
    }

    if (!g_haoBitmapCache.destroyRegistered) {
        atexit(hao_bitmap_cache_destroy);
        g_haoBitmapCache.destroyRegistered = 1;
    }

    if (hao_bitmap_ctx_init(&g_haoBitmapCache.ctx, src_bitmap, src_size) != 0) {
        fprintf(stderr, "[cache] init failed, fallback\n");
        g_haoBitmapCache.ctx.bitmap = (u8 *)src_bitmap;
        g_haoBitmapCache.ctx.bitmapSize = src_size;
        g_haoBitmapCache.ctx.cm.addr = NULL;
        g_haoBitmapCache.ctx.cm.fd = -1;
    }

    g_haoBitmapCache.hdr = hdr;
    g_haoBitmapCache.srcBitmap = src_bitmap;
    g_haoBitmapCache.srcSize = src_size;
    g_haoBitmapCache.initialized = 1;
    return g_haoBitmapCache.ctx.bitmap;
}

#else

typedef struct {
    u8 *bitmap;
    u32 bitmapSize;
} HaoBitmapCtx;

static const u8 *hao_bitmap_cache_get(const struct HAORuntimeHeader *hdr,
                                      const u8 *src_bitmap, u32 src_size)
{
    (void)hdr;
    (void)src_size;
    return src_bitmap;
}

#endif /* HAO_BITMAP_CACHE_LOCK */
static struct HAORuntimeStats g_haoStats;
static int g_haoStatsForceEnabled;
static int g_haoStatsEnvEnabled = -1;
static int g_haoStatsAtexitRegistered;
static int g_haoStatsActive;

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
    const u32 *primaryIdx;
    u32 byteIndex[HAO_BATCH_MAX_WIDTH];
    u8 bitMask[HAO_BATCH_MAX_WIDTH];
    u32 activeLaneIndex[HAO_BATCH_MAX_WIDTH];
    u32 activeEncoded[HAO_BATCH_MAX_WIDTH];
    u32 groupedBaseByte;
    u8 groupedSpan;
    u8 groupedReady;
    u16 reserved0;
    u32 groupedLaneMaskByByteBit[HAO_BITMAP_GROUPED_BYTES][8];
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
                const u32 primaryIdx = state->primaryIdx[activeLane];
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
        u8 gatheredBytes[HAO_BATCH_MAX_WIDTH];

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
                const u32 primaryIdx = state->primaryIdx[activeLane];
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
            primaryHashTable[state->primaryIdx[lane]];
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
    if ((u64a)hdr->primaryBitmapRawOffset + (u64a)hdr->primaryBitmapSize >
        (u64a)blobSize) {
        return 0;
    }
    if ((u64a)hdr->primaryRawOffset + (u64a)hdr->primaryCount * sizeof(u32) >
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
u32 haoEntrySlotCountFromMask(const struct HAORuntimeSecondaryHashEntry *entry,
                              u32 slotMask) {

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
u32 haoEntryLaneMaskFromByteMatchesPrepared(
    const struct HAORuntimeSecondaryHashEntry *entry, u32 slotMask,
    u32 byteMatchMask) {
    u32 laneMask = 0;

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
u32 haoEntryLaneMaskFromByteMatches(
    const struct HAORuntimeSecondaryHashEntry *entry, u32 byteMatchMask) {
    return haoEntryLaneMaskFromByteMatchesPrepared(entry, haoEntrySlotMask(entry),
                                                   byteMatchMask);
}

static really_inline
u32 haoEntrySingleSlotMatchMaskPrepared(
    const struct HAORuntimeSecondaryHashEntry *entry,
    struct HAOPositionContext *ctx, u32 slotMask, u32 shuffledValidMask,
    int identityTableControl) {
    u32 mask = 0;

    if (!entry || !ctx || !slotMask) {
        return 0;
    }

    if (identityTableControl) {
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
        return slotMask;
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

    return slotMask;
}

static really_inline
u32 haoEntrySingleSlotMatchMaskFromContext(
    const struct HAORuntimeSecondaryHashEntry *entry,
    struct HAOPositionContext *ctx, u32 shuffledValidMask) {
    const u32 slotMask = haoEntrySlotMask(entry);

    if (!entry || !ctx || !slotMask) {
        return 0;
    }

    haoEnsureLaneWindowContext(ctx);
    return haoEntrySingleSlotMatchMaskPrepared(
        entry, ctx, slotMask, shuffledValidMask,
        haoEntryHasIdentityTableControl(entry));
}

static u32 haoEntryMatchMaskFromContext(
    const struct HAORuntimeSecondaryHashEntry *entry,
    struct HAOPositionContext *ctx) {
    u32 shuffledValidMask = 0;
    const u32 slotMask = haoEntrySlotMask(entry);
    const u32 slotCount = haoEntrySlotCountFromMask(entry, slotMask);
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
            haoEntrySingleSlotMatchMaskPrepared(entry, ctx, slotMask,
                                                shuffledValidMask,
                                                identityTableControl);
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
                haoEntryLaneMaskFromByteMatchesPrepared(entry, slotMask,
                                                        byteMatchMask);

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
                    haoEntryLaneMaskFromByteMatchesPrepared(entry, slotMask,
                                                            byteMatchMask);
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

static really_inline
u32 haoEntryMatchMaskFromPreparedContext(
    const struct HAORuntimeSecondaryHashEntry *entry,
    struct HAOPositionContext *ctx) {
    u32 shuffledValidMask = 0;
    const u32 slotMask = haoEntrySlotMask(entry);
    const u32 slotCount = haoEntrySlotCountFromMask(entry, slotMask);
    const int identityTableControl = haoEntryHasIdentityTableControl(entry);

    if (!entry || !slotMask || !ctx) {
        return 0;
    }

    HAO_STATS_ADD(verifierCalls, 1);
    if (identityTableControl) {
        shuffledValidMask = ctx->validMask32 & entry->tailMask;
    } else {
        shuffledValidMask = haoBuildShuffledValidMask(entry, ctx);
    }

    if (slotCount == 1) {
        const u32 laneMask =
            haoEntrySingleSlotMatchMaskPrepared(entry, ctx, slotMask,
                                                shuffledValidMask,
                                                identityTableControl);
        if (laneMask) {
            HAO_STATS_ADD(verifierEntryHits, 1);
            HAO_STATS_ADD(verifierSlotHits, 1);
        }
        return laneMask;
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

        {
            const u32 byteMatchMask =
                (eqLo | (eqHi << 16)) & shuffledValidMask & entry->tailMask;
            const u32 laneMask =
                haoEntryLaneMaskFromByteMatchesPrepared(entry, slotMask,
                                                        byteMatchMask);

            if (laneMask) {
                HAO_STATS_ADD(verifierEntryHits, 1);
                HAO_STATS_ADD(verifierSlotHits, popcount32(laneMask));
            }
            return laneMask;
        }
    }
#else
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
                haoEntryLaneMaskFromByteMatchesPrepared(entry, slotMask,
                                                        byteMatchMask);
            if (laneMask) {
                HAO_STATS_ADD(verifierEntryHits, 1);
                HAO_STATS_ADD(verifierSlotHits, popcount32(laneMask));
            }
            return laneMask;
        }
    }
#endif
}

static u32 haoEntryMatchMaskFromContextScalarForTest(
    const struct HAORuntimeSecondaryHashEntry *entry,
    struct HAOPositionContext *ctx) {
    u32 shuffledValidMask = 0;
    const u32 slotMask = haoEntrySlotMask(entry);
    const u32 slotCount = haoEntrySlotCountFromMask(entry, slotMask);
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
        return haoEntrySingleSlotMatchMaskPrepared(entry, ctx, slotMask,
                                                   shuffledValidMask,
                                                   identityTableControl);
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

        return haoEntryLaneMaskFromByteMatchesPrepared(entry, slotMask,
                                                       byteMatchMask);
    }
}

static u32 haoEntryMatchMaskFromContextVectorForTest(
    const struct HAORuntimeSecondaryHashEntry *entry,
    struct HAOPositionContext *ctx) {
    u32 shuffledValidMask = 0;
    const u32 slotMask = haoEntrySlotMask(entry);
    const u32 slotCount = haoEntrySlotCountFromMask(entry, slotMask);
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
        return haoEntrySingleSlotMatchMaskPrepared(entry, ctx, slotMask,
                                                   shuffledValidMask,
                                                   identityTableControl);
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

        return haoEntryLaneMaskFromByteMatchesPrepared(
            entry, slotMask,
            (eqLo | (eqHi << 16)) & shuffledValidMask & entry->tailMask);
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
    u32 visitedCount = 0;
    u32 bucketRuleCount = 0;
    int anyReport = 0;

    if (!encoded || !ctx) {
        return HWLM_SUCCESS;
    }

    HAO_STATS_ADD(encodedRangeCalls, 1);
    haoDecodePrimaryValue(encoded, &offset, &count);
    for (n = 0; n < count; n++) {
        const u32 off = offset + n;
        const struct HAORuntimeSecondaryHashEntry *entry;
        u32 laneMask;
        u32 matchMask;

        if (!off || off >= hdr->secondaryCount) {
            break;
        }

        entry = &secondaryHashTable[off];
        HAO_STATS_IF_ENABLED({
            visitedCount++;
            bucketRuleCount += haoEntrySlotCount(entry);
        });
        HAO_STATS_ADD(encodedEntriesVisited, 1);
        laneMask = haoEntryMatchMaskFromContext(entry, ctx);
        if (!laneMask) {
            continue;
        }

        matchMask = laneMask;
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
                haoStatsObserveRangeShape(visitedCount, bucketRuleCount);
                HAO_STATS_ADD(encodedRangeReportCalls, 1);
                return HWLM_TERMINATED;
            }
            matchMask &= matchMask - 1U;
        }
    }

    haoStatsObserveRangeShape(visitedCount, bucketRuleCount);
    HAO_STATS_ADD(encodedRangeReportCalls, anyReport ? 1 : 0);

    return HWLM_SUCCESS;
}

static int haoProcessEncodedRangePrepared(
    const struct HAORuntimeHeader *hdr,
    const struct HAORuntimeSecondaryHashEntry *secondaryHashTable,
    const struct HAORuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct HAOPositionContext *ctx, u32 encoded) {
    u32 offset = 0;
    u32 count = 0;
    u32 n;
    u32 visitedCount = 0;
    u32 bucketRuleCount = 0;
    int anyReport = 0;

    if (!encoded || !ctx) {
        return HWLM_SUCCESS;
    }

    HAO_STATS_ADD(encodedRangeCalls, 1);
    haoDecodePrimaryValue(encoded, &offset, &count);
    for (n = 0; n < count; n++) {
        const u32 off = offset + n;
        const struct HAORuntimeSecondaryHashEntry *entry;
        u32 laneMask;
        u32 matchMask;

        if (!off || off >= hdr->secondaryCount) {
            break;
        }

        entry = &secondaryHashTable[off];
        HAO_STATS_IF_ENABLED({
            visitedCount++;
            bucketRuleCount += haoEntrySlotCount(entry);
        });
        HAO_STATS_ADD(encodedEntriesVisited, 1);
        laneMask = haoEntryMatchMaskFromPreparedContext(entry, ctx);
        if (!laneMask) {
            continue;
        }

        matchMask = laneMask;
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
                haoStatsObserveRangeShape(visitedCount, bucketRuleCount);
                HAO_STATS_ADD(encodedRangeReportCalls, 1);
                return HWLM_TERMINATED;
            }
            matchMask &= matchMask - 1U;
        }
    }

    haoStatsObserveRangeShape(visitedCount, bucketRuleCount);
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
    size_t blockStart;
    u8 fullyValidBlock;
    u8 reserved0;
    u16 reserved1;
    u8 blockBytes[HAO_RUNTIME_BLOCK_BYTES + 16U];
};

/* 构建块字节视图：7 history bytes + 32 current bytes */
static void haoBuildBlockByteView(const struct FDR_Runtime_Args *a,
                                  size_t blockStart, u32 laneCount,
                                  u8 *blockBytes) {
    const u32 prefix = HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U;
    u32 i;

    memset(blockBytes, 0, prefix + laneCount);
    for (i = 0; i < prefix; i++) {
        u8 b = 0;
        haoGetByteAt(a, (s64a)blockStart - (s64a)prefix + i, &b);
        blockBytes[i] = b;
    }

    memcpy(blockBytes + prefix, a->buf + blockStart, laneCount);
}

static void haoBuildByteLanesFromBlockBytes(
    const u8 *blockBytes, u32 laneCount,
    u8 byteLanes[HAO_RUNTIME_BYTES_PER_RULE_SLOT][HAO_BATCH_MAX_WIDTH]) {
    u32 byteIdx;
    u32 lane;

    if (!blockBytes || !byteLanes || !laneCount) {
        return;
    }

    memset(byteLanes, 0,
           sizeof(u8) * HAO_RUNTIME_BYTES_PER_RULE_SLOT *
               HAO_BATCH_MAX_WIDTH);

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

        if (laneCount < HAO_BATCH_MAX_WIDTH) {
            for (byteIdx = 0; byteIdx < HAO_RUNTIME_BYTES_PER_RULE_SLOT;
                 byteIdx++) {
                memset(byteLanes[byteIdx] + laneCount, 0,
                       HAO_BATCH_MAX_WIDTH - laneCount);
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

// TODO: 使用向量化的方式进行窗口构建以提升性能
static really_inline
u64a haoBuildWindowFromBlockBytes(const u8 *blockBytes, u32 lane,
                                  u32 windowBytes) {
    u64a window = 0;
    u32 i;

    if (!blockBytes) {
        return 0;
    }

    if (!windowBytes || windowBytes > HAO_RUNTIME_BYTES_PER_RULE_SLOT) {
        windowBytes = HAO_RUNTIME_BYTES_PER_RULE_SLOT;
    }

    if (windowBytes == HAO_RUNTIME_BYTES_PER_RULE_SLOT) {
        return haoByteReverse64(unaligned_load_u64a(blockBytes + lane));
    }

    for (i = 0; i < windowBytes; i++) {
        window = (window << 8U) | blockBytes[lane + i];
    }
    return window;
}

/* 根据块字节视图构建窗口，用于后续BEXT指令批量提取 */
static void haoBuildWindowsFromBlockBytes(const u8 *blockBytes, u32 laneCount,
                                          u32 windowBytes, u64a *windows) {
    u32 lane;
    u64a windowMask;

    if (!blockBytes || !windows || !laneCount) {
        return;
    }

    if (!windowBytes || windowBytes > HAO_RUNTIME_BYTES_PER_RULE_SLOT) {
        windowBytes = HAO_RUNTIME_BYTES_PER_RULE_SLOT;
    }

    if (windowBytes == HAO_RUNTIME_BYTES_PER_RULE_SLOT) {
        for (lane = 0; lane + 3U < laneCount; lane += 4U) {
            windows[lane] =
                haoByteReverse64(unaligned_load_u64a(blockBytes + lane));
            windows[lane + 1U] =
                haoByteReverse64(unaligned_load_u64a(blockBytes + lane + 1U));
            windows[lane + 2U] =
                haoByteReverse64(unaligned_load_u64a(blockBytes + lane + 2U));
            windows[lane + 3U] =
                haoByteReverse64(unaligned_load_u64a(blockBytes + lane + 3U));
        }

        for (; lane < laneCount; lane++) {
            windows[lane] =
                haoByteReverse64(unaligned_load_u64a(blockBytes + lane));
        }
        return;
    }

    windows[0] = haoBuildWindowFromBlockBytes(blockBytes, 0, windowBytes);
    if (laneCount == 1U) {
        return;
    }

    windowMask = windowBytes >= HAO_RUNTIME_BYTES_PER_RULE_SLOT
                     ? ~0ULL
                     : (((u64a)1 << (windowBytes * 8U)) - 1ULL);

    for (lane = 1; lane < laneCount; lane++) {
        windows[lane] =
            ((windows[lane - 1] << 8U) | blockBytes[lane + windowBytes - 1U]) &
            windowMask;
    }
}

/* 根据块字节视图中提取L1 key*/
static void haoExtractKeysFromBlockBytes(
    const struct HAORuntimeHeader *hdr,
    const struct HAORuntimeBitSelector *selectors, const u8 *blockBytes,
    u32 laneCount, u32 *keys) {
    u32 sel;
    const u32 laneMaskLimit = laneCount < 32U
                                  ? ((1U << laneCount) - 1U)
                                  : 0xffffffffU;

    if (!hdr || !selectors || !blockBytes || !keys || !laneCount) {
        return;
    }

    memset(keys, 0, sizeof(keys[0]) * laneCount);

    if (hdr->extractMode == HAO_RUNTIME_EXTRACT_MODE_BEXT) {
        u64a windows[HAO_BATCH_MAX_WIDTH];

        haoBuildWindowsFromBlockBytes(blockBytes, laneCount, hdr->windowBytes,
                                      windows);
        haoExtractKeysFromBextWindows(hdr, windows, laneCount, keys);
        return;
    }

    {
        u8 byteLanes[HAO_RUNTIME_BYTES_PER_RULE_SLOT][HAO_BATCH_MAX_WIDTH];

        haoBuildByteLanesFromBlockBytes(blockBytes, laneCount, byteLanes);

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
                    keys[lane] |= (1U << sel);
                    activeMask &= activeMask - 1U;
                }
                continue;
            }
#endif

            for (u32 lane = 0; lane < laneCount; lane++) {
                if (byteLanes[srcByte][lane] & bit) {
                    keys[lane] |= (1U << sel);
                }
            }
        }
    }
}

static really_inline
void haoBuildWindowsFromBlockBytesFixed8(const u8 *blockBytes, u32 laneCount,
                                         u64a *windows) {
    u32 lane = 0;

    if (!blockBytes || !windows || !laneCount) {
        return;
    }

    for (; lane + 7U < laneCount; lane += 8U) {
        windows[lane] =
            haoByteReverse64(unaligned_load_u64a(blockBytes + lane));
        windows[lane + 1U] =
            haoByteReverse64(unaligned_load_u64a(blockBytes + lane + 1U));
        windows[lane + 2U] =
            haoByteReverse64(unaligned_load_u64a(blockBytes + lane + 2U));
        windows[lane + 3U] =
            haoByteReverse64(unaligned_load_u64a(blockBytes + lane + 3U));
        windows[lane + 4U] =
            haoByteReverse64(unaligned_load_u64a(blockBytes + lane + 4U));
        windows[lane + 5U] =
            haoByteReverse64(unaligned_load_u64a(blockBytes + lane + 5U));
        windows[lane + 6U] =
            haoByteReverse64(unaligned_load_u64a(blockBytes + lane + 6U));
        windows[lane + 7U] =
            haoByteReverse64(unaligned_load_u64a(blockBytes + lane + 7U));
    }

    for (; lane + 3U < laneCount; lane += 4U) {
        windows[lane] =
            haoByteReverse64(unaligned_load_u64a(blockBytes + lane));
        windows[lane + 1U] =
            haoByteReverse64(unaligned_load_u64a(blockBytes + lane + 1U));
        windows[lane + 2U] =
            haoByteReverse64(unaligned_load_u64a(blockBytes + lane + 2U));
        windows[lane + 3U] =
            haoByteReverse64(unaligned_load_u64a(blockBytes + lane + 3U));
    }

    for (; lane < laneCount; lane++) {
        windows[lane] =
            haoByteReverse64(unaligned_load_u64a(blockBytes + lane));
    }
}

static really_inline
void haoExtractBextKeysLocal(
    const struct HAORuntimeHeader *hdr, const u8 *blockBytes, u32 laneCount,
    u32 *keys) {
    u64a windows[HAO_BATCH_MAX_WIDTH];
    const u32 keyMask = hdr ? haoPackedKeyMask(hdr->selectorCount) : 0;

    if (!hdr || !blockBytes || !keys || !laneCount) {
        return;
    }

    if (haoRuntimeCanUseBextFastPath() &&
        hdr->windowBytes == HAO_RUNTIME_BYTES_PER_RULE_SLOT) {
        haoBuildWindowsFromBlockBytesFixed8(blockBytes, laneCount, windows);
        haoExtractPackedBitsSveBitPermBatchToKeys(windows, laneCount,
                                                  hdr->bextMask, keyMask,
                                                  keys);
        return;
    }

    haoBuildWindowsFromBlockBytes(blockBytes, laneCount, hdr->windowBytes,
                                  windows);
    haoExtractKeysFromBextWindows(hdr, windows, laneCount, keys);
}

/* Build one HAO batch block state. The block byte view is materialized once
 * per block and reused later to build active-lane L2 context on demand. */
static int haoBuildBlockState(const struct FDR_Runtime_Args *a,
                              size_t blockStart, u32 blockLaneCount,
                              struct HAOBlockState *state) {
    u32 laneCount;

    if (!a || !state || !blockLaneCount || blockLaneCount > HAO_BATCH_MAX_WIDTH) {
        return 0;
    }
    if (blockStart >= a->len) {
        return 0;
    }
    laneCount = MIN(blockLaneCount, (u32)(a->len - blockStart));

    if (!laneCount) {
        return 0;
    }

    state->blockStart = blockStart;
    state->laneCount = laneCount;
    state->fullyValidBlock =
        ((u64a)blockStart + a->len_history) >=
        (HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U);
    haoBuildBlockByteView(a, blockStart, laneCount, state->blockBytes);
    return laneCount;
}

static void haoBuildContextFromBlockState(const struct HAORuntimeHeader *hdr,
                                          const struct FDR_Runtime_Args *a,
                                          const struct HAOBlockState *state,
                                          u32 lane,
                                          struct HAOPositionContext *ctx) {
    size_t endPos;

    if (!hdr || !a || !state || !ctx || lane >= state->laneCount) {
        return;
    }

    endPos = state->blockStart + lane;
    memset(ctx, 0, sizeof(*ctx));
    ctx->endPos = endPos;
    ctx->window64 =
        haoBuildWindowFromBlockBytes(state->blockBytes, lane, hdr->windowBytes);
    ctx->validMask8 =
        state->fullyValidBlock ? 0xffU : haoComputeValidMask8(a, endPos);
}


#ifdef __ARM_FEATURE_SVE
#include <arm_sve.h>

static really_inline
u32 haoProbeCompactAndLoadPrimaryDirect_sve_word(
        const u8 *bitmap, u32 bitmapSize,
        const u32 *primaryHashTable,
        const u32 *primaryIdx, u32 laneCount,
        u32 *activeLaneIndex, u32 *activeEncoded);

static really_inline
svuint32_t sve_u8gather_u32(svbool_t pg, const u8 *base,
                             svuint32_t byteIndices) {
    svuint32_t result;
    __asm__ volatile (
        "ld1b {%[res].s}, %[pg]/z, [%[base], %[idx].s, uxtw]"
        : [res] "=w" (result)
        : [pg]  "Upa" (pg),
          [base] "r"  (base),
          [idx]  "w"  (byteIndices)
        :
    );
    return result;
}

static really_inline
void haoStoreActivePrimaryScalar(const u32 *primaryHashTable, svbool_t pg,
                                 svuint32_t vhit, svuint32_t vidx,
                                 svuint32_t vlaneBase, u32 laneSpan,
                                 u32 *activeLaneIndex,
                                 u32 *activeEncoded) {
    u32 hitWords[HAO_BATCH_MAX_WIDTH] = {0};
    u32 idxWords[HAO_BATCH_MAX_WIDTH] = {0};
    u32 laneWords[HAO_BATCH_MAX_WIDTH] = {0};
    u32 i;
    u32 out = 0;

    svst1_u32(pg, hitWords, vhit);
    svst1_u32(pg, idxWords, vidx);
    svst1_u32(pg, laneWords, vlaneBase);

    for (i = 0; i < laneSpan; i++) {
        if (!hitWords[i]) {
            continue;
        }
        activeLaneIndex[out] = laneWords[i];
        activeEncoded[out] = primaryHashTable[idxWords[i]];
        out++;
    }
}

static really_inline
void haoStoreActivePrimaryScalarFromArray(
        const u32 *primaryHashTable, svbool_t pg, svuint32_t vhit,
        const u32 *primaryIdxBase, u32 laneBase, u32 laneSpan,
        u32 *activeLaneIndex, u32 *activeEncoded) {
    u32 hitWords[HAO_BATCH_MAX_WIDTH] = {0};
    u32 i;
    u32 out = 0;

    svst1_u32(pg, hitWords, vhit);

    for (i = 0; i < laneSpan; i++) {
        if (!hitWords[i]) {
            continue;
        }
        activeLaneIndex[out] = laneBase + i;
        activeEncoded[out] = primaryHashTable[primaryIdxBase[i]];
        out++;
    }
}

static really_inline
void haoStoreActivePrimaryScalarFromVec(
        const u32 *primaryHashTable, svbool_t pg, svuint32_t vhit,
        svuint32_t vidx, u32 laneBase, u32 laneSpan,
        u32 *activeLaneIndex, u32 *activeEncoded) {
    u32 hitWords[HAO_BATCH_MAX_WIDTH] = {0};
    u32 idxWords[HAO_BATCH_MAX_WIDTH] = {0};
    u32 i;
    u32 out = 0;

    svst1_u32(pg, hitWords, vhit);
    svst1_u32(pg, idxWords, vidx);

    for (i = 0; i < laneSpan; i++) {
        if (!hitWords[i]) {
            continue;
        }
        activeLaneIndex[out] = laneBase + i;
        activeEncoded[out] = primaryHashTable[idxWords[i]];
        out++;
    }
}

static really_inline
u32 haoStoreActivePrimaryScalarFromVecDirect(
        const u32 *primaryHashTable, svbool_t pg, svuint32_t vhit,
        svuint32_t vidx, u32 laneBase, u32 laneSpan,
        u32 *activeLaneIndex, u32 *activeEncoded) {
    u32 hitWords[HAO_BATCH_MAX_WIDTH] = {0};
    u32 idxWords[HAO_BATCH_MAX_WIDTH] = {0};
    u32 i;
    u32 out = 0;

    svst1_u32(pg, hitWords, vhit);
    svst1_u32(pg, idxWords, vidx);

    for (i = 0; i < laneSpan; i++) {
        if (!hitWords[i]) {
            continue;
        }
        activeLaneIndex[out] = laneBase + i;
        activeEncoded[out] = primaryHashTable[idxWords[i]];
        out++;
    }

    return out;
}

static really_inline
u32 haoStoreActivePrimaryScalarStride(
        const u32 *primaryHashTable, svbool_t pg, svuint32_t vhit,
        svuint32_t vidx, svuint32_t vlaneBase, u32 laneSpan,
        u32 *activeLaneIndex, u32 *activeEncoded) {
    u32 hitWords[HAO_BATCH_MAX_WIDTH] = {0};
    u32 idxWords[HAO_BATCH_MAX_WIDTH] = {0};
    u32 laneWords[HAO_BATCH_MAX_WIDTH] = {0};
    u32 i;
    u32 out = 0;

    svst1_u32(pg, hitWords, vhit);
    svst1_u32(pg, idxWords, vidx);
    svst1_u32(pg, laneWords, vlaneBase);

    for (i = 0; i < laneSpan; i++) {
        if (!hitWords[i]) {
            continue;
        }
        activeLaneIndex[out] = laneWords[i];
        activeEncoded[out] = primaryHashTable[idxWords[i]];
        out++;
    }

    return out;
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

static really_inline
u32 haoRetirePrimaryByteVecDirectScalar(
        svbool_t pg, svuint32_t vone, const u32 *primaryHashTable,
        svuint32_t vidx, svuint32_t vbitPos, svuint32_t vbitmapBytes,
        u32 laneBase, u32 laneSpan, u32 *activeLaneIndex,
        u32 *activeEncoded) {
    const svuint32_t vbitMask = svlsl_u32_x(pg, vone, vbitPos);
    const svuint32_t vhit = svand_u32_x(pg, vbitmapBytes, vbitMask);

    return haoStoreActivePrimaryScalarFromVecDirect(
        primaryHashTable, pg, vhit, vidx, laneBase, laneSpan,
        activeLaneIndex, activeEncoded);
}

static really_inline
u32 haoRetirePrimaryByteVec(
        svbool_t pg, svuint32_t vone, const u32 *primaryHashTable,
        svuint32_t vidx, svuint32_t vbitPos, svuint32_t vbitmapBytes,
        svuint32_t vlaneBase, u32 laneBase, u32 laneSpan,
        u32 *activeLaneIndex, u32 *activeEncoded) {
#if HAO_SVE_BYTE_RETIRE_DIRECT_VHIT_SCALAR
    (void)vlaneBase;
    return haoRetirePrimaryByteVecDirectScalar(
        pg, vone, primaryHashTable, vidx, vbitPos, vbitmapBytes,
        laneBase, laneSpan, activeLaneIndex, activeEncoded);
#else
    const svuint32_t vbitMask = svlsl_u32_x(pg, vone, vbitPos);
    const svuint32_t vhit = svand_u32_x(pg, vbitmapBytes, vbitMask);
    const svbool_t phit = svcmpne_n_u32(pg, vhit, 0U);
    const u32 hitCount = (u32)svcntp_b32(pg, phit);

    if (hitCount > 0) {
#if HAO_SVE_BYTE_RETIRE_HYBRID_PRIMARY_LOAD
        if (hitCount <= HAO_SVE_BYTE_RETIRE_HYBRID_THRESHOLD) {
            haoStoreActivePrimaryScalarFromVec(
                primaryHashTable, pg, vhit, vidx, laneBase, laneSpan,
                activeLaneIndex, activeEncoded);
        } else {
            haoStoreActivePrimaryVector(primaryHashTable, phit, vidx,
                                        vlaneBase, hitCount, activeLaneIndex,
                                        activeEncoded);
        }
#elif HAO_SVE_BYTE_RETIRE_SCALAR_PRIMARY_LOAD
        haoStoreActivePrimaryScalarFromVec(
            primaryHashTable, pg, vhit, vidx, laneBase, laneSpan,
            activeLaneIndex, activeEncoded);
#else
        haoStoreActivePrimaryVector(primaryHashTable, phit, vidx, vlaneBase,
                                    hitCount, activeLaneIndex, activeEncoded);
#endif
    }

    return hitCount;
#endif
}

static really_inline
u32 haoProbePrimaryByteFromArray(
        svbool_t pg, svuint32_t vone, const u8 *bitmap,
        const u32 *primaryHashTable, const u32 *primaryIdxBase,
        u32 laneBase, u32 laneSpan, u32 *activeLaneIndex,
        u32 *activeEncoded) {
    const svuint32_t vidx = svld1_u32(pg, primaryIdxBase);
    const svuint32_t vlaneBase = svindex_u32(laneBase, 1U);
    const svuint32_t vbyteIdx = svlsr_n_u32_x(pg, vidx, 3);
    const svuint32_t vbitPos = svand_n_u32_x(pg, vidx, 7U);
    const svuint32_t vbitmapBytes = sve_u8gather_u32(pg, bitmap, vbyteIdx);
    const svuint32_t vbitMask = svlsl_u32_x(pg, vone, vbitPos);
    const svuint32_t vhit = svand_u32_x(pg, vbitmapBytes, vbitMask);
    const svbool_t phit = svcmpne_n_u32(pg, vhit, 0U);
    const u32 hitCount = (u32)svcntp_b32(pg, phit);

    if (!hitCount) {
        return 0;
    }

#if HAO_SVE_BYTE_RETIRE_HYBRID_PRIMARY_LOAD
    if (hitCount <= HAO_SVE_BYTE_RETIRE_HYBRID_THRESHOLD) {
        haoStoreActivePrimaryScalarFromArray(primaryHashTable, pg, vhit,
                                             primaryIdxBase, laneBase,
                                             laneSpan, activeLaneIndex,
                                             activeEncoded);
    } else {
        haoStoreActivePrimaryVector(primaryHashTable, phit, vidx, vlaneBase,
                                    hitCount, activeLaneIndex, activeEncoded);
    }
#elif HAO_SVE_BYTE_RETIRE_SCALAR_PRIMARY_LOAD
    haoStoreActivePrimaryScalarFromArray(primaryHashTable, pg, vhit,
                                         primaryIdxBase, laneBase, laneSpan,
                                         activeLaneIndex, activeEncoded);
#else
    haoStoreActivePrimaryVector(primaryHashTable, phit, vidx, vlaneBase,
                                hitCount, activeLaneIndex, activeEncoded);
#endif

    return hitCount;
}

#if defined(HS_BUILD_HAVE_SVEBITPERM) && defined(__ARM_FEATURE_SVE2_BITPERM)
static really_inline
svuint32_t haoExtractBextKeysFixed8Vec(const u8 *chunkBytes, u64a bextMask,
                                       svuint32_t vkeyMask) {
    u64a windows[8];
    const svbool_t pg64 = svptrue_b64();
    const svbool_t pg32 = svptrue_b32();
    const uint64_t m = (uint64_t)bextMask;
    svuint64_t w0;
    svuint64_t w1;
    svuint64_t p0;
    svuint64_t p1;
    svuint32_t vkeys;

    windows[0] = haoByteReverse64(unaligned_load_u64a(chunkBytes));
    windows[1] = haoByteReverse64(unaligned_load_u64a(chunkBytes + 1U));
    windows[2] = haoByteReverse64(unaligned_load_u64a(chunkBytes + 2U));
    windows[3] = haoByteReverse64(unaligned_load_u64a(chunkBytes + 3U));
    windows[4] = haoByteReverse64(unaligned_load_u64a(chunkBytes + 4U));
    windows[5] = haoByteReverse64(unaligned_load_u64a(chunkBytes + 5U));
    windows[6] = haoByteReverse64(unaligned_load_u64a(chunkBytes + 6U));
    windows[7] = haoByteReverse64(unaligned_load_u64a(chunkBytes + 7U));

    w0 = svld1_u64(pg64, (const uint64_t *)windows);
    w1 = svld1_u64(pg64, (const uint64_t *)(windows + 4));
    p0 = svbext_n_u64(w0, m);
    p1 = svbext_n_u64(w1, m);
    vkeys = svuzp1_u32(svreinterpret_u32_u64(p0), svreinterpret_u32_u64(p1));
    return svand_u32_x(pg32, vkeys, vkeyMask);
}

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
void haoLoadRawSrc32(const struct FDR_Runtime_Args *a, size_t blockStart,
                     svuint8_t *vlo, svuint8_t *vhi) {
    const svbool_t pgb = svptrue_b8();
    u8 loBytes[32];
    u8 hiBytes[32] = {0};
    u32 i;

    assert(a);
    assert(vlo);
    assert(vhi);

    if (blockStart >= HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U) {
        *vlo = svld1_u8(
            pgb,
            (const uint8_t *)(a->buf + blockStart -
                              (HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U)));
    } else {
        for (i = 0; i < 32U; i++) {
            u8 b = 0;
            haoGetByteAt(a,
                         (s64a)blockStart -
                             (s64a)(HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U) + i,
                         &b);
            loBytes[i] = b;
        }
        *vlo = svld1_u8(pgb, loBytes);
    }

    memcpy(hiBytes, a->buf + blockStart + 25U, 7U);
    *vhi = svld1_u8(pgb, hiBytes);
}

static really_inline
svuint8_t haoExtRawBytes32(svuint8_t vlo, svuint8_t vhi, u32 shift) {
    switch (shift) {
    case 0:
        return svext_u8(vlo, vhi, 0);
    case 1:
        return svext_u8(vlo, vhi, 1);
    case 2:
        return svext_u8(vlo, vhi, 2);
    case 3:
        return svext_u8(vlo, vhi, 3);
    case 4:
        return svext_u8(vlo, vhi, 4);
    case 5:
        return svext_u8(vlo, vhi, 5);
    case 6:
        return svext_u8(vlo, vhi, 6);
    case 7:
        return svext_u8(vlo, vhi, 7);
    default:
        return vlo;
    }
}

static really_inline
svuint32_t haoExtractRawKeys4(svuint8_t vrow, u64a bextMaskRaw) {
    const svuint64_t packed =
        svbext_n_u64(svreinterpret_u64_u8(vrow), (uint64_t)bextMaskRaw);
    const svuint32_t as32 = svreinterpret_u32_u64(packed);

    return svuzp1_u32(as32, as32);
}

static really_inline
svuint8_t haoNormalizeRawRowVec(svuint8_t vrow) {
    const svbool_t pgb = svptrue_b8();
    const svuint8_t vminus = svsub_n_u8_x(pgb, vrow, 32U);
    const svbool_t ge_a = svcmpge_n_u8(pgb, vrow, (u8)'a');
    const svbool_t le_z = svcmple_n_u8(pgb, vrow, (u8)'z');
    const svuint8_t maybeUpper = svsel_u8(ge_a, vminus, vrow);

    return svsel_u8(le_z, maybeUpper, vrow);
}

static really_inline
u64a haoBuildRawLaneWord(svuint8_t vrow, u32 group) {
    u8 laneBytes[HAO_RUNTIME_BYTES_PER_RULE_SLOT];
    const svbool_t pg8 =
        svwhilelt_b8((u64a)0, (u64a)HAO_RUNTIME_BYTES_PER_RULE_SLOT);

    vrow = haoNormalizeRawRowVec(vrow);

    switch (group) {
    case 1:
        vrow = svext_u8(vrow, vrow, 8);
        break;
    case 2:
        vrow = svext_u8(vrow, vrow, 16);
        break;
    case 3:
        vrow = svext_u8(vrow, vrow, 24);
        break;
    default:
        break;
    }

    svst1_u8(pg8, laneBytes, vrow);
    return unaligned_load_u64a(laneBytes);
}

static really_inline
void haoFillRawCtxFast(struct HAOPositionContext *ctx, size_t endPos,
                       u32 validMask8, u64a laneWord) {
    const u32 validMask32 = validMask8 * 0x01010101U;

    assert(ctx);
    ctx->endPos = endPos;
    ctx->key = 0;
    ctx->validMask8 = validMask8;
    ctx->laneWindowReady = 1;
    ctx->reserved0 = 0;
    ctx->reserved1 = 0;
    ctx->window64 = 0;
    ctx->validMask32 = validMask32;

    unaligned_store_u64a(ctx->laneWindow32, laneWord);
    unaligned_store_u64a(ctx->laneWindow32 + 8, laneWord);
    unaligned_store_u64a(ctx->laneWindow32 + 16, laneWord);
    unaligned_store_u64a(ctx->laneWindow32 + 24, laneWord);
}

static really_inline
void haoPrepareRawKeysVec(const u8 *primaryBitmap, svuint32_t vkeys,
                          svuint32_t *vbitPos,
                          svuint32_t *vbitmapBytes) {
    const svbool_t pg32 = svptrue_b32();
    const svuint32_t vbyteIdx = svlsr_n_u32_x(pg32, vkeys, 3);
    assert(vbitPos);
    assert(vbitmapBytes);

    *vbitPos = svand_n_u32_x(pg32, vkeys, 7U);
    *vbitmapBytes = sve_u8gather_u32(pg32, primaryBitmap, vbyteIdx);
}

static really_inline
void haoAppendRawActiveSorted(u32 lane, u32 encoded, u32 *activeLaneIndex,
                              u32 *activeEncoded, u32 *activeCount) {
    u32 pos;

    assert(activeLaneIndex);
    assert(activeEncoded);
    assert(activeCount);
    assert(*activeCount < HAO_BATCH_MAX_WIDTH);

    pos = *activeCount;
    while (pos && activeLaneIndex[pos - 1U] > lane) {
        activeLaneIndex[pos] = activeLaneIndex[pos - 1U];
        activeEncoded[pos] = activeEncoded[pos - 1U];
        pos--;
    }

    activeLaneIndex[pos] = lane;
    activeEncoded[pos] = encoded;
    (*activeCount)++;
}

static really_inline
u32 haoRetireRawKeysVec(const u32 *primaryHashTable, svuint32_t vkeys,
                        svuint32_t vlaneIds, svuint32_t vbitPos,
                        svuint32_t vbitmapBytes, u32 *blockActiveLaneIndex,
                        u32 *blockActiveEncoded, u32 *blockActiveCount) {
    const svbool_t pg32 = svptrue_b32();
    const svuint32_t vone = svdup_n_u32(1U);
    u32 activeLaneIndex[8];
    u32 activeEncoded[8];
    u32 activeCount = 0;
    u32 i;

    assert(blockActiveLaneIndex);
    assert(blockActiveEncoded);
    assert(blockActiveCount);

#if HAO_SVE_BYTE_RETIRE_DIRECT_VHIT_SCALAR
    {
        const svuint32_t vbitMask = svlsl_u32_x(pg32, vone, vbitPos);
        const svuint32_t vhit = svand_u32_x(pg32, vbitmapBytes, vbitMask);
        activeCount = haoStoreActivePrimaryScalarStride(primaryHashTable, pg32,
                                                        vhit, vkeys, vlaneIds,
                                                        8U, activeLaneIndex,
                                                        activeEncoded);
    }
#else
    {
        const svuint32_t vbitMask = svlsl_u32_x(pg32, vone, vbitPos);
        const svuint32_t vhit = svand_u32_x(pg32, vbitmapBytes, vbitMask);
        const svbool_t phit = svcmpne_n_u32(pg32, vhit, 0U);
        const u32 hitCount = (u32)svcntp_b32(pg32, phit);

        if (hitCount) {
#if HAO_SVE_BYTE_RETIRE_HYBRID_PRIMARY_LOAD
            if (hitCount <= HAO_SVE_BYTE_RETIRE_HYBRID_THRESHOLD) {
                activeCount = haoStoreActivePrimaryScalarStride(
                    primaryHashTable, pg32, vhit, vkeys, vlaneIds, 8U,
                    activeLaneIndex, activeEncoded);
            } else {
                haoStoreActivePrimaryVector(primaryHashTable, phit, vkeys,
                                            vlaneIds, hitCount,
                                            activeLaneIndex, activeEncoded);
                activeCount = hitCount;
            }
#elif HAO_SVE_BYTE_RETIRE_SCALAR_PRIMARY_LOAD
            activeCount = haoStoreActivePrimaryScalarStride(
                primaryHashTable, pg32, vhit, vkeys, vlaneIds, 8U,
                activeLaneIndex, activeEncoded);
#else
            haoStoreActivePrimaryVector(primaryHashTable, phit, vkeys,
                                        vlaneIds, hitCount, activeLaneIndex,
                                        activeEncoded);
            activeCount = hitCount;
#endif
        }
    }
#endif

    for (i = 0; i < activeCount; i++) {
        haoAppendRawActiveSorted(activeLaneIndex[i], activeEncoded[i],
                                 blockActiveLaneIndex, blockActiveEncoded,
                                 blockActiveCount);
    }

    HAO_STATS_ADD(primaryActiveLanes, activeCount);
    return activeCount;
}

static really_inline
int haoRunRaw32V2(const struct HAORuntimeHeader *hdr, const u8 *primaryBitmap,
                  const u32 *primaryHashTable,
                  const struct HAORuntimeSecondaryHashEntry *secondaryHashTable,
                  const struct HAORuntimeRuleMeta *ruleMeta,
                  const u8 *literalBlob, const struct FDR_Runtime_Args *a,
                  hwlm_group_t *control, size_t blockStart) {
    u32 blockActiveLaneIndex[HAO_BATCH_MAX_WIDTH];
    u32 blockActiveEncoded[HAO_BATCH_MAX_WIDTH];
    u32 blockActiveCount = 0;
    const u32 fullValidBlock = blockStart + a->len_history >=
                               HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U;
    svuint8_t vlo;
    svuint8_t vhi;
    svuint32_t vbitPos01;
    svuint32_t vbitPos23;
    svuint32_t vbitPos45;
    svuint32_t vbitPos67;
    svuint32_t vbitmapBytes01;
    svuint32_t vbitmapBytes23;
    svuint32_t vbitmapBytes45;
    svuint32_t vbitmapBytes67;
    const u64a bextMask = hdr->bextMaskRaw;
    haoLoadRawSrc32(a, blockStart, &vlo, &vhi);

    const svuint8_t vrow0 = svext_u8(vlo, vhi, 0);
    const svuint8_t vrow1 = svext_u8(vlo, vhi, 1);
    const svuint8_t vrow2 = svext_u8(vlo, vhi, 2);
    const svuint8_t vrow3 = svext_u8(vlo, vhi, 3);
    const svuint8_t vrow4 = svext_u8(vlo, vhi, 4);
    const svuint8_t vrow5 = svext_u8(vlo, vhi, 5);
    const svuint8_t vrow6 = svext_u8(vlo, vhi, 6);
    const svuint8_t vrow7 = svext_u8(vlo, vhi, 7);

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

    const svuint32_t vlanes01 = svzip1_u32(svindex_u32(0U, 8U), svindex_u32(1U, 8U));
    const svuint32_t vlanes23 = svzip1_u32(svindex_u32(2U, 8U), svindex_u32(3U, 8U));
    const svuint32_t vlanes45 = svzip1_u32(svindex_u32(4U, 8U), svindex_u32(5U, 8U));
    const svuint32_t vlanes67 = svzip1_u32(svindex_u32(6U, 8U), svindex_u32(7U, 8U));

    haoPrepareRawKeysVec(primaryBitmap, vkeys01, &vbitPos01, &vbitmapBytes01);
    haoPrepareRawKeysVec(primaryBitmap, vkeys23, &vbitPos23, &vbitmapBytes23);
    haoRetireRawKeysVec(primaryHashTable, vkeys01, vlanes01, vbitPos01,
                        vbitmapBytes01, blockActiveLaneIndex,
                        blockActiveEncoded, &blockActiveCount);
    haoPrepareRawKeysVec(primaryBitmap, vkeys45, &vbitPos45, &vbitmapBytes45);

    haoRetireRawKeysVec(primaryHashTable, vkeys23, vlanes23, vbitPos23,
                        vbitmapBytes23, blockActiveLaneIndex,
                        blockActiveEncoded, &blockActiveCount);
    haoPrepareRawKeysVec(primaryBitmap, vkeys67, &vbitPos67, &vbitmapBytes67);

    haoRetireRawKeysVec(primaryHashTable, vkeys45, vlanes45, vbitPos45,
                        vbitmapBytes45, blockActiveLaneIndex,
                        blockActiveEncoded, &blockActiveCount);
    haoRetireRawKeysVec(primaryHashTable, vkeys67, vlanes67, vbitPos67,
                        vbitmapBytes67, blockActiveLaneIndex,
                        blockActiveEncoded, &blockActiveCount);

    if (blockActiveCount) {
        u64a laneWordByShift[8][4];
        u8 laneWordReadyMaskByShift[8] = {0};
        u32 i;

        for (i = 0; i < blockActiveCount; i++) {
            struct HAOPositionContext ctx;
            const u32 lane = blockActiveLaneIndex[i];
            const u32 encoded = blockActiveEncoded[i];
            const u32 shift = lane & 7U;
            const u32 group = lane >> 3;
            const u32 validMask8 =
                fullValidBlock ? 0xffU
                               : haoComputeValidMask8(a, blockStart + lane);

            if (!(laneWordReadyMaskByShift[shift] & (1U << group))) {
                switch (shift) {
                case 0:
                    laneWordByShift[0][group] = haoBuildRawLaneWord(vrow0, group);
                    break;
                case 1:
                    laneWordByShift[1][group] = haoBuildRawLaneWord(vrow1, group);
                    break;
                case 2:
                    laneWordByShift[2][group] = haoBuildRawLaneWord(vrow2, group);
                    break;
                case 3:
                    laneWordByShift[3][group] = haoBuildRawLaneWord(vrow3, group);
                    break;
                case 4:
                    laneWordByShift[4][group] = haoBuildRawLaneWord(vrow4, group);
                    break;
                case 5:
                    laneWordByShift[5][group] = haoBuildRawLaneWord(vrow5, group);
                    break;
                case 6:
                    laneWordByShift[6][group] = haoBuildRawLaneWord(vrow6, group);
                    break;
                default:
                    laneWordByShift[7][group] = haoBuildRawLaneWord(vrow7, group);
                    break;
                }
                laneWordReadyMaskByShift[shift] |= (u8)(1U << group);
            }

            haoFillRawCtxFast(&ctx, blockStart + lane, validMask8,
                              laneWordByShift[shift][group]);
            if (haoProcessEncodedRangePrepared(
                    hdr, secondaryHashTable, ruleMeta, literalBlob,
                    hdr->literalBlobSize, a, control, &ctx, encoded) ==
                HWLM_TERMINATED) {
                return HWLM_TERMINATED;
            }
        }
    }

    return HWLM_SUCCESS;
}

static really_inline
void haoLoadRaw32(const struct FDR_Runtime_Args *a, size_t blockStart,
                  m128 *src0, m128 *src1, m128 *src2) {
    u8 src0Bytes[16];
    u8 src2Bytes[16] = {0};
    u32 i;

    assert(a);
    assert(src0);
    assert(src1);
    assert(src2);

    if (blockStart >= HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U) {
        *src0 = loadu128(a->buf + blockStart -
                         (HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U));
    } else {
        for (i = 0; i < 16U; i++) {
            u8 b = 0;
            haoGetByteAt(a,
                         (s64a)blockStart -
                             (s64a)(HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U) + i,
                         &b);
            src0Bytes[i] = b;
        }
        *src0 = loadu128(src0Bytes);
    }

    memcpy(src2Bytes, a->buf + blockStart + 25U, 7U);

    *src1 = loadu128(a->buf + blockStart + 9U);
    *src2 = loadu128(src2Bytes);
}

static really_inline
void haoAlign16(m128 lo, m128 hi, int shift, u8 *chunk) {
    const m128 aligned = palignr(hi, lo, shift);
    storeu128(chunk, aligned);
}

static really_inline
svuint32_t haoExtractRaw8Vec(const u8 *chunkBytes, u64a bextMaskRaw,
                             svuint32_t vkeyMask) {
    u64a lanes[8];
    const svbool_t pg64 = svptrue_b64();
    const svbool_t pg32 = svptrue_b32();
    const uint64_t m = (uint64_t)bextMaskRaw;
    svuint64_t w0;
    svuint64_t w1;
    svuint64_t p0;
    svuint64_t p1;
    svuint32_t vkeys;

    lanes[0] = unaligned_load_u64a(chunkBytes);
    lanes[1] = unaligned_load_u64a(chunkBytes + 1U);
    lanes[2] = unaligned_load_u64a(chunkBytes + 2U);
    lanes[3] = unaligned_load_u64a(chunkBytes + 3U);
    lanes[4] = unaligned_load_u64a(chunkBytes + 4U);
    lanes[5] = unaligned_load_u64a(chunkBytes + 5U);
    lanes[6] = unaligned_load_u64a(chunkBytes + 6U);
    lanes[7] = unaligned_load_u64a(chunkBytes + 7U);

    w0 = svld1_u64(pg64, (const uint64_t *)lanes);
    w1 = svld1_u64(pg64, (const uint64_t *)(lanes + 4));
    p0 = svbext_n_u64(w0, m);
    p1 = svbext_n_u64(w1, m);
    vkeys = svuzp1_u32(svreinterpret_u32_u64(p0), svreinterpret_u32_u64(p1));
    return svand_u32_x(pg32, vkeys, vkeyMask);
}

static really_inline
int haoRunRaw8(const struct HAORuntimeHeader *hdr,
               const struct HAORuntimeSecondaryHashEntry *secondaryHashTable,
               const struct HAORuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
               const struct FDR_Runtime_Args *a, hwlm_group_t *control,
               const u32 *primaryHashTable, size_t blockStart, u32 laneBase,
               const u8 *chunkBytes, svuint32_t vkeys,
               svuint32_t vbitmapBytes) {
    const svbool_t pg32 = svptrue_b32();
    const svuint32_t vone = svdup_n_u32(1U);
    const svuint32_t vbitPos = svand_n_u32_x(pg32, vkeys, 7U);
    u32 activeLaneIndex[8];
    u32 activeEncoded[8];
    u32 activeCount;
    u32 i;

    activeCount = haoRetirePrimaryByteVec(
        pg32, vone, primaryHashTable, vkeys, vbitPos, vbitmapBytes,
        svindex_u32(laneBase, 1U), laneBase, 8U, activeLaneIndex,
        activeEncoded);
    HAO_STATS_ADD(primaryActiveLanes, activeCount);

    for (i = 0; i < activeCount; i++) {
        struct HAOPositionContext ctx;
        const u32 lane = activeLaneIndex[i] - laneBase;

        memset(&ctx, 0, sizeof(ctx));
        ctx.endPos = blockStart + activeLaneIndex[i];
        ctx.validMask8 = haoComputeValidMask8(a, ctx.endPos);
        ctx.window64 = haoByteReverse64(unaligned_load_u64a(chunkBytes + lane));

        if (haoProcessEncodedRange(hdr, secondaryHashTable, ruleMeta,
                                   literalBlob, hdr->literalBlobSize, a,
                                   control, &ctx, activeEncoded[i]) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static really_inline
int haoRunRaw32(const struct HAORuntimeHeader *hdr, const u8 *primaryBitmap,
                const u32 *primaryHashTable,
                const struct HAORuntimeSecondaryHashEntry *secondaryHashTable,
                const struct HAORuntimeRuleMeta *ruleMeta,
                const u8 *literalBlob, const struct FDR_Runtime_Args *a,
                hwlm_group_t *control, size_t blockStart) {
    const svbool_t pg32 = svptrue_b32();
    const svuint32_t vkeyMask =
        svdup_n_u32(haoPackedKeyMask(hdr->selectorCount));
    m128 src0;
    m128 src1;
    m128 src2;
    u8 chunk0[16];
    u8 chunk1[16];
    u8 chunk2[16];
    u8 chunk3[16];
    svuint32_t vkeys0;
    svuint32_t vkeys1;
    svuint32_t vkeys2;
    svuint32_t vkeys3;
    svuint32_t vbyteIdx0;
    svuint32_t vbyteIdx1;
    svuint32_t vbyteIdx2;
    svuint32_t vbyteIdx3;
    svuint32_t vbitmapBytes0;
    svuint32_t vbitmapBytes1;
    svuint32_t vbitmapBytes2;
    svuint32_t vbitmapBytes3;

    haoLoadRaw32(a, blockStart, &src0, &src1, &src2);

    haoAlign16(src0, src1, 0, chunk0);
    haoAlign16(src0, src1, 8, chunk1);
    vkeys0 = haoExtractRaw8Vec(chunk0, hdr->bextMaskRaw, vkeyMask);
    vkeys1 = haoExtractRaw8Vec(chunk1, hdr->bextMaskRaw, vkeyMask);
    vbyteIdx0 = svlsr_n_u32_x(pg32, vkeys0, 3);
    vbyteIdx1 = svlsr_n_u32_x(pg32, vkeys1, 3);
    vbitmapBytes0 = sve_u8gather_u32(pg32, primaryBitmap, vbyteIdx0);
    vbitmapBytes1 = sve_u8gather_u32(pg32, primaryBitmap, vbyteIdx1);

    haoAlign16(src1, src2, 0, chunk2);
    vkeys2 = haoExtractRaw8Vec(chunk2, hdr->bextMaskRaw, vkeyMask);
    vbyteIdx2 = svlsr_n_u32_x(pg32, vkeys2, 3);
    if (haoRunRaw8(hdr, secondaryHashTable, ruleMeta, literalBlob, a, control,
                   primaryHashTable, blockStart, 0U, chunk0, vkeys0,
                   vbitmapBytes0) == HWLM_TERMINATED) {
        return HWLM_TERMINATED;
    }
    vbitmapBytes2 = sve_u8gather_u32(pg32, primaryBitmap, vbyteIdx2);

    haoAlign16(src1, src2, 8, chunk3);
    vkeys3 = haoExtractRaw8Vec(chunk3, hdr->bextMaskRaw, vkeyMask);
    vbyteIdx3 = svlsr_n_u32_x(pg32, vkeys3, 3);
    if (haoRunRaw8(hdr, secondaryHashTable, ruleMeta, literalBlob, a, control,
                   primaryHashTable, blockStart, 8U, chunk1, vkeys1,
                   vbitmapBytes1) == HWLM_TERMINATED) {
        return HWLM_TERMINATED;
    }
    vbitmapBytes3 = sve_u8gather_u32(pg32, primaryBitmap, vbyteIdx3);

    if (haoRunRaw8(hdr, secondaryHashTable, ruleMeta, literalBlob, a, control,
                   primaryHashTable, blockStart, 16U, chunk2, vkeys2,
                   vbitmapBytes2) == HWLM_TERMINATED) {
        return HWLM_TERMINATED;
    }
    return haoRunRaw8(hdr, secondaryHashTable, ruleMeta, literalBlob, a,
                      control, primaryHashTable, blockStart, 24U, chunk3,
                      vkeys3, vbitmapBytes3);
}

static really_inline
u32 haoRunBextPrimaryFused32Byte(
        const struct HAORuntimeHeader *hdr, const u8 *blockBytes,
        const u8 *primaryBitmap, const u32 *primaryHashTable,
        u32 *activeLaneIndex, u32 *activeEncoded) {
    
    u32 activeCount = 0;
    svuint32_t vkeys2;
    svuint32_t vkeys3;
    svuint32_t vbitPos2;
    svuint32_t vbitPos3;
    svuint32_t vbyteIdx2;
    svuint32_t vbyteIdx3;
    svuint32_t vbitmapBytes2;
    svuint32_t vbitmapBytes3;
            
    const svbool_t pg32 = svptrue_b32();
    const svuint32_t vone = svdup_n_u32(1U);
    const svuint32_t vkeyMask = svdup_n_u32(haoPackedKeyMask(hdr->selectorCount));
    const svuint32_t vlaneBase0 = svindex_u32(0U, 1U);
    const svuint32_t vlaneBase1 = svindex_u32(8U, 1U);
    const svuint32_t vlaneBase2 = svindex_u32(16U, 1U);
    const svuint32_t vlaneBase3 = svindex_u32(24U, 1U);
    const svuint32_t vkeys0 =
        haoExtractBextKeysFixed8Vec(blockBytes, hdr->bextMask, vkeyMask);
    const svuint32_t vkeys1 =
        haoExtractBextKeysFixed8Vec(blockBytes + 8U, hdr->bextMask, vkeyMask);

    const svuint32_t vbitPos0 = svand_n_u32_x(pg32, vkeys0, 7U);
    const svuint32_t vbyteIdx0 = svlsr_n_u32_x(pg32, vkeys0, 3);
    const svuint32_t vbitPos1 = svand_n_u32_x(pg32, vkeys1, 7U);
    const svuint32_t vbyteIdx1 = svlsr_n_u32_x(pg32, vkeys1, 3);

    const svuint32_t vbitmapBytes0 =
        sve_u8gather_u32(pg32, primaryBitmap, vbyteIdx0);
    const svuint32_t vbitmapBytes1 =
        sve_u8gather_u32(pg32, primaryBitmap, vbyteIdx1);

    vkeys2 = haoExtractBextKeysFixed8Vec(blockBytes + 16U, hdr->bextMask,
                                         vkeyMask);
    vbitPos2 = svand_n_u32_x(pg32, vkeys2, 7U);
    vbyteIdx2 = svlsr_n_u32_x(pg32, vkeys2, 3);

    activeCount += haoRetirePrimaryByteVec(
        pg32, vone, primaryHashTable, vkeys0, vbitPos0, vbitmapBytes0,
        vlaneBase0, 0U, 8U, activeLaneIndex + activeCount,
        activeEncoded + activeCount);
    vbitmapBytes2 = sve_u8gather_u32(pg32, primaryBitmap, vbyteIdx2);

    vkeys3 = haoExtractBextKeysFixed8Vec(blockBytes + 24U, hdr->bextMask,
                                         vkeyMask);
    vbitPos3 = svand_n_u32_x(pg32, vkeys3, 7U);
    vbyteIdx3 = svlsr_n_u32_x(pg32, vkeys3, 3);

    activeCount += haoRetirePrimaryByteVec(
        pg32, vone, primaryHashTable, vkeys1, vbitPos1, vbitmapBytes1,
        vlaneBase1, 8U, 8U, activeLaneIndex + activeCount,
        activeEncoded + activeCount);
    vbitmapBytes3 = sve_u8gather_u32(pg32, primaryBitmap, vbyteIdx3);

    activeCount += haoRetirePrimaryByteVec(
        pg32, vone, primaryHashTable, vkeys2, vbitPos2, vbitmapBytes2,
        vlaneBase2, 16U, 8U, activeLaneIndex + activeCount,
        activeEncoded + activeCount);
    activeCount += haoRetirePrimaryByteVec(
        pg32, vone, primaryHashTable, vkeys3, vbitPos3, vbitmapBytes3,
        vlaneBase3, 24U, 8U, activeLaneIndex + activeCount,
        activeEncoded + activeCount);

    return activeCount;
}
#endif

static really_inline
u32 haoProbeCompactAndLoadPrimaryDirect_sve_byte(
        const u8 *bitmap, u32 bitmapSize,
        const u32 *primaryHashTable,
        const u32 *primaryIdx, u32 laneCount,
        u32 *activeLaneIndex, u32 *activeEncoded) {

    u32 activeCount = 0;
    u32 lane        = 0;
    const u32 vl    = (u32)svcntw();
    const u32 vl2   = vl * 2;
    const svbool_t pg = svptrue_b32();
    const svuint32_t vone = svdup_n_u32(1U);

    (void)bitmapSize;

    if (laneCount == HAO_BATCH_MAX_WIDTH && vl == 8U) {
        activeCount += haoProbePrimaryByteFromArray(
            pg, vone, bitmap, primaryHashTable, primaryIdx, 0U, 8U,
            activeLaneIndex + activeCount, activeEncoded + activeCount);
        activeCount += haoProbePrimaryByteFromArray(
            pg, vone, bitmap, primaryHashTable, primaryIdx + 8U, 8U, 8U,
            activeLaneIndex + activeCount, activeEncoded + activeCount);
        activeCount += haoProbePrimaryByteFromArray(
            pg, vone, bitmap, primaryHashTable, primaryIdx + 16U, 16U, 8U,
            activeLaneIndex + activeCount, activeEncoded + activeCount);
        activeCount += haoProbePrimaryByteFromArray(
            pg, vone, bitmap, primaryHashTable, primaryIdx + 24U, 24U, 8U,
            activeLaneIndex + activeCount, activeEncoded + activeCount);

        return activeCount;
    }

    while (lane + vl2 <= laneCount) {
        activeCount += haoProbePrimaryByteFromArray(
            pg, vone, bitmap, primaryHashTable, primaryIdx + lane, lane, vl,
            activeLaneIndex + activeCount, activeEncoded + activeCount);
        activeCount += haoProbePrimaryByteFromArray(
            pg, vone, bitmap, primaryHashTable, primaryIdx + lane + vl,
            lane + vl, vl, activeLaneIndex + activeCount,
            activeEncoded + activeCount);

        lane += vl2;
    }

    while (lane < laneCount) {
        const svbool_t pg_tail = svwhilelt_b32(lane, laneCount);
        activeCount += haoProbePrimaryByteFromArray(
            pg_tail, vone, bitmap, primaryHashTable, primaryIdx + lane, lane,
            laneCount - lane, activeLaneIndex + activeCount,
            activeEncoded + activeCount);
        lane += vl;
    }

    return activeCount;
}

static really_inline
u32 haoProbeCompactAndLoadPrimaryDirect_sve_word(
        const u8 *bitmap, u32 bitmapSize,
        const u32 *primaryHashTable,
        const u32 *primaryIdx, u32 laneCount,
        u32 *activeLaneIndex, u32 *activeEncoded) {
    const u32 *bitmapWords = (const u32 *)bitmap;
    u32 activeCount = 0;
    u32 lane        = 0;
    const u32 vl    = (u32)svcntw();
    const u32 vl2   = vl * 2;
    const svbool_t pg = svptrue_b32();
    const svuint32_t vone = svdup_n_u32(1U);

    (void)bitmapSize;

    while (lane + vl2 <= laneCount) {
        svuint32_t vidx_a     = svld1_u32(pg, primaryIdx + lane);
        svuint32_t vwordIdx_a = svlsr_n_u32_x(pg, vidx_a, 5);
        svuint32_t vbitPos_a  = svand_n_u32_x(pg, vidx_a, 31U);

        svuint32_t vidx_b     = svld1_u32(pg, primaryIdx + lane + vl);
        svuint32_t vwordIdx_b = svlsr_n_u32_x(pg, vidx_b, 5);
        svuint32_t vbitPos_b  = svand_n_u32_x(pg, vidx_b, 31U);

        svuint32_t vbitmapWords_a = svld1_gather_u32index_u32(
                                       pg, bitmapWords, vwordIdx_a);
        svuint32_t vbitmapWords_b = svld1_gather_u32index_u32(
                                       pg, bitmapWords, vwordIdx_b);

        svuint32_t vbitMask_a = svlsl_u32_x(pg, vone, vbitPos_a);
        svuint32_t vhit_a     = svand_u32_x(pg, vbitmapWords_a, vbitMask_a);
        svbool_t   phit_a     = svcmpne_n_u32(pg, vhit_a, 0U);

        svuint32_t vbitMask_b = svlsl_u32_x(pg, vone, vbitPos_b);
        svuint32_t vhit_b     = svand_u32_x(pg, vbitmapWords_b, vbitMask_b);
        svbool_t   phit_b     = svcmpne_n_u32(pg, vhit_b, 0U);

        u32 hitCount_a = (u32)svcntp_b32(pg, phit_a);
        if (hitCount_a > 0) {
            svbool_t pgw_a = svwhilelt_b32((u32)0, hitCount_a);
            svuint32_t vlaneBase_a = svindex_u32(lane, 1U);
            svuint32_t vactiveLn_a = svcompact_u32(phit_a, vlaneBase_a);
            svuint32_t vactiveIdx_a = svcompact_u32(phit_a, vidx_a);
            svuint32_t vactiveEnc_a = svld1_gather_u32index_u32(
                                           pgw_a, primaryHashTable,
                                           vactiveIdx_a);
            svst1_u32(pgw_a, activeLaneIndex + activeCount, vactiveLn_a);
            svst1_u32(pgw_a, activeEncoded + activeCount, vactiveEnc_a);
            activeCount += hitCount_a;
        }

        u32 hitCount_b = (u32)svcntp_b32(pg, phit_b);
        if (hitCount_b > 0) {
            svbool_t pgw_b = svwhilelt_b32((u32)0, hitCount_b);
            svuint32_t vlaneBase_b = svindex_u32(lane + vl, 1U);
            svuint32_t vactiveLn_b = svcompact_u32(phit_b, vlaneBase_b);
            svuint32_t vactiveIdx_b = svcompact_u32(phit_b, vidx_b);
            svuint32_t vactiveEnc_b = svld1_gather_u32index_u32(
                                           pgw_b, primaryHashTable,
                                           vactiveIdx_b);
            svst1_u32(pgw_b, activeLaneIndex + activeCount, vactiveLn_b);
            svst1_u32(pgw_b, activeEncoded + activeCount, vactiveEnc_b);
            activeCount += hitCount_b;
        }

        lane += vl2;
    }

    while (lane < laneCount) {
        svbool_t pg_tail = svwhilelt_b32(lane, laneCount);

        svuint32_t vidx     = svld1_u32(pg_tail, primaryIdx + lane);
        svuint32_t vwordIdx = svlsr_n_u32_x(pg_tail, vidx, 5);
        svuint32_t vbitPos  = svand_n_u32_x(pg_tail, vidx, 31U);

        svuint32_t vbitmapWords = svld1_gather_u32index_u32(
                                      pg_tail, bitmapWords, vwordIdx);
        svuint32_t vbitMask = svlsl_u32_x(pg_tail, vone, vbitPos);
        svuint32_t vhit     = svand_u32_x(pg_tail, vbitmapWords, vbitMask);
        svbool_t   phit     = svcmpne_n_u32(pg_tail, vhit, 0U);

        u32 hitCount = (u32)svcntp_b32(pg_tail, phit);
        if (hitCount > 0) {
            svbool_t pgw = svwhilelt_b32((u32)0, hitCount);
            svuint32_t vlaneBase = svindex_u32(lane, 1U);
            svuint32_t vactiveLane = svcompact_u32(phit, vlaneBase);
            svuint32_t vactiveIdx = svcompact_u32(phit, vidx);
            svuint32_t vactiveEnc = svld1_gather_u32index_u32(
                                        pgw, primaryHashTable, vactiveIdx);
            svst1_u32(pgw, activeLaneIndex + activeCount, vactiveLane);
            svst1_u32(pgw, activeEncoded + activeCount, vactiveEnc);
            activeCount += hitCount;
        }
        lane += vl;
    }

    return activeCount;
}

static really_inline
u32 haoProbeCompactAndLoadPrimaryDirect_sve(
        const u8 *bitmap, u32 bitmapSize,
        const u32 *primaryHashTable,
        const u32 *primaryIdx, u32 laneCount,
        u32 *activeLaneIndex, u32 *activeEncoded) {
#if HAO_SVE_WORD_LEVEL_BITMAP_PROBE
    return haoProbeCompactAndLoadPrimaryDirect_sve_word(
        bitmap, bitmapSize, primaryHashTable, primaryIdx,
        laneCount, activeLaneIndex, activeEncoded);
#else
    return haoProbeCompactAndLoadPrimaryDirect_sve_byte(
        bitmap, bitmapSize, primaryHashTable, primaryIdx,
        laneCount, activeLaneIndex, activeEncoded);
#endif
}
#endif


static really_inline
u32 haoProbeCompactAndLoadPrimaryDirect_scalar(const u8 *bitmap, u32 bitmapSize,
                                        const u32 *primaryHashTable,
                                        const u32 *primaryIdx, u32 laneCount,
                                        u32 *activeLaneIndex,
                                        u32 *activeEncoded) {
    enum { HAO_PRIMARY_BITMAP_CACHE_SLOTS = 32 };
    u32 activeCount = 0;
    u32 lane;
    u32 cachedByteIndex[HAO_PRIMARY_BITMAP_CACHE_SLOTS];
    u8 cachedByteValue[HAO_PRIMARY_BITMAP_CACHE_SLOTS];

    if (!bitmap || !primaryHashTable || !primaryIdx || !activeLaneIndex ||
        !activeEncoded || !laneCount || laneCount > HAO_BATCH_MAX_WIDTH) {
        return 0;
    }

    for (lane = 0; lane < HAO_PRIMARY_BITMAP_CACHE_SLOTS; lane++) {
        cachedByteIndex[lane] = 0xffffffffU;
    }

    for (lane = 0; lane < laneCount; lane++) {
        const u32 idx = primaryIdx[lane];
        const u32 byteIdx = idx >> 3;
        const u8 bit = (u8)(1U << (idx & 7U));
        const u32 cacheSlot =
            (byteIdx ^ (byteIdx >> 5)) & (HAO_PRIMARY_BITMAP_CACHE_SLOTS - 1U);
        u8 bitmapByte;

        if (cachedByteIndex[cacheSlot] != byteIdx) {
            cachedByteIndex[cacheSlot] = byteIdx;
            cachedByteValue[cacheSlot] =
                byteIdx < bitmapSize ? bitmap[byteIdx] : 0;
        }
        bitmapByte = cachedByteValue[cacheSlot];

        if (bitmapByte & bit) {
            activeLaneIndex[activeCount] = lane;
            activeCount++;
        }
    }

    for (lane = 0; lane < activeCount; lane++) {
        const u32 activeLane = activeLaneIndex[lane];
        activeEncoded[lane] = primaryHashTable[primaryIdx[activeLane]];
    }

    return activeCount;
}

static really_inline
u32 haoProbeCompactAndLoadPrimaryDirect(
        const u8 *bitmap, u32 bitmapSize,
        const u32 *primaryHashTable,
        const u32 *primaryIdx, u32 laneCount,
        u32 *activeLaneIndex, u32 *activeEncoded) {

#if defined(__ARM_FEATURE_SVE)
    return haoProbeCompactAndLoadPrimaryDirect_sve(
        bitmap, bitmapSize, primaryHashTable,
        primaryIdx, laneCount, activeLaneIndex, activeEncoded);
#else
    return haoProbeCompactAndLoadPrimaryDirect_scalar(
        bitmap, bitmapSize, primaryHashTable,
        primaryIdx, laneCount, activeLaneIndex, activeEncoded);
#endif
}


static int haoProcessBlockBatch(const struct HAORuntimeHeader *hdr,
                                const struct HAORuntimeBitSelector *selectors,
                                const u8 *primaryBitmap,
                                const u32 *primaryHashTable,
                                const u8 *primaryBitmapRaw,
                                const u32 *primaryHashTableRaw,
                                const struct HAORuntimeSecondaryHashEntry *secondaryHashTable,
                                const struct HAORuntimeRuleMeta *ruleMeta,
                                const u8 *literalBlob,
                                const u32 *residualRuleIndexes,
                                const struct FDR_Runtime_Args *a,
                                hwlm_group_t *control, size_t blockStart,
                                u32 blockLaneCount) {
    struct HAOBlockState block;
    u32 blockKeys[HAO_BATCH_MAX_WIDTH];
    u32 encodedByLane[HAO_BATCH_MAX_WIDTH] = {0};
    u32 activeLaneIndex[HAO_BATCH_MAX_WIDTH];
    u32 activeEncoded[HAO_BATCH_MAX_WIDTH];
    u32 activeCount = 0;
    u32 lane;

    if (!hdr || !selectors || !primaryBitmap || !primaryHashTable ||
        !primaryBitmapRaw || !primaryHashTableRaw ||
        !secondaryHashTable || !ruleMeta || !literalBlob || !a || !control ||
        !blockLaneCount || blockLaneCount > HAO_BATCH_MAX_WIDTH) {
        return HWLM_SUCCESS;
    }

#if HAO_SVE_STREAM32_BYTE && defined(__ARM_FEATURE_SVE) && \
    defined(HS_BUILD_HAVE_SVEBITPERM) && defined(__ARM_FEATURE_SVE2_BITPERM)
    if (haoCanRunRaw32(hdr, a, blockStart, blockLaneCount)) {
        HAO_STATS_ADD(blockCalls, 1);
        HAO_STATS_ADD(blockLanes, HAO_BATCH_MAX_WIDTH);
        HAO_STATS_ADD(primaryProbeLanes, HAO_BATCH_MAX_WIDTH);

#if HAO_SVE_STREAM32_BYTE_V2
        if (haoCanRunRaw32V2(hdr, a, blockStart, blockLaneCount)) {
            return haoRunRaw32V2(hdr, primaryBitmapRaw, primaryHashTableRaw,
                                 secondaryHashTable, ruleMeta, literalBlob, a,
                                 control, blockStart);
        }
#endif
        return haoRunRaw32(hdr, primaryBitmapRaw, primaryHashTableRaw,
                           secondaryHashTable, ruleMeta, literalBlob, a,
                           control, blockStart);
    }
#endif

    if (!haoBuildBlockState(a, blockStart, blockLaneCount, &block)) {
        return HWLM_SUCCESS;
    }

    HAO_STATS_ADD(blockCalls, 1);
    HAO_STATS_ADD(blockLanes, block.laneCount);

    HAO_STATS_ADD(primaryProbeLanes, block.laneCount);
#if defined(__ARM_FEATURE_SVE) && defined(HS_BUILD_HAVE_SVEBITPERM) && \
    defined(__ARM_FEATURE_SVE2_BITPERM)
    if (hdr->extractMode == HAO_RUNTIME_EXTRACT_MODE_BEXT &&
        haoRuntimeCanUseBextFastPath() &&
        hdr->windowBytes == HAO_RUNTIME_BYTES_PER_RULE_SLOT &&
        block.laneCount == HAO_BATCH_MAX_WIDTH &&
        svcntw() == 8U) {
        activeCount = haoRunBextPrimaryFused32Byte(
            hdr, block.blockBytes, primaryBitmap, primaryHashTable,
            activeLaneIndex, activeEncoded);
    } else if (hdr->extractMode == HAO_RUNTIME_EXTRACT_MODE_BEXT) {
        haoExtractBextKeysLocal(hdr, block.blockBytes, block.laneCount,
                                blockKeys);
        activeCount = haoProbeCompactAndLoadPrimaryDirect(
            primaryBitmap, hdr->primaryBitmapSize, primaryHashTable, blockKeys,
            block.laneCount, activeLaneIndex, activeEncoded);
    } else {
        haoExtractKeysFromBlockBytes(hdr, selectors, block.blockBytes,
                                     block.laneCount, blockKeys);
        activeCount = haoProbeCompactAndLoadPrimaryDirect(
            primaryBitmap, hdr->primaryBitmapSize, primaryHashTable, blockKeys,
            block.laneCount, activeLaneIndex, activeEncoded);
    }
#else
    if (hdr->extractMode == HAO_RUNTIME_EXTRACT_MODE_BEXT) {
        haoExtractBextKeysLocal(hdr, block.blockBytes, block.laneCount,
                                blockKeys);
    } else {
        haoExtractKeysFromBlockBytes(hdr, selectors, block.blockBytes,
                                     block.laneCount, blockKeys);
    }
    activeCount = haoProbeCompactAndLoadPrimaryDirect(
        primaryBitmap, hdr->primaryBitmapSize, primaryHashTable, blockKeys,
        block.laneCount, activeLaneIndex, activeEncoded);
#endif
    HAO_STATS_ADD(primaryActiveLanes, activeCount);

    if (!hdr->residualRuleCount) {
        for (lane = 0; lane < activeCount; lane++) {
            const u32 activeLane = activeLaneIndex[lane];
            struct HAOPositionContext ctx;

            haoBuildContextFromBlockState(hdr, a, &block, activeLane, &ctx);
            if (haoProcessEncodedRange(hdr, secondaryHashTable, ruleMeta,
                                       literalBlob, hdr->literalBlobSize, a,
                                       control, &ctx,
                                       activeEncoded[lane]) ==
                HWLM_TERMINATED) {
                return HWLM_TERMINATED;
            }
        }

        return HWLM_SUCCESS;
    }

    for (lane = 0; lane < activeCount; lane++) {
        encodedByLane[activeLaneIndex[lane]] = activeEncoded[lane];
    }

    for (lane = 0; lane < block.laneCount; lane++) {
        if (encodedByLane[lane]) {
            struct HAOPositionContext ctx;

            haoBuildContextFromBlockState(hdr, a, &block, lane, &ctx);
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
                                             block.blockStart + lane) ==
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
    const u8 *cachedPrimaryBitmap;
    const u32 *primaryHashTable;
    const u8 *primaryBitmapRaw;
    const u32 *primaryHashTableRaw;
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
    primaryBitmapRaw = (const u8 *)hdr + hdr->primaryBitmapRawOffset;
    primaryHashTableRaw =
        (const u32 *)((const u8 *)hdr + hdr->primaryRawOffset);
    secondaryHashTable = (const struct HAORuntimeSecondaryHashEntry *)(
        (const u8 *)hdr + hdr->secondaryOffset);
    ruleMeta = (const struct HAORuntimeRuleMeta *)((const u8 *)hdr +
                                                   hdr->ruleMetaOffset);
    literalBlob = (const u8 *)hdr + hdr->literalBlobOffset;
    residualRuleIndexes = (const u32 *)((const u8 *)hdr +
                                        hdr->residualRuleIndexOffset);
    cachedPrimaryBitmap =
        hao_bitmap_cache_get(hdr, primaryBitmap, hdr->primaryBitmapSize);
    
    for (i = a->start_offset; i < a->len; i += HAO_RUNTIME_BLOCK_BYTES) {
        const size_t remaining = a->len - i;
        const u32 blockLaneCount = remaining > HAO_RUNTIME_BLOCK_BYTES
                                       ? HAO_RUNTIME_BLOCK_BYTES
                                       : (u32)remaining;

        if (haoProcessBlockBatch(hdr, selectors,
                                 cachedPrimaryBitmap,
                                 primaryHashTable, primaryBitmapRaw,
                                 primaryHashTableRaw, secondaryHashTable,
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
