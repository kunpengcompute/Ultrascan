/*
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "khsel_core.h"
#include "core_precomp.h"
#include "rose_internal.h"
#include "khsel_runtime.h"
#include "report.h"

#define GET_LO_4(chars) And128(chars, low4bits)
#define GET_HI_4(chars) Rshift8_m128(chars, BYTE_SIZE_FOUR)

static REALLY_INLINE
const char *getLily(const struct RoseEngine *t)
{
    if (!t->lilyOffset) {
        return NULL;
    }

    const char *maskLily = (const char *)t + t->lilyOffset;
    return maskLily;
}

static REALLY_INLINE
u32 *getLilyReportVec(const struct RoseEngine *t)
{
    if (!t->lilyOffset) {
        return NULL;
    }
    u32 *reportVec = (u32 *)((const char *)t + t->lilyOffset + LILY_VEC_LEN * BYTE_SIZE_FOUR);
    return reportVec;
}

static REALLY_INLINE
u32 *getLilyEkeyVec(const struct RoseEngine *t)
{
    if (!t->lilyOffset) {
        return NULL;
    }
    size_t offset = 0x2 * LILY_VEC_LEN * BYTE_SIZE_FOUR;
    u32 *ekeyVec = (u32 *)((const char *)t + t->lilyOffset + offset);
    return ekeyVec;
}

static REALLY_INLINE
int RoseDeliverReport(u64a offset, ReportID onmatch, s32 offset_adjust,
                      struct hs_scratch *scratch, u32 ekey) 
{
    struct core_info *ci = &scratch->core_info;
    u32 flags = 0;
    u64a fromOffset = 0;
    u64a toOffset = offset + offset_adjust;

    int halt = ci->userCallback(onmatch, fromOffset, toOffset, flags,
                                ci->userContext);
    if (halt) {
        ci->status |= SCRATCH_STATUS_TERMINATED;
        return KHSEL_MO_HALT_MATCHING;
    }

    if (ekey != INVALID_EKEY) {
        MarkAsMatched(ci->rose, ci->exhaustionVector, ekey);
        return KHSEL_MO_CONTINUE_MATCHING;
    } else {
        return KHSEL_ROSE_CONTINUE_MATCHING_NO_EXHAUST;
    }
}

static REALLY_INLINE
int lilyMatch(u64a conf, u32 *reportVec, u32 *ekeyVec, const u8 *ptr, hs_scratch_t *scratch)
{
    if (likely(!conf)) {
        return -1;
    }
    const u8 bucket = 8;

    do {
        u32 bit = CTZ64(conf);
        u32 byte = bit / bucket;
        u32 index = bit % bucket;
        conf = conf & (conf - 1);

        size_t i = ptr - scratch->core_info.buf + byte;

        if (ekeyVec[index] == INVALID_EKEY) {
            if (scratch->core_info.userCallback(reportVec[index], 0, i + 1, 0,
                scratch->core_info.userContext) == 1) {
                return 1;
            }
        } else if (!IsExhausted(scratch->core_info.rose,
                scratch->core_info.exhaustionVector, ekeyVec[index])) {
            int ret = RoseDeliverReport(i + 1 ,reportVec[index], 0, scratch, ekeyVec[index]);
            if (ret == 0) {
                return 1;
            }
        }
    } while (unlikely(!!conf));
    return 0;
}

static REALLY_INLINE
int runLily(const char *maskLily, u32 *reportVec, u32 *ekeyVec, hs_scratch_t *scratch)
{
    const size_t step = 16;
    const u8 *buffer = scratch->core_info.buf;
    size_t length = scratch->core_info.len;

    const m128 low4bits = Set16x8(0xf);
    m128 mask_lo = Load128(maskLily);
    m128 mask_hi = Load128(maskLily + step);

    const u8 *itPtr = buffer;

    for (; itPtr + step < buffer + length; itPtr += step) {
        m128 chars = Loadu128(itPtr);
        
        m128 c_lo  = Pshufb_m128_opt(mask_lo, GET_LO_4(chars));
        m128 c_hi  = Pshufb_m128_opt(mask_hi, GET_HI_4(chars));
        m128 rst = And128(c_lo, c_hi);

        u64a conf0 = vgetq_lane_u64(rst.vectU64, 0);
        u64a conf8 = vgetq_lane_u64(rst.vectU64, 1);
        if (lilyMatch(conf0, reportVec, ekeyVec, itPtr, scratch) == KHSEL_MATCHING_TERMINATED || 
            lilyMatch(conf8, reportVec, ekeyVec, itPtr + 8, scratch) == KHSEL_MATCHING_TERMINATED) {
            return KHSEL_MATCHING_TERMINATED;
        }
    }

    u8 zerobuf[16] = {0};
    memcpy(zerobuf, itPtr, length - (itPtr - buffer));
    m128 chars = Loadu128(zerobuf);
    
    m128 c_lo  = Pshufb_m128_opt(mask_lo, GET_LO_4(chars));
    m128 c_hi  = Pshufb_m128_opt(mask_hi, GET_HI_4(chars));
    m128 rst = And128(c_lo, c_hi);

    u64a conf0 = vgetq_lane_u64(rst.vectU64, 0);
    u64a conf8 = vgetq_lane_u64(rst.vectU64, 1);
    if (lilyMatch(conf0, reportVec, ekeyVec, itPtr, scratch) == KHSEL_MATCHING_TERMINATED || 
        lilyMatch(conf8, reportVec, ekeyVec, itPtr + 8, scratch) == 1) {
        return KHSEL_MATCHING_TERMINATED;
    }
    return KHSEL_MATCHING_SUCCESS;
}

hs_error_t KHSEL_LilyRunExec(const struct RoseEngine *rose, hs_scratch_t *scratch)
{
    u32 *reportVec = getLilyReportVec(rose);
    u32 *ekeyVec = getLilyEkeyVec(rose);
    const char *maskLily = getLily(rose);

    hs_error_t ret = runLily(maskLily, reportVec, ekeyVec, scratch);

    return ret;
}
