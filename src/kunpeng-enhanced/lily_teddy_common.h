/*
 * Copyright (c) 2015-2020, Intel Corporation
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef LILY_TEDDY_COMMON_H
#define LILY_TEDDY_COMMON_H

#include "../util/simd_arm.h"

static REALLY_INLINE
m128 prep_conf_teddy_m1(const m128 *maskBase, m128 val) {
    m128 mask = set16x8(0xf);
    m128 lo = and128(val, mask);
    m128 hi = and128(Rshift64_m128(val, 4), mask);
    return or128(Pshufb_m128_opt(maskBase[0 * 2], lo),
                 Pshufb_m128_opt(maskBase[0 * 2 + 1], hi));
}

static REALLY_INLINE
m128 prep_conf_teddy_m2(const m128 *maskBase, m128 *old_1, m128 val) {
    m128 mask = set16x8(0xf);
    m128 lo = and128(val, mask);
    m128 hi = and128(Rshift64_m128(val, 4), mask);
    m128 r = prep_conf_teddy_m1(maskBase, val);

    m128 res_1 = or128(Pshufb_m128_opt(maskBase[1 * 2], lo),
                       Pshufb_m128_opt(maskBase[1 * 2 + 1], hi));
    m128 res_shifted_1 = palignr(res_1, *old_1, 16 - 1);
    *old_1 = res_1;
    return or128(r, res_shifted_1);
}

static REALLY_INLINE
m128 prep_conf_teddy_m3(const m128 *maskBase, m128 *old_1, m128 *old_2,
                        m128 val) {
    m128 mask = set16x8(0xf);
    m128 lo = and128(val, mask);
    m128 hi = and128(Rshift64_m128(val, 4), mask);
    m128 r = prep_conf_teddy_m2(maskBase, old_1, val);

    m128 res_2 = or128(Pshufb_m128_opt(maskBase[2 * 2], lo),
                       Pshufb_m128_opt(maskBase[2 * 2 + 1], hi));
    m128 res_shifted_2 = palignr(res_2, *old_2, 16 - 2);
    *old_2 = res_2;
    return or128(r, res_shifted_2);
}

static REALLY_INLINE
m128 prep_conf_teddy_m4(const m128 *maskBase, m128 *old_1, m128 *old_2,
                        m128 *old_3, m128 val) {
    m128 mask = set16x8(0xf);
    m128 lo = and128(val, mask);
    m128 hi = and128(Rshift64_m128(val, 4), mask);
    m128 r = prep_conf_teddy_m3(maskBase, old_1, old_2, val);

    m128 res_3 = or128(Pshufb_m128_opt(maskBase[3 * 2], lo),
                       Pshufb_m128_opt(maskBase[3 * 2 + 1], hi));
    m128 res_shifted_3 = palignr(res_3, *old_3, 16 - 3);
    *old_3 = res_3;
    return or128(r, res_shifted_3);
}

#define FDR_EXEC_TEDDY_RES_OLD_1

#define FDR_EXEC_TEDDY_RES_OLD_2                                              \
    m128 res_old_1 = zeroes128();

#define FDR_EXEC_TEDDY_RES_OLD_3                                              \
    m128 res_old_1 = zeroes128();                                             \
    m128 res_old_2 = zeroes128();

#define FDR_EXEC_TEDDY_RES_OLD_4                                              \
    m128 res_old_1 = zeroes128();                                             \
    m128 res_old_2 = zeroes128();                                             \
    m128 res_old_3 = zeroes128();

#define FDR_EXEC_TEDDY_RES_OLD(n) FDR_EXEC_TEDDY_RES_OLD_##n

#define FDR_EXEC_TEDDY_RES_OLD_APPLY_PMASK_1(p_mask)

#define FDR_EXEC_TEDDY_RES_OLD_APPLY_PMASK_2(p_mask)                          \
    res_old_1 = or128(res_old_1, p_mask);

#define FDR_EXEC_TEDDY_RES_OLD_APPLY_PMASK_3(p_mask)                          \
    res_old_1 = or128(res_old_1, p_mask);                                     \
    res_old_2 = or128(res_old_2, p_mask);

#define FDR_EXEC_TEDDY_RES_OLD_APPLY_PMASK_4(p_mask)                          \
    res_old_1 = or128(res_old_1, p_mask);                                     \
    res_old_2 = or128(res_old_2, p_mask);                                     \
    res_old_3 = or128(res_old_3, p_mask);

#define FDR_EXEC_TEDDY_RES_OLD_APPLY_PMASK(p_mask, n) FDR_EXEC_TEDDY_RES_OLD_APPLY_PMASK_##n(p_mask)

#define PREP_CONF_FN_1(mask_base, val)                                        \
    prep_conf_teddy_m1(mask_base, val)

#define PREP_CONF_FN_2(mask_base, val)                                        \
    prep_conf_teddy_m2(mask_base, &res_old_1, val)

#define PREP_CONF_FN_3(mask_base, val)                                        \
    prep_conf_teddy_m3(mask_base, &res_old_1, &res_old_2, val)

#define PREP_CONF_FN_4(mask_base, val)                                        \
    prep_conf_teddy_m4(mask_base, &res_old_1, &res_old_2, &res_old_3, val)

#define PREP_CONF_FN(mask_base, val, n)                                       \
    PREP_CONF_FN_##n(mask_base, val)

#endif // LILY_TEDDY_COMMON_H
