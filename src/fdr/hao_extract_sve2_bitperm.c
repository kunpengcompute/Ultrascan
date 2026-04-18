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

void haoExtractPackedBitsSveBitPermBatchToKeys(const u64a *windows, u32 count,
                                               u64a mask, u32 keyMask,
                                               u32 *keysOut) {
    const u32 lanes64 = (u32)svcntd();
    const u32 lanes32 = (u32)svcntw();
    const u32 step = lanes64 * 2U;
    const u32 step2 = step * 2U;
    const svbool_t pg64 = svptrue_b64();
    const svbool_t pg32 = svptrue_b32();
    const svuint32_t vkeyMask = svdup_n_u32(keyMask);
    const uint64_t m = (uint64_t)mask;
    u32 i = 0;

    if (!windows || !keysOut) {
        return;
    }

    if (lanes64 * 2U != lanes32) {
        for (; i < count; i++) {
            keysOut[i] = (u32)(haoExtractPackedBitsSveBitPerm(windows[i], mask) &
                               keyMask);
        }
        return;
    }

    for (; i + step2 <= count; i += step2) {
        const svuint64_t w0 =
            svld1_u64(pg64, (const uint64_t *)(windows + i));
        const svuint64_t w1 =
            svld1_u64(pg64, (const uint64_t *)(windows + i + lanes64));
        const svuint64_t w2 =
            svld1_u64(pg64, (const uint64_t *)(windows + i + step));
        const svuint64_t w3 =
            svld1_u64(pg64, (const uint64_t *)(windows + i + step + lanes64));
        const svuint64_t p0 = svbext_n_u64(w0, m);
        const svuint64_t p1 = svbext_n_u64(w1, m);
        const svuint64_t p2 = svbext_n_u64(w2, m);
        const svuint64_t p3 = svbext_n_u64(w3, m);
        svuint32_t keys0 =
            svuzp1_u32(svreinterpret_u32_u64(p0), svreinterpret_u32_u64(p1));
        svuint32_t keys1 =
            svuzp1_u32(svreinterpret_u32_u64(p2), svreinterpret_u32_u64(p3));
        keys0 = svand_u32_x(pg32, keys0, vkeyMask);
        keys1 = svand_u32_x(pg32, keys1, vkeyMask);
        svst1_u32(pg32, (uint32_t *)(keysOut + i), keys0);
        svst1_u32(pg32, (uint32_t *)(keysOut + i + step), keys1);
    }

    for (; i + step <= count; i += step) {
        const svuint64_t w0 =
            svld1_u64(pg64, (const uint64_t *)(windows + i));
        const svuint64_t w1 =
            svld1_u64(pg64, (const uint64_t *)(windows + i + lanes64));
        const svuint64_t p0 = svbext_n_u64(w0, m);
        const svuint64_t p1 = svbext_n_u64(w1, m);
        svuint32_t keys =
            svuzp1_u32(svreinterpret_u32_u64(p0), svreinterpret_u32_u64(p1));
        keys = svand_u32_x(pg32, keys, vkeyMask);
        svst1_u32(pg32, (uint32_t *)(keysOut + i), keys);
    }

    for (; i < count; i++) {
        keysOut[i] = (u32)(haoExtractPackedBitsSveBitPerm(windows[i], mask) &
                           keyMask);
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

void haoExtractPackedBitsSveBitPermBatchToKeys(const u64a *windows, u32 count,
                                               u64a mask, u32 keyMask,
                                               u32 *keysOut) {
    u32 i;

    if (!windows || !keysOut) {
        return;
    }

    for (i = 0; i < count; i++) {
        keysOut[i] = (u32)(haoExtractPackedBitsSveBitPerm(windows[i], mask) &
                           keyMask);
    }
}

#endif
