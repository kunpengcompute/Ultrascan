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

#include "fp_collector.h"

#include <string.h>

#include "hs_runtime.h"

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_collector_create(const hs_database_t *db,
                                           hs_fp_collector_t **collector) {
    (void)db;
    if (!collector) {
        return HS_INVALID;
    }
    *collector = NULL;
    return HS_ARCH_ERROR;
}

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_collector_reset(hs_fp_collector_t *collector) {
    (void)collector;
    return HS_ARCH_ERROR;
}

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_collector_merge(hs_fp_collector_t *const *collectors,
                                          unsigned int count,
                                          hs_fp_collector_t **collector) {
    (void)collectors;
    (void)count;
    if (!collector) {
        return HS_INVALID;
    }
    *collector = NULL;
    return HS_ARCH_ERROR;
}

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_collector_free(hs_fp_collector_t *collector) {
    (void)collector;
    return HS_SUCCESS;
}

hs_error_t hs_fp_collector_report(const hs_fp_collector_t *collector,
                                  hs_fp_report_t **report) {
    (void)collector;
    if (!report) {
        return HS_INVALID;
    }
    *report = NULL;
    return HS_ARCH_ERROR;
}

hs_error_t hs_fp_report_free(hs_fp_report_t *report) {
    (void)report;
    return HS_SUCCESS;
}

hs_error_t hs_fp_report_get_summary(const hs_fp_report_t *report,
                                    hs_fp_report_summary_t *summary) {
    (void)report;
    if (!summary) {
        return HS_INVALID;
    }
    memset(summary, 0, sizeof(*summary));
    return HS_ARCH_ERROR;
}

hs_error_t hs_fp_report_get_fragment(const hs_fp_report_t *report, u32 index,
                                     hs_fp_fragment_info_t *fragment) {
    (void)report;
    (void)index;
    if (!fragment) {
        return HS_INVALID;
    }
    memset(fragment, 0, sizeof(*fragment));
    return HS_ARCH_ERROR;
}

hs_error_t hs_fp_feedback_build(const hs_fp_report_t *report,
                                hs_fp_feedback_t **feedback) {
    return hs_fp_feedback_build_ext(report, NULL, feedback);
}

hs_error_t hs_fp_feedback_build_ext(const hs_fp_report_t *report,
                                    const hs_fp_feedback_params_t *params,
                                    hs_fp_feedback_t **feedback) {
    (void)report;
    (void)params;
    if (!feedback) {
        return HS_INVALID;
    }
    *feedback = NULL;
    return HS_ARCH_ERROR;
}

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_collector_to_feedback(
    hs_fp_collector_t *collector, const hs_fp_feedback_params_t *params,
    hs_fp_feedback_t **feedback) {
    return hs_fp_collector_to_feedback_with_dump(collector, params, NULL, NULL,
                                                 feedback);
}

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_collector_to_feedback_with_dump(
    hs_fp_collector_t *collector, const hs_fp_feedback_params_t *params,
    const hs_fp_feedback_dump_callbacks_t *callbacks, void *context,
    hs_fp_feedback_t **feedback) {
    (void)collector;
    (void)params;
    (void)callbacks;
    (void)context;
    if (!feedback) {
        return HS_INVALID;
    }
    *feedback = NULL;
    return HS_ARCH_ERROR;
}

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_feedback_free(hs_fp_feedback_t *feedback) {
    (void)feedback;
    return HS_SUCCESS;
}

hs_error_t hs_fp_feedback_get_summary(const hs_fp_feedback_t *feedback,
                                      hs_fp_feedback_summary_t *summary) {
    (void)feedback;
    if (!summary) {
        return HS_INVALID;
    }
    memset(summary, 0, sizeof(*summary));
    return HS_ARCH_ERROR;
}

u32 hs_fp_feedback_fragment_count(const hs_fp_feedback_t *feedback) {
    (void)feedback;
    return 0;
}

hs_error_t hs_fp_feedback_get_fragment(const hs_fp_feedback_t *feedback,
                                       u32 index,
                                       hs_fp_fragment_info_t *fragment) {
    (void)feedback;
    (void)index;
    if (!fragment) {
        return HS_INVALID;
    }
    memset(fragment, 0, sizeof(*fragment));
    return HS_ARCH_ERROR;
}

hs_error_t hs_fp_feedback_clone(const hs_fp_feedback_t *src,
                                hs_fp_feedback_t **dst) {
    (void)src;
    if (!dst) {
        return HS_INVALID;
    }
    *dst = NULL;
    return HS_ARCH_ERROR;
}

hs_error_t hs_fp_feedback_create_from_fragments(
    const struct hs_fp_feedback_import_fragment *fragments, u32 fragment_count,
    hs_fp_feedback_t **feedback) {
    (void)fragments;
    (void)fragment_count;
    if (!feedback) {
        return HS_INVALID;
    }
    *feedback = NULL;
    return HS_ARCH_ERROR;
}

u32 hs_fp_feedback_count_matches_in_rose(const hs_fp_feedback_t *feedback,
                                         const struct RoseEngine *rose,
                                         u32 *checked_count) {
    (void)feedback;
    (void)rose;
    if (checked_count) {
        *checked_count = 0;
    }
    return 0;
}

char hs_fp_feedback_literal_is_bad(const hs_fp_feedback_t *feedback, u32 table,
                                   const char *bytes, size_t length,
                                   char nocase) {
    (void)feedback;
    (void)table;
    (void)bytes;
    (void)length;
    (void)nocase;
    return 0;
}

char hs_fp_feedback_fragment_is_bad(const hs_fp_feedback_t *feedback, u32 table,
                                    const char *bytes, size_t length,
                                    char nocase, const u8 *mask, const u8 *cmp,
                                    size_t mask_length) {
    (void)feedback;
    (void)table;
    (void)bytes;
    (void)length;
    (void)nocase;
    (void)mask;
    (void)cmp;
    (void)mask_length;
    return 0;
}

char hs_fp_feedback_fragment_match_index(const hs_fp_feedback_t *feedback,
                                         u32 table, const char *bytes,
                                         size_t length, char nocase,
                                         const u8 *mask, const u8 *cmp,
                                         size_t mask_length,
                                         u32 *feedback_index) {
    if (feedback_index) {
        *feedback_index = HS_FP_FEEDBACK_INDEX_INVALID;
    }
    (void)feedback;
    (void)table;
    (void)bytes;
    (void)length;
    (void)nocase;
    (void)mask;
    (void)cmp;
    (void)mask_length;
    (void)feedback_index;
    return 0;
}

hs_error_t hs_fp_collector_check_db(const hs_fp_collector_t *collector,
                                    const hs_database_t *db) {
    (void)collector;
    (void)db;
    return HS_ARCH_ERROR;
}

hs_error_t hs_fp_collector_check_rose(const hs_fp_collector_t *collector,
                                      const struct RoseEngine *rose) {
    (void)collector;
    (void)rose;
    return HS_ARCH_ERROR;
}

void hs_fp_collector_record_scan(hs_fp_collector_t *collector) {
    (void)collector;
}

void hs_fp_collector_flush(hs_fp_collector_t *collector) { (void)collector; }

void hs_fp_collector_begin_trigger(struct hs_scratch *scratch, u32 key) {
    (void)scratch;
    (void)key;
}

void hs_fp_collector_end_trigger(struct hs_scratch *scratch) { (void)scratch; }

void hs_fp_collector_record_final_report(struct hs_scratch *scratch) {
    (void)scratch;
}
