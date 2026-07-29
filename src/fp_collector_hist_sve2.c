/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Huawei Corporation nor the names of its contributors
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

#include "fp_collector_hist.h"

#include <arm_sve.h>
#include <assert.h>

#if !defined(__ARM_FEATURE_SVE2)
#error "fp_collector_hist_sve2.c must be compiled with SVE2 enabled"
#endif

#if !defined(HS_BUILD_HAVE_SVE2_HISTCNT)
#error "fp_collector_hist_sve2.c requires HS_BUILD_HAVE_SVE2_HISTCNT"
#endif

/*
 * 跨段查重（向量化版）：在 [0, end) 范围内查找 key 是否已出现过。
 * 用于判断当前向量段里发现的"段内首次出现"是否其实已经在
 * 更早的段里被 emit 过（即真正意义上的全局首次出现）。
 *
 * 相比逐元素标量扫描，这里用 svcntw() 宽度一次比较多个历史元素，
 * 命中即可通过 svptest_any 提前返回。
 */
static char key_seen_before(const u32 *keys, u32 end, u32 key) {
    const u32 lanes = (u32)svcntw();
    const svuint32_t vkey = svdup_n_u32(key);

    for (u32 pos = 0; pos < end; pos += lanes) {
        const svbool_t pg = svwhilelt_b32(pos, end);
        const svuint32_t vals = svld1_u32(pg, keys + pos);
        const svbool_t hit = svcmpeq_u32(pg, vals, vkey);

        if (svptest_any(pg, hit)) {
            return 1;
        }
    }

    return 0;
}

/*
 * 统计 key 在 [keys, keys + count) 内的出现次数。
 *
 * 调用方保证传入的 keys 指针已经指向 key 的“全局首次出现”位置，
 * 因此这里只需要向后扫一次，不需要像原版那样每次都从数组开头
 * 全量重扫。理论比较总量由 O(uniq_count * n) 降为 O(n^2 / 2)。
 *
 * 另外，归约（svaddv）被延迟到循环结束后只做一次；循环体内只用
 * svadd_u32_m 做纯数据并行的掩码累加，避免每个向量段都触发一次
 * 代价较高的水平归约（svcntp）。
 */
static u32 count_key_in_batch_sve2(const u32 *keys, u32 count, u32 key) {
    const u32 lanes = (u32)svcntw();
    const svuint32_t vkey = svdup_n_u32(key);
    const svuint32_t one = svdup_n_u32(1);
    svuint32_t acc = svdup_n_u32(0);

    for (u32 pos = 0; pos < count; pos += lanes) {
        const svbool_t pg = svwhilelt_b32(pos, count);
        const svuint32_t vals = svld1_u32(pg, keys + pos);
        const svbool_t hit = svcmpeq_u32(pg, vals, vkey);

        acc = svadd_u32_m(hit, acc, one);
    }

    return (u32)svaddv_u32(svptrue_b32(), acc);
}

void NEVER_INLINE hs_fp_histogram_count_batch_sve2(const u32 *keys, u32 count,
                                                   hs_fp_histogram_emit_fn emit,
                                                   void *ctx) {
    assert(count <= HS_FP_TRIGGER_HISTOGRAM_BATCH_SIZE);
    if (!count) {
        return;
    }
    assert(keys);
    assert(emit);

    const u32 lanes = (u32)svcntw();
    u32 key_buf[HS_FP_TRIGGER_HISTOGRAM_BATCH_SIZE];
    u32 rank_buf[HS_FP_TRIGGER_HISTOGRAM_BATCH_SIZE];

    for (u32 pos = 0; pos < count; pos += lanes) {
        const u32 active = MIN(lanes, count - pos);
        const svbool_t pg = svwhilelt_b32((u32)0, active);
        const svuint32_t vals = svld1_u32(pg, keys + pos);
        const svuint32_t ranks = svhistcnt_u32_z(pg, vals, vals);

        svst1_u32(pg, key_buf, vals);
        svst1_u32(pg, rank_buf, ranks);

        for (u32 lane = 0; lane < active; lane++) {
            const u32 key = key_buf[lane];

            if (rank_buf[lane] != 1U ||
                (pos && key_seen_before(keys, pos, key))) {
                continue;
            }

            const u32 first = pos + lane;
            emit(ctx, key,
                 count_key_in_batch_sve2(keys + first, count - first, key));
        }
    }
}
