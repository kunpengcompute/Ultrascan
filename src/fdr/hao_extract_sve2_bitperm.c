/*
 * Copyright (c) 2026, Huawei Technologies Co., Ltd.
 */

#include "hao_runtime.h"

#if defined(HS_BUILD_HAVE_SVEBITPERM) && defined(__ARM_FEATURE_SVE2_BITPERM)
#include <arm_sve.h>
#include <stdint.h>

u64a haoExtractPackedBitsSveBitPerm(u64a window, u64a mask) {
    const svbool_t pg = svptrue_b64();
    const svuint64_t windowVec = svdup_n_u64((uint64_t)window);
    const svuint64_t packedVec = svbext_n_u64(windowVec, (uint64_t)mask);
    uint64_t lanes[32] = {0};

    svst1_u64(pg, lanes, packedVec);
    return (u64a)lanes[0];
}

u32 haoExtractPackedBitsSveBitPermLaneCount(void) {
    return (u32)svcntd();
}

// void haoExtractPackedBitsSveBitPermBatch(const u64a *windows, u32 count,
//                                          u64a mask, u64a *packedOut) {
//     const u32 lanesPerVec = (u32)svcntd();
//     const svbool_t fullPg = svptrue_b64();
//     u32 i;

//     if (!windows || !packedOut) {
//         return;
//     }

//     for (i = 0; i + lanesPerVec <= count; i += lanesPerVec) {
//         const svuint64_t windowVec =
//             svld1_u64(fullPg, (const uint64_t *)(windows + i));
//         const svuint64_t packedVec = svbext_n_u64(windowVec, (uint64_t)mask);
//         svst1_u64(fullPg, (uint64_t *)(packedOut + i), packedVec);
//     }

//     if (i < count) {
//         const svbool_t tailPg = svwhilelt_b64((uint64_t)i, (uint64_t)count);
//         const svuint64_t windowVec =
//             svld1_u64(tailPg, (const uint64_t *)(windows + i));
//         const svuint64_t packedVec = svbext_n_u64(windowVec, (uint64_t)mask);
//         svst1_u64(tailPg, (uint64_t *)(packedOut + i), packedVec);
//     }
// }

void haoExtractPackedBitsSveBitPermBatch(const u64a *windows, u32 count,
                                             u64a mask, u64a *packedOut) {
    if (!windows || !packedOut) return;

    const u32 vl       = (u32)svcntd();          // SVE 256-bit => 4 lanes
    const svbool_t pg  = svptrue_b64();
    const uint64_t m   = (uint64_t)mask;
    u32 i = 0;

    // ── 4x unroll 主循环 ──────────────────────────────────────
    const u32 unroll4 = (count / (4 * vl)) * (4 * vl);
    for (; i < unroll4; i += 4 * vl) {
        // 4 路并发 load（让 CPU 同时发射多条 load）
        svuint64_t w0 = svld1_u64(pg, (const uint64_t *)(windows + i + 0 * vl));
        svuint64_t w1 = svld1_u64(pg, (const uint64_t *)(windows + i + 1 * vl));
        svuint64_t w2 = svld1_u64(pg, (const uint64_t *)(windows + i + 2 * vl));
        svuint64_t w3 = svld1_u64(pg, (const uint64_t *)(windows + i + 3 * vl));

        // 4 路 BEXT（延迟互相掩盖）
        svuint64_t p0 = svbext_n_u64(w0, m);
        svuint64_t p1 = svbext_n_u64(w1, m);
        svuint64_t p2 = svbext_n_u64(w2, m);
        svuint64_t p3 = svbext_n_u64(w3, m);

        svst1_u64(pg, (uint64_t *)(packedOut + i + 0 * vl), p0);
        svst1_u64(pg, (uint64_t *)(packedOut + i + 1 * vl), p1);
        svst1_u64(pg, (uint64_t *)(packedOut + i + 2 * vl), p2);
        svst1_u64(pg, (uint64_t *)(packedOut + i + 3 * vl), p3);
    }

    // ── 2x unroll 补充 ────────────────────────────────────────
    for (; i + 2 * vl <= count; i += 2 * vl) {
        svuint64_t w0 = svld1_u64(pg, (const uint64_t *)(windows + i + 0 * vl));
        svuint64_t w1 = svld1_u64(pg, (const uint64_t *)(windows + i + 1 * vl));
        svst1_u64(pg, (uint64_t *)(packedOut + i + 0 * vl), svbext_n_u64(w0, m));
        svst1_u64(pg, (uint64_t *)(packedOut + i + 1 * vl), svbext_n_u64(w1, m));
    }

    // ── 1x 整向量 ─────────────────────────────────────────────
    for (; i + vl <= count; i += vl) {
        svuint64_t w = svld1_u64(pg, (const uint64_t *)(windows + i));
        svst1_u64(pg, (uint64_t *)(packedOut + i), svbext_n_u64(w, m));
    }

    // ── 尾部 predicate ────────────────────────────────────────
    if (i < count) {
        svbool_t tail = svwhilelt_b64((uint64_t)i, (uint64_t)count);
        svuint64_t w  = svld1_u64(tail, (const uint64_t *)(windows + i));
        svst1_u64(tail, (uint64_t *)(packedOut + i), svbext_n_u64(w, m));
    }
}

#else

u64a haoExtractPackedBitsSveBitPerm(u64a window, u64a mask) {
    u64a packed = 0;
    u32 outBit;

    for (outBit = 0; mask && outBit < HAO_RUNTIME_MAX_SELECTORS; outBit++) {
        const u64a lowest = mask & (0 - mask);
        if (window & lowest) {
            packed |= ((u64a)1 << outBit);
        }
        mask &= mask - 1;
    }

    return packed;
}

u32 haoExtractPackedBitsSveBitPermLaneCount(void) {
    return 1;
}

void haoExtractPackedBitsSveBitPermBatch(const u64a *windows, u32 count,
                                         u64a mask, u64a *packedOut) {
    u32 i;

    if (!windows || !packedOut) {
        return;
    }

    for (i = 0; i + 3U < count; i += 4U) {
        packedOut[i] = haoExtractPackedBitsSveBitPerm(windows[i], mask);
        packedOut[i + 1U] = haoExtractPackedBitsSveBitPerm(windows[i + 1U], mask);
        packedOut[i + 2U] = haoExtractPackedBitsSveBitPerm(windows[i + 2U], mask);
        packedOut[i + 3U] = haoExtractPackedBitsSveBitPerm(windows[i + 3U], mask);
    }

    for (; i < count; i++) {
        packedOut[i] = haoExtractPackedBitsSveBitPerm(windows[i], mask);
    }
}

#endif

