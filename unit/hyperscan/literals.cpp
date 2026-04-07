/*
 * Copyright (c) 2017, Intel Corporation
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

#include "config.h"
#include "test_util.h"
#include "gtest/gtest.h"

#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_set>
#include <boost/random.hpp>

using namespace std;
using namespace testing;

class HyperscanLiteralTest
    : public TestWithParam<tuple<unsigned /* hyperscan mode */,
                                 unsigned /* flags to apply to all patterns */,
                                 unsigned /* number of literals */,
                                 pair<unsigned, unsigned> /* len min,max */,
                                 bool /* add non-literal case */>> {
protected:
    virtual void SetUp() {
        tie(mode, all_flags, num, bounds, add_non_literal) = GetParam();
        rng.seed(29785643);

        if (mode & HS_MODE_STREAM && all_flags & HS_FLAG_SOM_LEFTMOST) {
            mode |= HS_MODE_SOM_HORIZON_LARGE;
        }
    }

    // Returns (regex, corpus)
    pair<string, string> random_lit(unsigned min_len, unsigned max_len) {
        boost::random::uniform_int_distribution<> len_dist(min_len, max_len);
        size_t len = len_dist(rng);

        // Limit alphabet to [a-z] so that caseless tests include only alpha
        // chars and can be entirely caseless.
        boost::random::uniform_int_distribution<> dist('a', 'z');

        ostringstream oss;
        string corpus;
        for (size_t i = 0; i < len; i++) {
            char c = dist(rng);
            oss << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                << ((unsigned)c & 0xff);
            corpus.push_back(c);
        }
        return {oss.str(), corpus};
    }

    virtual void TearDown() {}

    boost::random::mt19937 rng;
    unsigned mode;
    unsigned all_flags;
    unsigned num;
    pair<unsigned, unsigned> bounds;
    bool add_non_literal;
};

static
int count_cb(unsigned, unsigned long long, unsigned long long, unsigned,
             void *ctxt) {
    size_t *count = (size_t *)ctxt;
    (*count)++;
    return 0;
}

static
void do_scan_block(const vector<string> &corpora, const hs_database_t *db,
                   hs_scratch_t *scratch) {
    size_t count = 0;
    for (const auto &s : corpora) {
        size_t before = count;
        hs_error_t err =
            hs_scan(db, s.c_str(), s.size(), 0, scratch, count_cb, &count);
        ASSERT_EQ(HS_SUCCESS, err);
        ASSERT_LT(before, count);
    }
}

static
void do_scan_stream(const vector<string> &corpora, const hs_database_t *db,
                    hs_scratch_t *scratch) {
    size_t count = 0;
    for (const auto &s : corpora) {
        size_t before = count;
        hs_stream_t *stream = nullptr;
        hs_error_t err = hs_open_stream(db, 0, &stream);
        ASSERT_EQ(HS_SUCCESS, err);
        err = hs_scan_stream(stream, s.c_str(), s.size(), 0, scratch, count_cb,
                             &count);
        ASSERT_EQ(HS_SUCCESS, err);
        ASSERT_LT(before, count);
        err = hs_close_stream(stream, scratch, dummy_cb, nullptr);
        ASSERT_EQ(HS_SUCCESS, err);
    }
}

static
void do_scan_vectored(const vector<string> &corpora, const hs_database_t *db,
                      hs_scratch_t *scratch) {
    size_t count = 0;
    for (const auto &s : corpora) {
        size_t before = count;
        const char *const data[] = {s.c_str()};
        const unsigned int data_len[] = {(unsigned int)s.size()};
        hs_error_t err = hs_scan_vector(db, data, data_len, 1, 0, scratch,
                                        count_cb, &count);
        ASSERT_EQ(HS_SUCCESS, err);
        ASSERT_LT(before, count);
    }
}

static
void do_scan(unsigned mode, const vector<string> &corpora,
             const hs_database_t *db) {
    hs_scratch_t *scratch = nullptr;
    hs_error_t err = hs_alloc_scratch(db, &scratch);
    ASSERT_EQ(HS_SUCCESS, err);

    if (mode & HS_MODE_BLOCK) {
        do_scan_block(corpora, db, scratch);
    } else if (mode & HS_MODE_STREAM) {
        do_scan_stream(corpora, db, scratch);
    } else if (mode & HS_MODE_VECTORED) {
        do_scan_vectored(corpora, db, scratch);
    }

    err = hs_free_scratch(scratch);
    ASSERT_EQ(HS_SUCCESS, err);
}


TEST_P(HyperscanLiteralTest, Caseful) {
    vector<pattern> patterns;
    vector<string> corpora;
    for (unsigned i = 0; i < num; i++) {
        auto r = random_lit(bounds.first, bounds.second);
        unsigned flags = all_flags;
        patterns.emplace_back(std::move(r.first), flags, i);
        corpora.emplace_back(std::move(r.second));
    }

    if (add_non_literal) {
        patterns.emplace_back("hatstand.*teakettle", 0, num + 1);
        corpora.push_back("hatstand teakettle");
    }

    auto *db = buildDB(patterns, mode);
    ASSERT_TRUE(db != nullptr);

    do_scan(mode, corpora, db);

    hs_free_database(db);
}

TEST_P(HyperscanLiteralTest, Caseless) {
    vector<pattern> patterns;
    vector<string> corpora;
    for (unsigned i = 0; i < num; i++) {
        auto r = random_lit(bounds.first, bounds.second);
        unsigned flags = all_flags | HS_FLAG_CASELESS;
        patterns.emplace_back(std::move(r.first), flags, i);
        corpora.emplace_back(std::move(r.second));
    }

    if (add_non_literal) {
        patterns.emplace_back("hatstand.*teakettle", 0, num + 1);
        corpora.push_back("hatstand teakettle");
    }

    auto *db = buildDB(patterns, mode);
    ASSERT_TRUE(db != nullptr);

    do_scan(mode, corpora, db);

    hs_free_database(db);
}

TEST_P(HyperscanLiteralTest, MixedCase) {
    vector<pattern> patterns;
    vector<string> corpora;
    for (unsigned i = 0; i < num; i++) {
        auto r = random_lit(bounds.first, bounds.second);
        unsigned flags = all_flags;
        if (i % 2) {
            flags |= HS_FLAG_CASELESS;
        }
        patterns.emplace_back(std::move(r.first), flags, i);
        corpora.emplace_back(std::move(r.second));
    }

    if (add_non_literal) {
        patterns.emplace_back("hatstand.*teakettle", 0, num + 1);
        corpora.push_back("hatstand teakettle");
    }

    auto *db = buildDB(patterns, mode);
    ASSERT_TRUE(db != nullptr);

    do_scan(mode, corpora, db);

    hs_free_database(db);
}

static const unsigned test_modes[] = {HS_MODE_BLOCK, HS_MODE_STREAM,
                                      HS_MODE_VECTORED};

static const unsigned test_flags[] = {0, HS_FLAG_SINGLEMATCH,
                                      HS_FLAG_SOM_LEFTMOST};

static const unsigned test_sizes[] = {1, 10, 100, 500, 10000};

static const pair<unsigned, unsigned> test_bounds[] = {{3u, 10u}, {10u, 100u}};

// Test for Lily algorithm - single character literals with specific report IDs
TEST(HyperscanLiteralTest, LilySingleCharReportIDs) {
    // Test with single character literals and specific report IDs
    // This test verifies that the Lily algorithm correctly reports the right IDs
    vector<pattern> patterns;
    
    // Add single character patterns with different report IDs
    patterns.emplace_back("a", 0, 100);  // 'a' with report ID 100
    patterns.emplace_back("b", 0, 200);  // 'b' with report ID 200
    patterns.emplace_back("c", 0, 300);  // 'c' with report ID 300
    patterns.emplace_back("d", 0, 400);  // 'd' with report ID 400
    patterns.emplace_back("e", 0, 500);  // 'e' with report ID 500
    patterns.emplace_back("f", 0, 600);  // 'f' with report ID 600
    patterns.emplace_back("g", 0, 700);  // 'g' with report ID 700
    patterns.emplace_back("h", 0, 800);  // 'h' with report ID 800
    
    // Build database in block mode
    const unsigned mode = HS_MODE_BLOCK;
    auto *db = buildDB(patterns, mode);
    ASSERT_TRUE(db != nullptr);
    
    // Scratch space
    hs_scratch_t *scratch = nullptr;
    hs_error_t err = hs_alloc_scratch(db, &scratch);
    ASSERT_EQ(HS_SUCCESS, err);
    
    // Test string containing all characters
    const string test_str = "abcdefgh";
    
    // Track which report IDs were found
    unordered_set<unsigned> found_ids;
    
    // Callback function to capture report IDs
    auto capture_id_cb = [](unsigned int id, unsigned long long, 
                            unsigned long long, unsigned int, 
                            void *ctxt) -> int {
        auto *ids = static_cast<unordered_set<unsigned>*>(ctxt);
        ids->insert(id);
        return 0;
    };
    
    // Scan the test string
    err = hs_scan(db, test_str.c_str(), test_str.size(), 0, scratch, capture_id_cb, &found_ids);
    ASSERT_EQ(HS_SUCCESS, err);
    
    // Verify all report IDs were found
    ASSERT_EQ(found_ids.size(), patterns.size());
    for (const auto &p : patterns) {
        ASSERT_TRUE(found_ids.count(p.id) > 0) << "Report ID " << p.id << " not found";
    }
    
    // Free resources
    err = hs_free_scratch(scratch);
    ASSERT_EQ(HS_SUCCESS, err);
    hs_free_database(db);
}

// Block块模式多引擎联合匹配，上报顺序校验
TEST(MMAdaptor, RoseAfterLilyBlockMode) {
    hs_database_t *db = nullptr;
    hs_compile_error_t *compile_err = nullptr;
    CallBackContext c;
    const string data = "a456a";
    const char *expr[] = {"a", "456"};
    unsigned flags[] = {0, 0};
    unsigned ids[] = {10, 20};

    // 编译多规则集，块模式
    hs_error_t err = hs_compile_multi(expr, flags, ids, 2, HS_MODE_BLOCK,
                                      nullptr, &db, &compile_err);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(db != nullptr);

    hs_scratch_t *scratch = nullptr;
    err = hs_alloc_scratch(db, &scratch);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(scratch != nullptr);

    // 初始化上下文+块模式扫描
    c.clear();
    err = hs_scan(db, data.c_str(), data.size(), 0, scratch, record_cb, (void *)&c);
    ASSERT_EQ(HS_SUCCESS, err);

    // 校验1：匹配数量（3次）
    ASSERT_EQ(3U, c.matches.size());
    // 校验2：目标匹配记录存在
    ASSERT_TRUE(find(c.matches.begin(), c.matches.end(), MatchRecord(1, 10)) != c.matches.end());
    ASSERT_TRUE(find(c.matches.begin(), c.matches.end(), MatchRecord(4, 20)) != c.matches.end());
    ASSERT_TRUE(find(c.matches.begin(), c.matches.end(), MatchRecord(5, 10)) != c.matches.end());
    // 校验3：to严格递增
    for (size_t i = 1; i < c.matches.size(); ++i) {
        ASSERT_TRUE(c.matches[i].to > c.matches[i-1].to);
    }

    hs_free_database(db);
    err = hs_free_scratch(scratch);
    ASSERT_EQ(HS_SUCCESS, err);
    hs_free_compile_error(compile_err);
}

// Stream流模式多引擎联合匹配，上报顺序校验
TEST(MMAdaptor, RoseAfterLilyStreamSingleMode) {
    hs_database_t *db = nullptr;
    hs_compile_error_t *compile_err = nullptr;
    CallBackContext c;
    const string data = "a456a";
    const char *expr[] = {"a", "456"};
    unsigned flags[] = {0, 0};
    unsigned ids[] = {10, 20};

    // 编译多规则集，流模式
    hs_error_t err = hs_compile_multi(expr, flags, ids, 2, HS_MODE_STREAM,
                                      nullptr, &db, &compile_err);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(db != nullptr);

    hs_scratch_t *scratch = nullptr;
    err = hs_alloc_scratch(db, &scratch);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(scratch != nullptr);

    hs_stream_t *stream = nullptr;
    err = hs_open_stream(db, 0, &stream);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(stream != nullptr);

    // 初始化上下文+流模式扫描（对应hscollider的-t 1参数配置，整段数据一次扫描）
    c.clear();
    err = hs_scan_stream(stream, data.c_str(), data.size(), 0, scratch, record_cb, (void *)&c);
    ASSERT_TRUE(err == HS_SUCCESS || err == HS_SCAN_TERMINATED);
    err = hs_close_stream(stream, scratch, record_cb, (void *)&c);
    ASSERT_EQ(HS_SUCCESS, err);

    // 校验：数量+记录+to递增
    ASSERT_EQ(3U, c.matches.size());
    ASSERT_TRUE(find(c.matches.begin(), c.matches.end(), MatchRecord(1, 10)) != c.matches.end());
    ASSERT_TRUE(find(c.matches.begin(), c.matches.end(), MatchRecord(4, 20)) != c.matches.end());
    ASSERT_TRUE(find(c.matches.begin(), c.matches.end(), MatchRecord(5, 10)) != c.matches.end());
    for (size_t i = 1; i < c.matches.size(); ++i) {
        ASSERT_TRUE(c.matches[i].to > c.matches[i-1].to);
    }

    hs_free_database(db);
    err = hs_free_scratch(scratch);
    ASSERT_EQ(HS_SUCCESS, err);
    hs_free_compile_error(compile_err);
}

INSTANTIATE_TEST_CASE_P(LiteralTest, HyperscanLiteralTest,
                        Combine(ValuesIn(test_modes), ValuesIn(test_flags),
                                ValuesIn(test_sizes), ValuesIn(test_bounds),
                                Bool()));
