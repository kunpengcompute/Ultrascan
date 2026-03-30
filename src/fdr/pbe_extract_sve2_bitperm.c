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

#else

u64a pbeExtractPackedBitsSveBitPerm(u64a window, u64a mask) {
    (void)window;
    (void)mask;
    return 0;
}

#endif
