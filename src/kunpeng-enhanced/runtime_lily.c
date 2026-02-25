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
void initLilyItems(hs_scratch_t *scratch) {
    if (!scratch) {
        return;
    }
    scratch->lily_ctx.size = 0;
    scratch->lily_ctx.start = 0;
}

REALLY_INLINE
void initLilyForTeddyItems(hs_scratch_t *scratch) {
    if (!scratch) {
        return;
    }
    scratch->lily_for_teddy_ctx.size = 0;
    scratch->lily_for_teddy_ctx.start = 0;
}

REALLY_INLINE
int pushLilyItems(const LilyMatchItem *item, LilyEngineCtx *ctx) {
    if (!ctx || !item || !ctx->items || ctx->capacity == 0) {
        return HS_INVALID;
    }

    if (ctx->size >= ctx->capacity) {
        return HS_INSUFFICIENT_SPACE; // 空间不足直接返回报错，不插入新项
    }

    // 尾插, O(1)
    ctx->items[ctx->size++] = *item;
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
    u64a toOffset = (u64a)item->toOffset;

    int halt = scratch->core_info.userCallback(onmatch, 0, toOffset, 0, scratch->core_info.userContext);
    if (halt) {
        // 标记终止状态，与原生逻辑对齐
        scratch->core_info.status |= SCRATCH_STATUS_TERMINATED;
        return 1;
    }

    return 0;
}

// ===================== REPORT_RANGE（非交叉块批量上报） =====================
#define REPORT_RANGE(SCRATCH, CTX, END, HALT, REPORT_VEC) \
do {                                                      \
    for (; (CTX)->start < (END) && !(HALT); (CTX)->start++) { \
        (HALT) = lilyItemReport((SCRATCH), &(CTX)->items[(CTX)->start], (REPORT_VEC)); \
    }                                                     \
} while (0)

// ===================== REPORT_RANGE_WITH_OFFSET_CHECK（剩余元素上报） =====================
#define REPORT_RANGE_WITH_OFFSET_CHECK(SCRATCH, CTX, END, TO_OFFSET, HALT, REPORT_VEC) \
do {                                                                                  \
    for (; (CTX)->start < (END) && !(HALT); (CTX)->start++) {                         \
        if (likely((u64a)(CTX)->items[(CTX)->start].toOffset <= (TO_OFFSET))) {      \
            (HALT) = lilyItemReport((SCRATCH), &(CTX)->items[(CTX)->start], (REPORT_VEC)); \
        } else {                                                                       \
            break; /* 升序特性：当前项>阈值，后续均>，终止遍历 */                      \
        }                                                                              \
    }                                                                                   \
} while (0)

#define PROCESS_CROSS_BLOCK(L_BLOCK_END, T_BLOCK_END) \
do {                                                       \
    size_t l_ptr = l_ctx->start;                           \
    size_t t_ptr = t_ctx->start;                           \
    /* 当前阈值下队列是否还有有效项（升序特性，仅用于提前终止） */ \
    int l_has_valid = 1;                                   \
    int t_has_valid = 1;                                   \
    /* 循环条件：halt+块范围+两队列首都有至少一个有效项*/ \
    while (!halt && l_has_valid && t_has_valid && l_ptr < L_BLOCK_END && t_ptr < T_BLOCK_END) { \
        /* 解析当前项+有效性判断 */ \
        uint64_t l_toOffset = (u64a)l_ctx->items[l_ptr].toOffset; \
        uint64_t t_toOffset = (u64a)t_ctx->items[t_ptr].toOffset; \
        l_has_valid = (l_toOffset <= to_offset);            \
        t_has_valid = (t_toOffset <= to_offset);            \
        /* 上报决策 */     \
        if (t_has_valid && (!l_has_valid || t_toOffset < l_toOffset)) { \
            halt = lilyItemReport(scratch, &t_ctx->items[t_ptr], report_vec_table[HS_ENGINE_LILY_FOR_TEDDY]); \
            t_ptr++; /* 仅推进块内指针，循环结束后同步到start */ \
        } else if (l_has_valid) {                          \
            halt = lilyItemReport(scratch, &l_ctx->items[l_ptr], report_vec_table[HS_ENGINE_LILY]);           \
            l_ptr++; /* 仅推进块内指针，循环结束后同步到start */ \
        } else {                                         \
            /* 双队列当前项均超阈值 → 提前终止循环 */          \
            break;                                       \
        }                                                   \
    }                                                       \
    l_ctx->start = l_ptr;                                   \
    t_ctx->start = t_ptr;                                   \
} while (0)

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
    if (!scratch || (scratch->lily_ctx.start >= scratch->lily_ctx.size &&
                     scratch->lily_for_teddy_ctx.start >= scratch->lily_for_teddy_ctx.size)) {
        return 0;
    }

    int halt = 0;
    u32* report_vec_table[] = {
        [HS_ENGINE_LILY] = getLilyReportVec(scratch->core_info.rose),
        [HS_ENGINE_LILY_FOR_TEDDY] = getLilyReportVec(scratch->core_info.rose) // 后续调整成真实getLilyForTeddyReportVec函数
    };

    if (scratch->lily_ctx.start >= scratch->lily_ctx.size) { // 仅teddy有数据
        REPORT_RANGE_WITH_OFFSET_CHECK(scratch, &scratch->lily_for_teddy_ctx, scratch->lily_for_teddy_ctx.size, to_offset, halt, report_vec_table[HS_ENGINE_LILY_FOR_TEDDY]);
        return halt;
    } else if (scratch->lily_for_teddy_ctx.start >= scratch->lily_for_teddy_ctx.size) { // 仅lily有数据
        REPORT_RANGE_WITH_OFFSET_CHECK(scratch, &scratch->lily_ctx, scratch->lily_ctx.size, to_offset, halt, report_vec_table[HS_ENGINE_LILY]);
        return halt;
    }

    // 绑定ctx指针
    LilyEngineCtx *l_ctx = &scratch->lily_ctx;
    LilyEngineCtx *t_ctx = &scratch->lily_for_teddy_ctx;

    // 批量归并主循环
    while (!halt
           && l_ctx->start < l_ctx->size && t_ctx->start < t_ctx->size
           && (u64a)l_ctx->items[l_ctx->start].toOffset <= to_offset
           && (u64a)t_ctx->items[t_ctx->start].toOffset <= to_offset) {
        // 计算块边界（直接用ctx->start/size）
        size_t l_block_end = l_ctx->start + LILY_MATCH_ITEMS_PER_CACHELINE;
        l_block_end = (l_block_end > l_ctx->size) ? l_ctx->size : l_block_end;
        size_t t_block_end = t_ctx->start + LILY_MATCH_ITEMS_PER_CACHELINE;
        t_block_end = (t_block_end > t_ctx->size) ? t_ctx->size : t_block_end;

        // 计算块边界值
        u64a l_block_max = (l_ctx->items && l_ctx->start < l_block_end) ? (u64a)l_ctx->items[l_block_end-1].toOffset : UINT64_MAX;
        u64a t_block_max = (t_ctx->items && t_ctx->start < t_block_end) ? (u64a)t_ctx->items[t_block_end-1].toOffset : UINT64_MAX;
        u64a l_block_min = (l_ctx->items && l_ctx->start < l_block_end) ? (u64a)l_ctx->items[l_ctx->start].toOffset : UINT64_MAX;
        u64a t_block_min = (t_ctx->items && t_ctx->start < t_block_end) ? (u64a)t_ctx->items[t_ctx->start].toOffset : UINT64_MAX;

        // 非交叉块处理
        if (l_block_max <= to_offset && l_block_max < t_block_min) {
            REPORT_RANGE(scratch, l_ctx, l_block_end, halt, report_vec_table[HS_ENGINE_LILY]);
        } else if (t_block_max <= to_offset && t_block_max < l_block_min) {
            REPORT_RANGE(scratch, t_ctx, t_block_end, halt, report_vec_table[HS_ENGINE_LILY_FOR_TEDDY]);
        } else {
            // 交叉块处理（与非交叉块处理互斥）
            PROCESS_CROSS_BLOCK(l_block_end, t_block_end);
        }
    }

    if (!halt) {
        // 剩余元素上报
        REPORT_RANGE_WITH_OFFSET_CHECK(scratch, l_ctx, l_ctx->size,
                                       to_offset, halt,
                                       report_vec_table[HS_ENGINE_LILY]);
        REPORT_RANGE_WITH_OFFSET_CHECK(
            scratch, t_ctx, t_ctx->size, to_offset, halt,
            report_vec_table[HS_ENGINE_LILY_FOR_TEDDY]);
    }

    return halt;
}

static REALLY_INLINE
int RoseDeliverReport(u64a offset, uint8_t index, s32 offset_adjust,
                      struct hs_scratch *scratch, u32 ekey) 
{
    struct core_info *ci = &scratch->core_info;
    u64a toOffset = offset + offset_adjust;

    if (index >=  8U || (unsigned long long)toOffset > LILY_TO_OFFSET_MAX) {
        ci->status |= SCRATCH_STATUS_TERMINATED;
        return KHSEL_MO_HALT_MATCHING;
    }

    LilyMatchItem item = {
        .onmatch_index = index,
        .toOffset = toOffset
    };

    // 暂存
    int ret = pushLilyItems(&item, &scratch->lily_ctx);
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

    return ret;
}