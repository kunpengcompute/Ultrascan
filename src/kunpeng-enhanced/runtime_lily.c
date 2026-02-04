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
#include "hs_common.h"
#include "hs_compile.h"

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
u8 getLilyQuietFlags(const struct RoseEngine *t)
{
    if (!t->lilyOffset) {
        return 0;
    }
    size_t offset = 0x2 * LILY_VEC_LEN * BYTE_SIZE_FOUR;
    u8 flagsQuiet = *((u8 *)((const char *)t + t->lilyOffset + offset + LILY_VEC_LEN * BYTE_SIZE_FOUR));
    return flagsQuiet;
}

REALLY_INLINE
int packLilyItem(uint8_t index, u64a toOffset, LilyMatchItem *out_item) {
    if (index >= 8U || (unsigned long long)toOffset > LILY_TO_OFFSET_MAX) {
        return HS_INVALID;
    }
    out_item->onmatch_index = index;
    out_item->toOffset = (unsigned long long)toOffset;
    return 0;
}

REALLY_INLINE
u64a unpackToOffset(const LilyMatchItem *item) {
    return (u64a)item->toOffset;
}

REALLY_INLINE
void initLilyItems(hs_scratch_t *scratch) {
    if (!scratch) {
        return;
    }
    scratch->lily_items_size = 0;
    scratch->lily_items_start = 0;
}

REALLY_INLINE
int pushLilyItems(hs_scratch_t *scratch, const LilyMatchItem *item) {
    if (!scratch || !item || !scratch->lily_items || scratch->lily_items_capacity == 0) {
        return HS_INVALID;
    }

    if (scratch->lily_items_size >= scratch->lily_items_capacity) {
        return HS_INSUFFICIENT_SPACE; // 空间不足直接返回报错，不插入新项
    }

    // 尾插, O(1)
    scratch->lily_items[scratch->lily_items_size++] = *item;
    return 0;
}

/**
 * @brief 单条Lily匹配项上报
 * @param scratch 扫描上下文
 * @param item 待上报的Lily匹配项
 * @return 0=继续上报，1=触发终止（userCallback返回1）
 */
static REALLY_INLINE
int lilyItemReport(hs_scratch_t *scratch, const LilyMatchItem *item, u32 *reportVec) {
    uint8_t index = item->onmatch_index;
    ReportID onmatch = reportVec[index];
    u64a toOffset = unpackToOffset(item);

    int halt = scratch->core_info.userCallback(onmatch, 0, toOffset, 0, scratch->core_info.userContext);
    if (halt) {
        // 标记终止状态，与原生逻辑对齐
        scratch->core_info.status |= SCRATCH_STATUS_TERMINATED;
        return 1;
    }

    return 0;
}

/**
 * @brief 上报指定范围的Lily匹配项（核心函数，仅一个入口）
 * @param scratch 扫描上下文
 * @param to_offset 筛选条件：
 *        - ALL_LILY_MATCH_ITEMS：上报所有项并释放内存
 *        - 具体值：仅上报toOffset <= to_offset的项，保留其余项
 * @return 0=上报完成，1=触发终止
 */
REALLY_INLINE
int flushStoredLilyMatches(hs_scratch_t *scratch, u64a to_offset) {
    // 无未上报项直接返回
    if (!scratch || scratch->lily_items == NULL || scratch->lily_items_start >= scratch->lily_items_size) {
        return 0;
    }

    int halt = 0;
    u32 *reportVec = getLilyReportVec(scratch->core_info.rose);
    struct LilyMatchItem *item = NULL;
    // 从lily_items_start开始遍历
    size_t i = scratch->lily_items_start;

    // 遍历筛选并上报（已预先确保lily_items有序，这里是连续内存遍历，无冗余拷贝）
    for (; i < scratch->lily_items_size && !halt; i++) {
        item = &scratch->lily_items[i];
        // 筛选逻辑：上报所有 或 上报 <= to_offset的项
        if (likely(item->toOffset <= to_offset)) {
            halt = lilyItemReport(scratch, item, reportVec);
        } else {
            // 有序数组特性：当前项>目标值，后续都>，直接终止遍历
            break;
        }
    }

    // 更新未上报起始位置（无论是否终止/提前退出）
    scratch->lily_items_start = i;

    return halt;
}

static REALLY_INLINE
int RoseDeliverReport(u64a offset, uint8_t index, s32 offset_adjust,
                      struct hs_scratch *scratch, u32 ekey) 
{
    struct core_info *ci = &scratch->core_info;
    u64a toOffset = offset + offset_adjust;

    LilyMatchItem item = {0};
    int pack_ret = packLilyItem(index, toOffset, &item);
    if (pack_ret != 0) {
        ci->status |= SCRATCH_STATUS_TERMINATED;
        return KHSEL_MO_HALT_MATCHING;
    }

    // 暂存
    int ret = pushLilyItems(scratch, &item);
    if (ret != 0) { // 暂存失败，停止后续匹配
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
int lilyMatch(u64a conf, u32 *ekeyVec, u8 flagsQuiet, const u8 *ptr, hs_scratch_t *scratch)
{
    if (likely(!conf)) {
        return -1;
    }
    const u8 bucket = 8;
    size_t i = 0;

    do {
        u32 bit = CTZ64(conf);
        u32 byte = bit / bucket;
        u32 index = bit % bucket;
        conf = conf & (conf - 1);

        // 只有当规则不是quiet模式时才上报
        if ((ekeyVec[index] == INVALID_EKEY ||
            !IsExhausted(scratch->core_info.rose, scratch->core_info.exhaustionVector, ekeyVec[index])) &&
            !(flagsQuiet & (1 << index))) {
            i = scratch->core_info.buf_offset + ptr - scratch->core_info.buf + byte;
            int ret = RoseDeliverReport(i + 1 , index, 0, scratch, ekeyVec[index]);
            if (ret == 0) { // 上报异常，终止后续操作
                return 1;
            }
        }
    } while (unlikely(!!conf));
    return 0;
}

static REALLY_INLINE
int runLily(const char *maskLily, u32 *ekeyVec, u8 flagsQuiet, hs_scratch_t *scratch)
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
        if (lilyMatch(conf0, ekeyVec, flagsQuiet, itPtr, scratch) == KHSEL_MATCHING_TERMINATED ||
            lilyMatch(conf8, ekeyVec, flagsQuiet, itPtr + 8, scratch) == KHSEL_MATCHING_TERMINATED) {
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
    if (lilyMatch(conf0, ekeyVec, flagsQuiet, itPtr, scratch) == KHSEL_MATCHING_TERMINATED ||
        lilyMatch(conf8, ekeyVec, flagsQuiet, itPtr + 8, scratch) == 1) {
        return KHSEL_MATCHING_TERMINATED;
    }
    return KHSEL_MATCHING_SUCCESS;
}

hs_error_t KHSEL_LilyRunExec(const struct RoseEngine *rose, hs_scratch_t *scratch)
{
    u32 *ekeyVec = getLilyEkeyVec(rose);
    u8 flagsQuiet = getLilyQuietFlags(rose);
    const char *maskLily = getLily(rose);

    initLilyItems(scratch);

    hs_error_t ret = runLily(maskLily, ekeyVec, flagsQuiet, scratch);

    // 初始化未上报起始位置（保证其他算法首次flush从0开始）
    scratch->lily_items_start = 0;

    return ret;
}