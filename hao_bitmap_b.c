#include <arm_sve.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

typedef uint8_t  u8;
typedef uint32_t u32;
typedef uint64_t u64;

#define really_inline __attribute__((always_inline)) inline

#define BITMAP_BYTES_DEFAULT   (512u * 1024u)
#define HASH_TABLE_SZ_DEFAULT  (4u * 1024u * 1024u)
#define LANE_COUNT_DEFAULT     4100u
#define ITERS_DEFAULT          10000000u
#define HIT_RATE_PCT_DEFAULT   3u
#define WARMUP_ITERS_DEFAULT   500u
#define VERIFY_ROUNDS_DEFAULT  32u
#define ALLOC_ALIGN            64u

typedef u32 (*probe_fn_t)(const u8 *bitmap, u32 bitmapSize,
                          const u32 *primaryHashTable,
                          const u32 *primaryIdx, u32 laneCount,
                          u32 *activeLaneIndex, u32 *activeEncoded);

enum bench_mode {
    MODE_BYTE,
    MODE_WORD_OLD,
    MODE_WORD,
    MODE_BOTH,
    MODE_ALL,
    MODE_CHECK
};

struct bench_config {
    u32 bitmapBytes;
    u32 hashTableSize;
    u32 laneCount;
    u32 iterations;
    u32 hitRatePct;
    u32 warmupIters;
    u32 verifyRounds;
    enum bench_mode mode;
};

static really_inline
svuint32_t sve_u8gather_u32(svbool_t pg, const u8 *base,
                            svuint32_t byteIndices) {
    svuint32_t result;
    __asm__ volatile(
        "ld1b {%[res].s}, %[pg]/z, [%[base], %[idx].s, uxtw]"
        : [res] "=w" (result)
        : [pg] "Upa" (pg), [base] "r" (base), [idx] "w" (byteIndices)
        :
    );
    return result;
}

static really_inline
u32 haoProbeCompactAndLoadPrimaryDirect_sve_byte(
        const u8 *bitmap, u32 bitmapSize,
        const u32 *primaryHashTable,
        const u32 *primaryIdx, u32 laneCount,
        u32 *activeLaneIndex, u32 *activeEncoded) {

    u32 activeCount = 0;
    u32 lane = 0;
    const u32 vl = (u32)svcntw();
    const u32 vl2 = vl * 2;
    const svbool_t pg = svptrue_b32();
    const svuint32_t vone = svdup_n_u32(1U);

    (void)bitmapSize;

    while (lane + vl2 <= laneCount) {
        svuint32_t vidx_a = svld1_u32(pg, primaryIdx + lane);
        svuint32_t vbyteIdx_a = svlsr_n_u32_x(pg, vidx_a, 3);
        svuint32_t vbitPos_a = svand_n_u32_x(pg, vidx_a, 7U);

        svuint32_t vidx_b = svld1_u32(pg, primaryIdx + lane + vl);
        svuint32_t vbyteIdx_b = svlsr_n_u32_x(pg, vidx_b, 3);
        svuint32_t vbitPos_b = svand_n_u32_x(pg, vidx_b, 7U);

        svuint32_t vbitmapBytes_a = sve_u8gather_u32(pg, bitmap, vbyteIdx_a);
        svuint32_t vbitmapBytes_b = sve_u8gather_u32(pg, bitmap, vbyteIdx_b);

        svuint32_t vbitMask_a = svlsl_u32_x(pg, vone, vbitPos_a);
        svuint32_t vhit_a = svand_u32_x(pg, vbitmapBytes_a, vbitMask_a);
        svbool_t phit_a = svcmpne_n_u32(pg, vhit_a, 0U);

        svuint32_t vbitMask_b = svlsl_u32_x(pg, vone, vbitPos_b);
        svuint32_t vhit_b = svand_u32_x(pg, vbitmapBytes_b, vbitMask_b);
        svbool_t phit_b = svcmpne_n_u32(pg, vhit_b, 0U);

        svuint32_t vencoded_a =
            svld1_gather_u32index_u32(phit_a, primaryHashTable, vidx_a);
        svuint32_t vlaneBase_a = svindex_u32(lane, 1U);
        svuint32_t vactiveLn_a = svcompact_u32(phit_a, vlaneBase_a);
        svuint32_t vactiveEnc_a = svcompact_u32(phit_a, vencoded_a);

        svuint32_t vencoded_b =
            svld1_gather_u32index_u32(phit_b, primaryHashTable, vidx_b);
        svuint32_t vlaneBase_b = svindex_u32(lane + vl, 1U);
        svuint32_t vactiveLn_b = svcompact_u32(phit_b, vlaneBase_b);
        svuint32_t vactiveEnc_b = svcompact_u32(phit_b, vencoded_b);

        u32 hitCount_a = (u32)svcntp_b32(pg, phit_a);
        if (hitCount_a > 0) {
            svbool_t pgw_a = svwhilelt_b32((u32)0, hitCount_a);
            svst1_u32(pgw_a, activeLaneIndex + activeCount, vactiveLn_a);
            svst1_u32(pgw_a, activeEncoded + activeCount, vactiveEnc_a);
            activeCount += hitCount_a;
        }

        u32 hitCount_b = (u32)svcntp_b32(pg, phit_b);
        if (hitCount_b > 0) {
            svbool_t pgw_b = svwhilelt_b32((u32)0, hitCount_b);
            svst1_u32(pgw_b, activeLaneIndex + activeCount, vactiveLn_b);
            svst1_u32(pgw_b, activeEncoded + activeCount, vactiveEnc_b);
            activeCount += hitCount_b;
        }

        lane += vl2;
    }

    while (lane < laneCount) {
        svbool_t pg_tail = svwhilelt_b32(lane, laneCount);

        svuint32_t vidx = svld1_u32(pg_tail, primaryIdx + lane);
        svuint32_t vbyteIdx = svlsr_n_u32_x(pg_tail, vidx, 3);
        svuint32_t vbitPos = svand_n_u32_x(pg_tail, vidx, 7U);

        svuint32_t vbitmapBytes = sve_u8gather_u32(pg_tail, bitmap, vbyteIdx);

        svuint32_t vbitMask = svlsl_u32_x(pg_tail, vone, vbitPos);
        svuint32_t vhit = svand_u32_x(pg_tail, vbitmapBytes, vbitMask);
        svbool_t phit = svcmpne_n_u32(pg_tail, vhit, 0U);

        svuint32_t vencoded =
            svld1_gather_u32index_u32(phit, primaryHashTable, vidx);
        svuint32_t vlaneBase = svindex_u32(lane, 1U);
        svuint32_t vactiveLane = svcompact_u32(phit, vlaneBase);
        svuint32_t vactiveEnc = svcompact_u32(phit, vencoded);

        u32 hitCount = (u32)svcntp_b32(pg_tail, phit);
        if (hitCount > 0) {
            svbool_t pgw = svwhilelt_b32((u32)0, hitCount);
            svst1_u32(pgw, activeLaneIndex + activeCount, vactiveLane);
            svst1_u32(pgw, activeEncoded + activeCount, vactiveEnc);
            activeCount += hitCount;
        }

        lane += vl;
    }

    return activeCount;
}

static really_inline
u32 haoProbeCompactAndLoadPrimaryDirect_sve_word_old(
        const u8 *bitmap, u32 bitmapSize,
        const u32 *primaryHashTable,
        const u32 *primaryIdx, u32 laneCount,
        u32 *activeLaneIndex, u32 *activeEncoded) {
    const u32 *bitmapWords = (const u32 *)bitmap;
    u32 activeCount = 0;
    u32 lane = 0;
    const u32 vl = (u32)svcntw();
    const u32 vl2 = vl * 2;
    const svbool_t pg = svptrue_b32();
    const svuint32_t vone = svdup_n_u32(1U);

    (void)bitmapSize;

    while (lane + vl2 <= laneCount) {
        svuint32_t vidx_a = svld1_u32(pg, primaryIdx + lane);
        svuint32_t vwordIdx_a = svlsr_n_u32_x(pg, vidx_a, 5);
        svuint32_t vbitPos_a = svand_n_u32_x(pg, vidx_a, 31U);

        svuint32_t vidx_b = svld1_u32(pg, primaryIdx + lane + vl);
        svuint32_t vwordIdx_b = svlsr_n_u32_x(pg, vidx_b, 5);
        svuint32_t vbitPos_b = svand_n_u32_x(pg, vidx_b, 31U);

        svuint32_t vbitmapW_a =
            svld1_gather_u32index_u32(pg, bitmapWords, vwordIdx_a);
        svuint32_t vbitmapW_b =
            svld1_gather_u32index_u32(pg, bitmapWords, vwordIdx_b);

        svuint32_t vhit_a =
            svand_u32_x(pg, vbitmapW_a, svlsl_u32_x(pg, vone, vbitPos_a));
        svuint32_t vhit_b =
            svand_u32_x(pg, vbitmapW_b, svlsl_u32_x(pg, vone, vbitPos_b));

        svbool_t phit_a = svcmpne_n_u32(pg, vhit_a, 0U);
        svbool_t phit_b = svcmpne_n_u32(pg, vhit_b, 0U);

        svuint32_t vencoded_a =
            svld1_gather_u32index_u32(phit_a, primaryHashTable, vidx_a);
        svuint32_t vencoded_b =
            svld1_gather_u32index_u32(phit_b, primaryHashTable, vidx_b);

        svuint32_t vactiveLn_a = svcompact_u32(phit_a, svindex_u32(lane, 1U));
        svuint32_t vactiveEnc_a = svcompact_u32(phit_a, vencoded_a);

        svuint32_t vactiveLn_b =
            svcompact_u32(phit_b, svindex_u32(lane + vl, 1U));
        svuint32_t vactiveEnc_b = svcompact_u32(phit_b, vencoded_b);

        u32 hitCount_a = (u32)svcntp_b32(pg, phit_a);
        if (hitCount_a > 0) {
            svbool_t pgw_a = svwhilelt_b32((u32)0, hitCount_a);
            svst1_u32(pgw_a, activeLaneIndex + activeCount, vactiveLn_a);
            svst1_u32(pgw_a, activeEncoded + activeCount, vactiveEnc_a);
            activeCount += hitCount_a;
        }

        u32 hitCount_b = (u32)svcntp_b32(pg, phit_b);
        if (hitCount_b > 0) {
            svbool_t pgw_b = svwhilelt_b32((u32)0, hitCount_b);
            svst1_u32(pgw_b, activeLaneIndex + activeCount, vactiveLn_b);
            svst1_u32(pgw_b, activeEncoded + activeCount, vactiveEnc_b);
            activeCount += hitCount_b;
        }

        lane += vl2;
    }

    while (lane < laneCount) {
        svbool_t pg_tail = svwhilelt_b32(lane, laneCount);

        svuint32_t vidx = svld1_u32(pg_tail, primaryIdx + lane);
        svuint32_t vwordIdx = svlsr_n_u32_x(pg_tail, vidx, 5);
        svuint32_t vbitPos = svand_n_u32_x(pg_tail, vidx, 31U);

        svuint32_t vbitmapW =
            svld1_gather_u32index_u32(pg_tail, bitmapWords, vwordIdx);
        svuint32_t vhit = svand_u32_x(
            pg_tail, vbitmapW, svlsl_u32_x(pg_tail, vone, vbitPos));
        svbool_t phit = svcmpne_n_u32(pg_tail, vhit, 0U);

        svuint32_t vencoded =
            svld1_gather_u32index_u32(phit, primaryHashTable, vidx);
        svuint32_t vactiveLane = svcompact_u32(phit, svindex_u32(lane, 1U));
        svuint32_t vactiveEnc = svcompact_u32(phit, vencoded);

        u32 hitCount = (u32)svcntp_b32(pg_tail, phit);
        if (hitCount > 0) {
            svbool_t pgw = svwhilelt_b32((u32)0, hitCount);
            svst1_u32(pgw, activeLaneIndex + activeCount, vactiveLane);
            svst1_u32(pgw, activeEncoded + activeCount, vactiveEnc);
            activeCount += hitCount;
        }

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
    u32 lane = 0;
    const u32 vl = (u32)svcntw();
    const u32 vl2 = vl * 2;
    const svbool_t pg = svptrue_b32();
    const svuint32_t vone = svdup_n_u32(1U);

    (void)bitmapSize;

    while (lane + vl2 <= laneCount) {
        svuint32_t vidx_a = svld1_u32(pg, primaryIdx + lane);
        svuint32_t vwordIdx_a = svlsr_n_u32_x(pg, vidx_a, 5);
        svuint32_t vbitPos_a = svand_n_u32_x(pg, vidx_a, 31U);

        svuint32_t vidx_b = svld1_u32(pg, primaryIdx + lane + vl);
        svuint32_t vwordIdx_b = svlsr_n_u32_x(pg, vidx_b, 5);
        svuint32_t vbitPos_b = svand_n_u32_x(pg, vidx_b, 31U);

        svuint32_t vbitmapWords_a =
            svld1_gather_u32index_u32(pg, bitmapWords, vwordIdx_a);
        svuint32_t vbitmapWords_b =
            svld1_gather_u32index_u32(pg, bitmapWords, vwordIdx_b);

        svuint32_t vbitMask_a = svlsl_u32_x(pg, vone, vbitPos_a);
        svuint32_t vhit_a = svand_u32_x(pg, vbitmapWords_a, vbitMask_a);
        svbool_t phit_a = svcmpne_n_u32(pg, vhit_a, 0U);

        svuint32_t vbitMask_b = svlsl_u32_x(pg, vone, vbitPos_b);
        svuint32_t vhit_b = svand_u32_x(pg, vbitmapWords_b, vbitMask_b);
        svbool_t phit_b = svcmpne_n_u32(pg, vhit_b, 0U);

        u32 hitCount_a = (u32)svcntp_b32(pg, phit_a);
        if (hitCount_a > 0) {
            svbool_t pgw_a = svwhilelt_b32((u32)0, hitCount_a);
            svuint32_t vlaneBase_a = svindex_u32(lane, 1U);
            svuint32_t vactiveLn_a = svcompact_u32(phit_a, vlaneBase_a);
            svuint32_t vactiveIdx_a = svcompact_u32(phit_a, vidx_a);
            svuint32_t vactiveEnc_a =
                svld1_gather_u32index_u32(pgw_a, primaryHashTable, vactiveIdx_a);
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
            svuint32_t vactiveEnc_b =
                svld1_gather_u32index_u32(pgw_b, primaryHashTable, vactiveIdx_b);
            svst1_u32(pgw_b, activeLaneIndex + activeCount, vactiveLn_b);
            svst1_u32(pgw_b, activeEncoded + activeCount, vactiveEnc_b);
            activeCount += hitCount_b;
        }

        lane += vl2;
    }

    while (lane < laneCount) {
        svbool_t pg_tail = svwhilelt_b32(lane, laneCount);

        svuint32_t vidx = svld1_u32(pg_tail, primaryIdx + lane);
        svuint32_t vwordIdx = svlsr_n_u32_x(pg_tail, vidx, 5);
        svuint32_t vbitPos = svand_n_u32_x(pg_tail, vidx, 31U);

        svuint32_t vbitmapWords =
            svld1_gather_u32index_u32(pg_tail, bitmapWords, vwordIdx);
        svuint32_t vbitMask = svlsl_u32_x(pg_tail, vone, vbitPos);
        svuint32_t vhit = svand_u32_x(pg_tail, vbitmapWords, vbitMask);
        svbool_t phit = svcmpne_n_u32(pg_tail, vhit, 0U);

        u32 hitCount = (u32)svcntp_b32(pg_tail, phit);
        if (hitCount > 0) {
            svbool_t pgw = svwhilelt_b32((u32)0, hitCount);
            svuint32_t vlaneBase = svindex_u32(lane, 1U);
            svuint32_t vactiveLane = svcompact_u32(phit, vlaneBase);
            svuint32_t vactiveIdx = svcompact_u32(phit, vidx);
            svuint32_t vactiveEnc =
                svld1_gather_u32index_u32(pgw, primaryHashTable, vactiveIdx);
            svst1_u32(pgw, activeLaneIndex + activeCount, vactiveLane);
            svst1_u32(pgw, activeEncoded + activeCount, vactiveEnc);
            activeCount += hitCount;
        }

        lane += vl;
    }

    return activeCount;
}

static u64 now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

static void *xaligned_alloc(size_t size) {
    void *ptr = NULL;
    if (posix_memalign(&ptr, ALLOC_ALIGN, size) != 0) {
        return NULL;
    }
    memset(ptr, 0, size);
    return ptr;
}

static const char *mode_name(enum bench_mode mode) {
    switch (mode) {
    case MODE_BYTE:
        return "byte";
    case MODE_WORD_OLD:
        return "word_old";
    case MODE_WORD:
        return "word";
    case MODE_BOTH:
        return "both";
    case MODE_ALL:
        return "all";
    case MODE_CHECK:
        return "check";
    default:
        return "unknown";
    }
}

static probe_fn_t probe_for_mode(enum bench_mode mode) {
    switch (mode) {
    case MODE_BYTE:
        return haoProbeCompactAndLoadPrimaryDirect_sve_byte;
    case MODE_WORD_OLD:
        return haoProbeCompactAndLoadPrimaryDirect_sve_word_old;
    case MODE_WORD:
        return haoProbeCompactAndLoadPrimaryDirect_sve_word;
    default:
        return NULL;
    }
}

static int parse_u32_arg(const char *text, u32 *out) {
    char *end = NULL;
    unsigned long value;
    if (!text || !*text || !out) {
        return 0;
    }
    value = strtoul(text, &end, 10);
    if (*end != '\0' || value > UINT32_MAX) {
        return 0;
    }
    *out = (u32)value;
    return 1;
}

static int parse_mode(const char *text, enum bench_mode *modeOut) {
    if (!text || !modeOut) {
        return 0;
    }
    if (!strcmp(text, "byte")) {
        *modeOut = MODE_BYTE;
        return 1;
    }
    if (!strcmp(text, "word_old")) {
        *modeOut = MODE_WORD_OLD;
        return 1;
    }
    if (!strcmp(text, "word")) {
        *modeOut = MODE_WORD;
        return 1;
    }
    if (!strcmp(text, "both")) {
        *modeOut = MODE_BOTH;
        return 1;
    }
    if (!strcmp(text, "all")) {
        *modeOut = MODE_ALL;
        return 1;
    }
    if (!strcmp(text, "check")) {
        *modeOut = MODE_CHECK;
        return 1;
    }
    return 0;
}

static void print_usage(const char *argv0) {
    printf("Usage: %s [byte|word_old|word|both|all|check] [iterations] [laneCount] [hitRatePct]\n",
           argv0);
}

static u32 fill_bitmap_random(u8 *bitmap, u32 bitmapBytes, u32 hitRatePct) {
    u32 actualSetBits = 0;
    u32 i;
    for (i = 0; i < bitmapBytes; i++) {
        u8 byte = 0;
        int b;
        for (b = 0; b < 8; b++) {
            if ((u32)(rand() % 100) < hitRatePct) {
                byte |= (u8)(1U << b);
                actualSetBits++;
            }
        }
        bitmap[i] = byte;
    }
    return actualSetBits;
}

static void fill_random_primary_table(u32 *primaryHashTable, u32 hashTableSize) {
    u32 i;
    for (i = 0; i < hashTableSize; i++) {
        primaryHashTable[i] = (u32)rand();
    }
}

static void fill_random_primary_idx(u32 *primaryIdx, u32 laneCount, u32 maxIdx) {
    u32 i;
    for (i = 0; i < laneCount; i++) {
        primaryIdx[i] = (u32)(rand() % maxIdx);
    }
}

static int verify_equivalence(const u8 *bitmap, u32 bitmapBytes,
                              const u32 *primaryHashTable,
                              u32 *primaryIdx, u32 laneCount, u32 maxIdx,
                              u32 verifyRounds,
                              u32 *laneBufA, u32 *encBufA,
                              u32 *laneBufB, u32 *encBufB,
                              u32 *laneBufC, u32 *encBufC) {
    u32 round;

    for (round = 0; round < verifyRounds; round++) {
        u32 countA;
        u32 countB;
        u32 countC;
        fill_random_primary_idx(primaryIdx, laneCount, maxIdx);

        countA = haoProbeCompactAndLoadPrimaryDirect_sve_byte(
            bitmap, bitmapBytes, primaryHashTable, primaryIdx,
            laneCount, laneBufA, encBufA);
        countB = haoProbeCompactAndLoadPrimaryDirect_sve_word_old(
            bitmap, bitmapBytes, primaryHashTable, primaryIdx,
            laneCount, laneBufB, encBufB);
        countC = haoProbeCompactAndLoadPrimaryDirect_sve_word(
            bitmap, bitmapBytes, primaryHashTable, primaryIdx,
            laneCount, laneBufC, encBufC);

        if (countA != countB) {
            fprintf(stderr,
                    "verify failed at round %u: activeCount mismatch byte=%u word_old=%u\n",
                    round, countA, countB);
            return 0;
        }
        if (countA != countC) {
            fprintf(stderr,
                    "verify failed at round %u: activeCount mismatch byte=%u word=%u\n",
                    round, countA, countC);
            return 0;
        }
        if (memcmp(laneBufA, laneBufB, countA * sizeof(u32)) != 0) {
            fprintf(stderr,
                    "verify failed at round %u: activeLaneIndex mismatch for word_old\n",
                    round);
            return 0;
        }
        if (memcmp(encBufA, encBufB, countA * sizeof(u32)) != 0) {
            fprintf(stderr,
                    "verify failed at round %u: activeEncoded mismatch for word_old\n",
                    round);
            return 0;
        }
        if (memcmp(laneBufA, laneBufC, countA * sizeof(u32)) != 0) {
            fprintf(stderr,
                    "verify failed at round %u: activeLaneIndex mismatch for word\n",
                    round);
            return 0;
        }
        if (memcmp(encBufA, encBufC, countA * sizeof(u32)) != 0) {
            fprintf(stderr,
                    "verify failed at round %u: activeEncoded mismatch for word\n",
                    round);
            return 0;
        }
    }

    return 1;
}

static void run_benchmark(const char *label, probe_fn_t probe,
                          const struct bench_config *cfg,
                          const u8 *bitmap, const u32 *primaryHashTable,
                          const u32 *primaryIdx,
                          u32 *activeLaneIndex, u32 *activeEncoded,
                          u32 warmupSeed) {
    u32 w;
    u32 it;
    u64 t0;
    u64 t1;
    u64 totalLanes;
    u64 sink = 0;
    u64 warmupSink = 0;
    double elapsedS;
    double lanesPerSec;
    double throughputGBs;
    double nsPerCall;
    double nsPerLane;
    double realHitPct;

    srand(warmupSeed);
    for (w = 0; w < cfg->warmupIters; w++) {
        warmupSink += probe(bitmap, cfg->bitmapBytes, primaryHashTable,
                            primaryIdx, cfg->laneCount,
                            activeLaneIndex, activeEncoded);
    }

    t0 = now_ns();
    for (it = 0; it < cfg->iterations; it++) {
        sink += probe(bitmap, cfg->bitmapBytes, primaryHashTable,
                      primaryIdx, cfg->laneCount,
                      activeLaneIndex, activeEncoded);
    }
    t1 = now_ns();

    totalLanes = (u64)cfg->iterations * cfg->laneCount;
    elapsedS = (double)(t1 - t0) / 1e9;
    lanesPerSec = (double)totalLanes / elapsedS;
    throughputGBs = lanesPerSec * 4.0 / 1e9;
    nsPerCall = (double)(t1 - t0) / cfg->iterations;
    nsPerLane = (double)(t1 - t0) / (double)totalLanes;
    realHitPct = 100.0 * (double)sink / (double)totalLanes;

    printf("\n=== %s benchmark ===\n", label);
    printf("SVE vector width : %u bits (%u u32/vector)\n",
           (u32)svcntb() * 8, (u32)svcntw());
    printf("total lanes      : %" PRIu64 "\n", totalLanes);
    printf("elapsed          : %.3f ms\n", elapsedS * 1000.0);
    printf("throughput        : %.2f M lanes/sec\n", lanesPerSec / 1e6);
    printf("throughput        : %.2f GB/s (4B/lane)\n", throughputGBs);
    printf("time per call    : %.1f ns (%u lanes)\n", nsPerCall, cfg->laneCount);
    printf("time per lane    : %.3f ns\n", nsPerLane);
    printf("actual hit rate  : %.2f%%\n", realHitPct);
    printf("sink             : %" PRIu64 "\n", sink + warmupSink);
}

int main(int argc, char **argv) {
    struct bench_config cfg;
    u8 *bitmap;
    u32 *primaryHashTable;
    u32 *primaryIdx;
    u32 *laneBufA;
    u32 *encBufA;
    u32 *laneBufB;
    u32 *encBufB;
    u32 *laneBufC;
    u32 *encBufC;
    u32 actualSetBits;
    u32 maxIdx;

    cfg.bitmapBytes = BITMAP_BYTES_DEFAULT;
    cfg.hashTableSize = HASH_TABLE_SZ_DEFAULT;
    cfg.laneCount = LANE_COUNT_DEFAULT;
    cfg.iterations = ITERS_DEFAULT;
    cfg.hitRatePct = HIT_RATE_PCT_DEFAULT;
    cfg.warmupIters = WARMUP_ITERS_DEFAULT;
    cfg.verifyRounds = VERIFY_ROUNDS_DEFAULT;
    cfg.mode = MODE_ALL;

    if (argc >= 2 && !parse_mode(argv[1], &cfg.mode)) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc >= 3 && !parse_u32_arg(argv[2], &cfg.iterations)) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc >= 4 && !parse_u32_arg(argv[3], &cfg.laneCount)) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc >= 5 && !parse_u32_arg(argv[4], &cfg.hitRatePct)) {
        print_usage(argv[0]);
        return 1;
    }

    maxIdx = cfg.bitmapBytes * 8U;
    if (cfg.hashTableSize != maxIdx) {
        fprintf(stderr,
                "hashTableSize (%u) must match bitmap bit count (%u)\n",
                cfg.hashTableSize, maxIdx);
        return 1;
    }

    bitmap = (u8 *)xaligned_alloc(cfg.bitmapBytes);
    primaryHashTable =
        (u32 *)xaligned_alloc((size_t)cfg.hashTableSize * sizeof(u32));
    primaryIdx = (u32 *)xaligned_alloc((size_t)cfg.laneCount * sizeof(u32));
    laneBufA = (u32 *)xaligned_alloc((size_t)cfg.laneCount * sizeof(u32));
    encBufA = (u32 *)xaligned_alloc((size_t)cfg.laneCount * sizeof(u32));
    laneBufB = (u32 *)xaligned_alloc((size_t)cfg.laneCount * sizeof(u32));
    encBufB = (u32 *)xaligned_alloc((size_t)cfg.laneCount * sizeof(u32));
    laneBufC = (u32 *)xaligned_alloc((size_t)cfg.laneCount * sizeof(u32));
    encBufC = (u32 *)xaligned_alloc((size_t)cfg.laneCount * sizeof(u32));

    if (!bitmap || !primaryHashTable || !primaryIdx ||
        !laneBufA || !encBufA || !laneBufB || !encBufB ||
        !laneBufC || !encBufC) {
        fprintf(stderr, "allocation failed\n");
        free(bitmap);
        free(primaryHashTable);
        free(primaryIdx);
        free(laneBufA);
        free(encBufA);
        free(laneBufB);
        free(encBufB);
        free(laneBufC);
        free(encBufC);
        return 1;
    }

    srand(42);
    actualSetBits = fill_bitmap_random(bitmap, cfg.bitmapBytes, cfg.hitRatePct);
    fill_random_primary_table(primaryHashTable, cfg.hashTableSize);
    fill_random_primary_idx(primaryIdx, cfg.laneCount, maxIdx);

    printf("=== config ===\n");
    printf("mode           : %s\n", mode_name(cfg.mode));
    printf("bitmap         : %u KB\n", cfg.bitmapBytes / 1024U);
    printf("hashTable      : %u MB (%u entries)\n",
           (u32)((u64)cfg.hashTableSize * 4U / 1024U / 1024U),
           cfg.hashTableSize);
    printf("max idx        : %u (= hashTableSize: %s)\n",
           maxIdx, maxIdx == cfg.hashTableSize ? "yes" : "no");
    printf("laneCount      : %u\n", cfg.laneCount);
    printf("iterations     : %u\n", cfg.iterations);
    printf("warmup         : %u\n", cfg.warmupIters);
    printf("verifyRounds   : %u\n", cfg.verifyRounds);
    printf("target hitRate : ~%u%%\n", cfg.hitRatePct);
    printf("actual set bits: %u / %u (%.2f%%)\n",
           actualSetBits, maxIdx, 100.0 * actualSetBits / maxIdx);

    printf("\n=== verify ===\n");
    if (!verify_equivalence(bitmap, cfg.bitmapBytes, primaryHashTable,
                            primaryIdx, cfg.laneCount, maxIdx,
                            cfg.verifyRounds,
                            laneBufA, encBufA,
                            laneBufB, encBufB,
                            laneBufC, encBufC)) {
        free(bitmap);
        free(primaryHashTable);
        free(primaryIdx);
        free(laneBufA);
        free(encBufA);
        free(laneBufB);
        free(encBufB);
        free(laneBufC);
        free(encBufC);
        return 2;
    }
    printf("byte/word_old/word outputs match across %u random rounds\n",
           cfg.verifyRounds);

    if (cfg.mode == MODE_CHECK) {
        free(bitmap);
        free(primaryHashTable);
        free(primaryIdx);
        free(laneBufA);
        free(encBufA);
        free(laneBufB);
        free(encBufB);
        free(laneBufC);
        free(encBufC);
        return 0;
    }

    if (cfg.mode == MODE_BYTE || cfg.mode == MODE_BOTH ||
        cfg.mode == MODE_ALL) {
        run_benchmark("byte", probe_for_mode(MODE_BYTE), &cfg,
                      bitmap, primaryHashTable, primaryIdx,
                      laneBufA, encBufA, 1001U);
    }

    if (cfg.mode == MODE_WORD_OLD || cfg.mode == MODE_BOTH ||
        cfg.mode == MODE_ALL) {
        run_benchmark("word_old", probe_for_mode(MODE_WORD_OLD), &cfg,
                      bitmap, primaryHashTable, primaryIdx,
                      laneBufB, encBufB, 2002U);
    }

    if (cfg.mode == MODE_WORD || cfg.mode == MODE_BOTH ||
        cfg.mode == MODE_ALL) {
        run_benchmark("word", probe_for_mode(MODE_WORD), &cfg,
                      bitmap, primaryHashTable, primaryIdx,
                      laneBufC, encBufC, 3003U);
    }

    free(bitmap);
    free(primaryHashTable);
    free(primaryIdx);
    free(laneBufA);
    free(encBufA);
    free(laneBufB);
    free(encBufB);
    free(laneBufC);
    free(encBufC);
    return 0;
}
