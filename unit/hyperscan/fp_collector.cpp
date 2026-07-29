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

#include "fp_collector.h"
#include "hs.h"
#include "scratch.h"
#include "test_util.h"
#include "gtest/gtest.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

using FpReportEntry = hs_fp_fragment_info_t;
using FpFeedbackEntry = hs_fp_fragment_info_t;

bool isKnownEngine(unsigned int engine) {
    switch (engine) {
    case HS_FP_ENGINE_NOODLE:
    case HS_FP_ENGINE_FDR:
    case HS_FP_ENGINE_NEO_FDR:
    case HS_FP_ENGINE_HAO:
    case HS_FP_ENGINE_TEDDY:
        return true;
    default:
        return false;
    }
}

hs_compile_context_checkpoint_info_t
getCheckpointInfo(const hs_compile_context_t *ctx, unsigned int checkpoint) {
    hs_compile_context_checkpoint_info_t info = {};
    EXPECT_EQ(HS_SUCCESS,
              hs_compile_context_get_checkpoint_info(ctx, checkpoint, &info));
    return info;
}

unsigned int sumCheckpointChecked(const hs_compile_context_t *ctx) {
    unsigned int total = 0;
    for (unsigned int i = 0; i < HS_FP_COMPILE_CHECKPOINT_COUNT; i++) {
        hs_compile_context_checkpoint_info_t info = getCheckpointInfo(ctx, i);
        total += info.checked_count;
    }
    return total;
}

unsigned int sumCheckpointBlocked(const hs_compile_context_t *ctx) {
    unsigned int total = 0;
    for (unsigned int i = 0; i < HS_FP_COMPILE_CHECKPOINT_COUNT; i++) {
        hs_compile_context_checkpoint_info_t info = getCheckpointInfo(ctx, i);
        total += info.blocked_count;
    }
    return total;
}

bool findEntryByBytes(const hs_fp_report_t *report, const std::string &needle,
                      FpReportEntry *out) {
    hs_fp_report_summary_t summary = {};
    if (hs_fp_report_get_summary(report, &summary) != HS_SUCCESS) {
        return false;
    }

    for (u32 i = 0; i < summary.fragment_count; i++) {
        FpReportEntry entry = {};
        hs_error_t err = hs_fp_report_get_fragment(report, i, &entry);
        if (err != HS_SUCCESS || !entry.bytes) {
            continue;
        }

        std::string fragment(reinterpret_cast<const char *>(entry.bytes),
                             entry.length);
        if (fragment == needle) {
            if (out) {
                *out = entry;
            }
            return true;
        }
    }

    return false;
}

bool findFeedbackByBytes(const hs_fp_feedback_t *feedback,
                         const std::string &needle, FpFeedbackEntry *out) {
    hs_fp_feedback_summary_t summary = {};
    if (hs_fp_feedback_get_summary(feedback, &summary) != HS_SUCCESS) {
        return false;
    }

    for (u32 i = 0; i < summary.bad_fragment_count; i++) {
        FpFeedbackEntry entry = {};
        hs_error_t err = hs_fp_feedback_get_fragment(feedback, i, &entry);
        if (err != HS_SUCCESS || !entry.bytes) {
            continue;
        }

        std::string fragment(reinterpret_cast<const char *>(entry.bytes),
                             entry.length);
        if (fragment == needle) {
            if (out) {
                *out = entry;
            }
            return true;
        }
    }

    return false;
}

bool findMaskedFeedback(const hs_fp_feedback_t *feedback,
                        FpFeedbackEntry *out) {
    hs_fp_feedback_summary_t summary = {};
    if (hs_fp_feedback_get_summary(feedback, &summary) != HS_SUCCESS) {
        return false;
    }

    for (u32 i = 0; i < summary.bad_fragment_count; i++) {
        FpFeedbackEntry entry = {};
        hs_error_t err = hs_fp_feedback_get_fragment(feedback, i, &entry);
        if (err != HS_SUCCESS || !entry.mask_length) {
            continue;
        }

        if (out) {
            *out = entry;
        }
        return true;
    }

    return false;
}

std::string escapeLiteralBytes(const std::string &bytes) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 4);
    for (unsigned char c : bytes) {
        out.push_back('\\');
        out.push_back('x');
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 0xf]);
    }
    return out;
}

void collectFalsePositiveSamples(hs_database_t *db, hs_scratch_t *scratch,
                                 hs_fp_collector_t *collector, const char *data,
                                 size_t length, u32 repeat_count = 1000) {
    for (u32 i = 0; i < repeat_count; i++) {
        CallBackContext c;
        ASSERT_EQ(HS_SUCCESS, hs_scan_with_collector(
                                  db, data, static_cast<unsigned int>(length),
                                  0, scratch, record_cb, &c, collector));
        ASSERT_TRUE(c.matches.empty());
    }
}

void buildFalsePositiveFeedback(const char *expr, const char *data,
                                hs_fp_feedback_t **feedback,
                                u64a min_offset = 10) {
    ASSERT_NE(nullptr, feedback);
    *feedback = nullptr;

    unsigned int flags = 0;
    unsigned int id = 1;
    hs_expr_ext ext = {};
    ext.flags = HS_EXT_FLAG_MIN_OFFSET;
    ext.min_offset = min_offset;
    const hs_expr_ext *extp = &ext;

    hs_compile_error_t *compile_err = nullptr;
    hs_database_t *db = nullptr;
    ASSERT_EQ(HS_SUCCESS,
              hs_compile_ext_multi(&expr, &flags, &id, &extp, 1, HS_MODE_BLOCK,
                                   nullptr, &db, &compile_err));
    ASSERT_NE(nullptr, db);

    hs_scratch_t *scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db, &scratch));

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector));

    collectFalsePositiveSamples(db, scratch, collector, data, strlen(data));

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_build(report, feedback));
    ASSERT_NE(nullptr, *feedback);

    hs_fp_feedback_summary_t summary = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_get_summary(*feedback, &summary));
    ASSERT_GE(summary.bad_fragment_count, 1U);

    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
    hs_free_compile_error(compile_err);
}

void expectBlockScansMatch(hs_database_t *normal_db,
                           hs_scratch_t *normal_scratch, hs_database_t *ctx_db,
                           hs_scratch_t *ctx_scratch, const std::string &data) {
    CallBackContext normal_matches;
    CallBackContext ctx_matches;
    ASSERT_EQ(HS_SUCCESS, hs_scan(normal_db, data.data(),
                                  static_cast<unsigned int>(data.size()), 0,
                                  normal_scratch, record_cb, &normal_matches));
    ASSERT_EQ(HS_SUCCESS, hs_scan(ctx_db, data.data(),
                                  static_cast<unsigned int>(data.size()), 0,
                                  ctx_scratch, record_cb, &ctx_matches));
    ASSERT_EQ(normal_matches.matches, ctx_matches.matches);
}

struct FeedbackDumpCapture {
    hs_fp_feedback_dump_summary_t summary = {};
    unsigned int summary_calls = 0;
    unsigned int fragment_calls = 0;
    unsigned int selected_calls = 0;
    std::vector<hs_fp_fragment_info_t> selected;
};

void HS_CDECL captureDumpSummary(const hs_fp_feedback_dump_summary_t *summary,
                                 void *context) {
    ASSERT_NE(nullptr, summary);
    ASSERT_NE(nullptr, context);
    FeedbackDumpCapture *capture = static_cast<FeedbackDumpCapture *>(context);
    capture->summary = *summary;
    capture->summary_calls++;
}

void HS_CDECL captureDumpFragment(const hs_fp_fragment_info_t *fragment,
                                  unsigned int selected, void *context) {
    ASSERT_NE(nullptr, fragment);
    ASSERT_NE(nullptr, context);
    FeedbackDumpCapture *capture = static_cast<FeedbackDumpCapture *>(context);
    capture->fragment_calls++;
    if (selected) {
        capture->selected_calls++;
        capture->selected.push_back(*fragment);
    }
}

} // namespace

TEST(FpCollector, NullArguments) {
    hs_fp_collector_t *collector = nullptr;
    hs_fp_report_t *report = nullptr;
    hs_fp_feedback_t *feedback = nullptr;
    hs_fp_report_summary_t report_summary = {};
    hs_fp_feedback_summary_t feedback_summary = {};
    hs_fp_fragment_info_t fragment = {};

    ASSERT_EQ(HS_INVALID, hs_fp_collector_create(nullptr, nullptr));
    ASSERT_EQ(HS_INVALID, hs_fp_collector_create(nullptr, &collector));
    ASSERT_EQ(HS_INVALID, hs_fp_collector_reset(nullptr));
    hs_fp_collector_t *merged = nullptr;
    ASSERT_EQ(HS_INVALID, hs_fp_collector_merge(nullptr, 0, &merged));
    ASSERT_EQ(HS_INVALID, hs_fp_collector_merge(nullptr, 1, &merged));
    ASSERT_EQ(HS_INVALID, hs_fp_collector_merge(&collector, 0, &merged));
    ASSERT_EQ(HS_INVALID, hs_fp_collector_merge(&collector, 1, nullptr));
    ASSERT_EQ(HS_INVALID, hs_fp_collector_report(nullptr, &report));
    ASSERT_EQ(HS_INVALID, hs_fp_collector_report(collector, nullptr));
    ASSERT_EQ(HS_INVALID, hs_fp_feedback_build(nullptr, &feedback));
    ASSERT_EQ(HS_INVALID, hs_fp_feedback_build(report, nullptr));
    ASSERT_EQ(HS_INVALID,
              hs_fp_feedback_build_ext(nullptr, nullptr, &feedback));
    ASSERT_EQ(HS_INVALID, hs_fp_feedback_build_ext(report, nullptr, nullptr));
    EXPECT_EQ(HS_INVALID, hs_fp_report_get_summary(nullptr, &report_summary));
    EXPECT_EQ(HS_INVALID, hs_fp_report_get_summary(report, nullptr));
    EXPECT_EQ(HS_INVALID, hs_fp_report_get_fragment(nullptr, 0, &fragment));
    EXPECT_EQ(HS_INVALID, hs_fp_report_get_fragment(report, 0, nullptr));
    EXPECT_EQ(HS_INVALID,
              hs_fp_feedback_get_summary(nullptr, &feedback_summary));
    EXPECT_EQ(HS_INVALID, hs_fp_feedback_get_summary(feedback, nullptr));
    EXPECT_EQ(HS_INVALID, hs_fp_feedback_get_fragment(nullptr, 0, &fragment));
    EXPECT_EQ(HS_INVALID, hs_fp_feedback_get_fragment(feedback, 0, nullptr));
    EXPECT_EQ(0U, hs_compile_context_observe_checked_count(nullptr));
    EXPECT_EQ(0U, hs_compile_context_observe_hit_count(nullptr));
    hs_compile_context_checkpoint_info_t checkpoint_info = {};
    EXPECT_EQ(HS_INVALID, hs_compile_context_get_checkpoint_info(
                              nullptr, HS_FP_COMPILE_CHECKPOINT_LITERAL_SPLIT,
                              &checkpoint_info));
    ASSERT_EQ(HS_INVALID, hs_compile_context_create(nullptr));
    ASSERT_EQ(HS_INVALID, hs_compile_context_set_fp_feedback(nullptr, nullptr));

    hs_compile_context_t *ctx = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_create(&ctx));
    EXPECT_EQ(HS_SUCCESS, hs_compile_context_get_checkpoint_info(
                              ctx, HS_FP_COMPILE_CHECKPOINT_LITERAL_SPLIT,
                              &checkpoint_info));
    EXPECT_EQ(0U, checkpoint_info.checked_count);
    EXPECT_EQ(0U, checkpoint_info.hit_count);
    EXPECT_EQ(0U, checkpoint_info.blocked_count);
    EXPECT_EQ(0U, checkpoint_info.passed_count);
    EXPECT_EQ(HS_INVALID,
              hs_compile_context_get_checkpoint_info(
                  ctx, HS_FP_COMPILE_CHECKPOINT_COUNT, &checkpoint_info));
    EXPECT_EQ(HS_INVALID,
              hs_compile_context_get_checkpoint_info(
                  ctx, HS_FP_COMPILE_CHECKPOINT_LITERAL_SPLIT, nullptr));
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_free(ctx));

    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(nullptr));
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(nullptr));
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(nullptr));
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_free(nullptr));
}

TEST(FpCollector, LifecycleEmptyReportAndFeedback) {
    hs_database_t *db = buildDB("foo", 0, 0, HS_MODE_BLOCK);
    ASSERT_NE(nullptr, db);

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector));
    ASSERT_NE(nullptr, collector);

    hs_fp_collector_t *collector2 = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector2));
    hs_fp_collector_t *merge_inputs[] = {collector, collector2};
    hs_fp_collector_t *merged = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_merge(merge_inputs, 2, &merged));
    ASSERT_NE(nullptr, merged);
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    collector = merged;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_reset(collector));

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);
    hs_fp_report_summary_t report_summary = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_get_summary(report, &report_summary));
    EXPECT_EQ(0U, report_summary.fragment_count);
    hs_fp_fragment_info_t fragment = {};
    EXPECT_EQ(HS_INVALID, hs_fp_report_get_fragment(report, 0, &fragment));

    hs_fp_feedback_t *feedback = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_build(report, &feedback));
    ASSERT_NE(nullptr, feedback);
    hs_fp_feedback_summary_t feedback_summary = {};
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_feedback_get_summary(feedback, &feedback_summary));
    EXPECT_EQ(0U, feedback_summary.bad_fragment_count);
    EXPECT_EQ(HS_INVALID, hs_fp_feedback_get_fragment(feedback, 0, &fragment));

    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector2));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
}

TEST(FpCollector, FeedbackCreateFromFragmentsCopiesAndValidates) {
    unsigned char bytes[] = {'f', 'o', 'o'};
    hs_fp_feedback_import_fragment import = {};
    import.key = 0x12345678ULL;
    import.table = HS_FP_TABLE_FLOATING;
    import.engine = HS_FP_ENGINE_FDR;
    import.bytes = bytes;
    import.length = sizeof(bytes);
    import.trigger_count = 100;
    import.true_trigger_count = 1;
    import.false_positive_count = 99;

    hs_fp_feedback_t *feedback = nullptr;
    EXPECT_EQ(HS_INVALID,
              hs_fp_feedback_create_from_fragments(&import, 1, nullptr));
    EXPECT_EQ(HS_INVALID,
              hs_fp_feedback_create_from_fragments(nullptr, 1, &feedback));

    ASSERT_EQ(HS_SUCCESS,
              hs_fp_feedback_create_from_fragments(&import, 1, &feedback));
    ASSERT_NE(nullptr, feedback);

    bytes[0] = 'x';
    hs_fp_feedback_summary_t summary = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_get_summary(feedback, &summary));
    EXPECT_EQ(1U, summary.bad_fragment_count);

    hs_fp_fragment_info_t fragment = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_get_fragment(feedback, 0, &fragment));
    ASSERT_NE(nullptr, fragment.bytes);
    EXPECT_EQ(0, std::memcmp(fragment.bytes, "foo", 3));
    EXPECT_EQ(3U, fragment.length);
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));

    import.length = 0;
    EXPECT_EQ(HS_INVALID,
              hs_fp_feedback_create_from_fragments(&import, 1, &feedback));
}

TEST(FpCollector, FeedbackBuildExtThresholdParameters) {
    const char *exprs[] = {"foo", "bar"};
    unsigned int flags[] = {0, 0};
    unsigned int ids[] = {1, 2};
    hs_expr_ext ext0 = {};
    hs_expr_ext ext1 = {};
    ext0.flags = HS_EXT_FLAG_MIN_OFFSET;
    ext1.flags = HS_EXT_FLAG_MIN_OFFSET;
    ext0.min_offset = 10;
    ext1.min_offset = 10;
    const hs_expr_ext *exts[] = {&ext0, &ext1};

    hs_compile_error_t *compile_err = nullptr;
    hs_database_t *db = nullptr;
    ASSERT_EQ(HS_SUCCESS,
              hs_compile_ext_multi(exprs, flags, ids, exts, 2, HS_MODE_BLOCK,
                                   nullptr, &db, &compile_err));
    ASSERT_NE(nullptr, db);

    hs_scratch_t *scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db, &scratch));

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector));

    collectFalsePositiveSamples(db, scratch, collector, "foo", 3, 10);
    collectFalsePositiveSamples(db, scratch, collector, "bar", 3, 5);

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);

    hs_fp_feedback_t *feedback = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_build(report, &feedback));
    ASSERT_NE(nullptr, feedback);
    hs_fp_feedback_summary_t summary = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_get_summary(feedback, &summary));
    EXPECT_EQ(0U, summary.bad_fragment_count);
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));
    feedback = nullptr;

    hs_fp_feedback_params_t params = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_build_ext(report, &params, &feedback));
    ASSERT_NE(nullptr, feedback);
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_get_summary(feedback, &summary));
    EXPECT_EQ(0U, summary.bad_fragment_count);
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));
    feedback = nullptr;

    params.flags = HS_FP_FEEDBACK_PARAM_MIN_TRIGGER_COUNT |
                   HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_COUNT |
                   HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_RATE |
                   HS_FP_FEEDBACK_PARAM_MIN_WASTE_SHARE;
    params.min_trigger_count = 1;
    params.min_false_positive_count = 1;
    params.min_false_positive_rate = HS_FP_FEEDBACK_RATE_SCALE;
    params.min_waste_share = 1;
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_build_ext(report, &params, &feedback));
    ASSERT_NE(nullptr, feedback);
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_get_summary(feedback, &summary));
    EXPECT_GE(summary.bad_fragment_count, 2U);
    EXPECT_TRUE(findFeedbackByBytes(feedback, "foo", nullptr));
    EXPECT_TRUE(findFeedbackByBytes(feedback, "bar", nullptr));
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));
    feedback = nullptr;

    params.min_trigger_count = 0;
    params.min_false_positive_count = 0;
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_build_ext(report, &params, &feedback));
    ASSERT_NE(nullptr, feedback);
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_get_summary(feedback, &summary));
    EXPECT_GE(summary.bad_fragment_count, 2U);
    EXPECT_TRUE(findFeedbackByBytes(feedback, "foo", nullptr));
    EXPECT_TRUE(findFeedbackByBytes(feedback, "bar", nullptr));
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));
    feedback = nullptr;

    params.min_trigger_count = 1;
    params.min_false_positive_count = 1;
    params.flags |= HS_FP_FEEDBACK_PARAM_MAX_BAD_FRAGMENTS;
    params.max_bad_fragments = 1;
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_build_ext(report, &params, &feedback));
    ASSERT_NE(nullptr, feedback);
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_get_summary(feedback, &summary));
    ASSERT_EQ(1U, summary.bad_fragment_count);
    EXPECT_TRUE(findFeedbackByBytes(feedback, "foo", nullptr));
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));
    feedback = nullptr;

    hs_fp_feedback_params_t invalid = {};
    invalid.flags = 0x80000000U;
    EXPECT_EQ(HS_INVALID,
              hs_fp_feedback_build_ext(report, &invalid, &feedback));
    invalid = {};
    invalid.flags = HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_RATE;
    invalid.min_false_positive_rate = HS_FP_FEEDBACK_RATE_SCALE + 1;
    EXPECT_EQ(HS_INVALID,
              hs_fp_feedback_build_ext(report, &invalid, &feedback));
    invalid = {};
    invalid.flags = HS_FP_FEEDBACK_PARAM_MAX_BAD_FRAGMENTS;
    invalid.max_bad_fragments = 0;
    EXPECT_EQ(HS_INVALID,
              hs_fp_feedback_build_ext(report, &invalid, &feedback));

    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
    hs_free_compile_error(compile_err);
}

TEST(FpCollector, CollectorToFeedbackWithDumpBuildsSelectedFragments) {
    const char *expr = "foo";
    unsigned int flags = 0;
    unsigned int id = 1;
    hs_expr_ext ext = {};
    ext.flags = HS_EXT_FLAG_MIN_OFFSET;
    ext.min_offset = 10;
    const hs_expr_ext *extp = &ext;

    hs_compile_error_t *compile_err = nullptr;
    hs_database_t *db = nullptr;
    ASSERT_EQ(HS_SUCCESS,
              hs_compile_ext_multi(&expr, &flags, &id, &extp, 1, HS_MODE_BLOCK,
                                   nullptr, &db, &compile_err));
    ASSERT_NE(nullptr, db);

    hs_scratch_t *scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db, &scratch));

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector));

    collectFalsePositiveSamples(db, scratch, collector, "foo", 3);

    hs_fp_feedback_params_t params = {};
    params.flags = HS_FP_FEEDBACK_PARAM_MIN_TRIGGER_COUNT |
                   HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_COUNT |
                   HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_RATE |
                   HS_FP_FEEDBACK_PARAM_MIN_WASTE_SHARE;
    params.min_trigger_count = 1;
    params.min_false_positive_count = 1;
    params.min_false_positive_rate = HS_FP_FEEDBACK_RATE_SCALE;
    params.min_waste_share = 0;

    hs_fp_feedback_t *feedback = nullptr;
    FeedbackDumpCapture capture;
    hs_fp_feedback_dump_callbacks_t callbacks = {};
    callbacks.on_summary = captureDumpSummary;
    callbacks.on_fragment = captureDumpFragment;
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_collector_to_feedback_with_dump(
                  collector, &params, &callbacks, &capture, &feedback));
    ASSERT_NE(nullptr, feedback);
    EXPECT_EQ(1U, capture.summary_calls);
    EXPECT_GE(capture.summary.fragment_count, 1U);
    EXPECT_GE(capture.summary.bad_fragment_count, 1U);
    EXPECT_GE(capture.summary.trigger_count, 1000U);
    EXPECT_EQ(0U, capture.summary.true_trigger_count);
    EXPECT_GE(capture.summary.false_positive_count, 1000U);
    EXPECT_GE(capture.fragment_calls, capture.summary.fragment_count);
    EXPECT_GE(capture.selected_calls, 1U);
    ASSERT_FALSE(capture.selected.empty());
    EXPECT_NE(nullptr, capture.selected[0].bytes);
    EXPECT_EQ(0U, capture.selected[0].true_trigger_count);
    EXPECT_GE(capture.selected[0].false_positive_count, 1000U);
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));

    feedback = nullptr;
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_collector_to_feedback(collector, &params, &feedback));
    ASSERT_NE(nullptr, feedback);
    EXPECT_GE(hs_fp_feedback_fragment_count(feedback), 1U);
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));

    feedback = nullptr;
    EXPECT_EQ(HS_INVALID,
              hs_fp_collector_to_feedback(nullptr, &params, &feedback));
    EXPECT_EQ(HS_INVALID,
              hs_fp_collector_to_feedback(collector, &params, nullptr));
    EXPECT_EQ(HS_INVALID,
              hs_fp_collector_to_feedback_with_dump(
                  collector, &params, &callbacks, &capture, nullptr));

    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
    hs_free_compile_error(compile_err);
}

TEST(FpCollector, BlockScanMatchesNormalScan) {
    hs_scratch_t *scratch = nullptr;
    hs_database_t *db = buildDBAndScratch("foo", 0, 0, HS_MODE_BLOCK, &scratch);
    ASSERT_NE(nullptr, db);
    ASSERT_NE(nullptr, scratch);

    const char data[] = "xxfooyy";
    CallBackContext normal;
    ASSERT_EQ(HS_SUCCESS, hs_scan(db, data, sizeof(data) - 1, 0, scratch,
                                  record_cb, &normal));

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector));

    EXPECT_EQ(HS_INVALID,
              hs_scan_with_collector(nullptr, data, sizeof(data) - 1, 0,
                                     scratch, record_cb, nullptr, collector));
    EXPECT_EQ(HS_INVALID,
              hs_scan_with_collector(db, data, sizeof(data) - 1, 0, scratch,
                                     record_cb, nullptr, nullptr));

    CallBackContext with_collector;
    ASSERT_EQ(HS_SUCCESS,
              hs_scan_with_collector(db, data, sizeof(data) - 1, 0, scratch,
                                     record_cb, &with_collector, collector));
    ASSERT_EQ(normal.matches, with_collector.matches);

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);
    hs_fp_report_summary_t summary = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_get_summary(report, &summary));
    EXPECT_GE(summary.trigger_count, 1U);
    EXPECT_EQ(1U, summary.true_trigger_count);
    EXPECT_EQ(0U, summary.false_positive_count);

    FpReportEntry entry;
    ASSERT_TRUE(findEntryByBytes(report, "foo", &entry));
    EXPECT_NE(0U, entry.table);
    EXPECT_TRUE(isKnownEngine(entry.engine));
    EXPECT_GE(entry.trigger_count, 1U);
    EXPECT_EQ(1U, entry.true_trigger_count);

    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
}

TEST(FpCollector, VectorScanWithCollectorMatchesNormalScan) {
    hs_scratch_t *scratch = nullptr;
    hs_database_t *db = buildDBAndScratch("^foo.*bar", HS_FLAG_DOTALL, 0,
                                          HS_MODE_VECTORED, &scratch);
    ASSERT_NE(nullptr, db);
    ASSERT_NE(nullptr, scratch);

    const char *data[] = {"foo", "-", "bar"};
    unsigned int len[] = {3, 1, 3};

    CallBackContext normal;
    ASSERT_EQ(HS_SUCCESS,
              hs_scan_vector(db, data, len, 3, 0, scratch, record_cb, &normal));

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector));

    EXPECT_EQ(HS_INVALID,
              hs_scan_vector_with_collector(db, data, len, 3, 0, scratch,
                                            record_cb, nullptr, nullptr));
    EXPECT_EQ(HS_INVALID,
              hs_scan_vector_with_collector(db, nullptr, len, 3, 0, scratch,
                                            record_cb, nullptr, collector));
    EXPECT_EQ(HS_INVALID,
              hs_scan_vector_with_collector(db, data, nullptr, 3, 0, scratch,
                                            record_cb, nullptr, collector));

    CallBackContext with_collector;
    ASSERT_EQ(HS_SUCCESS, hs_scan_vector_with_collector(
                              db, data, len, 3, 0, scratch, record_cb,
                              &with_collector, collector));
    ASSERT_EQ(normal.matches, with_collector.matches);

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);
    hs_fp_report_summary_t summary = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_get_summary(report, &summary));
    EXPECT_GE(summary.trigger_count, 1U);
    EXPECT_GE(summary.true_trigger_count, 1U);

    hs_scratch_t *block_scratch = nullptr;
    hs_database_t *block_db =
        buildDBAndScratch("foo", 0, 0, HS_MODE_BLOCK, &block_scratch);
    ASSERT_NE(nullptr, block_db);
    ASSERT_NE(nullptr, block_scratch);
    hs_fp_collector_t *block_collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(block_db, &block_collector));
    EXPECT_EQ(HS_DB_MODE_ERROR, hs_scan_vector_with_collector(
                                    block_db, data, len, 1, 0, block_scratch,
                                    dummy_cb, nullptr, block_collector));
    EXPECT_EQ(HS_INVALID, hs_scan_vector_with_collector(
                              block_db, data, len, 1, 0, block_scratch,
                              dummy_cb, nullptr, collector));

    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(block_collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(block_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(block_db));
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
}

TEST(FpCollector, StreamCollectorApisMatchNormalAndValidateArguments) {
    hs_scratch_t *scratch = nullptr;
    hs_database_t *db = buildDBAndScratch("foo.*bar", HS_FLAG_DOTALL, 0,
                                          HS_MODE_STREAM, &scratch);
    ASSERT_NE(nullptr, db);
    ASSERT_NE(nullptr, scratch);

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector));

    hs_stream_t *normal_stream = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_open_stream(db, 0, &normal_stream));
    ASSERT_NE(nullptr, normal_stream);
    CallBackContext normal;
    ASSERT_EQ(HS_SUCCESS, hs_scan_stream(normal_stream, "foo", 3, 0, scratch,
                                         record_cb, &normal));
    ASSERT_EQ(HS_SUCCESS, hs_scan_stream(normal_stream, "bar", 3, 0, scratch,
                                         record_cb, &normal));
    ASSERT_EQ(HS_SUCCESS,
              hs_close_stream(normal_stream, scratch, record_cb, &normal));

    hs_stream_t *stream = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_open_stream(db, 0, &stream));
    ASSERT_NE(nullptr, stream);

    EXPECT_EQ(HS_INVALID,
              hs_scan_stream_with_collector(nullptr, "foo", 3, 0, scratch,
                                            dummy_cb, nullptr, collector));
    EXPECT_EQ(HS_INVALID,
              hs_scan_stream_with_collector(stream, "foo", 3, 0, scratch,
                                            dummy_cb, nullptr, nullptr));

    CallBackContext with_collector;
    ASSERT_EQ(HS_SUCCESS, hs_scan_stream_with_collector(
                              stream, "foo", 3, 0, scratch, record_cb,
                              &with_collector, collector));
    ASSERT_EQ(HS_SUCCESS, hs_scan_stream_with_collector(
                              stream, "bar", 3, 0, scratch, record_cb,
                              &with_collector, collector));
    ASSERT_EQ(normal.matches, with_collector.matches);

    hs_stream_t *copy_stream = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_open_stream(db, 0, &copy_stream));
    ASSERT_NE(nullptr, copy_stream);
    ASSERT_EQ(HS_SUCCESS, hs_reset_and_copy_stream(copy_stream, stream, nullptr,
                                                   nullptr, nullptr));
    ASSERT_EQ(HS_SUCCESS,
              hs_reset_stream(stream, 0, nullptr, nullptr, nullptr));

    size_t used = 0;
    ASSERT_EQ(HS_INSUFFICIENT_SPACE,
              hs_compress_stream(copy_stream, nullptr, 0, &used));
    ASSERT_GT(used, 0U);
    std::vector<char> compressed(used);
    ASSERT_EQ(HS_SUCCESS, hs_compress_stream(copy_stream, compressed.data(),
                                             compressed.size(), &used));
    ASSERT_EQ(HS_SUCCESS,
              hs_reset_and_expand_stream(stream, compressed.data(), used,
                                         nullptr, nullptr, nullptr));

    hs_database_t *other_db = buildDB("bar", 0, 0, HS_MODE_STREAM);
    ASSERT_NE(nullptr, other_db);
    hs_fp_collector_t *other_collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(other_db, &other_collector));
    EXPECT_EQ(HS_INVALID, hs_scan_stream_with_collector(
                              stream, "bar", 3, 0, scratch, dummy_cb, nullptr,
                              other_collector));
    ASSERT_EQ(HS_SUCCESS,
              hs_close_stream(copy_stream, scratch, nullptr, nullptr));
    ASSERT_EQ(HS_SUCCESS, hs_close_stream(stream, scratch, nullptr, nullptr));

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);
    hs_fp_report_summary_t summary = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_get_summary(report, &summary));
    EXPECT_GE(summary.trigger_count, 1U);
    EXPECT_GE(summary.true_trigger_count, 1U);

    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(other_collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(other_db));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
}

TEST(FpCollector, CollectorResetClearsRuntimeCounters) {
    hs_scratch_t *scratch = nullptr;
    hs_database_t *db = buildDBAndScratch("foo", 0, 0, HS_MODE_BLOCK, &scratch);
    ASSERT_NE(nullptr, db);
    ASSERT_NE(nullptr, scratch);

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector));

    const char data[] = "xxfooyy";
    CallBackContext matches;
    ASSERT_EQ(HS_SUCCESS,
              hs_scan_with_collector(db, data, sizeof(data) - 1, 0, scratch,
                                     record_cb, &matches, collector));
    ASSERT_FALSE(matches.matches.empty());

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);
    hs_fp_report_summary_t summary = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_get_summary(report, &summary));
    EXPECT_GE(summary.fragment_count, 1U);
    EXPECT_GE(summary.trigger_count, 1U);
    EXPECT_GE(summary.true_trigger_count, 1U);
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));

    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_reset(collector));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);
    memset(&summary, 0, sizeof(summary));
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_get_summary(report, &summary));
    EXPECT_EQ(0U, summary.fragment_count);
    EXPECT_EQ(0U, summary.trigger_count);
    EXPECT_EQ(0U, summary.true_trigger_count);
    EXPECT_EQ(0U, summary.false_positive_count);

    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
}

TEST(FpCollector, StableFragmentKeyAcrossCompiles) {
    hs_scratch_t *scratch1 = nullptr;
    hs_database_t *db1 =
        buildDBAndScratch("foo", 0, 0, HS_MODE_BLOCK, &scratch1);
    ASSERT_NE(nullptr, db1);

    const char *expressions[] = {"barbarbar", "foo"};
    const unsigned int flags[] = {0, 0};
    const unsigned int ids[] = {1, 2};
    hs_compile_error_t *compile_err = nullptr;
    hs_database_t *db2 = nullptr;
    ASSERT_EQ(HS_SUCCESS,
              hs_compile_multi(expressions, flags, ids, 2, HS_MODE_BLOCK,
                               nullptr, &db2, &compile_err));
    ASSERT_NE(nullptr, db2);

    hs_scratch_t *scratch2 = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db2, &scratch2));

    hs_scratch_t *nocase_scratch = nullptr;
    hs_database_t *nocase_db = buildDBAndScratch(
        "foo", HS_FLAG_CASELESS, 0, HS_MODE_BLOCK, &nocase_scratch);
    ASSERT_NE(nullptr, nocase_db);

    hs_fp_collector_t *collector1 = nullptr;
    hs_fp_collector_t *collector2 = nullptr;
    hs_fp_collector_t *nocase_collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db1, &collector1));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db2, &collector2));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(nocase_db, &nocase_collector));

    const char data[] = "foo";
    CallBackContext matches1;
    CallBackContext matches2;
    ASSERT_EQ(HS_SUCCESS,
              hs_scan_with_collector(db1, data, sizeof(data) - 1, 0, scratch1,
                                     record_cb, &matches1, collector1));
    ASSERT_EQ(HS_SUCCESS,
              hs_scan_with_collector(db2, data, sizeof(data) - 1, 0, scratch2,
                                     record_cb, &matches2, collector2));

    const char nocase_data[] = "FOO";
    CallBackContext nocase_matches;
    ASSERT_EQ(HS_SUCCESS, hs_scan_with_collector(
                              nocase_db, nocase_data, sizeof(nocase_data) - 1,
                              0, nocase_scratch, record_cb, &nocase_matches,
                              nocase_collector));

    hs_fp_report_t *report1 = nullptr;
    hs_fp_report_t *report2 = nullptr;
    hs_fp_report_t *nocase_report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector1, &report1));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector2, &report2));
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_collector_report(nocase_collector, &nocase_report));

    FpReportEntry entry1 = {};
    FpReportEntry entry2 = {};
    ASSERT_TRUE(findEntryByBytes(report1, "foo", &entry1));
    ASSERT_TRUE(findEntryByBytes(report2, "foo", &entry2));
    EXPECT_NE(0U, entry1.key);
    EXPECT_TRUE(isKnownEngine(entry1.engine));
    EXPECT_TRUE(isKnownEngine(entry2.engine));
    EXPECT_EQ(entry1.key, entry2.key);

    hs_fp_report_summary_t nocase_summary = {};
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_report_get_summary(nocase_report, &nocase_summary));
    ASSERT_EQ(1U, nocase_summary.fragment_count);
    FpReportEntry nocase_entry = {};
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_report_get_fragment(nocase_report, 0, &nocase_entry));
    EXPECT_NE(0U, nocase_entry.flags & HS_FP_FRAGMENT_FLAG_NOCASE);
    EXPECT_TRUE(isKnownEngine(nocase_entry.engine));
    EXPECT_NE(entry1.key, nocase_entry.key);

    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(nocase_report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report2));
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report1));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(nocase_collector));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector2));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector1));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(nocase_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch2));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch1));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(nocase_db));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db2));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db1));
    hs_free_compile_error(compile_err);
}

TEST(FpCollector, BlockScanRecordsFalsePositiveTrigger) {
    const char *expr = "foo|bar";
    unsigned int flags = 0;
    unsigned int id = 1;
    hs_expr_ext ext = {};
    ext.flags = HS_EXT_FLAG_MIN_OFFSET;
    ext.min_offset = 10;
    const hs_expr_ext *extp = &ext;

    hs_compile_error_t *compile_err = nullptr;
    hs_database_t *db = nullptr;
    ASSERT_EQ(HS_SUCCESS,
              hs_compile_ext_multi(&expr, &flags, &id, &extp, 1, HS_MODE_BLOCK,
                                   nullptr, &db, &compile_err));
    ASSERT_NE(nullptr, db);

    hs_scratch_t *scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db, &scratch));

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector));

    const char data[] = "foo";
    CallBackContext c;
    ASSERT_EQ(HS_SUCCESS,
              hs_scan_with_collector(db, data, sizeof(data) - 1, 0, scratch,
                                     record_cb, &c, collector));
    ASSERT_TRUE(c.matches.empty());

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);
    hs_fp_report_summary_t summary = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_get_summary(report, &summary));
    EXPECT_GE(summary.trigger_count, 1U);
    EXPECT_EQ(0U, summary.true_trigger_count);
    EXPECT_GE(summary.false_positive_count, 1U);

    FpReportEntry entry;
    ASSERT_TRUE(findEntryByBytes(report, "foo", &entry));
    EXPECT_NE(0U, entry.table);
    EXPECT_TRUE(isKnownEngine(entry.engine));
    EXPECT_GE(entry.trigger_count, 1U);
    EXPECT_EQ(0U, entry.true_trigger_count);

    hs_fp_feedback_t *feedback = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_build(report, &feedback));
    ASSERT_NE(nullptr, feedback);
    hs_fp_feedback_summary_t feedback_summary = {};
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_feedback_get_summary(feedback, &feedback_summary));
    EXPECT_EQ(0U, feedback_summary.bad_fragment_count);

    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
    hs_free_compile_error(compile_err);
}

TEST(FpCollector, TriggerHistogramFlushesBatchedTriggers) {
    const char *expr = "foo";
    unsigned int flags = 0;
    unsigned int id = 1;
    hs_expr_ext ext = {};
    ext.flags = HS_EXT_FLAG_MIN_OFFSET;
    ext.min_offset = 20;
    const hs_expr_ext *extp = &ext;

    hs_compile_error_t *compile_err = nullptr;
    hs_database_t *db = nullptr;
    ASSERT_EQ(HS_SUCCESS,
              hs_compile_ext_multi(&expr, &flags, &id, &extp, 1, HS_MODE_BLOCK,
                                   nullptr, &db, &compile_err));
    ASSERT_NE(nullptr, db);

    hs_scratch_t *scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db, &scratch));

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector));

    const std::string data = "foofoofoofoofoofoofoofoo";
    CallBackContext c;
    ASSERT_EQ(HS_SUCCESS,
              hs_scan_with_collector(db, data.data(),
                                     static_cast<unsigned int>(data.size()), 0,
                                     scratch, record_cb, &c, collector));
    ASSERT_EQ(2U, c.matches.size());

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);

    hs_fp_report_summary_t summary = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_get_summary(report, &summary));
    EXPECT_GE(summary.trigger_count, 8U);
    EXPECT_EQ(2U, summary.true_trigger_count);
    EXPECT_GE(summary.false_positive_count, 6U);

    FpReportEntry entry;
    ASSERT_TRUE(findEntryByBytes(report, "foo", &entry));
    EXPECT_GE(entry.trigger_count, 8U);
    EXPECT_EQ(2U, entry.true_trigger_count);
    EXPECT_GE(entry.false_positive_count, 6U);

    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
    hs_free_compile_error(compile_err);
}

TEST(FpCollector, FeedbackBuildIgnoresUnmappedRuntimeKeys) {
    hs_scratch_t *scratch = nullptr;
    hs_database_t *db = buildDBAndScratch("foo", 0, 0, HS_MODE_BLOCK, &scratch);
    ASSERT_NE(nullptr, db);
    ASSERT_NE(nullptr, scratch);

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector));

    scratch->core_info.fp_collector = collector;
    hs_fp_collector_begin_trigger(scratch, 0xffffffffU);
    hs_fp_collector_end_trigger(scratch);
    hs_fp_collector_flush(collector);
    scratch->core_info.fp_collector = nullptr;

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);

    hs_fp_report_summary_t report_summary = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_get_summary(report, &report_summary));
    EXPECT_EQ(0U, report_summary.fragment_count);
    EXPECT_EQ(0U, report_summary.trigger_count);
    EXPECT_EQ(0U, report_summary.false_positive_count);

    hs_fp_fragment_info_t fragment = {};
    EXPECT_EQ(HS_INVALID, hs_fp_report_get_fragment(report, 0, &fragment));

    hs_fp_feedback_params_t params = {};
    params.flags = HS_FP_FEEDBACK_PARAM_MIN_TRIGGER_COUNT |
                   HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_COUNT |
                   HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_RATE |
                   HS_FP_FEEDBACK_PARAM_MIN_WASTE_SHARE;
    params.min_trigger_count = 1;
    params.min_false_positive_count = 1;
    params.min_false_positive_rate = HS_FP_FEEDBACK_RATE_SCALE;
    params.min_waste_share = 1;

    hs_fp_feedback_t *feedback = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_build_ext(report, &params, &feedback));
    ASSERT_NE(nullptr, feedback);
    hs_fp_feedback_summary_t feedback_summary = {};
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_feedback_get_summary(feedback, &feedback_summary));
    EXPECT_EQ(0U, feedback_summary.bad_fragment_count);

    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
}

TEST(FpCollector, UnmappedRuntimeKeysDoNotCreateReportFragments) {
    hs_scratch_t *scratch = nullptr;
    hs_database_t *db = buildDBAndScratch("foo", 0, 0, HS_MODE_BLOCK, &scratch);
    ASSERT_NE(nullptr, db);
    ASSERT_NE(nullptr, scratch);

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector));

    scratch->core_info.fp_collector = collector;
    hs_fp_collector_begin_trigger(scratch, 0xfffffffeU);
    hs_fp_collector_end_trigger(scratch);
    hs_fp_collector_begin_trigger(scratch, 0xffffffffU);
    hs_fp_collector_end_trigger(scratch);
    hs_fp_collector_begin_trigger(scratch, 0xfffffffeU);
    hs_fp_collector_end_trigger(scratch);
    hs_fp_collector_flush(collector);
    scratch->core_info.fp_collector = nullptr;

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);

    hs_fp_report_summary_t summary = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_get_summary(report, &summary));
    EXPECT_EQ(0U, summary.fragment_count);
    EXPECT_EQ(0U, summary.trigger_count);
    EXPECT_EQ(0U, summary.false_positive_count);

    hs_fp_fragment_info_t fragment = {};
    EXPECT_EQ(HS_INVALID, hs_fp_report_get_fragment(report, 0, &fragment));

    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
}

TEST(FpCollector, MergeAggregatesTriggerSummary) {
    hs_scratch_t *scratch1 = nullptr;
    hs_database_t *db =
        buildDBAndScratch("foo", 0, 0, HS_MODE_BLOCK, &scratch1);
    ASSERT_NE(nullptr, db);
    ASSERT_NE(nullptr, scratch1);

    hs_scratch_t *scratch2 = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db, &scratch2));

    hs_fp_collector_t *collector1 = nullptr;
    hs_fp_collector_t *collector2 = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector1));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector2));

    const char data[] = "foo";
    CallBackContext c1;
    CallBackContext c2;
    ASSERT_EQ(HS_SUCCESS,
              hs_scan_with_collector(db, data, sizeof(data) - 1, 0, scratch1,
                                     record_cb, &c1, collector1));
    ASSERT_EQ(HS_SUCCESS,
              hs_scan_with_collector(db, data, sizeof(data) - 1, 0, scratch2,
                                     record_cb, &c2, collector2));
    hs_fp_collector_t *merge_inputs[] = {collector1, collector2};
    hs_fp_collector_t *merged = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_merge(merge_inputs, 2, &merged));
    ASSERT_NE(nullptr, merged);

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(merged, &report));
    ASSERT_NE(nullptr, report);
    hs_fp_report_summary_t summary = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_get_summary(report, &summary));
    EXPECT_EQ(2U, summary.true_trigger_count);
    EXPECT_EQ(0U, summary.false_positive_count);

    FpReportEntry entry;
    ASSERT_TRUE(findEntryByBytes(report, "foo", &entry));
    EXPECT_GE(entry.trigger_count, 2U);
    EXPECT_EQ(2U, entry.true_trigger_count);

    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(merged));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector2));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector1));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch2));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch1));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
}

TEST(FpCollector, CollectorDatabaseMismatchRejected) {
    hs_database_t *db1 = buildDB("foo", 0, 0, HS_MODE_BLOCK);
    hs_database_t *db2 = buildDB("bar", 0, 0, HS_MODE_BLOCK);
    ASSERT_NE(nullptr, db1);
    ASSERT_NE(nullptr, db2);

    hs_scratch_t *scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db2, &scratch));

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db1, &collector));
    hs_fp_collector_t *collector2 = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db2, &collector2));
    hs_fp_collector_t *merge_inputs[] = {collector, collector2};
    hs_fp_collector_t *merged = nullptr;
    EXPECT_EQ(HS_INVALID, hs_fp_collector_merge(merge_inputs, 2, &merged));
    EXPECT_EQ(nullptr, merged);

    const char data[] = "bar";
    ASSERT_EQ(HS_INVALID,
              hs_scan_with_collector(db2, data, sizeof(data) - 1, 0, scratch,
                                     dummy_cb, nullptr, collector));

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);

    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector2));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db2));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db1));
}

TEST(FpCollector, FeedbackBuildClassifiesBadFragment) {
    const char *expr = "foo";
    unsigned int flags = 0;
    unsigned int id = 1;
    hs_expr_ext ext = {};
    ext.flags = HS_EXT_FLAG_MIN_OFFSET;
    ext.min_offset = 10;
    const hs_expr_ext *extp = &ext;

    hs_compile_error_t *compile_err = nullptr;
    hs_database_t *db = nullptr;
    ASSERT_EQ(HS_SUCCESS,
              hs_compile_ext_multi(&expr, &flags, &id, &extp, 1, HS_MODE_BLOCK,
                                   nullptr, &db, &compile_err));
    ASSERT_NE(nullptr, db);

    hs_scratch_t *scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db, &scratch));

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector));

    const char data[] = "foo";
    for (u32 i = 0; i < 1000; i++) {
        CallBackContext c;
        ASSERT_EQ(HS_SUCCESS,
                  hs_scan_with_collector(db, data, sizeof(data) - 1, 0, scratch,
                                         record_cb, &c, collector));
        ASSERT_TRUE(c.matches.empty());
    }

    const char data2[] = "bar";
    for (u32 i = 0; i < 1000; i++) {
        CallBackContext c;
        ASSERT_EQ(HS_SUCCESS,
                  hs_scan_with_collector(db, data2, sizeof(data2) - 1, 0,
                                         scratch, record_cb, &c, collector));
        ASSERT_TRUE(c.matches.empty());
    }

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);

    hs_fp_feedback_t *feedback = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_build(report, &feedback));
    ASSERT_NE(nullptr, feedback);
    hs_fp_feedback_summary_t feedback_summary = {};
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_feedback_get_summary(feedback, &feedback_summary));
    ASSERT_GE(feedback_summary.bad_fragment_count, 1U);

    FpFeedbackEntry entry;
    ASSERT_TRUE(findFeedbackByBytes(feedback, "foo", &entry));
    ASSERT_NE(nullptr, entry.bytes);
    EXPECT_EQ(3U, entry.length);

    hs_compile_context_t *ctx = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_create(&ctx));
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_set_fp_feedback(ctx, feedback));

    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));
    feedback = nullptr;

    const char *compile_expr = "foo";
    unsigned int compile_flags = 0;
    unsigned int compile_id = 7;
    hs_database_t *normal_db = nullptr;
    hs_database_t *ctx_db = nullptr;
    hs_compile_error_t *normal_err = nullptr;
    hs_compile_error_t *ctx_err = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_compile_multi(&compile_expr, &compile_flags,
                                           &compile_id, 1, HS_MODE_BLOCK,
                                           nullptr, &normal_db, &normal_err));
    ASSERT_EQ(HS_SUCCESS, hs_compile_multi_with_context(
                              &compile_expr, &compile_flags, &compile_id, 1,
                              HS_MODE_BLOCK, nullptr, ctx, &ctx_db, &ctx_err));
    ASSERT_NE(nullptr, normal_db);
    ASSERT_NE(nullptr, ctx_db);
    EXPECT_GE(hs_compile_context_observe_checked_count(ctx), 1U);
    EXPECT_GE(sumCheckpointChecked(ctx), 1U);
    hs_compile_context_checkpoint_info_t matcher_info =
        getCheckpointInfo(ctx, HS_FP_COMPILE_CHECKPOINT_MATCHER_BUILD);
    const unsigned int observe_hits = hs_compile_context_observe_hit_count(ctx);
    const unsigned int blocked_count = sumCheckpointBlocked(ctx);
    EXPECT_GE(observe_hits + blocked_count, 1U);
    if (observe_hits) {
        EXPECT_GE(matcher_info.checked_count, 1U);
        EXPECT_GE(matcher_info.hit_count, 1U);
        EXPECT_GE(matcher_info.passed_count, 1U);
    } else {
        EXPECT_GE(blocked_count, 1U);
    }

    hs_scratch_t *normal_scratch = nullptr;
    hs_scratch_t *ctx_scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(normal_db, &normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(ctx_db, &ctx_scratch));

    const char scan_data[] = "xxfooyy";
    CallBackContext normal_matches;
    CallBackContext ctx_matches;
    ASSERT_EQ(HS_SUCCESS,
              hs_scan(normal_db, scan_data, sizeof(scan_data) - 1, 0,
                      normal_scratch, record_cb, &normal_matches));
    ASSERT_EQ(HS_SUCCESS, hs_scan(ctx_db, scan_data, sizeof(scan_data) - 1, 0,
                                  ctx_scratch, record_cb, &ctx_matches));
    ASSERT_EQ(normal_matches.matches, ctx_matches.matches);

    hs_database_t *normal_ext_db = nullptr;
    hs_database_t *ctx_ext_db = nullptr;
    hs_compile_error_t *normal_ext_err = nullptr;
    hs_compile_error_t *ctx_ext_err = nullptr;
    ASSERT_EQ(HS_SUCCESS,
              hs_compile_ext_multi(&compile_expr, &compile_flags, &compile_id,
                                   &extp, 1, HS_MODE_BLOCK, nullptr,
                                   &normal_ext_db, &normal_ext_err));
    ASSERT_EQ(HS_SUCCESS,
              hs_compile_ext_multi_with_context(
                  &compile_expr, &compile_flags, &compile_id, &extp, 1,
                  HS_MODE_BLOCK, nullptr, ctx, &ctx_ext_db, &ctx_ext_err));
    ASSERT_NE(nullptr, normal_ext_db);
    ASSERT_NE(nullptr, ctx_ext_db);
    EXPECT_GE(sumCheckpointChecked(ctx), 1U);
    EXPECT_GE(sumCheckpointBlocked(ctx), 1U);
    hs_compile_context_checkpoint_info_t literal_info =
        getCheckpointInfo(ctx, HS_FP_COMPILE_CHECKPOINT_LITERAL_SPLIT);
    EXPECT_GE(literal_info.checked_count, 1U);
    EXPECT_GE(literal_info.hit_count, 1U);
    EXPECT_GE(literal_info.blocked_count, 1U);

    hs_scratch_t *normal_ext_scratch = nullptr;
    hs_scratch_t *ctx_ext_scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(normal_ext_db, &normal_ext_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(ctx_ext_db, &ctx_ext_scratch));

    const char short_data[] = "foo";
    CallBackContext normal_ext_short;
    CallBackContext ctx_ext_short;
    ASSERT_EQ(HS_SUCCESS,
              hs_scan(normal_ext_db, short_data, sizeof(short_data) - 1, 0,
                      normal_ext_scratch, record_cb, &normal_ext_short));
    ASSERT_EQ(HS_SUCCESS,
              hs_scan(ctx_ext_db, short_data, sizeof(short_data) - 1, 0,
                      ctx_ext_scratch, record_cb, &ctx_ext_short));
    ASSERT_EQ(normal_ext_short.matches, ctx_ext_short.matches);

    const char long_data[] = "0123456789foo";
    CallBackContext normal_ext_long;
    CallBackContext ctx_ext_long;
    ASSERT_EQ(HS_SUCCESS,
              hs_scan(normal_ext_db, long_data, sizeof(long_data) - 1, 0,
                      normal_ext_scratch, record_cb, &normal_ext_long));
    ASSERT_EQ(HS_SUCCESS,
              hs_scan(ctx_ext_db, long_data, sizeof(long_data) - 1, 0,
                      ctx_ext_scratch, record_cb, &ctx_ext_long));
    ASSERT_EQ(normal_ext_long.matches, ctx_ext_long.matches);

    hs_database_t *bad_db = nullptr;
    hs_compile_error_t *bad_err = nullptr;
    EXPECT_EQ(HS_COMPILER_ERROR,
              hs_compile_multi_with_context(nullptr, nullptr, nullptr, 1,
                                            HS_MODE_BLOCK, nullptr, ctx,
                                            &bad_db, &bad_err));
    EXPECT_EQ(nullptr, bad_db);
    EXPECT_EQ(0U, hs_compile_context_observe_checked_count(ctx));
    EXPECT_EQ(0U, hs_compile_context_observe_hit_count(ctx));
    EXPECT_EQ(0U, sumCheckpointChecked(ctx));
    EXPECT_EQ(0U, sumCheckpointBlocked(ctx));
    literal_info =
        getCheckpointInfo(ctx, HS_FP_COMPILE_CHECKPOINT_LITERAL_SPLIT);
    EXPECT_EQ(0U, literal_info.checked_count);
    EXPECT_EQ(0U, literal_info.hit_count);
    EXPECT_EQ(0U, literal_info.blocked_count);
    EXPECT_EQ(0U, literal_info.passed_count);
    hs_compile_context_checkpoint_info_t shortcut_info =
        getCheckpointInfo(ctx, HS_FP_COMPILE_CHECKPOINT_SHORTCUT_LITERAL);
    EXPECT_EQ(0U, shortcut_info.checked_count);
    EXPECT_EQ(0U, shortcut_info.hit_count);
    EXPECT_EQ(0U, shortcut_info.blocked_count);
    EXPECT_EQ(0U, shortcut_info.passed_count);
    hs_free_compile_error(bad_err);

    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(ctx_ext_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(normal_ext_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(ctx_ext_db));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(normal_ext_db));
    hs_free_compile_error(ctx_ext_err);
    hs_free_compile_error(normal_ext_err);

    const char *violet_expr = "bar.*foo";
    unsigned int violet_flags = 0;
    unsigned int violet_id = 11;
    hs_database_t *normal_violet_db = nullptr;
    hs_database_t *ctx_violet_db = nullptr;
    hs_compile_error_t *normal_violet_err = nullptr;
    hs_compile_error_t *ctx_violet_err = nullptr;
    ASSERT_EQ(HS_SUCCESS,
              hs_compile_multi(&violet_expr, &violet_flags, &violet_id, 1,
                               HS_MODE_BLOCK, nullptr, &normal_violet_db,
                               &normal_violet_err));
    ASSERT_EQ(HS_SUCCESS,
              hs_compile_multi_with_context(
                  &violet_expr, &violet_flags, &violet_id, 1, HS_MODE_BLOCK,
                  nullptr, ctx, &ctx_violet_db, &ctx_violet_err));
    ASSERT_NE(nullptr, normal_violet_db);
    ASSERT_NE(nullptr, ctx_violet_db);
    EXPECT_GE(sumCheckpointChecked(ctx), 1U);
    hs_compile_context_checkpoint_info_t violet_info =
        getCheckpointInfo(ctx, HS_FP_COMPILE_CHECKPOINT_VIOLET_SPLIT);
    EXPECT_GE(violet_info.checked_count, 1U);

    hs_scratch_t *normal_violet_scratch = nullptr;
    hs_scratch_t *ctx_violet_scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS,
              hs_alloc_scratch(normal_violet_db, &normal_violet_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(ctx_violet_db, &ctx_violet_scratch));

    const char violet_data[] = "barXYZfoo";
    CallBackContext normal_violet_matches;
    CallBackContext ctx_violet_matches;
    ASSERT_EQ(HS_SUCCESS,
              hs_scan(normal_violet_db, violet_data, sizeof(violet_data) - 1, 0,
                      normal_violet_scratch, record_cb,
                      &normal_violet_matches));
    ASSERT_EQ(HS_SUCCESS,
              hs_scan(ctx_violet_db, violet_data, sizeof(violet_data) - 1, 0,
                      ctx_violet_scratch, record_cb, &ctx_violet_matches));
    ASSERT_EQ(normal_violet_matches.matches, ctx_violet_matches.matches);

    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(ctx_violet_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(normal_violet_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(ctx_violet_db));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(normal_violet_db));
    hs_free_compile_error(ctx_violet_err);
    hs_free_compile_error(normal_violet_err);

    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(ctx_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(ctx_db));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(normal_db));
    hs_free_compile_error(ctx_err);
    hs_free_compile_error(normal_err);
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_free(ctx));
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
    hs_free_compile_error(compile_err);
}

TEST(FpCollector, FeedbackBlocksShortcutLiteral) {
    const char *collect_expr = "abcdefgh";
    unsigned int collect_flags = 0;
    unsigned int collect_id = 41;
    hs_expr_ext ext = {};
    ext.flags = HS_EXT_FLAG_MIN_OFFSET;
    ext.min_offset = 10;
    const hs_expr_ext *extp = &ext;

    hs_compile_error_t *compile_err = nullptr;
    hs_database_t *db = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_compile_ext_multi(
                              &collect_expr, &collect_flags, &collect_id, &extp,
                              1, HS_MODE_BLOCK, nullptr, &db, &compile_err));
    ASSERT_NE(nullptr, db);

    hs_scratch_t *scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db, &scratch));

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector));
    ASSERT_NE(nullptr, collector);

    const char data[] = "abcdefgh";
    for (u32 i = 0; i < 1000; i++) {
        CallBackContext c;
        ASSERT_EQ(HS_SUCCESS,
                  hs_scan_with_collector(db, data, sizeof(data) - 1, 0, scratch,
                                         record_cb, &c, collector));
        ASSERT_TRUE(c.matches.empty());
    }

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);

    hs_fp_feedback_t *feedback = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_build(report, &feedback));
    ASSERT_NE(nullptr, feedback);
    hs_fp_fragment_info_t bad_fragment = {};
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_feedback_get_fragment(feedback, 0, &bad_fragment));
    ASSERT_NE(nullptr, bad_fragment.bytes);
    ASSERT_GE(bad_fragment.length, 5U);
    std::string fragment_bytes(
        reinterpret_cast<const char *>(bad_fragment.bytes),
        bad_fragment.length);
    std::string expr_storage = escapeLiteralBytes(fragment_bytes);

    hs_compile_context_t *ctx = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_create(&ctx));
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_set_fp_feedback(ctx, feedback));
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));
    feedback = nullptr;

    const char *expr = expr_storage.c_str();
    unsigned int flags = 0;
    unsigned int id = 42;
    hs_database_t *normal_db = nullptr;
    hs_database_t *ctx_db = nullptr;
    hs_compile_error_t *normal_err = nullptr;
    hs_compile_error_t *ctx_err = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_compile_multi(&expr, &flags, &id, 1, HS_MODE_BLOCK,
                                           nullptr, &normal_db, &normal_err));
    ASSERT_EQ(HS_SUCCESS, hs_compile_multi_with_context(
                              &expr, &flags, &id, 1, HS_MODE_BLOCK, nullptr,
                              ctx, &ctx_db, &ctx_err));
    ASSERT_NE(nullptr, normal_db);
    ASSERT_NE(nullptr, ctx_db);
    EXPECT_GE(sumCheckpointChecked(ctx), 1U);
    EXPECT_GE(sumCheckpointBlocked(ctx), 1U);
    hs_compile_context_checkpoint_info_t shortcut_info =
        getCheckpointInfo(ctx, HS_FP_COMPILE_CHECKPOINT_SHORTCUT_LITERAL);
    EXPECT_GE(shortcut_info.checked_count, 1U);
    EXPECT_GE(shortcut_info.hit_count, 1U);
    EXPECT_GE(shortcut_info.blocked_count, 1U);

    hs_scratch_t *normal_scratch = nullptr;
    hs_scratch_t *ctx_scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(normal_db, &normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(ctx_db, &ctx_scratch));

    std::string scan_data = "xx" + fragment_bytes + "yy";
    CallBackContext normal_matches;
    CallBackContext ctx_matches;
    ASSERT_EQ(HS_SUCCESS,
              hs_scan(normal_db, scan_data.data(),
                      static_cast<unsigned int>(scan_data.size()), 0,
                      normal_scratch, record_cb, &normal_matches));
    ASSERT_EQ(HS_SUCCESS, hs_scan(ctx_db, scan_data.data(),
                                  static_cast<unsigned int>(scan_data.size()),
                                  0, ctx_scratch, record_cb, &ctx_matches));
    ASSERT_EQ(normal_matches.matches, ctx_matches.matches);

    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(ctx_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(ctx_db));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(normal_db));
    hs_free_compile_error(ctx_err);
    hs_free_compile_error(normal_err);
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_free(ctx));
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
    hs_free_compile_error(compile_err);
}

TEST(FpCollector, FeedbackBlocksDecoratedMaskedLiteral) {
    const char *expr = "[\\x00-\\x1f]foo";
    unsigned int flags = 0;
    unsigned int id = 12;
    hs_expr_ext ext = {};
    ext.flags = HS_EXT_FLAG_MIN_OFFSET;
    ext.min_offset = 10;
    const hs_expr_ext *extp = &ext;

    hs_compile_error_t *compile_err = nullptr;
    hs_database_t *db = nullptr;
    ASSERT_EQ(HS_SUCCESS,
              hs_compile_ext_multi(&expr, &flags, &id, &extp, 1, HS_MODE_BLOCK,
                                   nullptr, &db, &compile_err));
    ASSERT_NE(nullptr, db);

    hs_scratch_t *scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db, &scratch));

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector));

    const char collect_data[] = "\x01"
                                "foo";
    for (u32 i = 0; i < 1000; i++) {
        CallBackContext matches;
        ASSERT_EQ(HS_SUCCESS, hs_scan_with_collector(
                                  db, collect_data, sizeof(collect_data) - 1, 0,
                                  scratch, record_cb, &matches, collector));
        ASSERT_TRUE(matches.matches.empty());
    }

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));

    hs_fp_feedback_t *feedback = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_build(report, &feedback));
    hs_fp_feedback_summary_t feedback_summary = {};
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_feedback_get_summary(feedback, &feedback_summary));
    ASSERT_GE(feedback_summary.bad_fragment_count, 1U);

    FpFeedbackEntry entry;
    ASSERT_TRUE(findMaskedFeedback(feedback, &entry));
    EXPECT_GT(entry.mask_length, 0U);
    ASSERT_NE(nullptr, entry.bytes);

    hs_compile_context_t *ctx = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_create(&ctx));
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_set_fp_feedback(ctx, feedback));

    hs_database_t *normal_db = nullptr;
    hs_database_t *ctx_db = nullptr;
    hs_compile_error_t *normal_err = nullptr;
    hs_compile_error_t *ctx_err = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_compile_multi(&expr, &flags, &id, 1, HS_MODE_BLOCK,
                                           nullptr, &normal_db, &normal_err));
    ASSERT_EQ(HS_SUCCESS, hs_compile_multi_with_context(
                              &expr, &flags, &id, 1, HS_MODE_BLOCK, nullptr,
                              ctx, &ctx_db, &ctx_err));
    ASSERT_NE(nullptr, normal_db);
    ASSERT_NE(nullptr, ctx_db);
    EXPECT_GE(sumCheckpointChecked(ctx), 1U);
    EXPECT_GE(sumCheckpointBlocked(ctx), 1U);
    hs_compile_context_checkpoint_info_t masked_info =
        getCheckpointInfo(ctx, HS_FP_COMPILE_CHECKPOINT_MASKED_LITERAL);
    EXPECT_GE(masked_info.checked_count, 1U);
    EXPECT_GE(masked_info.hit_count, 1U);
    EXPECT_GE(masked_info.blocked_count, 1U);

    hs_scratch_t *normal_scratch = nullptr;
    hs_scratch_t *ctx_scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(normal_db, &normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(ctx_db, &ctx_scratch));

    const char *scan_data[] = {"\x01"
                               "foo",
                               "\x1f"
                               "foo",
                               "Afoo",
                               "xx\x02"
                               "fooyy"};
    for (const char *data : scan_data) {
        CallBackContext normal_matches;
        CallBackContext ctx_matches;
        const unsigned int length = static_cast<unsigned int>(strlen(data));
        ASSERT_EQ(HS_SUCCESS,
                  hs_scan(normal_db, data, length, 0, normal_scratch, record_cb,
                          &normal_matches));
        ASSERT_EQ(HS_SUCCESS, hs_scan(ctx_db, data, length, 0, ctx_scratch,
                                      record_cb, &ctx_matches));
        EXPECT_EQ(normal_matches.matches, ctx_matches.matches);
    }

    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(ctx_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(ctx_db));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(normal_db));
    hs_free_compile_error(ctx_err);
    hs_free_compile_error(normal_err);
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_free(ctx));
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
    hs_free_compile_error(compile_err);
}

TEST(FpCollector, FeedbackBlocksSmallLiteralSet) {
    hs_fp_feedback_t *feedback = nullptr;
    buildFalsePositiveFeedback("foo|bar|baz|qux", "foo", &feedback);
    ASSERT_NE(nullptr, feedback);

    FpFeedbackEntry entry = {};
    ASSERT_TRUE(findFeedbackByBytes(feedback, "foo", &entry));
    ASSERT_NE(nullptr, entry.bytes);
    EXPECT_EQ(3U, entry.length);

    hs_compile_context_t *ctx = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_create(&ctx));
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_set_fp_feedback(ctx, feedback));

    const char *expr = "foo|bar|baz|qux";
    unsigned int flags = 0;
    unsigned int id = 51;
    hs_database_t *normal_db = nullptr;
    hs_database_t *ctx_db = nullptr;
    hs_compile_error_t *normal_err = nullptr;
    hs_compile_error_t *ctx_err = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_compile_multi(&expr, &flags, &id, 1, HS_MODE_BLOCK,
                                           nullptr, &normal_db, &normal_err));
    ASSERT_EQ(HS_SUCCESS, hs_compile_multi_with_context(
                              &expr, &flags, &id, 1, HS_MODE_BLOCK, nullptr,
                              ctx, &ctx_db, &ctx_err));
    ASSERT_NE(nullptr, normal_db);
    ASSERT_NE(nullptr, ctx_db);

    hs_compile_context_checkpoint_info_t small_info =
        getCheckpointInfo(ctx, HS_FP_COMPILE_CHECKPOINT_SMALL_LITERAL_SET);
    EXPECT_GE(small_info.checked_count, 1U);
    EXPECT_GE(small_info.hit_count, 1U);
    EXPECT_GE(small_info.blocked_count, 1U);
    EXPECT_GE(sumCheckpointBlocked(ctx), 1U);

    hs_scratch_t *normal_scratch = nullptr;
    hs_scratch_t *ctx_scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(normal_db, &normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(ctx_db, &ctx_scratch));

    expectBlockScansMatch(normal_db, normal_scratch, ctx_db, ctx_scratch,
                          "xxfooyy");
    expectBlockScansMatch(normal_db, normal_scratch, ctx_db, ctx_scratch,
                          "xxbaryy");
    expectBlockScansMatch(normal_db, normal_scratch, ctx_db, ctx_scratch,
                          "nomatch");

    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(ctx_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(ctx_db));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(normal_db));
    hs_free_compile_error(ctx_err);
    hs_free_compile_error(normal_err);
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_free(ctx));
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));
}

TEST(FpCollector, FeedbackBlocksAnchoredAcyclic) {
    hs_fp_feedback_t *feedback = nullptr;
    buildFalsePositiveFeedback("abcdefgh", "abcdefgh", &feedback);
    ASSERT_NE(nullptr, feedback);

    FpFeedbackEntry entry = {};
    ASSERT_TRUE(findFeedbackByBytes(feedback, "abcdefgh", &entry));
    ASSERT_NE(nullptr, entry.bytes);
    EXPECT_EQ(8U, entry.length);

    hs_compile_context_t *ctx = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_create(&ctx));
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_set_fp_feedback(ctx, feedback));

    const char *expr = "^.{20}abcdefgh";
    unsigned int flags = HS_FLAG_DOTALL;
    unsigned int id = 53;
    hs_database_t *normal_db = nullptr;
    hs_database_t *ctx_db = nullptr;
    hs_compile_error_t *normal_err = nullptr;
    hs_compile_error_t *ctx_err = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_compile_multi(&expr, &flags, &id, 1, HS_MODE_BLOCK,
                                           nullptr, &normal_db, &normal_err));
    ASSERT_EQ(HS_SUCCESS, hs_compile_multi_with_context(
                              &expr, &flags, &id, 1, HS_MODE_BLOCK, nullptr,
                              ctx, &ctx_db, &ctx_err));
    ASSERT_NE(nullptr, normal_db);
    ASSERT_NE(nullptr, ctx_db);

    hs_compile_context_checkpoint_info_t anchored_info =
        getCheckpointInfo(ctx, HS_FP_COMPILE_CHECKPOINT_ANCHORED_ACYCLIC);
    EXPECT_GE(anchored_info.checked_count, 1U);
    EXPECT_GE(anchored_info.hit_count, 1U);
    EXPECT_GE(anchored_info.blocked_count, 1U);
    EXPECT_GE(sumCheckpointBlocked(ctx), 1U);

    hs_scratch_t *normal_scratch = nullptr;
    hs_scratch_t *ctx_scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(normal_db, &normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(ctx_db, &ctx_scratch));

    expectBlockScansMatch(normal_db, normal_scratch, ctx_db, ctx_scratch,
                          std::string(20, 'A') + "abcdefgh");
    expectBlockScansMatch(normal_db, normal_scratch, ctx_db, ctx_scratch,
                          std::string(19, 'A') + "abcdefgh");
    expectBlockScansMatch(normal_db, normal_scratch, ctx_db, ctx_scratch,
                          "nomatch");

    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(ctx_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(ctx_db));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(normal_db));
    hs_free_compile_error(ctx_err);
    hs_free_compile_error(normal_err);
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_free(ctx));
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));
}

TEST(FpCollector, FeedbackBlocksFixedWidthMaskLiteral) {
    hs_fp_feedback_t *feedback = nullptr;
    buildFalsePositiveFeedback("abcdefghijklmnop", "abcdefghijklmnop",
                               &feedback, 32);
    ASSERT_NE(nullptr, feedback);

    hs_fp_fragment_info_t bad_fragment = {};
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_feedback_get_fragment(feedback, 0, &bad_fragment));
    ASSERT_NE(nullptr, bad_fragment.bytes);
    ASSERT_GE(bad_fragment.length, 2U);
    std::string fragment_bytes(
        reinterpret_cast<const char *>(bad_fragment.bytes),
        bad_fragment.length);

    hs_compile_context_t *ctx = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_create(&ctx));
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_set_fp_feedback(ctx, feedback));

    std::string expr_storage = "^.{120}" + escapeLiteralBytes(fragment_bytes);
    const char *expr = expr_storage.c_str();
    unsigned int flags = 0;
    unsigned int id = 52;
    hs_database_t *normal_db = nullptr;
    hs_database_t *ctx_db = nullptr;
    hs_compile_error_t *normal_err = nullptr;
    hs_compile_error_t *ctx_err = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_compile_multi(&expr, &flags, &id, 1, HS_MODE_BLOCK,
                                           nullptr, &normal_db, &normal_err));
    ASSERT_EQ(HS_SUCCESS, hs_compile_multi_with_context(
                              &expr, &flags, &id, 1, HS_MODE_BLOCK, nullptr,
                              ctx, &ctx_db, &ctx_err));
    ASSERT_NE(nullptr, normal_db);
    ASSERT_NE(nullptr, ctx_db);

    hs_compile_context_checkpoint_info_t masked_info =
        getCheckpointInfo(ctx, HS_FP_COMPILE_CHECKPOINT_MASKED_LITERAL);
    EXPECT_GE(masked_info.checked_count, 1U);
    EXPECT_GE(masked_info.hit_count, 1U);
    EXPECT_GE(masked_info.blocked_count, 1U);
    EXPECT_GE(sumCheckpointBlocked(ctx), 1U);

    hs_scratch_t *normal_scratch = nullptr;
    hs_scratch_t *ctx_scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(normal_db, &normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(ctx_db, &ctx_scratch));

    std::string match_data(120, 'A');
    match_data += fragment_bytes;
    expectBlockScansMatch(normal_db, normal_scratch, ctx_db, ctx_scratch,
                          match_data);
    std::string short_data(119, 'A');
    short_data += fragment_bytes;
    expectBlockScansMatch(normal_db, normal_scratch, ctx_db, ctx_scratch,
                          short_data);

    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(ctx_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(ctx_db));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(normal_db));
    hs_free_compile_error(ctx_err);
    hs_free_compile_error(normal_err);
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_free(ctx));
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));
}

TEST(FpCollector, CompileExtMultiWithContextMatchesNormalCompile) {
    hs_compile_context_t *ctx = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_create(&ctx));
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_set_fp_feedback(ctx, nullptr));

    const char *expr = "foo";
    unsigned int flags = 0;
    unsigned int id = 9;
    hs_expr_ext ext = {};
    ext.flags = HS_EXT_FLAG_MIN_OFFSET;
    ext.min_offset = 2;
    const hs_expr_ext *extp = &ext;

    hs_database_t *normal_db = nullptr;
    hs_database_t *ctx_db = nullptr;
    hs_compile_error_t *normal_err = nullptr;
    hs_compile_error_t *ctx_err = nullptr;
    ASSERT_EQ(HS_SUCCESS,
              hs_compile_ext_multi(&expr, &flags, &id, &extp, 1, HS_MODE_BLOCK,
                                   nullptr, &normal_db, &normal_err));
    ASSERT_EQ(HS_SUCCESS, hs_compile_ext_multi_with_context(
                              &expr, &flags, &id, &extp, 1, HS_MODE_BLOCK,
                              nullptr, ctx, &ctx_db, &ctx_err));
    ASSERT_NE(nullptr, normal_db);
    ASSERT_NE(nullptr, ctx_db);
    EXPECT_EQ(0U, hs_compile_context_observe_checked_count(ctx));
    EXPECT_EQ(0U, hs_compile_context_observe_hit_count(ctx));
    EXPECT_EQ(0U, sumCheckpointChecked(ctx));
    EXPECT_EQ(0U, sumCheckpointBlocked(ctx));

    hs_scratch_t *normal_scratch = nullptr;
    hs_scratch_t *ctx_scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(normal_db, &normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(ctx_db, &ctx_scratch));

    const char data[] = "foo";
    CallBackContext normal_matches;
    CallBackContext ctx_matches;
    ASSERT_EQ(HS_SUCCESS, hs_scan(normal_db, data, sizeof(data) - 1, 0,
                                  normal_scratch, record_cb, &normal_matches));
    ASSERT_EQ(HS_SUCCESS, hs_scan(ctx_db, data, sizeof(data) - 1, 0,
                                  ctx_scratch, record_cb, &ctx_matches));
    ASSERT_EQ(normal_matches.matches, ctx_matches.matches);

    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(ctx_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(ctx_db));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(normal_db));
    hs_free_compile_error(ctx_err);
    hs_free_compile_error(normal_err);
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_free(ctx));
}

TEST(FpCollector, CompileWithFeedbackPublicApisMatchNormalCompile) {
    hs_fp_feedback_t *feedback = nullptr;
    buildFalsePositiveFeedback("foo", "foo", &feedback);
    ASSERT_NE(nullptr, feedback);

    const char *expr = "foo";
    unsigned int flags = 0;
    unsigned int id = 19;

    hs_database_t *normal_db = nullptr;
    hs_database_t *feedback_db = nullptr;
    hs_compile_error_t *normal_err = nullptr;
    hs_compile_error_t *feedback_err = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_compile_multi(&expr, &flags, &id, 1, HS_MODE_BLOCK,
                                           nullptr, &normal_db, &normal_err));
    ASSERT_EQ(HS_SUCCESS, hs_compile_multi_with_feedback(
                              &expr, &flags, &id, 1, HS_MODE_BLOCK, nullptr,
                              feedback, &feedback_db, &feedback_err));
    ASSERT_NE(nullptr, normal_db);
    ASSERT_NE(nullptr, feedback_db);

    hs_scratch_t *normal_scratch = nullptr;
    hs_scratch_t *feedback_scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(normal_db, &normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(feedback_db, &feedback_scratch));
    expectBlockScansMatch(normal_db, normal_scratch, feedback_db,
                          feedback_scratch, "xxfooyy");

    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(feedback_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(feedback_db));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(normal_db));
    hs_free_compile_error(feedback_err);
    hs_free_compile_error(normal_err);

    normal_db = nullptr;
    normal_scratch = nullptr;
    normal_err = nullptr;
    hs_database_t *null_feedback_db = nullptr;
    hs_compile_error_t *null_feedback_err = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_compile_multi(&expr, &flags, &id, 1, HS_MODE_BLOCK,
                                           nullptr, &normal_db, &normal_err));
    ASSERT_EQ(HS_SUCCESS, hs_compile_multi_with_feedback(
                              &expr, &flags, &id, 1, HS_MODE_BLOCK, nullptr,
                              nullptr, &null_feedback_db, &null_feedback_err));
    ASSERT_NE(nullptr, normal_db);
    ASSERT_NE(nullptr, null_feedback_db);

    hs_scratch_t *null_feedback_scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(normal_db, &normal_scratch));
    ASSERT_EQ(HS_SUCCESS,
              hs_alloc_scratch(null_feedback_db, &null_feedback_scratch));
    expectBlockScansMatch(normal_db, normal_scratch, null_feedback_db,
                          null_feedback_scratch, "xxfooyy");

    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(null_feedback_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(null_feedback_db));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(normal_db));
    hs_free_compile_error(null_feedback_err);
    hs_free_compile_error(normal_err);

    hs_expr_ext ext = {};
    ext.flags = HS_EXT_FLAG_MIN_OFFSET;
    ext.min_offset = 2;
    const hs_expr_ext *extp = &ext;
    normal_db = nullptr;
    feedback_db = nullptr;
    normal_err = nullptr;
    feedback_err = nullptr;
    ASSERT_EQ(HS_SUCCESS,
              hs_compile_ext_multi(&expr, &flags, &id, &extp, 1, HS_MODE_BLOCK,
                                   nullptr, &normal_db, &normal_err));
    ASSERT_EQ(HS_SUCCESS, hs_compile_ext_multi_with_feedback(
                              &expr, &flags, &id, &extp, 1, HS_MODE_BLOCK,
                              nullptr, feedback, &feedback_db, &feedback_err));
    ASSERT_NE(nullptr, normal_db);
    ASSERT_NE(nullptr, feedback_db);

    normal_scratch = nullptr;
    feedback_scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(normal_db, &normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(feedback_db, &feedback_scratch));
    expectBlockScansMatch(normal_db, normal_scratch, feedback_db,
                          feedback_scratch, "xxfooyy");

    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(feedback_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(feedback_db));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(normal_db));
    hs_free_compile_error(feedback_err);
    hs_free_compile_error(normal_err);
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));
}
