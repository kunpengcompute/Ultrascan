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

#include <cstdlib>
#include <cstring>
#include <string>

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

void expectFragmentsEqual(const hs_fp_fragment_info_t &a,
                          const hs_fp_fragment_info_t &b) {
    EXPECT_EQ(a.key, b.key);
    EXPECT_EQ(a.fragment_id, b.fragment_id);
    EXPECT_EQ(a.literal_count, b.literal_count);
    EXPECT_EQ(a.table, b.table);
    EXPECT_EQ(a.engine, b.engine);
    EXPECT_EQ(a.flags, b.flags);
    ASSERT_EQ(a.length, b.length);
    if (a.length) {
        ASSERT_NE(nullptr, a.bytes);
        ASSERT_NE(nullptr, b.bytes);
        EXPECT_EQ(0, memcmp(a.bytes, b.bytes, a.length));
    }
    ASSERT_EQ(a.mask_length, b.mask_length);
    if (a.mask_length) {
        ASSERT_NE(nullptr, a.mask);
        ASSERT_NE(nullptr, b.mask);
        ASSERT_NE(nullptr, a.cmp);
        ASSERT_NE(nullptr, b.cmp);
        EXPECT_EQ(0, memcmp(a.mask, b.mask, a.mask_length));
        EXPECT_EQ(0, memcmp(a.cmp, b.cmp, a.mask_length));
    } else {
        EXPECT_EQ(nullptr, b.mask);
        EXPECT_EQ(nullptr, b.cmp);
    }
    EXPECT_EQ(a.trigger_count, b.trigger_count);
    EXPECT_EQ(a.true_trigger_count, b.true_trigger_count);
    EXPECT_EQ(a.final_report_count, b.final_report_count);
    EXPECT_EQ(a.false_positive_count, b.false_positive_count);
}

void expectReportSummariesEqual(const hs_fp_report_t *a,
                                const hs_fp_report_t *b) {
    hs_fp_report_summary_t sa = {};
    hs_fp_report_summary_t sb = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_get_summary(a, &sa));
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_get_summary(b, &sb));
    EXPECT_EQ(sa.fragment_count, sb.fragment_count);
    EXPECT_EQ(sa.scan_calls, sb.scan_calls);
    EXPECT_EQ(sa.scan_bytes, sb.scan_bytes);
    EXPECT_EQ(sa.trigger_count, sb.trigger_count);
    EXPECT_EQ(sa.true_trigger_count, sb.true_trigger_count);
    EXPECT_EQ(sa.final_report_count, sb.final_report_count);
    EXPECT_EQ(sa.false_positive_count, sb.false_positive_count);
    EXPECT_EQ(sa.unknown_report_count, sb.unknown_report_count);
    EXPECT_EQ(sa.dropped_trigger_count, sb.dropped_trigger_count);
}

void expectFeedbackSummariesEqual(const hs_fp_feedback_t *a,
                                  const hs_fp_feedback_t *b) {
    hs_fp_feedback_summary_t sa = {};
    hs_fp_feedback_summary_t sb = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_get_summary(a, &sa));
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_get_summary(b, &sb));
    EXPECT_EQ(sa.bad_fragment_count, sb.bad_fragment_count);
    EXPECT_EQ(sa.scan_calls, sb.scan_calls);
    EXPECT_EQ(sa.scan_bytes, sb.scan_bytes);
    EXPECT_EQ(sa.total_false_positive_count,
              sb.total_false_positive_count);
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

} // namespace

TEST(FpCollector, NullArguments) {
    hs_fp_collector_t *collector = nullptr;
    hs_fp_report_t *report = nullptr;
    hs_fp_feedback_t *feedback = nullptr;
    hs_fp_report_summary_t report_summary = {};
    hs_fp_feedback_summary_t feedback_summary = {};
    hs_fp_fragment_info_t fragment = {};
    char *serialized = nullptr;
    size_t serialized_len = 0;

    ASSERT_EQ(HS_INVALID, hs_fp_collector_create(nullptr, nullptr));
    ASSERT_EQ(HS_INVALID, hs_fp_collector_create(nullptr, &collector));
    ASSERT_EQ(HS_INVALID, hs_fp_collector_reset(nullptr));
    ASSERT_EQ(HS_INVALID, hs_fp_collector_merge(nullptr, nullptr));
    ASSERT_EQ(HS_INVALID, hs_fp_collector_report(nullptr, &report));
    ASSERT_EQ(HS_INVALID, hs_fp_collector_report(collector, nullptr));
    ASSERT_EQ(HS_INVALID, hs_fp_feedback_build(nullptr, &feedback));
    ASSERT_EQ(HS_INVALID, hs_fp_feedback_build(report, nullptr));
    EXPECT_EQ(HS_INVALID,
              hs_fp_report_get_summary(nullptr, &report_summary));
    EXPECT_EQ(HS_INVALID, hs_fp_report_get_summary(report, nullptr));
    EXPECT_EQ(HS_INVALID,
              hs_fp_report_get_fragment(nullptr, 0, &fragment));
    EXPECT_EQ(HS_INVALID, hs_fp_report_get_fragment(report, 0, nullptr));
    EXPECT_EQ(HS_INVALID,
              hs_fp_feedback_get_summary(nullptr, &feedback_summary));
    EXPECT_EQ(HS_INVALID, hs_fp_feedback_get_summary(feedback, nullptr));
    EXPECT_EQ(HS_INVALID,
              hs_fp_feedback_get_fragment(nullptr, 0, &fragment));
    EXPECT_EQ(HS_INVALID, hs_fp_feedback_get_fragment(feedback, 0, nullptr));
    EXPECT_EQ(HS_INVALID,
              hs_fp_report_serialize(nullptr, &serialized, &serialized_len));
    EXPECT_EQ(HS_INVALID,
              hs_fp_report_serialize(report, nullptr, &serialized_len));
    EXPECT_EQ(HS_INVALID,
              hs_fp_report_serialize(report, &serialized, nullptr));
    EXPECT_EQ(HS_INVALID,
              hs_fp_report_deserialize(nullptr, 0, &report));
    EXPECT_EQ(HS_INVALID,
              hs_fp_report_deserialize("x", 1, nullptr));
    EXPECT_EQ(HS_INVALID,
              hs_fp_feedback_serialize(nullptr, &serialized,
                                       &serialized_len));
    EXPECT_EQ(HS_INVALID,
              hs_fp_feedback_serialize(feedback, nullptr, &serialized_len));
    EXPECT_EQ(HS_INVALID,
              hs_fp_feedback_serialize(feedback, &serialized, nullptr));
    EXPECT_EQ(HS_INVALID,
              hs_fp_feedback_deserialize(nullptr, 0, &feedback));
    EXPECT_EQ(HS_INVALID,
              hs_fp_feedback_deserialize("x", 1, nullptr));
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
    hs_fp_report_summary_t report_summary = {};
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_report_get_summary(report, &report_summary));
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
    EXPECT_EQ(0U, feedback_summary.total_false_positive_count);
    EXPECT_EQ(HS_INVALID,
              hs_fp_feedback_get_fragment(feedback, 0, &fragment));

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
    hs_fp_report_summary_t summary = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_get_summary(report, &summary));
    EXPECT_EQ(1U, summary.scan_calls);
    EXPECT_EQ(sizeof(data) - 1, summary.scan_bytes);
    EXPECT_GE(summary.trigger_count, 1U);
    EXPECT_EQ(1U, summary.true_trigger_count);
    EXPECT_EQ(1U, summary.final_report_count);
    EXPECT_EQ(0U, summary.false_positive_count);
    EXPECT_EQ(0U, summary.dropped_trigger_count);

    FpReportEntry entry;
    ASSERT_TRUE(findEntryByBytes(report, "foo", &entry));
    EXPECT_NE(0U, entry.table);
    EXPECT_TRUE(isKnownEngine(entry.engine));
    EXPECT_GE(entry.literal_count, 1U);
    EXPECT_GE(entry.trigger_count, 1U);
    EXPECT_EQ(1U, entry.true_trigger_count);
    EXPECT_EQ(1U, entry.final_report_count);

    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
}

TEST(FpCollector, StableFragmentKeyAcrossCompiles) {
    hs_scratch_t *scratch1 = nullptr;
    hs_database_t *db1 = buildDBAndScratch("foo", 0, 0, HS_MODE_BLOCK,
                                           &scratch1);
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
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_collector_create(nocase_db, &nocase_collector));

    const char data[] = "foo";
    CallBackContext matches1;
    CallBackContext matches2;
    ASSERT_EQ(HS_SUCCESS,
              hs_scan_with_collector(db1, data, sizeof(data) - 1, 0,
                                     scratch1, record_cb, &matches1,
                                     collector1));
    ASSERT_EQ(HS_SUCCESS,
              hs_scan_with_collector(db2, data, sizeof(data) - 1, 0,
                                     scratch2, record_cb, &matches2,
                                     collector2));

    const char nocase_data[] = "FOO";
    CallBackContext nocase_matches;
    ASSERT_EQ(HS_SUCCESS,
              hs_scan_with_collector(nocase_db, nocase_data,
                                     sizeof(nocase_data) - 1, 0,
                                     nocase_scratch, record_cb,
                                     &nocase_matches, nocase_collector));

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
    hs_fp_report_summary_t summary = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_get_summary(report, &summary));
    EXPECT_EQ(1U, summary.scan_calls);
    EXPECT_GE(summary.trigger_count, 1U);
    EXPECT_EQ(0U, summary.true_trigger_count);
    EXPECT_EQ(0U, summary.final_report_count);
    EXPECT_GE(summary.false_positive_count, 1U);
    EXPECT_EQ(0U, summary.dropped_trigger_count);

    FpReportEntry entry;
    ASSERT_TRUE(findEntryByBytes(report, "foo", &entry));
    EXPECT_NE(0U, entry.table);
    EXPECT_TRUE(isKnownEngine(entry.engine));
    EXPECT_GE(entry.literal_count, 1U);
    EXPECT_GE(entry.trigger_count, 1U);
    EXPECT_EQ(0U, entry.true_trigger_count);
    EXPECT_EQ(0U, entry.final_report_count);

    hs_fp_feedback_t *feedback = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_build(report, &feedback));
    ASSERT_NE(nullptr, feedback);
    hs_fp_feedback_summary_t feedback_summary = {};
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_feedback_get_summary(feedback, &feedback_summary));
    EXPECT_EQ(0U, feedback_summary.bad_fragment_count);
    EXPECT_GE(feedback_summary.total_false_positive_count, 1U);

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
    hs_fp_report_summary_t summary = {};
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_get_summary(report, &summary));
    EXPECT_EQ(2U, summary.scan_calls);
    EXPECT_EQ(2U, summary.true_trigger_count);
    EXPECT_EQ(2U, summary.final_report_count);
    EXPECT_EQ(0U, summary.false_positive_count);
    EXPECT_EQ(0U, summary.dropped_trigger_count);

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

TEST(FpCollector, SerializeReportAndFeedbackRoundTrip) {
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

    char *report_bytes = nullptr;
    size_t report_len = 0;
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_report_serialize(report, &report_bytes, &report_len));
    ASSERT_NE(nullptr, report_bytes);
    ASSERT_GT(report_len, 0U);

    hs_fp_report_t *report_copy = nullptr;
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_report_deserialize(report_bytes, report_len,
                                       &report_copy));
    ASSERT_NE(nullptr, report_copy);
    expectReportSummariesEqual(report, report_copy);

    FpReportEntry report_entry = {};
    FpReportEntry report_copy_entry = {};
    ASSERT_TRUE(findEntryByBytes(report, "foo", &report_entry));
    ASSERT_TRUE(findEntryByBytes(report_copy, "foo", &report_copy_entry));
    expectFragmentsEqual(report_entry, report_copy_entry);

    hs_fp_feedback_t *feedback = nullptr;
    hs_fp_feedback_t *feedback_from_copy = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_build(report, &feedback));
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_feedback_build(report_copy, &feedback_from_copy));
    ASSERT_NE(nullptr, feedback);
    ASSERT_NE(nullptr, feedback_from_copy);
    expectFeedbackSummariesEqual(feedback, feedback_from_copy);

    char *feedback_bytes = nullptr;
    size_t feedback_len = 0;
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_feedback_serialize(feedback, &feedback_bytes,
                                       &feedback_len));
    ASSERT_NE(nullptr, feedback_bytes);
    ASSERT_GT(feedback_len, 0U);

    hs_fp_feedback_t *feedback_copy = nullptr;
    ASSERT_EQ(HS_SUCCESS,
              hs_fp_feedback_deserialize(feedback_bytes, feedback_len,
                                         &feedback_copy));
    ASSERT_NE(nullptr, feedback_copy);
    expectFeedbackSummariesEqual(feedback, feedback_copy);

    FpFeedbackEntry feedback_entry = {};
    FpFeedbackEntry feedback_copy_entry = {};
    ASSERT_TRUE(findFeedbackByBytes(feedback, "foo", &feedback_entry));
    ASSERT_TRUE(findFeedbackByBytes(feedback_copy, "foo",
                                    &feedback_copy_entry));
    expectFragmentsEqual(feedback_entry, feedback_copy_entry);

    hs_fp_report_t *bad_report = nullptr;
    hs_fp_feedback_t *bad_feedback = nullptr;
    EXPECT_EQ(HS_INVALID,
              hs_fp_report_deserialize(report_bytes, report_len - 1,
                                       &bad_report));
    EXPECT_EQ(nullptr, bad_report);
    EXPECT_EQ(HS_INVALID,
              hs_fp_feedback_deserialize(feedback_bytes, feedback_len - 1,
                                         &bad_feedback));
    EXPECT_EQ(nullptr, bad_feedback);
    EXPECT_EQ(HS_INVALID,
              hs_fp_feedback_deserialize(report_bytes, report_len,
                                         &bad_feedback));
    EXPECT_EQ(HS_INVALID,
              hs_fp_report_deserialize(feedback_bytes, feedback_len,
                                       &bad_report));

    char saved = report_bytes[0];
    report_bytes[0] ^= 0x7f;
    EXPECT_EQ(HS_INVALID,
              hs_fp_report_deserialize(report_bytes, report_len,
                                       &bad_report));
    report_bytes[0] = saved;

    saved = report_bytes[4];
    report_bytes[4] ^= 0x7f;
    EXPECT_EQ(HS_DB_VERSION_ERROR,
              hs_fp_report_deserialize(report_bytes, report_len,
                                       &bad_report));
    report_bytes[4] = saved;

    saved = feedback_bytes[4];
    feedback_bytes[4] ^= 0x7f;
    EXPECT_EQ(HS_DB_VERSION_ERROR,
              hs_fp_feedback_deserialize(feedback_bytes, feedback_len,
                                         &bad_feedback));
    feedback_bytes[4] = saved;

    std::free(feedback_bytes);
    std::free(report_bytes);
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback_copy));
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback_from_copy));
    ASSERT_EQ(HS_SUCCESS, hs_fp_feedback_free(feedback));
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report_copy));
    ASSERT_EQ(HS_SUCCESS, hs_fp_report_free(report));
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_free(collector));
    ASSERT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
    ASSERT_EQ(HS_SUCCESS, hs_free_database(db));
    hs_free_compile_error(compile_err);
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
    EXPECT_GE(feedback_summary.total_false_positive_count, 1000U);
    ASSERT_GE(feedback_summary.bad_fragment_count, 1U);

    FpFeedbackEntry entry;
    ASSERT_TRUE(findFeedbackByBytes(feedback, "foo", &entry));
    EXPECT_NE(0U, entry.table);
    EXPECT_TRUE(isKnownEngine(entry.engine));
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
    EXPECT_GE(hs_compile_context_block_checked_count(ctx), 1U);

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
              hs_compile_multi_with_context(&violet_expr, &violet_flags,
                                            &violet_id, 1, HS_MODE_BLOCK,
                                            nullptr, ctx, &ctx_violet_db,
                                            &ctx_violet_err));
    ASSERT_NE(nullptr, normal_violet_db);
    ASSERT_NE(nullptr, ctx_violet_db);
    EXPECT_GE(hs_compile_context_block_checked_count(ctx), 1U);

    hs_scratch_t *normal_violet_scratch = nullptr;
    hs_scratch_t *ctx_violet_scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS,
              hs_alloc_scratch(normal_violet_db, &normal_violet_scratch));
    ASSERT_EQ(HS_SUCCESS,
              hs_alloc_scratch(ctx_violet_db, &ctx_violet_scratch));

    const char violet_data[] = "barXYZfoo";
    CallBackContext normal_violet_matches;
    CallBackContext ctx_violet_matches;
    ASSERT_EQ(HS_SUCCESS,
              hs_scan(normal_violet_db, violet_data, sizeof(violet_data) - 1,
                      0, normal_violet_scratch, record_cb,
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
              hs_compile_ext_multi(&expr, &flags, &id, &extp, 1,
                                   HS_MODE_BLOCK, nullptr, &db,
                                   &compile_err));
    ASSERT_NE(nullptr, db);

    hs_scratch_t *scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db, &scratch));

    hs_fp_collector_t *collector = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_fp_collector_create(db, &collector));

    const char collect_data[] = "\x01" "foo";
    for (u32 i = 0; i < 1000; i++) {
        CallBackContext matches;
        ASSERT_EQ(HS_SUCCESS,
                  hs_scan_with_collector(db, collect_data,
                                         sizeof(collect_data) - 1, 0, scratch,
                                         record_cb, &matches, collector));
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
    EXPECT_TRUE(isKnownEngine(entry.engine));

    hs_compile_context_t *ctx = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_create(&ctx));
    ASSERT_EQ(HS_SUCCESS, hs_compile_context_set_fp_feedback(ctx, feedback));

    hs_database_t *normal_db = nullptr;
    hs_database_t *ctx_db = nullptr;
    hs_compile_error_t *normal_err = nullptr;
    hs_compile_error_t *ctx_err = nullptr;
    ASSERT_EQ(HS_SUCCESS,
              hs_compile_multi(&expr, &flags, &id, 1, HS_MODE_BLOCK, nullptr,
                               &normal_db, &normal_err));
    ASSERT_EQ(HS_SUCCESS,
              hs_compile_multi_with_context(&expr, &flags, &id, 1,
                                            HS_MODE_BLOCK, nullptr, ctx,
                                            &ctx_db, &ctx_err));
    ASSERT_NE(nullptr, normal_db);
    ASSERT_NE(nullptr, ctx_db);
    EXPECT_GE(hs_compile_context_block_checked_count(ctx), 1U);
    EXPECT_GE(hs_compile_context_blocked_count(ctx), 1U);

    hs_scratch_t *normal_scratch = nullptr;
    hs_scratch_t *ctx_scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(normal_db, &normal_scratch));
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(ctx_db, &ctx_scratch));

    const char *scan_data[] = {"\x01" "foo", "\x1f" "foo", "Afoo",
                               "xx\x02" "fooyy"};
    for (const char *data : scan_data) {
        CallBackContext normal_matches;
        CallBackContext ctx_matches;
        const unsigned int length = static_cast<unsigned int>(strlen(data));
        ASSERT_EQ(HS_SUCCESS,
                  hs_scan(normal_db, data, length, 0, normal_scratch,
                          record_cb, &normal_matches));
        ASSERT_EQ(HS_SUCCESS,
                  hs_scan(ctx_db, data, length, 0, ctx_scratch, record_cb,
                          &ctx_matches));
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
