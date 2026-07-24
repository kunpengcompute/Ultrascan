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

#include <assert.h>

#include "hs_compile.h"
#include "util/cpuid_flags.h"

#if !defined(__aarch64__)
#error "false-positive feedback histogram requires AArch64 NEON"
#endif

#include <arm_neon.h>

static u32 count_key_in_range_neon(const u32 *keys, u32 count, u32 key) {
    const uint32x4_t vkey = vdupq_n_u32(key);
    u32 total = 0;
    u32 pos = 0;

    for (; pos + 4 <= count; pos += 4) {
        const uint32x4_t vals = vld1q_u32(keys + pos);
        const uint32x4_t hits = vceqq_u32(vals, vkey);
        total += vaddvq_u32(vshrq_n_u32(hits, 31));
    }

    for (; pos < count; pos++) {
        if (keys[pos] == key) {
            total++;
        }
    }

    return total;
}

static void histogram_count_batch_neon(const u32 *keys, u32 count,
                                       hs_fp_histogram_emit_fn emit,
                                       void *ctx) {
    for (u32 i = 0; i < count; i++) {
        const u32 key = keys[i];
        if (count_key_in_range_neon(keys, i, key)) {
            continue;
        }

        emit(ctx, key, count_key_in_range_neon(keys, count, key));
    }
}

u8 hs_fp_histogram_select_backend(void) {
#if defined(ARCH_AARCH64) && defined(HS_BUILD_HAVE_SVE2_HISTCNT)
    if (cpuid_flags() & HS_CPU_FEATURES_SVE2) {
        return HS_FP_HISTOGRAM_BACKEND_SVE2;
    }
#endif
    return HS_FP_HISTOGRAM_BACKEND_NEON;
}

void hs_fp_histogram_count_batch(u8 backend, const u32 *keys, u32 count,
                                 hs_fp_histogram_emit_fn emit, void *ctx) {
    assert(count <= HS_FP_TRIGGER_HISTOGRAM_BATCH_SIZE);
    if (!count) {
        return;
    }
    assert(keys);
    assert(emit);

#if defined(ARCH_AARCH64) && defined(HS_BUILD_HAVE_SVE2_HISTCNT)
    if (backend == HS_FP_HISTOGRAM_BACKEND_SVE2) {
        hs_fp_histogram_count_batch_sve2(keys, count, emit, ctx);
        return;
    }
#else
    (void)backend;
#endif

    histogram_count_batch_neon(keys, count, emit, ctx);
}
