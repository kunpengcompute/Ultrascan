/*
 * Copyright (c) 2026, Huawei Technologies Co., Ltd.
 */

#include "pbe_runtime.h"

#if defined(HS_BUILD_HAVE_SVEBITPERM) && defined(__ARM_FEATURE_SVE2_BITPERM)
#include <arm_sve.h>
#include <stdint.h>

u64a pbeExtractPackedBitsSveBitPerm(u64a window, u64a mask) {
    const svbool_t pg = svptrue_b64();
    const svuint64_t windowVec = svdup_n_u64((uint64_t)window);
    const svuint64_t packedVec = svbext_n_u64(windowVec, (uint64_t)mask);
    uint64_t lanes[32] = {0};

    svst1_u64(pg, lanes, packedVec);
    return (u64a)lanes[0];
}

u32 pbeExtractPackedBitsSveBitPermLaneCount(void) {
    return (u32)svcntd();
}

void pbeExtractPackedBitsSveBitPermBatch(const u64a *windows, u32 count,
                                         u64a mask, u64a *packedOut) {
    u32 i = 0;

    if (!windows || !packedOut) {
        return;
    }

    while (i < count) {
        const svbool_t pg = svwhilelt_b64((uint64_t)i, (uint64_t)count);
        const svuint64_t windowVec =
            svld1_u64(pg, (const uint64_t *)(windows + i));
        const svuint64_t packedVec = svbext_n_u64(windowVec, (uint64_t)mask);
        svst1_u64(pg, (uint64_t *)(packedOut + i), packedVec);
        i += svcntd();
    }
}

#else

u64a pbeExtractPackedBitsSveBitPerm(u64a window, u64a mask) {
    u64a packed = 0;
    u32 outBit = 0;

    while (mask && outBit < PBE_RUNTIME_MAX_SELECTORS) {
        const u64a lowest = mask & (0 - mask);
        if (window & lowest) {
            packed |= ((u64a)1 << outBit);
        }
        mask &= mask - 1;
        outBit++;
    }

    return packed;
}

u32 pbeExtractPackedBitsSveBitPermLaneCount(void) {
    return 1;
}

void pbeExtractPackedBitsSveBitPermBatch(const u64a *windows, u32 count,
                                         u64a mask, u64a *packedOut) {
    u32 i;

    if (!windows || !packedOut) {
        return;
    }

    for (i = 0; i < count; i++) {
        packedOut[i] = pbeExtractPackedBitsSveBitPerm(windows[i], mask);
    }
}

#endif
