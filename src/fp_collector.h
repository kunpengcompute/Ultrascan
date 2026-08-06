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

#ifndef FP_COLLECTOR_H
#define FP_COLLECTOR_H

#include <stddef.h>

#include "hs_common.h"
#include "hs_compile.h"
#include "hs_runtime.h"
#include "ue2common.h"

#ifdef __cplusplus
extern "C" {
#endif

struct RoseEngine;
struct hs_scratch;
struct hs_fp_report;
struct hs_compile_context;

typedef struct hs_fp_report hs_fp_report_t;
typedef struct hs_compile_context hs_compile_context_t;

/**
 * Private compile-time false-positive feedback checkpoint identifiers.
 *
 * These diagnostics are used by in-tree tools and tests only; they are not
 * part of the public API surface.
 */
#define HS_FP_COMPILE_CHECKPOINT_LITERAL_SPLIT 0U
#define HS_FP_COMPILE_CHECKPOINT_VIOLET_SPLIT 1U
#define HS_FP_COMPILE_CHECKPOINT_MASKED_LITERAL 2U
#define HS_FP_COMPILE_CHECKPOINT_MATCHER_BUILD 3U
#define HS_FP_COMPILE_CHECKPOINT_SHORTCUT_LITERAL 4U
#define HS_FP_COMPILE_CHECKPOINT_SMALL_LITERAL_SET 5U
#define HS_FP_COMPILE_CHECKPOINT_SOMBE_LITERAL 6U
#define HS_FP_COMPILE_CHECKPOINT_REWRITE_EOD_TO_FLOATING 7U
#define HS_FP_COMPILE_CHECKPOINT_REWRITE_ANCHORED_REHOME 8U
#define HS_FP_COMPILE_CHECKPOINT_REWRITE_FLOOD_SUFFIX 9U
#define HS_FP_COMPILE_CHECKPOINT_REWRITE_SMALL_BLOCK 10U
#define HS_FP_COMPILE_CHECKPOINT_ANCHORED_ACYCLIC 11U
#define HS_FP_COMPILE_CHECKPOINT_MIXED_SENSITIVITY 12U
#define HS_FP_COMPILE_CHECKPOINT_DELAY_TRANSFORM 13U
#define HS_FP_COMPILE_CHECKPOINT_COUNT 14U

#define HS_FP_FEEDBACK_INDEX_INVALID 0xffffffffU
#define HS_FP_MATCHER_BUILD_HIT_DETAIL_INITIAL_CAPACITY 64U
#define HS_FP_MATCHER_BUILD_SOURCE_LITERAL_LIMIT 64U

typedef struct hs_compile_context_checkpoint_info {
    unsigned int checked_count;
    unsigned int hit_count;
    unsigned int blocked_count;
    unsigned int passed_count;
} hs_compile_context_checkpoint_info_t;

typedef struct hs_compile_context_matcher_build_hit_info {
    u32 feedback_index;
    u32 table;
    u32 fragment_id;
    u32 lit_id;
    u32 source_table;
    u32 source_delay;
    u32 source_length;
    u32 source_copied_length;
    u32 source_nocase;
    u32 occurrences;
    u8 source_suffix[HS_FP_MATCHER_BUILD_SOURCE_LITERAL_LIMIT];
} hs_compile_context_matcher_build_hit_info_t;

hs_error_t hs_fp_collector_check_db(const hs_fp_collector_t *collector,
                                    const hs_database_t *db);

hs_error_t hs_fp_collector_check_rose(const hs_fp_collector_t *collector,
                                      const struct RoseEngine *rose);

void hs_fp_collector_record_scan(hs_fp_collector_t *collector);

void hs_fp_collector_flush(hs_fp_collector_t *collector);

void hs_fp_collector_begin_trigger(struct hs_scratch *scratch, u32 key);

void hs_fp_collector_end_trigger(struct hs_scratch *scratch);

void hs_fp_collector_record_final_report(struct hs_scratch *scratch);

hs_error_t hs_fp_feedback_clone(const hs_fp_feedback_t *src,
                                hs_fp_feedback_t **dst);

struct hs_fp_feedback_import_fragment {
    u64a key;
    u32 table;
    u32 engine;
    u32 flags;
    const u8 *bytes;
    size_t length;
    const u8 *mask;
    const u8 *cmp;
    size_t mask_length;
    u64a trigger_count;
    u64a true_trigger_count;
    u64a false_positive_count;
};

typedef struct hs_fp_report_summary {
    unsigned int fragment_count;
    u64a trigger_count;
    u64a true_trigger_count;
    u64a false_positive_count;
} hs_fp_report_summary_t;

typedef struct hs_fp_feedback_summary {
    unsigned int bad_fragment_count;
} hs_fp_feedback_summary_t;

hs_error_t hs_fp_collector_report(const hs_fp_collector_t *collector,
                                  hs_fp_report_t **report);

hs_error_t hs_fp_report_free(hs_fp_report_t *report);

hs_error_t hs_fp_report_get_summary(const hs_fp_report_t *report,
                                    hs_fp_report_summary_t *summary);

hs_error_t hs_fp_report_get_fragment(const hs_fp_report_t *report, u32 index,
                                     hs_fp_fragment_info_t *fragment);

hs_error_t hs_fp_feedback_build(const hs_fp_report_t *report,
                                hs_fp_feedback_t **feedback);

hs_error_t hs_fp_feedback_build_ext(const hs_fp_report_t *report,
                                    const hs_fp_feedback_params_t *params,
                                    hs_fp_feedback_t **feedback);

hs_error_t hs_fp_feedback_get_summary(const hs_fp_feedback_t *feedback,
                                      hs_fp_feedback_summary_t *summary);

hs_error_t hs_fp_feedback_create_from_fragments(
    const struct hs_fp_feedback_import_fragment *fragments, u32 fragment_count,
    hs_fp_feedback_t **feedback);

u32 hs_fp_feedback_fragment_count(const hs_fp_feedback_t *feedback);

hs_error_t hs_fp_feedback_get_fragment(const hs_fp_feedback_t *feedback,
                                       u32 index,
                                       hs_fp_fragment_info_t *fragment);

u32 hs_fp_feedback_count_matches_in_rose(const hs_fp_feedback_t *feedback,
                                         const struct RoseEngine *rose,
                                         u32 *checked_count);

char hs_fp_feedback_literal_is_bad(const hs_fp_feedback_t *feedback, u32 table,
                                   const char *bytes, size_t length,
                                   char nocase);

char hs_fp_feedback_fragment_is_bad(const hs_fp_feedback_t *feedback, u32 table,
                                    const char *bytes, size_t length,
                                    char nocase, const u8 *mask, const u8 *cmp,
                                    size_t mask_length);

char hs_fp_feedback_fragment_match_index(const hs_fp_feedback_t *feedback,
                                         u32 table, const char *bytes,
                                         size_t length, char nocase,
                                         const u8 *mask, const u8 *cmp,
                                         size_t mask_length,
                                         u32 *feedback_index);

unsigned int
hs_compile_context_observe_checked_count(const hs_compile_context_t *ctx);

unsigned int
hs_compile_context_observe_hit_count(const hs_compile_context_t *ctx);

hs_error_t hs_compile_context_get_checkpoint_info(
    const hs_compile_context_t *ctx, unsigned int checkpoint,
    hs_compile_context_checkpoint_info_t *info);

unsigned int
hs_compile_context_matcher_build_hit_count(const hs_compile_context_t *ctx);

unsigned int hs_compile_context_matcher_build_hit_dropped_count(
    const hs_compile_context_t *ctx);

hs_error_t hs_compile_context_get_matcher_build_hit_info(
    const hs_compile_context_t *ctx, unsigned int index,
    hs_compile_context_matcher_build_hit_info_t *info);

hs_error_t hs_compile_context_create(hs_compile_context_t **ctx);

hs_error_t hs_compile_context_set_fp_feedback(hs_compile_context_t *ctx,
                                              const hs_fp_feedback_t *feedback);

hs_error_t hs_compile_context_free(hs_compile_context_t *ctx);

hs_error_t hs_compile_multi_with_context(
    const char *const *expressions, const unsigned int *flags,
    const unsigned int *ids, unsigned int elements, unsigned int mode,
    const hs_platform_info_t *platform, const hs_compile_context_t *ctx,
    hs_database_t **db, hs_compile_error_t **error);

hs_error_t hs_compile_ext_multi_with_context(
    const char *const *expressions, const unsigned int *flags,
    const unsigned int *ids, const hs_expr_ext_t *const *ext,
    unsigned int elements, unsigned int mode,
    const hs_platform_info_t *platform, const hs_compile_context_t *ctx,
    hs_database_t **db, hs_compile_error_t **error);

#ifdef __cplusplus
} /* extern C */
#endif

#endif /* FP_COLLECTOR_H */
