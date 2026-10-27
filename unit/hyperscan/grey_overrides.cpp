/*
 * Copyright (c) 2026, Ultrascan Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
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
#include "hs.h"
#include "test_util.h"
#include "gtest/gtest.h"

#include <cstring>
#include <string>

using namespace std;

namespace {

class ScopedGreyOverridesReset {
public:
    ~ScopedGreyOverridesReset() { hs_reset_grey_overrides(); }
};

// ======================================================================
// hs_set_grey_overrides - Success Cases
// ======================================================================

TEST(GreyOverrides, SetSingleValidKey) {
    hs_error_t err = hs_set_grey_overrides("allowLily:1;");
    ASSERT_EQ(HS_SUCCESS, err);
    hs_reset_grey_overrides();
}

TEST(GreyOverrides, SetMultipleValidKeys) {
    // Same format as the old config.txt
    hs_error_t err =
        hs_set_grey_overrides("allowLily:1;allowHao:1;allowNeoFdr:1;");
    ASSERT_EQ(HS_SUCCESS, err);
    hs_reset_grey_overrides();
}

TEST(GreyOverrides, SetBooleanKeyZero) {
    hs_error_t err = hs_set_grey_overrides("allowLily:0;");
    ASSERT_EQ(HS_SUCCESS, err);
    hs_reset_grey_overrides();
}

TEST(GreyOverrides, SetIntegerKey) {
    hs_error_t err = hs_set_grey_overrides("limitPatternCount:1000;");
    ASSERT_EQ(HS_SUCCESS, err);
    hs_reset_grey_overrides();
}

TEST(GreyOverrides, SetLargeIntegerKey) {
    hs_error_t err = hs_set_grey_overrides(
        "limitPatternCount:8000000;limitDFASize:1073741824;");
    ASSERT_EQ(HS_SUCCESS, err);
    hs_reset_grey_overrides();
}

TEST(GreyOverrides, SetAllKnownEngineSwitches) {
    // Test that all commonly-used engine switches are recognised.
    hs_error_t err =
        hs_set_grey_overrides("allowGough:1;allowMcClellan:1;allowSheng:1;"
                              "allowMcSheng:1;allowNeoFdr:1;allowHao:1;"
                              "allowPuff:1;allowLily:1;allowCastle:1;"
                              "allowTamarama:1;allowSmallWrite:1;"
                              "fdrAllowTeddy:1;fdrAllowFlood:1;");
    ASSERT_EQ(HS_SUCCESS, err);
    hs_reset_grey_overrides();
}

TEST(GreyOverrides, EmptyStringResets) {
    // Set something first
    hs_error_t err = hs_set_grey_overrides("allowLily:1;");
    ASSERT_EQ(HS_SUCCESS, err);

    // Empty string should reset
    err = hs_set_grey_overrides("");
    ASSERT_EQ(HS_SUCCESS, err);

    // After reset, verify compilation uses defaults (no crash)
    hs_database_t *db = nullptr;
    hs_compile_error_t *compile_err = nullptr;
    err = hs_compile("test", 0, HS_MODE_BLOCK, nullptr, &db, &compile_err);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(db != nullptr);
    hs_free_database(db);
}

TEST(GreyOverrides, NullResets) {
    // Set something first
    hs_error_t err = hs_set_grey_overrides("allowHao:1;");
    ASSERT_EQ(HS_SUCCESS, err);

    // NULL should reset
    err = hs_set_grey_overrides(nullptr);
    ASSERT_EQ(HS_SUCCESS, err);
}

TEST(GreyOverrides, CompilationWithLilyEnabled) {
    ScopedGreyOverridesReset reset;

    // Enable Lily and verify compilation still works
    hs_error_t err = hs_set_grey_overrides("allowLily:1;");
    ASSERT_EQ(HS_SUCCESS, err);

    hs_database_t *db = nullptr;
    hs_compile_error_t *compile_err = nullptr;
    const char *expr[] = {"a", "bc", "def", "ghij"};
    unsigned flags[] = {0, 0, 0, 0};
    unsigned ids[] = {10, 20, 30, 40};
    const unsigned pattern_count = 4;

    err = hs_compile_multi(expr, flags, ids, pattern_count, HS_MODE_BLOCK,
                           nullptr, &db, &compile_err);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(db != nullptr);

    // Verify scanning still works
    hs_scratch_t *scratch = nullptr;
    err = hs_alloc_scratch(db, &scratch);
    ASSERT_EQ(HS_SUCCESS, err);

    CallBackContext c;
    err = hs_scan(db, "abcdefghij", 10, 0, scratch, record_cb, &c);
    ASSERT_EQ(HS_SUCCESS, err);
    EXPECT_EQ(pattern_count, c.matches.size());

    hs_free_scratch(scratch);
    hs_free_database(db);
}

TEST(GreyOverrides, CompilationWithHaoEnabled) {
    hs_error_t err = hs_set_grey_overrides("allowHao:1;");
    ASSERT_EQ(HS_SUCCESS, err);

    hs_database_t *db = nullptr;
    hs_compile_error_t *compile_err = nullptr;
    const char *expr[] = {"foo", "bar", "baz"};
    unsigned flags[] = {0, 0, 0};
    unsigned ids[] = {1, 2, 3};

    err = hs_compile_multi(expr, flags, ids, 3, HS_MODE_BLOCK, nullptr, &db,
                           &compile_err);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(db != nullptr);

    hs_scratch_t *scratch = nullptr;
    err = hs_alloc_scratch(db, &scratch);
    ASSERT_EQ(HS_SUCCESS, err);

    CallBackContext c;
    err = hs_scan(db, "foobarbaz", 9, 0, scratch, record_cb, &c);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_EQ(3U, c.matches.size());

    hs_free_scratch(scratch);
    hs_free_database(db);
    hs_reset_grey_overrides();
}

TEST(GreyOverrides, CompilationWithAllThreeSwitchesEnabled) {
    // Simulate the exact old config.txt content
    hs_error_t err =
        hs_set_grey_overrides("allowLily:1;allowHao:1;allowNeoFdr:1;");
    ASSERT_EQ(HS_SUCCESS, err);

    hs_database_t *db = nullptr;
    hs_compile_error_t *compile_err = nullptr;
    const char *expr[] = {"hello", "world"};
    unsigned flags[] = {0, 0};
    unsigned ids[] = {100, 200};

    err = hs_compile_multi(expr, flags, ids, 2, HS_MODE_BLOCK, nullptr, &db,
                           &compile_err);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(db != nullptr);

    hs_scratch_t *scratch = nullptr;
    err = hs_alloc_scratch(db, &scratch);
    ASSERT_EQ(HS_SUCCESS, err);

    CallBackContext c;
    err = hs_scan(db, "hello world", 11, 0, scratch, record_cb, &c);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_EQ(2U, c.matches.size());

    hs_free_scratch(scratch);
    hs_free_database(db);
    hs_reset_grey_overrides();
}

TEST(GreyOverrides, CompilationAfterResetUsesDefaults) {
    // Set overrides then reset
    hs_set_grey_overrides("allowLily:1;");
    hs_reset_grey_overrides();

    // Compilation should work with defaults
    hs_database_t *db = nullptr;
    hs_compile_error_t *compile_err = nullptr;
    const char *expr[] = {"test_pattern"};
    unsigned flags[] = {0};
    unsigned ids[] = {42};

    hs_error_t err = hs_compile_multi(expr, flags, ids, 1, HS_MODE_BLOCK,
                                      nullptr, &db, &compile_err);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(db != nullptr);
    hs_free_database(db);
}

TEST(GreyOverrides, SetBeforeEachCompilation) {
    // Verify that overrides can be changed between compilations
    for (int i = 0; i < 3; i++) {
        hs_error_t err = hs_set_grey_overrides("allowLily:1;");
        ASSERT_EQ(HS_SUCCESS, err);

        hs_database_t *db = nullptr;
        hs_compile_error_t *compile_err = nullptr;
        err = hs_compile("abc", 0, HS_MODE_BLOCK, nullptr, &db, &compile_err);
        ASSERT_EQ(HS_SUCCESS, err);
        ASSERT_TRUE(db != nullptr);
        hs_free_database(db);

        hs_reset_grey_overrides();
    }
}

TEST(GreyOverrides, SetResourceLimits) {
    // Test setting various resource limit parameters
    hs_error_t err = hs_set_grey_overrides(
        "limitPatternCount:100000;limitPatternLength:8000;"
        "limitGraphVertices:200000;limitGraphEdges:500000;"
        "limitDFASize:524288000;limitNFASize:524288;");
    ASSERT_EQ(HS_SUCCESS, err);

    hs_database_t *db = nullptr;
    hs_compile_error_t *compile_err = nullptr;
    err = hs_compile("test_pattern", 0, HS_MODE_BLOCK, nullptr, &db,
                     &compile_err);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(db != nullptr);
    hs_free_database(db);

    hs_reset_grey_overrides();
}

TEST(GreyOverrides, SetVioletParameters) {
    hs_error_t err = hs_set_grey_overrides(
        "violetAvoidSuffixes:2;violetDoubleCut:1;"
        "violetDoubleCutLiteralLen:5;violetEarlyCleanLiteralLen:8;");
    ASSERT_EQ(HS_SUCCESS, err);
    hs_reset_grey_overrides();
}

TEST(GreyOverrides, SetRoseParameters) {
    hs_error_t err =
        hs_set_grey_overrides("roseMcClellanPrefix:2;roseMcClellanSuffix:2;"
                              "roseMcClellanOutfix:2;roseTransformDelay:1;");
    ASSERT_EQ(HS_SUCCESS, err);
    hs_reset_grey_overrides();
}

TEST(GreyOverrides, SetSmallWriteParameters) {
    hs_error_t err = hs_set_grey_overrides(
        "allowSmallWrite:1;smallWriteLargestBuffer:100;"
        "smallWriteMaxPatterns:5000;smallWriteMaxLiterals:5000;");
    ASSERT_EQ(HS_SUCCESS, err);
    hs_reset_grey_overrides();
}

TEST(GreyOverrides, DisableAllOptionalEngines) {
    // Turn off many engines to verify compilation still works
    hs_error_t err =
        hs_set_grey_overrides("allowGough:0;allowSheng:0;allowMcSheng:0;"
                              "allowPuff:0;allowCastle:0;allowSmallWrite:0;"
                              "allowTamarama:0;");
    ASSERT_EQ(HS_SUCCESS, err);

    hs_database_t *db = nullptr;
    hs_compile_error_t *compile_err = nullptr;
    err = hs_compile("simple_pattern", 0, HS_MODE_BLOCK, nullptr, &db,
                     &compile_err);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(db != nullptr);
    hs_free_database(db);

    hs_reset_grey_overrides();
}

TEST(GreyOverrides, SingleCharValue) {
    // Value with no trailing semicolon should still work
    // (the parser handles end-of-string as delimiter)
    hs_error_t err = hs_set_grey_overrides("allowLily:1");
    ASSERT_EQ(HS_SUCCESS, err);
    hs_reset_grey_overrides();
}

// ======================================================================
// hs_set_grey_overrides - Failure Cases
// ======================================================================

TEST(GreyOverrides, InvalidKey) {
    hs_error_t err = hs_set_grey_overrides("nonexistent_key:1;");
    ASSERT_EQ(HS_INVALID, err);
}

TEST(GreyOverrides, InvalidKeyAmongValidKeys) {
    hs_error_t err = hs_set_grey_overrides("allowLily:1;bad_key:0;allowHao:1;");
    ASSERT_EQ(HS_INVALID, err);
}

TEST(GreyOverrides, MissingColon) {
    hs_error_t err = hs_set_grey_overrides("allowLily1;");
    ASSERT_EQ(HS_INVALID, err);
}

TEST(GreyOverrides, NonNumericValue) {
    hs_error_t err = hs_set_grey_overrides("allowLily:abc;");
    ASSERT_EQ(HS_INVALID, err);
}

TEST(GreyOverrides, NegativeValue) {
    // Negative values may be converted to large unsigned by lexical_cast;
    // the parser accepts them without error (behaviour depends on Boost
    // version).
    hs_error_t err = hs_set_grey_overrides("allowLily:-1;");
    // On most Boost versions this succeeds (converts to large positive number).
    // On some it throws bad_lexical_cast and returns HS_INVALID.
    // Either way, it does not crash.
    (void)err;
    hs_reset_grey_overrides();
}

TEST(GreyOverrides, HelpStringRejected) {
    hs_error_t err = hs_set_grey_overrides("help");
    ASSERT_EQ(HS_INVALID, err);
}

TEST(GreyOverrides, HelpColonStringRejected) {
    hs_error_t err = hs_set_grey_overrides("help:0");
    ASSERT_EQ(HS_INVALID, err);
}

TEST(GreyOverrides, EmptyKeyBeforeColon) {
    hs_error_t err = hs_set_grey_overrides(":1;");
    ASSERT_EQ(HS_INVALID, err);
}

TEST(GreyOverrides, OnlySemicolons) {
    // Only semicolons: parser skips leading semicolons between entries.
    // This is effectively an empty overrides string, which is valid.
    hs_error_t err = hs_set_grey_overrides(";;;");
    ASSERT_EQ(HS_SUCCESS, err);
    hs_reset_grey_overrides();
}

TEST(GreyOverrides, OnlyColon) {
    hs_error_t err = hs_set_grey_overrides(":");
    ASSERT_EQ(HS_INVALID, err);
}

TEST(GreyOverrides, InvalidFormatRandomString) {
    hs_error_t err = hs_set_grey_overrides("not_valid_format");
    ASSERT_EQ(HS_INVALID, err);
}

TEST(GreyOverrides, WhitespaceInKey) {
    // Keys with spaces should be rejected
    hs_error_t err = hs_set_grey_overrides("allow Lily:1;");
    ASSERT_EQ(HS_INVALID, err);
}

TEST(GreyOverrides, FloatValue) {
    hs_error_t err = hs_set_grey_overrides("allowLily:1.5;");
    ASSERT_EQ(HS_INVALID, err);
}

TEST(GreyOverrides, OutOfRangeValue) {
    // Extremely large value that exceeds unsigned int
    hs_error_t err =
        hs_set_grey_overrides("limitPatternCount:99999999999999999999;");
    ASSERT_EQ(HS_INVALID, err);
}

TEST(GreyOverrides, CompilationUnaffectedByInvalidOverrides) {
    // Setting invalid overrides should not affect compilation
    hs_set_grey_overrides("bad_key:1;");

    // Compilation should still work with defaults (no crash, valid result)
    hs_database_t *db = nullptr;
    hs_compile_error_t *compile_err = nullptr;
    hs_error_t err =
        hs_compile("test", 0, HS_MODE_BLOCK, nullptr, &db, &compile_err);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(db != nullptr);
    hs_free_database(db);
}

TEST(GreyOverrides, VeryLongKeyName) {
    string long_key(500, 'x');
    string overrides = long_key + ":1;";
    hs_error_t err = hs_set_grey_overrides(overrides.c_str());
    ASSERT_EQ(HS_INVALID, err);
}

TEST(GreyOverrides, EmptyKeyWithMultipleColons) {
    hs_error_t err = hs_set_grey_overrides("::1;");
    ASSERT_EQ(HS_INVALID, err);
}

TEST(GreyOverrides, KeyWithSpecialChars) {
    hs_error_t err = hs_set_grey_overrides("allow@Lily:1;");
    ASSERT_EQ(HS_INVALID, err);
}

TEST(GreyOverrides, LimitPatternCountAffectsCompilation) {
    hs_error_t err = hs_set_grey_overrides("limitPatternCount:1;");
    ASSERT_EQ(HS_SUCCESS, err);

    const char *expr[] = {"abc", "def"};
    unsigned flags[] = {0, 0};
    unsigned ids[] = {1, 2};
    hs_database_t *db = nullptr;
    hs_compile_error_t *compile_err = nullptr;

    err = hs_compile_multi(expr, flags, ids, 2, HS_MODE_BLOCK, nullptr, &db,
                           &compile_err);
    ASSERT_NE(HS_SUCCESS, err);
    ASSERT_TRUE(db == nullptr);
    hs_free_compile_error(compile_err);

    ASSERT_EQ(HS_SUCCESS, hs_reset_grey_overrides());

    compile_err = nullptr;
    err = hs_compile_multi(expr, flags, ids, 2, HS_MODE_BLOCK, nullptr, &db,
                           &compile_err);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(db != nullptr);
    hs_free_database(db);
}

TEST(GreyOverrides, InvalidOverridesAreTransactional) {
    hs_error_t err = hs_set_grey_overrides("limitPatternCount:1;bad_key:1;");
    ASSERT_EQ(HS_INVALID, err);

    const char *expr[] = {"abc", "def"};
    unsigned flags[] = {0, 0};
    unsigned ids[] = {1, 2};
    hs_database_t *db = nullptr;
    hs_compile_error_t *compile_err = nullptr;

    err = hs_compile_multi(expr, flags, ids, 2, HS_MODE_BLOCK, nullptr, &db,
                           &compile_err);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(db != nullptr);
    hs_free_database(db);
}

// ======================================================================
// hs_reset_grey_overrides - All Cases
// ======================================================================

TEST(GreyOverrides, ResetWhenNoOverridesSet) {
    // Should succeed even when no overrides were set
    hs_error_t err = hs_reset_grey_overrides();
    ASSERT_EQ(HS_SUCCESS, err);
}

TEST(GreyOverrides, ResetAfterSet) {
    hs_error_t err = hs_set_grey_overrides("allowLily:1;");
    ASSERT_EQ(HS_SUCCESS, err);

    err = hs_reset_grey_overrides();
    ASSERT_EQ(HS_SUCCESS, err);
}

TEST(GreyOverrides, ResetMultipleTimes) {
    // Multiple resets should all succeed
    ASSERT_EQ(HS_SUCCESS, hs_reset_grey_overrides());
    ASSERT_EQ(HS_SUCCESS, hs_reset_grey_overrides());
    ASSERT_EQ(HS_SUCCESS, hs_reset_grey_overrides());
}

TEST(GreyOverrides, SetResetSetCycle) {
    // Full cycle: set -> reset -> set -> compile -> reset
    hs_error_t err = hs_set_grey_overrides("allowHao:1;");
    ASSERT_EQ(HS_SUCCESS, err);

    err = hs_reset_grey_overrides();
    ASSERT_EQ(HS_SUCCESS, err);

    err = hs_set_grey_overrides("allowLily:1;");
    ASSERT_EQ(HS_SUCCESS, err);

    hs_database_t *db = nullptr;
    hs_compile_error_t *compile_err = nullptr;
    err = hs_compile("pattern", 0, HS_MODE_BLOCK, nullptr, &db, &compile_err);
    ASSERT_EQ(HS_SUCCESS, err);
    hs_free_database(db);

    err = hs_reset_grey_overrides();
    ASSERT_EQ(HS_SUCCESS, err);
}

// ======================================================================
// Integration: Overrides actually change compilation behavior
// ======================================================================

TEST(GreyOverrides, CompileWithOverridesThenWithout) {
    // First: compile with Lily enabled
    hs_set_grey_overrides("allowLily:1;");

    hs_database_t *db1 = nullptr;
    hs_compile_error_t *err1 = nullptr;
    hs_error_t ret =
        hs_compile("abcdef", 0, HS_MODE_BLOCK, nullptr, &db1, &err1);
    ASSERT_EQ(HS_SUCCESS, ret);
    ASSERT_TRUE(db1 != nullptr);

    // Then: reset and compile without Lily
    hs_reset_grey_overrides();

    hs_database_t *db2 = nullptr;
    hs_compile_error_t *err2 = nullptr;
    ret = hs_compile("abcdef", 0, HS_MODE_BLOCK, nullptr, &db2, &err2);
    ASSERT_EQ(HS_SUCCESS, ret);
    ASSERT_TRUE(db2 != nullptr);

    // Both should scan correctly
    const char *data = "test abcdef data";
    hs_scratch_t *scratch = nullptr;
    hs_alloc_scratch(db1, &scratch);

    CallBackContext c1;
    hs_scan(db1, data, (unsigned)strlen(data), 0, scratch, record_cb, &c1);
    ASSERT_EQ(1U, c1.matches.size());

    hs_scratch_t *scratch2 = nullptr;
    hs_alloc_scratch(db2, &scratch2);

    CallBackContext c2;
    hs_scan(db2, data, (unsigned)strlen(data), 0, scratch2, record_cb, &c2);
    ASSERT_EQ(1U, c2.matches.size());

    hs_free_scratch(scratch);
    hs_free_scratch(scratch2);
    hs_free_database(db1);
    hs_free_database(db2);
}

TEST(GreyOverrides, NeoFdrZeroTailLiteralFallsBackWithoutLosingMatch) {
    hs_error_t err = hs_set_grey_overrides("allowLily:1;allowNeoFdr:1;");
    ASSERT_EQ(HS_SUCCESS, err);

    const char *expressions[] = {"\\x13\\x08\\x00\\x00\\x00\\x00\\x00\\x00"
                                 "\\x00\\x00\\x00\\x00"};
    unsigned flags[] = {HS_FLAG_SINGLEMATCH};
    unsigned ids[] = {2824};

    hs_database_t *db = nullptr;
    hs_compile_error_t *compile_err = nullptr;
    err = hs_compile_multi(expressions, flags, ids, 1, HS_MODE_BLOCK, nullptr,
                           &db, &compile_err);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(db != nullptr);

    hs_scratch_t *scratch = nullptr;
    err = hs_alloc_scratch(db, &scratch);
    ASSERT_EQ(HS_SUCCESS, err);

    const char data[] = {'\x13', '\x08', '\x00', '\x00', '\x00', '\x00',
                         '\x00', '\x00', '\x00', '\x00', '\x00', '\x00'};
    CallBackContext c;
    err = hs_scan(db, data, sizeof(data), 0, scratch, record_cb, &c);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_EQ(1U, c.matches.size());
    EXPECT_EQ(2824, c.matches[0].id);
    EXPECT_EQ(sizeof(data), c.matches[0].to);

    hs_free_scratch(scratch);
    hs_free_database(db);
    hs_reset_grey_overrides();
}

TEST(GreyOverrides, NeoFdrVioletKeepsZeroDenseClassAlternative) {
    hs_error_t err = hs_set_grey_overrides("allowLily:1;allowNeoFdr:1;");
    ASSERT_EQ(HS_SUCCESS, err);

    const char *expressions[] = {
        "(\\x40\\x09.{19}|\\x41\\x0b.{23})[\\xf0-\\xff].{8}"
        "\\x01\\x00[\\x00\\x01\\x02\\x04\\x08\\x10\\x18\\x20]\\x00"};
    unsigned flags[] = {HS_FLAG_DOTALL | HS_FLAG_CASELESS | HS_FLAG_MULTILINE};
    unsigned ids[] = {23};

    hs_database_t *db = nullptr;
    hs_compile_error_t *compile_err = nullptr;
    err = hs_compile_multi(expressions, flags, ids, 1, HS_MODE_BLOCK, nullptr,
                           &db, &compile_err);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(db != nullptr);

    hs_scratch_t *scratch = nullptr;
    err = hs_alloc_scratch(db, &scratch);
    ASSERT_EQ(HS_SUCCESS, err);

    string data("\x40\x09", 2);
    data.append(19, 'a');
    data.push_back(static_cast<char>(0xf0));
    data.append(8, 'b');
    data.append("\x01\x00\x00\x00", 4);

    CallBackContext c;
    err = hs_scan(db, data.data(), static_cast<unsigned>(data.size()), 0,
                  scratch, record_cb, &c);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_EQ(1U, c.matches.size());
    EXPECT_EQ(23, c.matches[0].id);
    EXPECT_EQ(data.size(), c.matches[0].to);

    hs_free_scratch(scratch);
    hs_free_database(db);
    hs_reset_grey_overrides();
}

TEST(GreyOverrides, NeoFdrVioletKeepsZeroDenseBranchAlternative) {
    hs_error_t err = hs_set_grey_overrides("allowLily:1;allowNeoFdr:1;");
    ASSERT_EQ(HS_SUCCESS, err);

    const char *expressions[] = {"^.{4}(\\x05\\x00|\\x2e/x00|\\x2e\\x08).{21}"
                                 "(\\xff{7}\\x7f|\\xb8\\x0b\\x00{6}).{29}"
                                 "delete\\x20+from\\x20"};
    unsigned flags[] = {0};
    unsigned ids[] = {200001168};

    hs_database_t *db = nullptr;
    hs_compile_error_t *compile_err = nullptr;
    err = hs_compile_multi(expressions, flags, ids, 1, HS_MODE_BLOCK, nullptr,
                           &db, &compile_err);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(db != nullptr);

    hs_scratch_t *scratch = nullptr;
    err = hs_alloc_scratch(db, &scratch);
    ASSERT_EQ(HS_SUCCESS, err);

    string data("ABCD");
    data.append("\x05\x00", 2);
    data.append(21, 'c');
    data.push_back(static_cast<char>(0xb8));
    data.push_back('\x0b');
    data.append(6, '\x00');
    data.append(29, 'd');
    data.append("delete from ");

    CallBackContext c;
    err = hs_scan(db, data.data(), static_cast<unsigned>(data.size()), 0,
                  scratch, record_cb, &c);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_EQ(1U, c.matches.size());
    EXPECT_EQ(200001168U, c.matches[0].id);
    EXPECT_EQ(data.size(), c.matches[0].to);

    hs_free_scratch(scratch);
    hs_free_database(db);
    hs_reset_grey_overrides();
}

TEST(GreyOverrides, OverridesAppliedToMultiPatternCompilation) {
    hs_set_grey_overrides("allowHao:1;allowNeoFdr:1;");

    const int num_patterns = 10;
    const char *expr[] = {"apple",  "banana", "cherry", "dragon", "elephant",
                          "falcon", "guitar", "hammer", "island", "jungle"};
    unsigned flags[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    unsigned ids[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

    hs_database_t *db = nullptr;
    hs_compile_error_t *compile_err = nullptr;
    hs_error_t err =
        hs_compile_multi(expr, flags, ids, num_patterns, HS_MODE_BLOCK, nullptr,
                         &db, &compile_err);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(db != nullptr);

    hs_scratch_t *scratch = nullptr;
    err = hs_alloc_scratch(db, &scratch);
    ASSERT_EQ(HS_SUCCESS, err);

    // Scan data containing all patterns (each pattern is unique, no substrings)
    const char *data = "apple banana cherry dragon elephant "
                       "falcon guitar hammer island jungle";
    CallBackContext c;
    err = hs_scan(db, data, (unsigned)strlen(data), 0, scratch, record_cb, &c);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_EQ(10U, c.matches.size());

    hs_free_scratch(scratch);
    hs_free_database(db);
    hs_reset_grey_overrides();
}

TEST(GreyOverrides, StreamModeCompilationWithOverrides) {
    hs_set_grey_overrides("allowLily:1;allowHao:1;");

    hs_database_t *db = nullptr;
    hs_compile_error_t *compile_err = nullptr;
    hs_error_t err =
        hs_compile("stream_test", 0, HS_MODE_STREAM | HS_MODE_SOM_HORIZON_LARGE,
                   nullptr, &db, &compile_err);
    ASSERT_EQ(HS_SUCCESS, err);
    ASSERT_TRUE(db != nullptr);

    hs_scratch_t *scratch = nullptr;
    err = hs_alloc_scratch(db, &scratch);
    ASSERT_EQ(HS_SUCCESS, err);

    hs_stream_t *stream = nullptr;
    err = hs_open_stream(db, 0, &stream);
    ASSERT_EQ(HS_SUCCESS, err);

    CallBackContext c;
    err = hs_scan_stream(stream, "prefix stream_test suffix",
                         (unsigned)strlen("prefix stream_test suffix"), 0,
                         scratch, record_cb, &c);
    ASSERT_EQ(HS_SUCCESS, err);

    err = hs_close_stream(stream, scratch, record_cb, &c);
    ASSERT_EQ(HS_SUCCESS, err);

    ASSERT_GE(c.matches.size(), 1U);

    hs_free_scratch(scratch);
    hs_free_database(db);
    hs_reset_grey_overrides();
}

} // namespace
