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

#ifndef FP_COLLECTOR_HIST_H
#define FP_COLLECTOR_HIST_H

#include "ue2common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HS_FP_TRIGGER_HISTOGRAM_BATCH_SIZE 64U

enum hs_fp_histogram_backend {
    HS_FP_HISTOGRAM_BACKEND_SCALAR = 0,
    HS_FP_HISTOGRAM_BACKEND_SVE2 = 1
};

typedef void (*hs_fp_histogram_emit_fn)(void *ctx, u32 key, u64a count);

u8 hs_fp_histogram_select_backend(void);

void hs_fp_histogram_count_batch(u8 backend, const u32 *keys, u32 count,
                                 hs_fp_histogram_emit_fn emit, void *ctx);

#if defined(ARCH_AARCH64) && defined(HS_BUILD_HAVE_SVE2_HISTCNT)
void NEVER_INLINE hs_fp_histogram_count_batch_sve2(const u32 *keys, u32 count,
                                                   hs_fp_histogram_emit_fn emit,
                                                   void *ctx);
#endif

#ifdef __cplusplus
} /* extern C */
#endif

#endif /* FP_COLLECTOR_HIST_H */
