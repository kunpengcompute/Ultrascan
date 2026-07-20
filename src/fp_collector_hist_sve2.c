/*
 * Copyright (c) 2026, Intel Corporation
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

#include "fp_collector_hist.h"

#include <assert.h>
#include <arm_sve.h>

#if !defined(__ARM_FEATURE_SVE2)
#error "fp_collector_hist_sve2.c must be compiled with SVE2 enabled"
#endif

#if !defined(HS_BUILD_HAVE_SVE2_HISTCNT)
#error "fp_collector_hist_sve2.c requires HS_BUILD_HAVE_SVE2_HISTCNT"
#endif

static
char key_seen_before(const u32 *keys, u32 end, u32 key) {
    for (u32 i = 0; i < end; i++) {
        if (keys[i] == key) {
            return 1;
        }
    }

    return 0;
}

static
u32 count_key_in_batch_sve2(const u32 *keys, u32 count, u32 key) {
    const u32 lanes = (u32)svcntw();
    const svuint32_t vkey = svdup_n_u32(key);
    u32 total = 0;

    for (u32 pos = 0; pos < count; pos += lanes) {
        const u32 active = MIN(lanes, count - pos);
        const svbool_t pg = svwhilelt_b32((u32)0, active);
        const svuint32_t vals = svld1_u32(pg, keys + pos);
        const svbool_t hit = svcmpeq_u32(pg, vals, vkey);

        total += (u32)svcntp_b32(pg, hit);
    }

    return total;
}

void NEVER_INLINE hs_fp_histogram_count_batch_sve2(
        const u32 *keys, u32 count, hs_fp_histogram_emit_fn emit, void *ctx) {
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

            if (rank_buf[lane] != 1U || key_seen_before(keys, pos, key)) {
                continue;
            }

            emit(ctx, key, count_key_in_batch_sve2(keys, count, key));
        }
    }
}
