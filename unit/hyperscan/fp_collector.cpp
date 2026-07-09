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

#include "gtest/gtest.h"
#include "fp_collector.h"
#include "hs.h"
#include "test_util.h"

#include <string>

namespace {

struct FpReportEntry {
    u32 key = 0;
    u32 fragment_id = 0;
    u32 literal_count = 0;
    u8 table = 0;
    u8 flags = 0;
    u64a trigger_count = 0;
    u64a true_trigger_count = 0;
    u64a final_report_count = 0;
};

struct FpFeedbackEntry {
    u32 key = 0;
    u32 fragment_id = 0;
    u32 literal_count = 0;
    u8 table = 0;
    u8 flags = 0;
    u64a trigger_count = 0;
    u64a true_trigger_count = 0;
    u64a final_report_count = 0;
    u64a false_positive_count = 0;
};

bool findEntryByBytes(const hs_fp_report_t *report, const std::string &needle,
                      FpReportEntry *out) {
    for (u32 i = 0; i < hs_fp_report_entry_count(report); i++) {
        const u8 *bytes = nullptr;
        size_t length = 0;
        FpReportEntry entry;
        hs_error_t err = hs_fp_report_entry_info(
            report, i, &entry.key, &entry.fragment_id, &entry.literal_count,
            &entry.table, &entry.flags, &bytes, &length, nullptr, nullptr,
            nullptr, &entry.trigger_count, &entry.true_trigger_count,
            &entry.final_report_count);
        if (err != HS_SUCCESS || !bytes) {
            continue;
        }

        std::string fragment(reinterpret_cast<const char *>(bytes), length);
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
    for (u32 i = 0; i < hs_fp_feedback_bad_fragment_count(feedback); i++) {
        const u8 *bytes = nullptr;
        size_t length = 0;
        FpFeedbackEntry entry;
        hs_error_t err = hs_fp_feedback_bad_fragment_info(
            feedback, i, &entry.key, &entry.fragment_id,
            &entry.literal_count, &entry.table, &entry.flags, &bytes, &length,
            nullptr, nullptr, nullptr, &entry.trigger_count,
            &entry.true_trigger_count, &entry.final_report_count,
            &entry.false_positive_count);
        if (err != HS_SUCCESS || !bytes) {
            continue;
        }

        std::string fragment(reinterpret_cast<const char *>(bytes), length);
        if (fragment == needle) {
            if (out) {
                *out = entry;
            }
            return true;
        }
    }

    return false;
}

} // namespace

TEST(FpCollector, NullArguments) {
    hs_fp_collector_t *collector = nullptr;
    hs_fp_report_t *report = nullptr;
    hs_fp_feedback_t *feedback = nullptr;

    ASSERT_EQ(HS_INVALID, hs_fp_collector_create(nullptr, nullptr));
    ASSERT_EQ(HS_INVALID, hs_fp_collector_create(nullptr, &collector));
    ASSERT_EQ(HS_INVALID, hs_fp_collector_reset(nullptr));
    ASSERT_EQ(HS_INVALID, hs_fp_collector_merge(nullptr, nullptr));
    ASSERT_EQ(HS_INVALID, hs_fp_collector_report(nullptr, &report));
    ASSERT_EQ(HS_INVALID, hs_fp_collector_report(collector, nullptr));
    ASSERT_EQ(HS_INVALID, hs_fp_feedback_build(nullptr, &feedback));
    ASSERT_EQ(HS_INVALID, hs_fp_feedback_build(report, nullptr));
    ASSERT_EQ(HS_INVALID,
              hs_fp_report_entry_info(nullptr, 0, nullptr, nullptr, nullptr,
                                      nullptr, nullptr, nullptr, nullptr,
                                      nullptr, nullptr, nullptr, nullptr,
                                      nullptr, nullptr));
    ASSERT_EQ(HS_INVALID,
              hs_fp_feedback_bad_fragment_info(
                  nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr,
                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                  nullptr, nullptr, nullptr));
    EXPECT_EQ(0U, hs_compile_context_observe_checked_count(nullptr));
    EXPECT_EQ(0U, hs_compile_context_observe_hit_count(nullptr));
    EXPECT_EQ(0U, hs_compile_context_block_checked_count(nullptr));
    EXPECT_EQ(0U, hs_compile_context_blocked_count(nullptr));
    ASSERT_EQ(HS_INVALID, hs_compile_context_create(nullptr));
    ASSERT_EQ(HS_INVALID, hs_compile_context_set_fp_feedback(nullptr, nullptr));

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
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_merge(collector, collector2));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_reset(collector));

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);
    EXPECT_EQ(0U, hs_fp_report_entry_count(report));
    EXPECT_EQ(HS_INVALID,
              hs_fp_report_entry_info(report, 0, nullptr, nullptr, nullptr,
                                      nullptr, nullptr, nullptr, nullptr,
                                      nullptr, nullptr, nullptr, nullptr,
                                      nullptr, nullptr));

    hs_fp_feedback_t *feedback = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_build(report, &feedback));
    ASSERT_NE(nullptr, feedback);
    EXPECT_EQ(0U, hs_fp_feedback_bad_fragment_count(feedback));
    EXPECT_EQ(0U, hs_fp_feedback_total_false_positive_trigger_count(feedback));

    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector2));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
}

TEST(FpCollector, BlockScanMatchesNormalScan) {
    hs_scratch_t *scratch = nullptr;
    hs_database_t *db = buildDBAndScratch("foo", 0, 0, HS_MODE_BLOCK,
                                          &scratch);
    ASSERT_NE(nullptr, db);
    ASSERT_NE(nullptr, scratch);

    const char data[] = "xxfooyy";
    CallBackContext normal;
    ASSERT_EQ(HS_SUCCESS, hs_scan(db, data, sizeof(data) - 1, 0, scratch,
                                  record_cb, &normal));

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector));

    CallBackContext with_collector;
    ASSERT_EQ(HS_SUCCESS, hs_scan_with_collector(db, data, sizeof(data) - 1, 0,
                                                 scratch, record_cb,
                                                 &with_collector, collector));
    ASSERT_EQ(normal.matches, with_collector.matches);

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);
    EXPECT_EQ(1U, hs_fp_report_scan_calls(report));
    EXPECT_EQ(sizeof(data) - 1, hs_fp_report_scan_bytes(report));
    EXPECT_GE(hs_fp_report_trigger_count(report), 1U);
    EXPECT_EQ(1U, hs_fp_report_true_trigger_count(report));
    EXPECT_EQ(1U, hs_fp_report_final_report_count(report));
    EXPECT_EQ(0U, hs_fp_report_dropped_trigger_count(report));

    FpReportEntry entry;
    ASSERT_TRUE(findEntryByBytes(report, "foo", &entry));
    EXPECT_NE(0U, entry.table);
    EXPECT_GE(entry.literal_count, 1U);
    EXPECT_GE(entry.trigger_count, 1U);
    EXPECT_EQ(1U, entry.true_trigger_count);
    EXPECT_EQ(1U, entry.final_report_count);

    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
}

TEST(FpCollector, BlockScanRecordsFalsePositiveTrigger) {
    const char *expr = "foo";
    unsigned int flags = 0;
    unsigned int id = 1;
    hs_expr_ext ext = {};
    ext.flags = HS_EXT_FLAG_MIN_OFFSET;
    ext.min_offset = 10;
    const hs_expr_ext *extp = &ext;

    hs_compile_error_t *compile_err = nullptr;
    hs_database_t *db = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_compile_ext_multi(&expr, &flags, &id, &extp, 1,
                                               HS_MODE_BLOCK, nullptr, &db,
                                               &compile_err));
    ASSERT_NE(nullptr, db);

    hs_scratch_t *scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db, &scratch));

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector));

    const char data[] = "foo";
    CallBackContext c;
    ASSERT_EQ(HS_SUCCESS, hs_scan_with_collector(db, data, sizeof(data) - 1, 0,
                                                 scratch, record_cb, &c,
                                                 collector));
    ASSERT_TRUE(c.matches.empty());

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);
    EXPECT_EQ(1U, hs_fp_report_scan_calls(report));
    EXPECT_GE(hs_fp_report_trigger_count(report), 1U);
    EXPECT_EQ(0U, hs_fp_report_true_trigger_count(report));
    EXPECT_EQ(0U, hs_fp_report_final_report_count(report));
    EXPECT_EQ(0U, hs_fp_report_dropped_trigger_count(report));

    FpReportEntry entry;
    ASSERT_TRUE(findEntryByBytes(report, "foo", &entry));
    EXPECT_NE(0U, entry.table);
    EXPECT_GE(entry.literal_count, 1U);
    EXPECT_GE(entry.trigger_count, 1U);
    EXPECT_EQ(0U, entry.true_trigger_count);
    EXPECT_EQ(0U, entry.final_report_count);

    hs_fp_feedback_t *feedback = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_build(report, &feedback));
    ASSERT_NE(nullptr, feedback);
    EXPECT_EQ(0U, hs_fp_feedback_bad_fragment_count(feedback));
    EXPECT_GE(hs_fp_feedback_total_false_positive_trigger_count(feedback), 1U);

    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
    hs_free_compile_error(compile_err);
}

TEST(FpCollector, MergeAggregatesTriggerSummary) {
    hs_scratch_t *scratch1 = nullptr;
    hs_database_t *db = buildDBAndScratch("foo", 0, 0, HS_MODE_BLOCK,
                                          &scratch1);
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
    ASSERT_EQ(HS_SUCCESS, hs_scan_with_collector(db, data, sizeof(data) - 1, 0,
                                                 scratch1, record_cb, &c1,
                                                 collector1));
    ASSERT_EQ(HS_SUCCESS, hs_scan_with_collector(db, data, sizeof(data) - 1, 0,
                                                 scratch2, record_cb, &c2,
                                                 collector2));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_merge(collector1, collector2));

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector1, &report));
    ASSERT_NE(nullptr, report);
    EXPECT_EQ(2U, hs_fp_report_scan_calls(report));
    EXPECT_EQ(2U, hs_fp_report_true_trigger_count(report));
    EXPECT_EQ(2U, hs_fp_report_final_report_count(report));
    EXPECT_EQ(0U, hs_fp_report_dropped_trigger_count(report));

    FpReportEntry entry;
    ASSERT_TRUE(findEntryByBytes(report, "foo", &entry));
    EXPECT_GE(entry.trigger_count, 2U);
    EXPECT_EQ(2U, entry.true_trigger_count);
    EXPECT_EQ(2U, entry.final_report_count);

    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
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

    const char data[] = "bar";
    ASSERT_EQ(HS_INVALID, hs_scan_with_collector(db2, data, sizeof(data) - 1,
                                                 0, scratch, dummy_cb,
                                                 nullptr, collector));

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);

    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
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
    ASSERT_EQ(HS_SUCCESS, hs_compile_ext_multi(&expr, &flags, &id, &extp, 1,
                                               HS_MODE_BLOCK, nullptr, &db,
                                               &compile_err));
    ASSERT_NE(nullptr, db);

    hs_scratch_t *scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db, &scratch));

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector));

    const char data[] = "foo";
    for (u32 i = 0; i < 1000; i++) {
        CallBackContext c;
        ASSERT_EQ(HS_SUCCESS,
                  hs_scan_with_collector(db, data, sizeof(data) - 1, 0,
                                         scratch, record_cb, &c, collector));
        ASSERT_TRUE(c.matches.empty());
    }

    hs_fp_report_t *report = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_report(collector, &report));
    ASSERT_NE(nullptr, report);

    hs_fp_feedback_t *feedback = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_build(report, &feedback));
    ASSERT_NE(nullptr, feedback);
    EXPECT_GE(hs_fp_feedback_total_false_positive_trigger_count(feedback),
              1000U);
    ASSERT_GE(hs_fp_feedback_bad_fragment_count(feedback), 1U);

    FpFeedbackEntry entry;
    ASSERT_TRUE(findFeedbackByBytes(feedback, "foo", &entry));
    EXPECT_NE(0U, entry.table);
    EXPECT_GE(entry.literal_count, 1U);
    EXPECT_GE(entry.trigger_count, 1000U);
    EXPECT_EQ(0U, entry.true_trigger_count);
    EXPECT_EQ(0U, entry.final_report_count);
    EXPECT_GE(entry.false_positive_count, 1000U);

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
    ASSERT_EQ(HS_SUCCESS,
              hs_compile_multi_with_context(&compile_expr, &compile_flags,
                                            &compile_id, 1, HS_MODE_BLOCK,
                                            nullptr, ctx, &ctx_db, &ctx_err));
    ASSERT_NE(nullptr, normal_db);
    ASSERT_NE(nullptr, ctx_db);
    EXPECT_GE(hs_compile_context_observe_checked_count(ctx), 1U);
    EXPECT_GE(hs_compile_context_observe_hit_count(ctx), 1U);

    hs_scratch_t *normal_scratch = nullptr;
    hs_scratch_t *ctx_scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(normal_db, &normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(ctx_db, &ctx_scratch));

    const char scan_data[] = "xxfooyy";
    CallBackContext normal_matches;
    CallBackContext ctx_matches;
    ASSERT_EQ(HS_SUCCESS, hs_scan(normal_db, scan_data, sizeof(scan_data) - 1,
                                  0, normal_scratch, record_cb,
                                  &normal_matches));
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
              hs_compile_ext_multi_with_context(&compile_expr, &compile_flags,
                                                &compile_id, &extp, 1,
                                                HS_MODE_BLOCK, nullptr, ctx,
                                                &ctx_ext_db, &ctx_ext_err));
    ASSERT_NE(nullptr, normal_ext_db);
    ASSERT_NE(nullptr, ctx_ext_db);
    EXPECT_GE(hs_compile_context_block_checked_count(ctx), 1U);
    EXPECT_GE(hs_compile_context_blocked_count(ctx), 1U);

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
    EXPECT_EQ(0U, hs_compile_context_block_checked_count(ctx));
    EXPECT_EQ(0U, hs_compile_context_blocked_count(ctx));
    hs_free_compile_error(bad_err);

    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(ctx_ext_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(normal_ext_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(ctx_ext_db));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(normal_ext_db));
    hs_free_compile_error(ctx_ext_err);
    hs_free_compile_error(normal_ext_err);
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
    ASSERT_EQ(HS_SUCCESS, hs_compile_ext_multi(&expr, &flags, &id, &extp, 1,
                                               HS_MODE_BLOCK, nullptr,
                                               &normal_db, &normal_err));
    ASSERT_EQ(HS_SUCCESS,
              hs_compile_ext_multi_with_context(&expr, &flags, &id, &extp, 1,
                                                HS_MODE_BLOCK, nullptr, ctx,
                                                &ctx_db, &ctx_err));
    ASSERT_NE(nullptr, normal_db);
    ASSERT_NE(nullptr, ctx_db);
    EXPECT_EQ(0U, hs_compile_context_observe_checked_count(ctx));
    EXPECT_EQ(0U, hs_compile_context_observe_hit_count(ctx));
    EXPECT_EQ(0U, hs_compile_context_block_checked_count(ctx));
    EXPECT_EQ(0U, hs_compile_context_blocked_count(ctx));

    hs_scratch_t *normal_scratch = nullptr;
    hs_scratch_t *ctx_scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(normal_db, &normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(ctx_db, &ctx_scratch));

    const char data[] = "foo";
    CallBackContext normal_matches;
    CallBackContext ctx_matches;
    ASSERT_EQ(HS_SUCCESS, hs_scan(normal_db, data, sizeof(data) - 1, 0,
                                  normal_scratch, record_cb,
                                  &normal_matches));
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
