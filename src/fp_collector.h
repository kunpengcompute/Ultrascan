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

#ifndef FP_COLLECTOR_H
#define FP_COLLECTOR_H

#include <stddef.h>

#include "hs_common.h"
#include "ue2common.h"

#ifdef __cplusplus
extern "C"
{
#endif

struct RoseEngine;
struct hs_scratch;

#define HS_FP_UNKNOWN_SOURCE_NONE              0U
#define HS_FP_UNKNOWN_SOURCE_DELAYED_REPLAY    1U
#define HS_FP_UNKNOWN_SOURCE_ANCHORED_REPLAY   2U
#define HS_FP_UNKNOWN_SOURCE_EOD_OR_BOUNDARY   3U
#define HS_FP_UNKNOWN_SOURCE_FLUSH_COMBINATION 4U
#define HS_FP_UNKNOWN_SOURCE_MPV_OR_NFA_QUEUE  5U

hs_error_t hs_fp_collector_check_db(const hs_fp_collector_t *collector,
                                    const hs_database_t *db);

hs_error_t hs_fp_collector_check_rose(const hs_fp_collector_t *collector,
                                      const struct RoseEngine *rose);

void hs_fp_collector_record_scan(hs_fp_collector_t *collector, size_t bytes);

void hs_fp_collector_flush(hs_fp_collector_t *collector);

void hs_fp_collector_begin_trigger(struct hs_scratch *scratch, u32 key);

void hs_fp_collector_end_trigger(struct hs_scratch *scratch);

void hs_fp_collector_record_final_report(struct hs_scratch *scratch);

hs_error_t hs_fp_feedback_clone(const hs_fp_feedback_t *src,
                                hs_fp_feedback_t **dst);

u32 hs_fp_feedback_count_matches_in_rose(const hs_fp_feedback_t *feedback,
                                         const struct RoseEngine *rose,
                                         u32 *checked_count);

char hs_fp_feedback_literal_is_bad(const hs_fp_feedback_t *feedback,
                                   const char *bytes, size_t length,
                                   char nocase);

char hs_fp_feedback_fragment_is_bad(const hs_fp_feedback_t *feedback,
                                    const char *bytes, size_t length,
                                    char nocase, const u8 *mask,
                                    const u8 *cmp, size_t mask_length);

unsigned int hs_compile_context_observe_checked_count(
        const hs_compile_context_t *ctx);

unsigned int hs_compile_context_observe_hit_count(
        const hs_compile_context_t *ctx);

#ifdef __cplusplus
} /* extern C */
#endif

#endif /* FP_COLLECTOR_H */
