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
#include "khsel_hwlm.h"
#include "khsel_fdr_internal.h"
#include "report.h"
#include "../util/unaligned.h"
#include "../fdr/teddy_internal.h"
#include "hs_common.h"
#include "hs_compile.h"
#include "lily_teddy_common.h"

#define GET_LO_4(chars) And128(chars, low4bits)
#define GET_HI_4(chars) Rshift8_m128(chars, BYTE_SIZE_FOUR)
#define ones_u32            0xfffffffful
static const u8 KHSEL_ALIGN_DIRECTIVE p_mask_arr[17][32];

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
const u32 *getLilyReportVec(const struct RoseEngine *t)
{
    if (!t->lilyOffset) {
        return NULL;
    }
    const u32 *reportVec = (const u32 *)((const char *)t + t->lilyOffset + LILY_VEC_LEN * BYTE_SIZE_FOUR);
    return reportVec;
}

static REALLY_INLINE
const u32 *getLilyEkeyVec(const struct RoseEngine *t)
{
    if (!t->lilyOffset) {
        return NULL;
    }
    size_t offset = 0x2 * LILY_VEC_LEN * BYTE_SIZE_FOUR;
    const u32 *ekeyVec = (const u32 *)((const char *)t + t->lilyOffset + offset);
    return ekeyVec;
}

static REALLY_INLINE
u8 getLilyQuietFlags(const struct RoseEngine *t)
{
    if (!t->lilyOffset) {
        return 0;
    }
    size_t offset = 0x2 * LILY_VEC_LEN * BYTE_SIZE_FOUR;
    u8 flagsQuiet = *((const u8 *)((const char *)t + t->lilyOffset + offset + LILY_VEC_LEN * BYTE_SIZE_FOUR));
    return flagsQuiet;
}

static REALLY_INLINE
const u32 *getLilyForTeddyReportVec(const struct RoseEngine *t)
{
    if (!t->lilyForTeddyOffset) {
        return NULL;
    }
    const struct lilyTeddy *teddy = (const struct lilyTeddy *)((const char *)t + t->lilyForTeddyOffset);
    const u32 *reportVec = (const u32 *)((const char *)teddy + teddy->lilyReportOffset);
    return reportVec;
}

static REALLY_INLINE
const u32 *getLilyForTeddyEkeyVec(const struct RoseEngine *t)
{
    if (!t->lilyForTeddyOffset) {
        return NULL;
    }
    const struct lilyTeddy *teddy = (const struct lilyTeddy *)((const char *)t + t->lilyForTeddyOffset);
    const u32 *ekeyVec = (const u32 *)((const char *)teddy + teddy->lilyEkeyOffset);
    return ekeyVec;
}

static REALLY_INLINE
const u32 *getLilyForTeddyLenVec(const struct RoseEngine *t)
{
    if (!t->lilyForTeddyOffset) {
        return NULL;
    }
    const struct lilyTeddy *teddy = (const struct lilyTeddy *)((const char *)t + t->lilyForTeddyOffset);
    const u32 *lenVec = (const u32 *)((const char *)teddy + teddy->lilyLenOffset);
    return lenVec;
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
int lilyItemReport(hs_scratch_t *scratch, const LilyMatchItem *item, const u32 *reportVec) {
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

// ===================== report_range（非交叉块批量上报） =====================
static REALLY_INLINE
void report_range(hs_scratch_t *scratch, LilyEngineCtx *ctx, size_t end, int *halt, const u32 *report_vec)
{
    for (; ctx->start < end && !(*halt); ctx->start++) {
        *halt = lilyItemReport(scratch, &ctx->items[ctx->start], report_vec);
    }
}

// ===================== report_range_with_offset_check（剩余元素上报） =====================
static REALLY_INLINE
void report_range_with_offset_check(hs_scratch_t *scratch, LilyEngineCtx *ctx, size_t end, u64a to_offset,
                                    int *halt, const u32 *report_vec)
{
    for (; ctx->start < end && !(*halt); ctx->start++) {
        if (likely((u64a)ctx->items[ctx->start].toOffset <= to_offset)) {
            *halt = lilyItemReport(scratch, &ctx->items[ctx->start], report_vec);
        } else {
            break; /* 升序特性：当前项>阈值，后续均>，终止遍历 */
        }
    }
}

// ===================== process_cross_block（交叉块处理） =====================
static REALLY_INLINE
void process_cross_block(size_t l_block_end, size_t t_block_end, LilyEngineCtx *l_ctx, LilyEngineCtx *t_ctx,
                         int *halt, hs_scratch_t *scratch, u64a to_offset, const u32 **report_vec_table)
{
    size_t l_ptr = l_ctx->start;
    size_t t_ptr = t_ctx->start;
    // 当前阈值下队列是否还有有效项（升序特性，仅用于提前终止）
    int l_has_valid = 1;
    int t_has_valid = 1;
    // 循环条件：halt+块范围+两队列首都有至少一个有效项
    while (!(*halt) && l_has_valid && t_has_valid && l_ptr < l_block_end && t_ptr < t_block_end) {
        // 解析当前项+有效性判断
        uint64_t l_toOffset = (u64a)l_ctx->items[l_ptr].toOffset;
        uint64_t t_toOffset = (u64a)t_ctx->items[t_ptr].toOffset;
        l_has_valid = (l_toOffset <= to_offset);
        t_has_valid = (t_toOffset <= to_offset);
        // 上报决策
        if (t_has_valid && (!l_has_valid || t_toOffset < l_toOffset)) {
            *halt = lilyItemReport(scratch, &t_ctx->items[t_ptr], report_vec_table[HS_ENGINE_LILY_FOR_TEDDY]);
            t_ptr++; /* 仅推进块内指针，循环结束后同步到start */
        } else if (l_has_valid) {
            *halt = lilyItemReport(scratch, &l_ctx->items[l_ptr], report_vec_table[HS_ENGINE_LILY]);
            l_ptr++; /* 仅推进块内指针，循环结束后同步到start */
        } else {
            // 双队列当前项均超阈值 → 提前终止循环
            break;
        }
    }
    l_ctx->start = l_ptr;
    t_ctx->start = t_ptr;
}

// 计算块边界
static REALLY_INLINE
size_t calc_block_end(size_t start, size_t size) {
    size_t block_end = start + LILY_MATCH_ITEMS_PER_CACHELINE;
    return (block_end > size) ? size : block_end;
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
    if (!scratch || (scratch->lily_ctx.start >= scratch->lily_ctx.size &&
                     scratch->lily_for_teddy_ctx.start >= scratch->lily_for_teddy_ctx.size)) {
        return 0;
    }

    int halt = 0;
    const u32 *report_vec_table[] = {
        [HS_ENGINE_LILY] = getLilyReportVec(scratch->core_info.rose),
        [HS_ENGINE_LILY_FOR_TEDDY]  = getLilyForTeddyReportVec(scratch->core_info.rose)
    };

    if (scratch->lily_ctx.start >= scratch->lily_ctx.size) { // 仅teddy有数据
        report_range_with_offset_check(scratch, &scratch->lily_for_teddy_ctx, scratch->lily_for_teddy_ctx.size, to_offset, &halt, report_vec_table[HS_ENGINE_LILY_FOR_TEDDY]);
        return halt;
    } else if (scratch->lily_for_teddy_ctx.start >= scratch->lily_for_teddy_ctx.size) { // 仅lily有数据
        report_range_with_offset_check(scratch, &scratch->lily_ctx, scratch->lily_ctx.size, to_offset, &halt, report_vec_table[HS_ENGINE_LILY]);
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
        size_t l_block_end = calc_block_end(l_ctx->start, l_ctx->size);
        size_t t_block_end = calc_block_end(t_ctx->start, t_ctx->size);

        // 计算块边界值
        u64a l_block_max = (u64a)l_ctx->items[l_block_end-1].toOffset;
        u64a t_block_max = (u64a)t_ctx->items[t_block_end-1].toOffset;
        u64a l_block_min = (u64a)l_ctx->items[l_ctx->start].toOffset;
        u64a t_block_min = (u64a)t_ctx->items[t_ctx->start].toOffset;

        // 非交叉块处理
        if (l_block_max <= to_offset && l_block_max < t_block_min) {
            report_range(scratch, l_ctx, l_block_end, &halt, report_vec_table[HS_ENGINE_LILY]);
        } else if (t_block_max <= to_offset && t_block_max < l_block_min) {
            report_range(scratch, t_ctx, t_block_end, &halt, report_vec_table[HS_ENGINE_LILY_FOR_TEDDY]);
        } else {
            // 交叉块处理（与非交叉块处理互斥）
            process_cross_block(l_block_end, t_block_end, l_ctx, t_ctx, &halt, scratch, to_offset, report_vec_table);
        }
    }

    if (!halt) {
        // 剩余元素上报
        report_range_with_offset_check(scratch, l_ctx, l_ctx->size,
                                       to_offset, &halt,
                                       report_vec_table[HS_ENGINE_LILY]);
        report_range_with_offset_check(
            scratch, t_ctx, t_ctx->size, to_offset, &halt,
            report_vec_table[HS_ENGINE_LILY_FOR_TEDDY]);
    }

    return halt;
}

static REALLY_INLINE
int RoseDeliverReport(enum HsEngine engine_type, u64a offset, uint8_t index, s32 offset_adjust,
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
    int ret = 0;
    if (engine_type == HS_ENGINE_LILY) {
    	ret = pushLilyItems(&item, &scratch->lily_ctx);
    } else {
    	ret = pushLilyItems(&item, &scratch->lily_for_teddy_ctx);
    }
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
int lilyMatch(u64a conf, const u32 *ekeyVec, u8 flagsQuiet, const u8 *ptr, hs_scratch_t *scratch)
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
            int ret = RoseDeliverReport(HS_ENGINE_LILY, i + 1 , index, 0, scratch, ekeyVec[index]);
            if (ret == 0) { // 上报异常，终止后续操作
                return 1;
            }
        }
    } while (unlikely(!!conf));
    return 0;
}

static REALLY_INLINE
int lilyForTeddyMatch(u64a conf, const u32 *ekeyVec, u8 flagsQuiet, const u8 *ptr, hs_scratch_t *scratch) {
    conf = ~conf;
    if (likely(!conf)) {
        return -1;
    }
    const u8 bucket = 8;
    size_t i = 0;
    const u32 *lenVec = getLilyForTeddyLenVec(scratch->core_info.rose);

    do {
        u32 bit = CTZ64(conf);
        u32 byte = bit / bucket;
        u32 index = bit % bucket;
        conf = conf & (conf - 1);

        if ((ekeyVec[index] == INVALID_EKEY ||
            !IsExhausted(scratch->core_info.rose, scratch->core_info.exhaustionVector, ekeyVec[index])) &&
            !(flagsQuiet & (1 << index))) {
            i = scratch->core_info.buf_offset + ptr - scratch->core_info.buf + byte;
            if (i < scratch->core_info.len && i >= lenVec[index] - 1) {
                // 过滤掉ptr < core_info.buf，以及填充导致误报的场景
                int ret = RoseDeliverReport(HS_ENGINE_LILY_FOR_TEDDY, i + 1 , index, 0, scratch, ekeyVec[index]);
                if (ret == 0) {
                    return 1;
                }
            }
        }
    } while (unlikely(!!conf));
    return 0;
}

static REALLY_INLINE
int runLily(const char *maskLily, const u32 *ekeyVec, u8 flagsQuiet, hs_scratch_t *scratch)
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
    const u32 *ekeyVec = getLilyEkeyVec(rose);
    u8 flagsQuiet = getLilyQuietFlags(rose);
    const char *maskLily = getLily(rose);

    initLilyItems(scratch);

    hs_error_t ret = runLily(maskLily, ekeyVec, flagsQuiet, scratch);

    return ret;
}

static REALLY_INLINE
void copyRuntBlock128(u8 *dst, const u8 *src, size_t len) {
    switch (len) {
    case 0:
        break;
    case 1:
        *dst = *src;
        break;
    case 2:
        unaligned_store_u16(dst, unaligned_load_u16(src));
        break;
    case 3:
        unaligned_store_u16(dst, unaligned_load_u16(src));
        dst[2] = src[2];
        break;
    case 4:
        unaligned_store_u32(dst, unaligned_load_u32(src));
        break;
    case 5:
    case 6:
    case 7:
        /* Perform copy with two overlapping 4-byte chunks. */
        unaligned_store_u32(dst + len - 4, unaligned_load_u32(src + len - 4));
        unaligned_store_u32(dst, unaligned_load_u32(src));
        break;
    case 8:
        unaligned_store_u64a(dst, unaligned_load_u64a(src));
        break;
    default:
        /* Perform copy with two overlapping 8-byte chunks. */
        assert(len < 16);
        unaligned_store_u64a(dst + len - 8, unaligned_load_u64a(src + len - 8));
        unaligned_store_u64a(dst, unaligned_load_u64a(src));
        break;
    }
}

static REALLY_INLINE
m128 vectoredLoad128(m128 *p_mask, const u8 *ptr, const size_t start_offset,
                     const u8 *lo, const u8 *hi,
                     const u8 *buf_history, size_t len_history,
                     const u32 nMasks) {
    union {
        u8 val8[16];
        m128 val128;
    } u;
    u.val128 = zeroes128();

    uintptr_t copy_start;
    uintptr_t copy_len;

    if (ptr >= lo) { // short/end/start zone
        uintptr_t start = (uintptr_t)(ptr - lo);
        uintptr_t avail = (uintptr_t)(hi - ptr);
        if (avail >= 16) {
            assert(start_offset - start <= 16);
            *p_mask = Loadu128(p_mask_arr[16 - start_offset + start]
                               + 16 - start_offset + start);
            return Loadu128(ptr);
        }
        assert(start_offset - start <= avail);
        *p_mask = Loadu128(p_mask_arr[avail - start_offset + start]
                           + 16 - start_offset + start);
        copy_start = 0;
        copy_len = avail;
    } else { // start zone
        uintptr_t need = MIN((uintptr_t)(lo - ptr),
                             MIN(len_history, nMasks - 1));
        uintptr_t start = (uintptr_t)(lo - ptr);
        uintptr_t i;
        for (i = start - need; i < start; i++) {
            u.val8[i] = buf_history[len_history - (start - i)];
        }
        uintptr_t end = MIN(16, (uintptr_t)(hi - ptr));
        assert(start + start_offset <= end);
        *p_mask = Loadu128(p_mask_arr[end - start - start_offset]
                           + 16 - start - start_offset);
        copy_start = start;
        copy_len = end - start;
    }

    // Runt block from the buffer.
    copyRuntBlock128(&u.val8[copy_start], &ptr[copy_start], copy_len);

    return u.val128;
}

#define FDR_EXEC_LILY_TEDDY(fdr, a, ekeyVec, scratch, n_msk)                  \
do {                                                                          \
    const u8 *buf_end = a->buf + a->len;                                      \
    const u8 *ptr = a->buf + a->start_offset;                                 \
    const struct lilyTeddy *teddy = (const struct lilyTeddy *)fdr;            \
    const size_t iterBytes = 32;                                              \
    const m128 *maskBase = (const m128 *)((const u8 *)teddy + KHSEL_ROUNDUP_CL(sizeof(struct lilyTeddy)));                                \
    u8 flagsQuiet = *((const u8 *)((const char *)teddy + teddy->lilyQuietOffset));  \
                                                                              \
    FDR_EXEC_TEDDY_RES_OLD(n_msk);                                            \
    const u8 *mainStart = KHSEL_ROUNDUP_PTR(ptr, 16);                               \
    if (ptr < mainStart) {                                                    \
        ptr = mainStart - 16;                                                 \
        m128 p_mask;                                                          \
        m128 val_0 = vectoredLoad128(&p_mask, ptr, a->start_offset,           \
                                     a->buf, buf_end,                         \
                                     a->buf_history, a->len_history, n_msk);  \
        m128 r_0 = PREP_CONF_FN(maskBase, val_0, n_msk);                      \
        r_0 = Or128(r_0, p_mask);                                             \
        FDR_EXEC_TEDDY_RES_OLD_APPLY_PMASK(p_mask, n_msk);                    \
        u64a conf0 = vgetq_lane_u64(r_0.vectU64, 0);                          \
        u64a conf8 = vgetq_lane_u64(r_0.vectU64, 1);                          \
        if (lilyForTeddyMatch(conf0, ekeyVec, flagsQuiet, ptr, scratch) == KHSEL_MATCHING_TERMINATED ||      \
            lilyForTeddyMatch(conf8, ekeyVec, flagsQuiet, ptr + 8, scratch) == KHSEL_MATCHING_TERMINATED) {  \
            return KHSEL_MATCHING_TERMINATED;                                            \
        }                                                                     \
        ptr += 16;                                                            \
    }                                                                         \
                                                                              \
    if (ptr + 16 <= buf_end) {                                                \
        m128 r_0 = PREP_CONF_FN(maskBase, Load128(ptr), n_msk);               \
        u64a conf0 = vgetq_lane_u64(r_0.vectU64, 0);                          \
        u64a conf8 = vgetq_lane_u64(r_0.vectU64, 1);                          \
        if (lilyForTeddyMatch(conf0, ekeyVec, flagsQuiet, ptr, scratch) == KHSEL_MATCHING_TERMINATED ||      \
            lilyForTeddyMatch(conf8, ekeyVec, flagsQuiet, ptr + 8, scratch) == KHSEL_MATCHING_TERMINATED) {  \
            return KHSEL_MATCHING_TERMINATED;                                              \
        }                                                                     \
        ptr += 16;                                                            \
    }                                                                         \
                                                                              \
    for (; ptr + iterBytes <= buf_end; ptr += iterBytes) {                    \
        __builtin_prefetch(ptr + (iterBytes * 4));                            \
        m128 r_0 = PREP_CONF_FN(maskBase, Load128(ptr), n_msk);               \
        u64a conf0 = vgetq_lane_u64(r_0.vectU64, 0);                          \
        u64a conf8 = vgetq_lane_u64(r_0.vectU64, 1);                          \
        if (lilyForTeddyMatch(conf0, ekeyVec, flagsQuiet, ptr, scratch) == KHSEL_MATCHING_TERMINATED ||      \
            lilyForTeddyMatch(conf8, ekeyVec, flagsQuiet, ptr + 8, scratch) == KHSEL_MATCHING_TERMINATED) {  \
            return KHSEL_MATCHING_TERMINATED;                                              \
        }                                                                     \
        m128 r_1 = PREP_CONF_FN(maskBase, Load128(ptr + 16), n_msk);          \
        u64a conf16 = vgetq_lane_u64(r_1.vectU64, 0);                          \
        u64a conf24 = vgetq_lane_u64(r_1.vectU64, 1);                          \
        if (lilyForTeddyMatch(conf16, ekeyVec, flagsQuiet, ptr + 16, scratch) == KHSEL_MATCHING_TERMINATED ||      \
            lilyForTeddyMatch(conf24, ekeyVec, flagsQuiet, ptr + 24, scratch) == KHSEL_MATCHING_TERMINATED) {  \
            return KHSEL_MATCHING_TERMINATED;                                              \
        }                                                                     \
    }                                                                         \
                                                                              \
    if (ptr + 16 <= buf_end) {                                                \
        m128 r_0 = PREP_CONF_FN(maskBase, Load128(ptr), n_msk);               \
        u64a conf0 = vgetq_lane_u64(r_0.vectU64, 0);                          \
        u64a conf8 = vgetq_lane_u64(r_0.vectU64, 1);                          \
        if (lilyForTeddyMatch(conf0, ekeyVec, flagsQuiet, ptr, scratch) == KHSEL_MATCHING_TERMINATED ||      \
            lilyForTeddyMatch(conf8, ekeyVec, flagsQuiet, ptr + 8, scratch) == KHSEL_MATCHING_TERMINATED) {  \
            return KHSEL_MATCHING_TERMINATED;                                              \
        }                                                                     \
        ptr += 16;                                                            \
    }                                                                         \
                                                                              \
    assert(ptr + 16 > buf_end);                                               \
    if (ptr < buf_end) {                                                      \
        m128 p_mask;                                                          \
        m128 val_0 = vectoredLoad128(&p_mask, ptr, 0, ptr, buf_end,           \
                                     a->buf_history, a->len_history, n_msk);  \
        m128 r_0 = PREP_CONF_FN(maskBase, val_0, n_msk);                      \
        r_0 = Or128(r_0, p_mask);                                             \
        u64a conf0 = vgetq_lane_u64(r_0.vectU64, 0);                          \
        u64a conf8 = vgetq_lane_u64(r_0.vectU64, 1);                          \
        if (lilyForTeddyMatch(conf0, ekeyVec, flagsQuiet, ptr, scratch) == KHSEL_MATCHING_TERMINATED ||      \
            lilyForTeddyMatch(conf8, ekeyVec, flagsQuiet, ptr + 8, scratch) == KHSEL_MATCHING_TERMINATED) {  \
            return KHSEL_MATCHING_TERMINATED;                                              \
        }                                                                     \
    }                                                                         \
    return KHSEL_MATCHING_SUCCESS;                                                      \
} while(0)

static hs_error_t fdr_exec_lily_teddy_msks2(const struct FDR *fdr,
                                  const struct FDR_Runtime_Args *a,
                                  const u32 *ekeyVec,
                                  hs_scratch_t* scratch) {
    FDR_EXEC_LILY_TEDDY(fdr, a, ekeyVec, scratch, 2);
}


static hs_error_t fdr_exec_lily_teddy_msks3(const struct FDR *fdr,
                                  const struct FDR_Runtime_Args *a,
                                  const u32 *ekeyVec,
                                  hs_scratch_t* scratch) {
    FDR_EXEC_LILY_TEDDY(fdr, a, ekeyVec, scratch, 3);
}


static hs_error_t fdr_exec_lily_teddy_msks4(const struct FDR *fdr,
                                  const struct FDR_Runtime_Args *a,
                                  const u32 *ekeyVec,
                                  hs_scratch_t* scratch) {
    FDR_EXEC_LILY_TEDDY(fdr, a, ekeyVec, scratch, 4);
}

#define FAKE_HISTORY_SIZE 16
static const u8 fake_history[FAKE_HISTORY_SIZE];

hs_error_t KHSEL_LilyForTeddyRunExec(const struct RoseEngine *rose, hs_scratch_t *scratch)
{
    const struct FDR *lilyForTeddyFdr = (const struct FDR *)((const char *)rose + rose->lilyForTeddyOffset);
    const u32 *ekeyVec = getLilyForTeddyEkeyVec(rose);
    const u8* hbuf = fake_history + FAKE_HISTORY_SIZE;
    initLilyForTeddyItems(scratch);
    const struct FDR_Runtime_Args a = {
        scratch->core_info.buf,
        scratch->core_info.len,
        hbuf,
        0,
        0,
        NULL,
        scratch,
        NULL,
        0
    };
    hs_error_t ret = KHSEL_MATCHING_SUCCESS;
    switch (lilyForTeddyFdr->maxStringLen) {
        case 2:
            ret = fdr_exec_lily_teddy_msks2(lilyForTeddyFdr, &a, ekeyVec, scratch);
            break;
        case 3:
            ret = fdr_exec_lily_teddy_msks3(lilyForTeddyFdr, &a, ekeyVec, scratch);
            break;
        case 4:
            ret = fdr_exec_lily_teddy_msks4(lilyForTeddyFdr, &a, ekeyVec, scratch);
            break;
    }
    return ret;
}