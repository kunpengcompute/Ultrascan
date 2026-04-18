// bench_hao.c
// 编译：gcc -O2 -march=armv8.2-a+sve -o bench_hao bench_hao.c
// 运行：taskset -c 360 ./bench_hao

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <arm_sve.h>

typedef uint8_t  u8;
typedef uint32_t u32;
typedef uint64_t u64;

#define really_inline __attribute__((always_inline)) inline

/* ── 被测函数（原样）── */
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

    while (lane + vl2 <= laneCount) {
        /* 2批 idx 同时加载 */
        svuint32_t vidx_a = svld1_u32(pg, primaryIdx + lane);
        svuint32_t vidx_b = svld1_u32(pg, primaryIdx + lane + vl);

        svuint32_t vbyteIdx_a = svlsr_n_u32_x(pg, vidx_a, 3);
        svuint32_t vbyteIdx_b = svlsr_n_u32_x(pg, vidx_b, 3);

        /* 2条 bitmap gather 同时飞出，让 OOO 引擎并行执行 */
        svuint32_t vbitmapBytes_a = sve_u8gather_u32(pg, bitmap, vbyteIdx_a);
        svuint32_t vbitmapBytes_b = sve_u8gather_u32(pg, bitmap, vbyteIdx_b);

        svuint32_t vbitPos_a = svand_n_u32_x(pg, vidx_a, 7U);
        svuint32_t vbitPos_b = svand_n_u32_x(pg, vidx_b, 7U);

        svuint32_t vhit_a = svand_u32_x(pg, vbitmapBytes_a, svlsl_u32_x(pg, vone, vbitPos_a));
        svuint32_t vhit_b = svand_u32_x(pg, vbitmapBytes_b, svlsl_u32_x(pg, vone, vbitPos_b));

        svbool_t phit_a = svcmpne_n_u32(pg, vhit_a, 0U);
        svbool_t phit_b = svcmpne_n_u32(pg, vhit_b, 0U);

        svuint32_t vencoded_a = svld1_gather_u32index_u32(phit_a, primaryHashTable, vidx_a);
        svuint32_t vencoded_b = svld1_gather_u32index_u32(phit_b, primaryHashTable, vidx_b);

        svuint32_t vactiveLn_a  = svcompact_u32(phit_a, svindex_u32(lane,          1U));
        svuint32_t vactiveLn_b  = svcompact_u32(phit_b, svindex_u32(lane + vl,     1U));

        svuint32_t vactiveEnc_a = svcompact_u32(phit_a, vencoded_a);
        svuint32_t vactiveEnc_b = svcompact_u32(phit_b, vencoded_b);

        u32 hitCount_a = (u32)svcntp_b32(pg, phit_a);
        u32 hitCount_b = (u32)svcntp_b32(pg, phit_b);

        if (hitCount_a > 0) {
            svst1_u32(svwhilelt_b32((u32)0, hitCount_a),
                    activeLaneIndex + activeCount, vactiveLn_a);
            svst1_u32(svwhilelt_b32((u32)0, hitCount_a),
                    activeEncoded   + activeCount, vactiveEnc_a);
            activeCount += hitCount_a;
        }
        if (hitCount_b > 0) {
            svst1_u32(svwhilelt_b32((u32)0, hitCount_b),
                    activeLaneIndex + activeCount, vactiveLn_b);
            svst1_u32(svwhilelt_b32((u32)0, hitCount_b),
                    activeEncoded   + activeCount, vactiveEnc_b);
            activeCount += hitCount_b;
        }

        lane += vl2;
    }

    while (lane < laneCount) {
        svbool_t pg_tail = svwhilelt_b32(lane, laneCount);

        svuint32_t vidx     = svld1_u32(pg_tail, primaryIdx + lane);
        svuint32_t vbyteIdx = svlsr_n_u32_x(pg_tail, vidx, 3);
        svuint32_t vbitPos  = svand_n_u32_x(pg_tail, vidx, 7U);

        svuint32_t vbitmapBytes = sve_u8gather_u32(pg_tail, bitmap, vbyteIdx);

        svuint32_t vbitMask = svlsl_u32_x(pg_tail, vone, vbitPos);
        svuint32_t vhit     = svand_u32_x(pg_tail, vbitmapBytes, vbitMask);
        svbool_t   phit     = svcmpne_n_u32(pg_tail, vhit, 0U);

        svuint32_t vencoded    = svld1_gather_u32index_u32(phit, primaryHashTable, vidx);
        svuint32_t vlaneBase   = svindex_u32(lane, 1U);
        svuint32_t vactiveLane = svcompact_u32(phit, vlaneBase);
        svuint32_t vactiveEnc  = svcompact_u32(phit, vencoded);

        u32 hitCount = (u32)svcntp_b32(pg_tail, phit);
        if (hitCount > 0) {
            svbool_t pgw = svwhilelt_b32((u32)0, hitCount);
            svst1_u32(pgw, activeLaneIndex + activeCount, vactiveLane);
            svst1_u32(pgw, activeEncoded   + activeCount, vactiveEnc);
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
    u32 lane        = 0;
    const u32 vl    = (u32)svcntw();
    const u32 vl2   = vl * 2;
    const svbool_t pg = svptrue_b32();
    const svuint32_t vone = svdup_n_u32(1U);
    (void)bitmapSize;

    while (lane + vl2 <= laneCount) {
        svuint32_t vidx_a     = svld1_u32(pg, primaryIdx + lane);
        svuint32_t vwordIdx_a = svlsr_n_u32_x(pg, vidx_a, 5);   // idx/32
        svuint32_t vbitPos_a  = svand_n_u32_x(pg, vidx_a, 31U); // idx%32

        svuint32_t vidx_b     = svld1_u32(pg, primaryIdx + lane + vl);
        svuint32_t vwordIdx_b = svlsr_n_u32_x(pg, vidx_b, 5);
        svuint32_t vbitPos_b  = svand_n_u32_x(pg, vidx_b, 31U);

        // ★ bitmap gather：u32 版本，索引单位是 4 字节
        svuint32_t vbitmapW_a = svld1_gather_u32index_u32(pg, bitmapWords, vwordIdx_a);
        svuint32_t vbitmapW_b = svld1_gather_u32index_u32(pg, bitmapWords, vwordIdx_b);

        svuint32_t vhit_a = svand_u32_x(pg, vbitmapW_a, svlsl_u32_x(pg, vone, vbitPos_a));
        svuint32_t vhit_b = svand_u32_x(pg, vbitmapW_b, svlsl_u32_x(pg, vone, vbitPos_b));

        svbool_t phit_a = svcmpne_n_u32(pg, vhit_a, 0U);
        svbool_t phit_b = svcmpne_n_u32(pg, vhit_b, 0U);

        // ★ hashTable gather：和原始版本完全一致，用 phit 过滤
        svuint32_t vencoded_a = svld1_gather_u32index_u32(phit_a, primaryHashTable, vidx_a);
        svuint32_t vencoded_b = svld1_gather_u32index_u32(phit_b, primaryHashTable, vidx_b);

        svuint32_t vactiveLn_a  = svcompact_u32(phit_a, svindex_u32(lane,      1U));
        svuint32_t vactiveEnc_a = svcompact_u32(phit_a, vencoded_a);

        svuint32_t vactiveLn_b  = svcompact_u32(phit_b, svindex_u32(lane + vl, 1U));
        svuint32_t vactiveEnc_b = svcompact_u32(phit_b, vencoded_b);

        u32 hitCount_a = (u32)svcntp_b32(pg, phit_a);
        if (hitCount_a > 0) {
            svbool_t pgw_a = svwhilelt_b32((u32)0, hitCount_a);
            svst1_u32(pgw_a, activeLaneIndex + activeCount, vactiveLn_a);
            svst1_u32(pgw_a, activeEncoded   + activeCount, vactiveEnc_a);
            activeCount += hitCount_a;
        }

        u32 hitCount_b = (u32)svcntp_b32(pg, phit_b);
        if (hitCount_b > 0) {
            svbool_t pgw_b = svwhilelt_b32((u32)0, hitCount_b);
            svst1_u32(pgw_b, activeLaneIndex + activeCount, vactiveLn_b);
            svst1_u32(pgw_b, activeEncoded   + activeCount, vactiveEnc_b);
            activeCount += hitCount_b;
        }

        lane += vl2;
    }

    while (lane < laneCount) {
        svbool_t pg_tail = svwhilelt_b32(lane, laneCount);

        svuint32_t vidx     = svld1_u32(pg_tail, primaryIdx + lane);
        svuint32_t vwordIdx = svlsr_n_u32_x(pg_tail, vidx, 5);
        svuint32_t vbitPos  = svand_n_u32_x(pg_tail, vidx, 31U);

        svuint32_t vbitmapW = svld1_gather_u32index_u32(pg_tail, bitmapWords, vwordIdx);
        svuint32_t vhit     = svand_u32_x(pg_tail, vbitmapW,
                                  svlsl_u32_x(pg_tail, vone, vbitPos));
        svbool_t   phit     = svcmpne_n_u32(pg_tail, vhit, 0U);

        svuint32_t vencoded    = svld1_gather_u32index_u32(phit, primaryHashTable, vidx);
        svuint32_t vactiveLane = svcompact_u32(phit, svindex_u32(lane, 1U));
        svuint32_t vactiveEnc  = svcompact_u32(phit, vencoded);

        u32 hitCount = (u32)svcntp_b32(pg_tail, phit);
        if (hitCount > 0) {
            svbool_t pgw = svwhilelt_b32((u32)0, hitCount);
            svst1_u32(pgw, activeLaneIndex + activeCount, vactiveLane);
            svst1_u32(pgw, activeEncoded   + activeCount, vactiveEnc);
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
/* ── 计时 ── */
static u64 now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

int main(void) {
    /* ════════════════════════════════════════
     * 参数：bitmap=512KB，hashTable=16MB，命中率5%
     * ════════════════════════════════════════ */
    const u32 BITMAP_BYTES  = 512u * 1024u;        /* 512 KB → L2 压线 */
    const u32 HASH_TABLE_SZ = 4u * 1024u * 1024u;  /* 4M 条目 × 4B = 16 MB → L3 */
    const u32 LANE_COUNT    = 4100;
    const u32 ITERS         = 10000000;
    const u32 HIT_RATE_PCT  = 3;
    const u32 MAX_IDX       = BITMAP_BYTES * 8;    /* 4194304，与 HASH_TABLE_SZ 对齐 */

    printf("=== 参数确认 ===\n");
    printf("bitmap        : %u KB\n", BITMAP_BYTES / 1024);
    printf("hashTable     : %u MB (%u 条目)\n",
           (u32)((u64)HASH_TABLE_SZ * 4 / 1024 / 1024), HASH_TABLE_SZ);
    printf("MAX_IDX       : %u  (= HASH_TABLE_SZ: %s)\n",
           MAX_IDX, MAX_IDX == HASH_TABLE_SZ ? "对齐" : "不对齐");
    printf("laneCount     : %u\n", LANE_COUNT);
    printf("iterations    : %u\n", ITERS);
    printf("hit rate      : ~%u%%\n\n", HIT_RATE_PCT);

    /* ── 分配 ── */
    u8  *bitmap           = (u8  *)malloc(BITMAP_BYTES);
    u32 *primaryHashTable = (u32 *)malloc((u64)HASH_TABLE_SZ * sizeof(u32));
    u32 *primaryIdx       = (u32 *)malloc(LANE_COUNT * sizeof(u32));
    u32 *activeLaneIndex  = (u32 *)malloc(LANE_COUNT * sizeof(u32));
    u32 *activeEncoded    = (u32 *)malloc(LANE_COUNT * sizeof(u32));

    if (!bitmap || !primaryHashTable || !primaryIdx ||
        !activeLaneIndex || !activeEncoded) {
        fprintf(stderr, "malloc failed (需要约 %.1f MB)\n",
                (BITMAP_BYTES + (u64)HASH_TABLE_SZ * 4) / 1024.0 / 1024.0);
        return 1;
    }

    /* ── 初始化 ── */
    srand(42);

    /* bitmap：按 HIT_RATE_PCT 设置 bit */
    u32 actual_set_bits = 0;
    for (u32 i = 0; i < BITMAP_BYTES; i++) {
        u8 byte = 0;
        for (int b = 0; b < 8; b++) {
            if ((u32)(rand() % 100) < HIT_RATE_PCT) {
                byte |= (u8)(1U << b);
                actual_set_bits++;
            }
        }
        bitmap[i] = byte;
    }
    printf("bitmap 实际命中 bit 数: %u / %u (%.1f%%)\n",
           actual_set_bits, MAX_IDX,
           100.0 * actual_set_bits / MAX_IDX);

    /* primaryHashTable：随机填充，用 memset 快速初始化 */
    for (u32 i = 0; i < HASH_TABLE_SZ; i++)
        primaryHashTable[i] = (u32)rand();

    /* primaryIdx：均匀随机散布在 [0, MAX_IDX)，模拟随机 gather */
    for (u32 i = 0; i < LANE_COUNT; i++)
        primaryIdx[i] = (u32)(rand() % MAX_IDX);

    /* ── 预热（跑 500 次，让 bitmap 进 L2，hashTable 进 L3）── */
    u32 warmup_sink = 0;
    for (u32 w = 0; w < 500; w++) {
        warmup_sink += haoProbeCompactAndLoadPrimaryDirect_sve_word(
            bitmap, BITMAP_BYTES, primaryHashTable,
            primaryIdx, LANE_COUNT, activeLaneIndex, activeEncoded);
    }

    /* ── 正式计时 ── */
    u64 t0 = now_ns();
    u32 sink = 0;

    for (u32 it = 0; it < ITERS; it++) {
        sink += haoProbeCompactAndLoadPrimaryDirect_sve_word(
                    bitmap, BITMAP_BYTES, primaryHashTable,
                    primaryIdx, LANE_COUNT,
                    activeLaneIndex, activeEncoded);
    }

    u64 t1 = now_ns();
    u64 elapsed_ns = t1 - t0;

    /* ── 输出 ── */
    u64    total_lanes     = (u64)ITERS * LANE_COUNT;
    double elapsed_s       = (double)elapsed_ns / 1e9;
    double lanes_per_sec   = (double)total_lanes / elapsed_s;
    double throughput_gbps = lanes_per_sec * 4.0 / 1e9;
    double ns_per_call     = (double)elapsed_ns / ITERS;
    double ns_per_lane     = (double)elapsed_ns / (double)total_lanes;

    /* 预估实际命中率（sink = 总命中 lane 数）*/
    double real_hit_pct = 100.0 * (double)sink / (double)total_lanes;

    printf("\n=== benchmark 结果 ===\n");
    printf("SVE 向量宽度       : %u bits (%u u32/vector)\n",
           (u32)svcntb() * 8, (u32)svcntw());
    printf("----------------------------------------------\n");
    printf("总 lane 数         : %llu\n", (unsigned long long)total_lanes);
    printf("耗时               : %.3f ms\n", elapsed_s * 1000.0);
    printf("吞吐量             : %.2f M lanes/sec\n", lanes_per_sec / 1e6);
    printf("吞吐量             : %.2f GB/s (4B/lane)\n", throughput_gbps);
    printf("每次调用耗时       : %.1f ns (%u lanes)\n", ns_per_call, LANE_COUNT);
    printf("每 lane 耗时       : %.3f ns\n", ns_per_lane);
    printf("实际命中率         : %.2f%%\n", real_hit_pct);
    printf("sink (防优化)      : %u\n", sink + warmup_sink);

    free(bitmap);
    free(primaryHashTable);
    free(primaryIdx);
    free(activeLaneIndex);
    free(activeEncoded);
    return 0;
}