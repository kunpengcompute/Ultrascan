#include "../fuzz_test.h"
#include "hs.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <streambuf>
#include <tuple>
#include <vector>

namespace {

class NullBuffer : public std::streambuf {
public:
    int overflow(int c) override {
        return c == traits_type::eof() ? traits_type::not_eof(c) : c;
    }
};

bool fuzzVerbose() {
    const char *value = std::getenv("HS_FUZZ_VERBOSE");
    return value && value[0] != '\0' && value[0] != '0';
}

std::ostream &detailOut() {
    thread_local NullBuffer nullBuffer;
    thread_local std::ostream nullStream(&nullBuffer);
    return fuzzVerbose() ? std::cout : nullStream;
}

std::ostream &detailErr() {
    thread_local NullBuffer nullBuffer;
    thread_local std::ostream nullStream(&nullBuffer);
    return fuzzVerbose() ? std::cerr : nullStream;
}

std::string errString(hs_error_t err) {
    std::ostringstream out;
    out << err;
    return out.str();
}

std::string compileErrorMessage(hs_compile_error_t *&err) {
    if (!err) {
        return "no compile error detail";
    }

    std::string message =
        err->message ? err->message : "no compile error detail";
    hs_free_compile_error(err);
    err = nullptr;
    return message;
}

size_t maxUniqueErrors() {
    const char *value = std::getenv("HS_FUZZ_MAX_UNIQUE_ERRORS");
    if (!value || value[0] == '\0') {
        return 30;
    }

    return static_cast<size_t>(std::strtoul(value, nullptr, 10));
}

struct ApiStats {
    unsigned long long ok = 0;
    unsigned long long expectedFail = 0;
    unsigned long long errors = 0;
    unsigned long long skipped = 0;
    unsigned long long matches = 0;
};

class FuzzStats {
public:
    void ok(const std::string &api) { stats[api].ok++; }

    void expectedFail(const std::string &api, const std::string &message) {
        stats[api].expectedFail++;
        recordMessage(api, message);
    }

    void error(const std::string &api, hs_error_t err) {
        error(api, "error " + errString(err));
    }

    void error(const std::string &api, const std::string &message) {
        stats[api].errors++;
        recordMessage(api, message);
    }

    void skipped(const std::string &api) { stats[api].skipped++; }

    void matches(const std::string &api, unsigned long long count) {
        stats[api].matches += count;
    }

    void print(std::ostream &os) const {
        os << "api summary:" << std::endl;
        if (stats.empty()) {
            os << "  no api calls recorded" << std::endl;
        }

        for (const auto &item : stats) {
            const ApiStats &value = item.second;
            os << "  " << item.first << ": ok=" << value.ok
               << ", expected_fail=" << value.expectedFail
               << ", errors=" << value.errors;
            if (value.skipped) {
                os << ", skipped=" << value.skipped;
            }
            if (value.matches) {
                os << ", matches=" << value.matches;
            }
            os << std::endl;
        }

        if (!messages.empty()) {
            os << "unique errors:" << std::endl;
            size_t shown = 0;
            const size_t limit = maxUniqueErrors();
            for (const auto &item : messages) {
                if (shown >= limit) {
                    os << "  ... " << (messages.size() - shown)
                       << " more unique errors hidden" << std::endl;
                    break;
                }
                os << "  [" << item.second << "] " << item.first << std::endl;
                shown++;
            }
        }
    }

private:
    void recordMessage(const std::string &api, const std::string &message) {
        std::string text = api + ": " + message;
        messages[text]++;
    }

    std::map<std::string, ApiStats> stats;
    std::map<std::string, unsigned long long> messages;
};

struct MatchRecord {
    unsigned int inputIndex = 0;
    unsigned int callbackPhase = 0;
    unsigned int id = 0;
    unsigned long long from = 0;
    unsigned long long to = 0;
    unsigned int flags = 0;

    bool operator<(const MatchRecord &other) const {
        return std::tie(inputIndex, callbackPhase, id, from, to, flags) <
               std::tie(other.inputIndex, other.callbackPhase, other.id,
                        other.from, other.to, other.flags);
    }

    bool operator==(const MatchRecord &other) const {
        return inputIndex == other.inputIndex &&
               callbackPhase == other.callbackPhase && id == other.id &&
               from == other.from && to == other.to && flags == other.flags;
    }
};

struct MatchCapture {
    unsigned int currentInput = 0;
    unsigned int currentPhase = 0;
    std::vector<unsigned long long> vectorEnds;
    std::vector<MatchRecord> matches;
};

struct LocalDatabase {
    hs_database_t *db = nullptr;
    hs_scratch_t *scratch = nullptr;

    LocalDatabase() = default;
    LocalDatabase(const LocalDatabase &) = delete;
    LocalDatabase &operator=(const LocalDatabase &) = delete;

    ~LocalDatabase() {
        if (scratch) {
            hs_free_scratch(scratch);
        }
        if (db) {
            hs_free_database(db);
        }
    }
};

struct ScopedCollector {
    hs_fp_collector_t *value = nullptr;

    ScopedCollector() = default;
    ScopedCollector(const ScopedCollector &) = delete;
    ScopedCollector &operator=(const ScopedCollector &) = delete;

    ~ScopedCollector() {
        if (value) {
            hs_fp_collector_free(value);
        }
    }
};

struct ScopedFeedback {
    hs_fp_feedback_t *value = nullptr;

    ScopedFeedback() = default;
    ScopedFeedback(const ScopedFeedback &) = delete;
    ScopedFeedback &operator=(const ScopedFeedback &) = delete;

    ~ScopedFeedback() {
        if (value) {
            hs_fp_feedback_free(value);
        }
    }
};

int HS_CDECL captureMatch(unsigned int id, unsigned long long from,
                          unsigned long long to, unsigned int flags,
                          void *context) {
    auto *capture = static_cast<MatchCapture *>(context);
    if (!capture) {
        return 0;
    }

    unsigned int inputIndex = capture->currentInput;
    if (!capture->vectorEnds.empty()) {
        auto it = std::lower_bound(capture->vectorEnds.begin(),
                                   capture->vectorEnds.end(), to);
        if (it == capture->vectorEnds.end()) {
            inputIndex =
                static_cast<unsigned int>(capture->vectorEnds.size() - 1);
        } else {
            inputIndex = static_cast<unsigned int>(
                std::distance(capture->vectorEnds.begin(), it));
        }
    }

    capture->matches.push_back(
        {inputIndex, capture->currentPhase, id, from, to, flags});
    return 0;
}

std::string describeMatch(const MatchRecord &match) {
    std::ostringstream out;
    out << "input=" << match.inputIndex << ", phase=" << match.callbackPhase
        << ", id=" << match.id << ", from=" << match.from << ", to=" << match.to
        << ", flags=" << match.flags;
    return out.str();
}

struct FeedbackDumpStats {
    unsigned int summaryCalls = 0;
    unsigned int fragmentCalls = 0;
    unsigned int selectedCalls = 0;
    unsigned long long triggerCount = 0;
    unsigned long long trueTriggerCount = 0;
    unsigned long long falsePositiveCount = 0;
    hs_fp_feedback_dump_summary_t summary = {};
    bool invalid = false;
    std::string invalidReason;

    void fail(const std::string &reason) {
        if (!invalid) {
            invalidReason = reason;
        }
        invalid = true;
    }
};

void HS_CDECL feedbackDumpSummary(const hs_fp_feedback_dump_summary_t *summary,
                                  void *context) {
    auto *state = static_cast<FeedbackDumpStats *>(context);
    if (!state) {
        return;
    }
    if (!summary) {
        state->fail("null summary callback payload");
        return;
    }

    state->summaryCalls++;
    state->summary = *summary;
    if (summary->bad_fragment_count > summary->fragment_count ||
        summary->true_trigger_count > summary->trigger_count ||
        summary->false_positive_count > summary->trigger_count) {
        state->fail("invalid summary counters");
    }
}

void HS_CDECL feedbackDumpFragment(const hs_fp_fragment_info_t *fragment,
                                   unsigned int selected, void *context) {
    auto *state = static_cast<FeedbackDumpStats *>(context);
    if (!state) {
        return;
    }
    if (!fragment) {
        state->fail("null fragment callback payload");
        return;
    }

    state->fragmentCalls++;
    if (selected) {
        state->selectedCalls++;
    }

    if (fragment->table < HS_FP_TABLE_FLOATING ||
        fragment->table > HS_FP_TABLE_SMALL_BLOCK) {
        state->fail("fragment uses an unknown or reserved table");
    } else if ((fragment->length && !fragment->bytes) ||
               (fragment->mask_length && (!fragment->mask || !fragment->cmp)) ||
               fragment->true_trigger_count > fragment->trigger_count ||
               fragment->false_positive_count > fragment->trigger_count) {
        state->fail("invalid fragment callback payload");
    }

    state->triggerCount += fragment->trigger_count;
    state->trueTriggerCount += fragment->true_trigger_count;
    state->falsePositiveCount += fragment->false_positive_count;
}

} // namespace

class HyperscanRunner : public Runner {
public:
    HyperscanRunner()
        : db(nullptr), fatDb(nullptr), compileErr(nullptr), scratch(nullptr) {}

    ~HyperscanRunner() { reset(); }

    bool compile(const FuzzTestCase &testCase,
                 unsigned int mode = HS_MODE_BLOCK) override {
        // 重置之前的状态
        reset();

        // 编译正则表达式
        hs_error_t err =
            hs_compile(testCase.pattern.c_str(), testCase.flags, mode,
                       nullptr, // 使用默认平台
                       &db, &compileErr);

        if (err != HS_SUCCESS) {
            // 编译失败，但这是正常的fuzz测试行为
            // 记录失败信息但返回true，表示测试执行成功
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("hs_compile", message);
                detailErr() << "Compilation failed for pattern " << testCase.id
                            << ": " << message << std::endl;
            } else {
                stats.expectedFail("hs_compile", errString(err));
            }
            return true;
        }

        // 编译成功
        stats.ok("hs_compile");
        detailOut() << "Compilation succeeded for pattern " << testCase.id
                    << std::endl;

        // 分配scratch空间
        err = hs_alloc_scratch(db, &scratch);
        if (err != HS_SUCCESS) {
            stats.error("hs_alloc_scratch", err);
            detailErr() << "Failed to allocate scratch space" << std::endl;
            return true;
        }

        return true;
    }

    bool fatCompile(const FuzzTestCase &testCase,
                    unsigned int mode = HS_MODE_BLOCK) override {
        // 重置之前的状态
        reset();

        // 编译通用字节码正则表达式
        hs_error_t err =
            fat_hs_compile(testCase.pattern.c_str(), testCase.flags, mode,
                           nullptr, // 使用默认平台
                           &fatDb, &compileErr);

        if (err != HS_SUCCESS) {
            // 编译失败是正常的fuzz测试行为
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("fat_hs_compile", message);
                detailErr() << "Fat compilation failed for pattern "
                            << testCase.id << ": " << message << std::endl;
            } else {
                stats.expectedFail("fat_hs_compile", errString(err));
            }
            return true;
        }

        stats.ok("fat_hs_compile");
        detailOut() << "Fat compilation succeeded for pattern " << testCase.id
                    << std::endl;
        if (shouldExerciseFatDatabaseApis(mode)) {
            exerciseFatDatabaseApis("fat_hs_compile");
        }
        return true;
    }

    bool scan(const std::string &data) override {
        if (!db || !scratch) {
            stats.skipped("hs_scan");
            return false;
        }

        // 定义匹配回调函数
        int matchCount = 0;
        auto matchCallback = [](unsigned int id, unsigned long long from,
                                unsigned long long to, unsigned int flags,
                                void *context) {
            int *count = static_cast<int *>(context);
            (*count)++;
            return 0; // 继续扫描
        };

        // 执行扫描
        hs_error_t err = hs_scan(db, data.c_str(), data.length(), 0, scratch,
                                 matchCallback, &matchCount);

        if (err != HS_SUCCESS && err != HS_SCAN_TERMINATED) {
            stats.error("hs_scan", err);
            detailErr() << "Scan failed with error: " << err << std::endl;
        } else {
            stats.ok("hs_scan");
        }

        stats.matches("hs_scan", matchCount);
        detailOut() << "Scan completed, found " << matchCount << " matches"
                    << std::endl;
        return true;
    }

    bool streamScan(const std::string &data) override {
        if (!db || !scratch) {
            stats.skipped("hs_scan_stream");
            return false;
        }

        // 打开流
        hs_stream_t *stream = nullptr;
        hs_error_t err = hs_open_stream(db, 0, &stream);
        if (err != HS_SUCCESS) {
            stats.error("hs_open_stream", err);
            detailErr() << "Failed to open stream: " << err << std::endl;
            return true;
        }

        // 定义匹配回调函数
        int matchCount = 0;
        auto matchCallback = [](unsigned int id, unsigned long long from,
                                unsigned long long to, unsigned int flags,
                                void *context) {
            int *count = static_cast<int *>(context);
            (*count)++;
            return 0; // 继续扫描
        };

        // 分块扫描
        const size_t chunkSize = 1024;
        size_t offset = 0;
        while (offset < data.length()) {
            size_t chunk = std::min(chunkSize, data.length() - offset);
            err = hs_scan_stream(stream, data.c_str() + offset, chunk, 0,
                                 scratch, matchCallback, &matchCount);
            if (err != HS_SUCCESS && err != HS_SCAN_TERMINATED) {
                stats.error("hs_scan_stream", err);
                detailErr()
                    << "Stream scan failed with error: " << err << std::endl;
                break;
            }
            offset += chunk;
        }

        // 关闭流
        err = hs_close_stream(stream, scratch, matchCallback, &matchCount);
        if (err != HS_SUCCESS) {
            stats.error("hs_close_stream", err);
            detailErr() << "Failed to close stream: " << err << std::endl;
        } else {
            stats.ok("hs_scan_stream");
        }

        stats.matches("hs_scan_stream", matchCount);
        detailOut() << "Stream scan completed, found " << matchCount
                    << " matches" << std::endl;
        return true;
    }

    bool compileMulti(const std::vector<FuzzTestCase> &testCases) override {
        if (testCases.empty()) {
            return true;
        }

        // 重置之前的状态
        reset();

        // 准备参数
        std::vector<const char *> patterns;
        std::vector<unsigned int> flags;
        std::vector<unsigned int> ids;

        for (const auto &testCase : testCases) {
            patterns.push_back(testCase.pattern.c_str());
            flags.push_back(testCase.flags);
            ids.push_back(testCase.id);
        }

        // 编译多模式
        hs_error_t err = hs_compile_multi(
            patterns.data(), flags.data(), ids.data(), testCases.size(),
            HS_MODE_BLOCK, nullptr, &db, &compileErr);

        if (err != HS_SUCCESS) {
            // 编译失败，但这是正常的fuzz测试行为
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("hs_compile_multi", message);
                detailErr()
                    << "Multi compilation failed: " << message << std::endl;
            } else {
                stats.expectedFail("hs_compile_multi", errString(err));
            }
            return true;
        }

        // 编译成功
        stats.ok("hs_compile_multi");
        detailOut() << "Multi compilation succeeded for " << testCases.size()
                    << " patterns" << std::endl;

        // 分配scratch空间
        err = hs_alloc_scratch(db, &scratch);
        if (err != HS_SUCCESS) {
            stats.error("hs_alloc_scratch", err);
            detailErr() << "Failed to allocate scratch space" << std::endl;
            return true;
        }

        return true;
    }

    bool compileExtMulti(const std::vector<FuzzTestCase> &testCases) override {
        if (testCases.empty()) {
            return true;
        }

        // 重置之前的状态
        reset();

        // 准备参数
        std::vector<const char *> patterns;
        std::vector<unsigned int> flags;
        std::vector<unsigned int> ids;
        std::vector<hs_expr_ext_t> extParams(testCases.size());
        std::vector<const hs_expr_ext_t *> extParamsPtrs;
        extParamsPtrs.reserve(testCases.size());

        for (size_t i = 0; i < testCases.size(); i++) {
            const auto &testCase = testCases[i];
            patterns.push_back(testCase.pattern.c_str());
            flags.push_back(testCase.flags);
            ids.push_back(testCase.id);

            // 创建扩展参数
            hs_expr_ext_t &ext = extParams[i];
            ext.flags = 0;
            ext.min_offset = 0;
            ext.max_offset = 0;
            ext.min_length = 0;
            ext.edit_distance = 0;
            ext.hamming_distance = 0;
            extParamsPtrs.push_back(&ext);
        }

        // 编译带扩展参数的多模式
        hs_error_t err = hs_compile_ext_multi(
            patterns.data(), flags.data(), ids.data(), extParamsPtrs.data(),
            testCases.size(), HS_MODE_BLOCK, nullptr, &db, &compileErr);

        if (err != HS_SUCCESS) {
            // 编译失败，但这是正常的fuzz测试行为
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("hs_compile_ext_multi", message);
                detailErr()
                    << "Ext multi compilation failed: " << message << std::endl;
            } else {
                stats.expectedFail("hs_compile_ext_multi", errString(err));
            }
            return true;
        }

        // 编译成功
        stats.ok("hs_compile_ext_multi");
        detailOut() << "Ext multi compilation succeeded for "
                    << testCases.size() << " patterns" << std::endl;

        // 分配scratch空间
        err = hs_alloc_scratch(db, &scratch);
        if (err != HS_SUCCESS) {
            stats.error("hs_alloc_scratch", err);
            detailErr() << "Failed to allocate scratch space" << std::endl;
            return true;
        }

        return true;
    }

    bool fatCompileMulti(const std::vector<FuzzTestCase> &testCases,
                         unsigned int mode = HS_MODE_BLOCK) override {
        if (testCases.empty()) {
            return true;
        }

        // 重置之前的状态
        reset();

        // 准备参数
        std::vector<const char *> patterns;
        std::vector<unsigned int> flags;
        std::vector<unsigned int> ids;

        for (const auto &testCase : testCases) {
            patterns.push_back(testCase.pattern.c_str());
            flags.push_back(testCase.flags);
            ids.push_back(testCase.id);
        }

        // 编译通用字节码多模式
        hs_error_t err = fat_hs_compile_multi(
            patterns.data(), flags.data(), ids.data(), testCases.size(), mode,
            nullptr, &fatDb, &compileErr);

        if (err != HS_SUCCESS) {
            // 编译失败是正常的fuzz测试行为
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("fat_hs_compile_multi", message);
                detailErr()
                    << "Fat multi compilation failed: " << message << std::endl;
            } else {
                stats.expectedFail("fat_hs_compile_multi", errString(err));
            }
            return true;
        }

        stats.ok("fat_hs_compile_multi");
        detailOut() << "Fat multi compilation succeeded for "
                    << testCases.size() << " patterns" << std::endl;
        if (shouldExerciseFatDatabaseApis(mode)) {
            exerciseFatDatabaseApis("fat_hs_compile_multi");
        }
        return true;
    }

    bool fatCompileExtMulti(const std::vector<FuzzTestCase> &testCases,
                            unsigned int mode = HS_MODE_BLOCK) override {
        if (testCases.empty()) {
            return true;
        }

        // 重置之前的状态
        reset();

        // 准备参数
        std::vector<const char *> patterns;
        std::vector<unsigned int> flags;
        std::vector<unsigned int> ids;
        std::vector<hs_expr_ext_t> extParams(testCases.size());
        std::vector<const hs_expr_ext_t *> extParamsPtrs;
        extParamsPtrs.reserve(testCases.size());

        for (size_t i = 0; i < testCases.size(); i++) {
            const auto &testCase = testCases[i];
            patterns.push_back(testCase.pattern.c_str());
            flags.push_back(testCase.flags);
            ids.push_back(testCase.id);

            // 创建扩展参数
            hs_expr_ext_t &ext = extParams[i];
            ext.flags = 0;
            ext.min_offset = 0;
            ext.max_offset = 0;
            ext.min_length = 0;
            ext.edit_distance = 0;
            ext.hamming_distance = 0;
            extParamsPtrs.push_back(&ext);
        }

        // 编译带扩展参数的通用字节码多模式
        hs_error_t err = fat_hs_compile_ext_multi(
            patterns.data(), flags.data(), ids.data(), extParamsPtrs.data(),
            testCases.size(), mode, nullptr, &fatDb, &compileErr);

        if (err != HS_SUCCESS) {
            // 编译失败是正常的fuzz测试行为
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("fat_hs_compile_ext_multi", message);
                detailErr() << "Fat ext multi compilation failed: " << message
                            << std::endl;
            } else {
                stats.expectedFail("fat_hs_compile_ext_multi", errString(err));
            }
            return true;
        }

        stats.ok("fat_hs_compile_ext_multi");
        detailOut() << "Fat ext multi compilation succeeded for "
                    << testCases.size() << " patterns" << std::endl;
        if (shouldExerciseFatDatabaseApis(mode)) {
            exerciseFatDatabaseApis("fat_hs_compile_ext_multi");
        }
        return true;
    }

    bool compileLit(const FuzzTestCase &testCase, size_t length) override {
        // 重置之前的状态
        reset();

        // 编译纯字面表达式
        hs_error_t err =
            hs_compile_lit(testCase.pattern.c_str(), testCase.flags, length,
                           HS_MODE_BLOCK, nullptr, &db, &compileErr);

        if (err != HS_SUCCESS) {
            // 编译失败，但这是正常的fuzz测试行为
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("hs_compile_lit", message);
                detailErr() << "Literal compilation failed for pattern "
                            << testCase.id << ": " << message << std::endl;
            } else {
                stats.expectedFail("hs_compile_lit", errString(err));
            }
            return true;
        }

        // 编译成功
        stats.ok("hs_compile_lit");
        detailOut() << "Literal compilation succeeded for pattern "
                    << testCase.id << std::endl;

        // 分配scratch空间
        err = hs_alloc_scratch(db, &scratch);
        if (err != HS_SUCCESS) {
            stats.error("hs_alloc_scratch", err);
            detailErr() << "Failed to allocate scratch space" << std::endl;
            return true;
        }

        return true;
    }

    bool fatCompileLit(const FuzzTestCase &testCase, size_t length,
                       unsigned int mode = HS_MODE_BLOCK) override {
        // 重置之前的状态
        reset();

        // 编译通用字节码纯字面表达式
        hs_error_t err =
            fat_hs_compile_lit(testCase.pattern.c_str(), testCase.flags, length,
                               mode, nullptr, &fatDb, &compileErr);

        if (err != HS_SUCCESS) {
            // 编译失败是正常的fuzz测试行为
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("fat_hs_compile_lit", message);
                detailErr() << "Fat literal compilation failed for pattern "
                            << testCase.id << ": " << message << std::endl;
            } else {
                stats.expectedFail("fat_hs_compile_lit", errString(err));
            }
            return true;
        }

        stats.ok("fat_hs_compile_lit");
        detailOut() << "Fat literal compilation succeeded for pattern "
                    << testCase.id << std::endl;
        if (shouldExerciseFatDatabaseApis(mode)) {
            exerciseFatDatabaseApis("fat_hs_compile_lit");
        }
        return true;
    }

    bool compileLitMulti(const std::vector<FuzzTestCase> &testCases,
                         const std::vector<size_t> &lengths) override {
        if (testCases.empty() || testCases.size() != lengths.size()) {
            return true;
        }

        // 重置之前的状态
        reset();

        // 准备参数
        std::vector<const char *> patterns;
        std::vector<unsigned int> flags;
        std::vector<unsigned int> ids;

        for (const auto &testCase : testCases) {
            patterns.push_back(testCase.pattern.c_str());
            flags.push_back(testCase.flags);
            ids.push_back(testCase.id);
        }

        // 编译多纯字面表达式
        hs_error_t err = hs_compile_lit_multi(
            patterns.data(), flags.data(), ids.data(), lengths.data(),
            testCases.size(), HS_MODE_BLOCK, nullptr, &db, &compileErr);

        if (err != HS_SUCCESS) {
            // 编译失败，但这是正常的fuzz测试行为
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("hs_compile_lit_multi", message);
                detailErr() << "Multi literal compilation failed: " << message
                            << std::endl;
            } else {
                stats.expectedFail("hs_compile_lit_multi", errString(err));
            }
            return true;
        }

        // 编译成功
        stats.ok("hs_compile_lit_multi");
        detailOut() << "Multi literal compilation succeeded for "
                    << testCases.size() << " patterns" << std::endl;

        // 分配scratch空间
        err = hs_alloc_scratch(db, &scratch);
        if (err != HS_SUCCESS) {
            stats.error("hs_alloc_scratch", err);
            detailErr() << "Failed to allocate scratch space" << std::endl;
            return true;
        }

        return true;
    }

    bool fatCompileLitMulti(const std::vector<FuzzTestCase> &testCases,
                            const std::vector<size_t> &lengths,
                            unsigned int mode = HS_MODE_BLOCK) override {
        if (testCases.empty() || testCases.size() != lengths.size()) {
            return true;
        }

        // 重置之前的状态
        reset();

        // 准备参数
        std::vector<const char *> patterns;
        std::vector<unsigned int> flags;
        std::vector<unsigned int> ids;

        for (const auto &testCase : testCases) {
            patterns.push_back(testCase.pattern.c_str());
            flags.push_back(testCase.flags);
            ids.push_back(testCase.id);
        }

        // 编译通用字节码多纯字面表达式
        hs_error_t err = fat_hs_compile_lit_multi(
            patterns.data(), flags.data(), ids.data(), lengths.data(),
            testCases.size(), mode, nullptr, &fatDb, &compileErr);

        if (err != HS_SUCCESS) {
            // 编译失败是正常的fuzz测试行为
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("fat_hs_compile_lit_multi", message);
                detailErr()
                    << "Fat multi literal compilation failed: " << message
                    << std::endl;
            } else {
                stats.expectedFail("fat_hs_compile_lit_multi", errString(err));
            }
            return true;
        }

        stats.ok("fat_hs_compile_lit_multi");
        detailOut() << "Fat multi literal compilation succeeded for "
                    << testCases.size() << " patterns" << std::endl;
        if (shouldExerciseFatDatabaseApis(mode)) {
            exerciseFatDatabaseApis("fat_hs_compile_lit_multi");
        }
        return true;
    }

    bool fatCompileInvalidArgs() override {
        detailOut() << "Testing fat_hs_compile invalid argument paths..."
                    << std::endl;

        const char *patterns[] = {"abc"};
        unsigned int flags[] = {0};
        unsigned int ids[] = {0};
        size_t lengths[] = {3};
        const hs_expr_ext_t *ext[] = {nullptr};
        const unsigned int tooManyElements = 8000001U;

        auto finishCase = [this](const char *name, hs_error_t err,
                                 fat_hs_database_t *localDb,
                                 hs_compile_error_t *localErr) {
            detailOut() << name << " returned " << err << std::endl;
            if (err != HS_SUCCESS) {
                std::string message =
                    localErr ? compileErrorMessage(localErr) : errString(err);
                stats.expectedFail("fat_invalid_args",
                                   std::string(name) + ": " + message);
                detailErr()
                    << name << " compile error: " << message << std::endl;
            } else {
                stats.error("fat_invalid_args",
                            std::string(name) + ": unexpected success");
            }
            if (localErr) {
                hs_free_compile_error(localErr);
            }
            if (localDb) {
                stats.error("fat_invalid_args",
                            std::string(name) +
                                ": unexpectedly produced a database");
                detailErr()
                    << name << " unexpectedly produced a database" << std::endl;
                fat_hs_free_database(localDb);
            }
        };

        fat_hs_database_t *localDb = nullptr;
        hs_compile_error_t *localErr = nullptr;

        // validate_fat_compile_args: comp_error is NULL.
        localDb = nullptr;
        finishCase("fat_hs_compile_multi null error",
                   fat_hs_compile_multi(patterns, flags, ids, 1, HS_MODE_BLOCK,
                                        nullptr, &localDb, nullptr),
                   localDb, nullptr);

        // validate_fat_compile_args: db is NULL.
        localErr = nullptr;
        finishCase("fat_hs_compile_multi null db",
                   fat_hs_compile_multi(patterns, flags, ids, 1, HS_MODE_BLOCK,
                                        nullptr, nullptr, &localErr),
                   nullptr, localErr);

        // validate_fat_compile_args: expressions is NULL.
        localDb = nullptr;
        localErr = nullptr;
        finishCase("fat_hs_compile_multi null expressions",
                   fat_hs_compile_multi(nullptr, flags, ids, 1, HS_MODE_BLOCK,
                                        nullptr, &localDb, &localErr),
                   localDb, localErr);

        // validate_fat_compile_args: elements is zero.
        localDb = nullptr;
        localErr = nullptr;
        finishCase("fat_hs_compile_multi zero elements",
                   fat_hs_compile_multi(patterns, flags, ids, 0, HS_MODE_BLOCK,
                                        nullptr, &localDb, &localErr),
                   localDb, localErr);

        // validate_fat_compile_args: elements exceeds Grey::limitPatternCount.
        localDb = nullptr;
        localErr = nullptr;
        finishCase("fat_hs_compile_multi too many elements",
                   fat_hs_compile_multi(patterns, flags, ids, tooManyElements,
                                        HS_MODE_BLOCK, nullptr, &localDb,
                                        &localErr),
                   localDb, localErr);

        // Exercise the same validator through the ext and literal multi entry
        // points.
        localDb = nullptr;
        localErr = nullptr;
        finishCase("fat_hs_compile_ext_multi zero elements",
                   fat_hs_compile_ext_multi(patterns, flags, ids, ext, 0,
                                            HS_MODE_BLOCK, nullptr, &localDb,
                                            &localErr),
                   localDb, localErr);

        localDb = nullptr;
        localErr = nullptr;
        finishCase("fat_hs_compile_lit_multi zero elements",
                   fat_hs_compile_lit_multi(patterns, flags, ids, lengths, 0,
                                            HS_MODE_BLOCK, nullptr, &localDb,
                                            &localErr),
                   localDb, localErr);

        // Adjacent fat compile error paths after validation.
        localDb = nullptr;
        localErr = nullptr;
        finishCase("fat_hs_compile_multi invalid mode none",
                   fat_hs_compile_multi(patterns, flags, ids, 1, 0, nullptr,
                                        &localDb, &localErr),
                   localDb, localErr);

        localDb = nullptr;
        localErr = nullptr;
        finishCase("fat_hs_compile_multi invalid mode multiple",
                   fat_hs_compile_multi(patterns, flags, ids, 1,
                                        HS_MODE_BLOCK | HS_MODE_STREAM, nullptr,
                                        &localDb, &localErr),
                   localDb, localErr);

        localDb = nullptr;
        localErr = nullptr;
        finishCase(
            "fat_hs_compile_multi invalid som mode",
            fat_hs_compile_multi(patterns, flags, ids, 1,
                                 HS_MODE_BLOCK | HS_MODE_SOM_HORIZON_LARGE,
                                 nullptr, &localDb, &localErr),
            localDb, localErr);

        hs_platform_info_t platform;
        platform.tune = 0;
        platform.cpu_features = ~0ULL;
        platform.reserved1 = 0;
        platform.reserved2 = 0;
        localDb = nullptr;
        localErr = nullptr;
        finishCase("fat_hs_compile_multi invalid cpu features",
                   fat_hs_compile_multi(patterns, flags, ids, 1, HS_MODE_BLOCK,
                                        &platform, &localDb, &localErr),
                   localDb, localErr);

        platform.tune = 0xffffffffU;
        platform.cpu_features = 0;
        localDb = nullptr;
        localErr = nullptr;
        finishCase("fat_hs_compile_multi invalid tune",
                   fat_hs_compile_multi(patterns, flags, ids, 1, HS_MODE_BLOCK,
                                        &platform, &localDb, &localErr),
                   localDb, localErr);

        return true;
    }

    bool expressionInfo(const FuzzTestCase &testCase) override {
        hs_expr_info_t *info = nullptr;
        hs_compile_error_t *error = nullptr;

        // 获取表达式信息
        hs_error_t err = hs_expression_info(testCase.pattern.c_str(),
                                            testCase.flags, &info, &error);

        if (err != HS_SUCCESS) {
            // 获取失败，但这是正常的fuzz测试行为
            if (error) {
                std::string message = compileErrorMessage(error);
                stats.expectedFail("hs_expression_info", message);
                detailErr() << "Expression info failed for pattern "
                            << testCase.id << ": " << message << std::endl;
            } else {
                stats.expectedFail("hs_expression_info", errString(err));
            }
            return true;
        }

        // 获取成功
        stats.ok("hs_expression_info");
        if (info) {
            detailOut() << "Expression info succeeded for pattern "
                        << testCase.id << ": "
                        << "min_width=" << info->min_width << ", "
                        << "max_width=" << info->max_width << ", "
                        << "unordered_matches=" << (int)info->unordered_matches
                        << ", "
                        << "matches_at_eod=" << (int)info->matches_at_eod
                        << ", "
                        << "matches_only_at_eod="
                        << (int)info->matches_only_at_eod << std::endl;
            free(info); // hs_expression_info使用malloc分配内存
        }

        return true;
    }

    bool expressionExtInfo(const FuzzTestCase &testCase) override {
        hs_expr_info_t *info = nullptr;
        hs_compile_error_t *error = nullptr;

        // 创建扩展参数
        hs_expr_ext_t ext;
        ext.flags = 0;
        ext.min_offset = 0;
        ext.max_offset = 0;
        ext.min_length = 0;
        ext.edit_distance = 0;
        ext.hamming_distance = 0;

        // 获取带扩展参数的表达式信息
        hs_error_t err = hs_expression_ext_info(
            testCase.pattern.c_str(), testCase.flags, &ext, &info, &error);

        if (err != HS_SUCCESS) {
            // 获取失败，但这是正常的fuzz测试行为
            if (error) {
                std::string message = compileErrorMessage(error);
                stats.expectedFail("hs_expression_ext_info", message);
                detailErr() << "Expression ext info failed for pattern "
                            << testCase.id << ": " << message << std::endl;
            } else {
                stats.expectedFail("hs_expression_ext_info", errString(err));
            }
            return true;
        }

        // 获取成功
        stats.ok("hs_expression_ext_info");
        if (info) {
            detailOut() << "Expression ext info succeeded for pattern "
                        << testCase.id << ": "
                        << "min_width=" << info->min_width << ", "
                        << "max_width=" << info->max_width << ", "
                        << "unordered_matches=" << (int)info->unordered_matches
                        << ", "
                        << "matches_at_eod=" << (int)info->matches_at_eod
                        << ", "
                        << "matches_only_at_eod="
                        << (int)info->matches_only_at_eod << std::endl;
            free(info); // hs_expression_ext_info使用malloc分配内存
        }

        return true;
    }

    bool populatePlatform() override {
        hs_platform_info_t platform;

        // 填充平台信息
        hs_error_t err = hs_populate_platform(&platform);

        if (err != HS_SUCCESS) {
            stats.error("hs_populate_platform", err);
            detailErr() << "Populate platform failed: " << err << std::endl;
            return true;
        }

        // 填充成功
        stats.ok("hs_populate_platform");
        detailOut() << "Populate platform succeeded: "
                    << "tune=" << platform.tune << ", "
                    << "cpu_features=" << platform.cpu_features << std::endl;

        return true;
    }

    bool resetStream() override {
        if (!db || !scratch) {
            stats.skipped("hs_reset_stream");
            return true;
        }

        // 打开流
        hs_stream_t *stream = nullptr;
        hs_error_t err = hs_open_stream(db, 0, &stream);
        if (err != HS_SUCCESS) {
            stats.error("hs_open_stream", err);
            detailErr() << "Failed to open stream for reset: " << err
                        << std::endl;
            return true;
        }

        // 定义匹配回调函数
        int matchCount = 0;
        auto matchCallback = [](unsigned int id, unsigned long long from,
                                unsigned long long to, unsigned int flags,
                                void *context) {
            int *count = static_cast<int *>(context);
            (*count)++;
            return 0; // 继续扫描
        };

        // 写入一些数据
        std::string testData = "test data for reset stream";
        err = hs_scan_stream(stream, testData.c_str(), testData.length(), 0,
                             scratch, matchCallback, &matchCount);
        if (err != HS_SUCCESS && err != HS_SCAN_TERMINATED) {
            stats.error("hs_scan_stream", err);
            detailErr() << "Stream scan failed: " << err << std::endl;
        }

        // 重置流
        err = hs_reset_stream(stream, 0, scratch, matchCallback, &matchCount);
        if (err != HS_SUCCESS) {
            stats.error("hs_reset_stream", err);
            detailErr() << "Reset stream failed: " << err << std::endl;
        } else {
            stats.ok("hs_reset_stream");
            detailOut() << "Reset stream succeeded" << std::endl;
        }

        // 关闭流
        hs_close_stream(stream, scratch, matchCallback, &matchCount);

        return true;
    }

    bool copyStream() override {
        if (!db) {
            stats.skipped("hs_copy_stream");
            return true;
        }

        // 打开流
        hs_stream_t *stream1 = nullptr;
        hs_error_t err = hs_open_stream(db, 0, &stream1);
        if (err != HS_SUCCESS) {
            stats.error("hs_open_stream", err);
            detailErr() << "Failed to open stream 1: " << err << std::endl;
            return true;
        }

        // 写入一些数据
        std::string testData = "test data for copy stream";
        int matchCount = 0;
        auto matchCallback = [](unsigned int id, unsigned long long from,
                                unsigned long long to, unsigned int flags,
                                void *context) {
            int *count = static_cast<int *>(context);
            (*count)++;
            return 0; // 继续扫描
        };

        err = hs_scan_stream(stream1, testData.c_str(), testData.length(), 0,
                             scratch, matchCallback, &matchCount);
        if (err != HS_SUCCESS && err != HS_SCAN_TERMINATED) {
            stats.error("hs_scan_stream", err);
            detailErr() << "Stream scan failed: " << err << std::endl;
        }

        // 复制流
        hs_stream_t *stream2 = nullptr;
        err = hs_copy_stream(&stream2, stream1);
        if (err != HS_SUCCESS) {
            stats.error("hs_copy_stream", err);
            detailErr() << "Copy stream failed: " << err << std::endl;
        } else {
            stats.ok("hs_copy_stream");
            detailOut() << "Copy stream succeeded" << std::endl;
        }

        // 关闭流
        hs_close_stream(stream1, scratch, matchCallback, &matchCount);
        if (stream2) {
            hs_close_stream(stream2, scratch, matchCallback, &matchCount);
        }

        return true;
    }

    bool resetAndCopyStream() override {
        if (!db || !scratch) {
            stats.skipped("hs_reset_and_copy_stream");
            return true;
        }

        // 打开两个流
        hs_stream_t *stream1 = nullptr;
        hs_stream_t *stream2 = nullptr;

        hs_error_t err = hs_open_stream(db, 0, &stream1);
        if (err != HS_SUCCESS) {
            stats.error("hs_open_stream", err);
            detailErr() << "Failed to open stream 1: " << err << std::endl;
            return true;
        }

        err = hs_open_stream(db, 0, &stream2);
        if (err != HS_SUCCESS) {
            stats.error("hs_open_stream", err);
            detailErr() << "Failed to open stream 2: " << err << std::endl;
            hs_close_stream(stream1, scratch, nullptr, nullptr);
            return true;
        }

        // 向流1写入数据
        std::string testData = "test data for reset and copy stream";
        int matchCount = 0;
        auto matchCallback = [](unsigned int id, unsigned long long from,
                                unsigned long long to, unsigned int flags,
                                void *context) {
            int *count = static_cast<int *>(context);
            (*count)++;
            return 0; // 继续扫描
        };

        err = hs_scan_stream(stream1, testData.c_str(), testData.length(), 0,
                             scratch, matchCallback, &matchCount);
        if (err != HS_SUCCESS && err != HS_SCAN_TERMINATED) {
            stats.error("hs_scan_stream", err);
            detailErr() << "Stream scan failed: " << err << std::endl;
        }

        // 重置并复制流
        err = hs_reset_and_copy_stream(stream2, stream1, scratch, matchCallback,
                                       &matchCount);
        if (err != HS_SUCCESS) {
            stats.error("hs_reset_and_copy_stream", err);
            detailErr() << "Reset and copy stream failed: " << err << std::endl;
        } else {
            stats.ok("hs_reset_and_copy_stream");
            detailOut() << "Reset and copy stream succeeded" << std::endl;
        }

        // 关闭流
        hs_close_stream(stream1, scratch, matchCallback, &matchCount);
        hs_close_stream(stream2, scratch, matchCallback, &matchCount);

        return true;
    }

    bool compressStream() override {
        if (!db || !scratch) {
            stats.skipped("hs_compress_stream");
            return true;
        }

        // 打开流
        hs_stream_t *stream = nullptr;
        hs_error_t err = hs_open_stream(db, 0, &stream);
        if (err != HS_SUCCESS) {
            stats.error("hs_open_stream", err);
            detailErr() << "Failed to open stream: " << err << std::endl;
            return true;
        }

        // 写入一些数据
        std::string testData = "test data for compress stream";
        int matchCount = 0;
        auto matchCallback = [](unsigned int id, unsigned long long from,
                                unsigned long long to, unsigned int flags,
                                void *context) {
            int *count = static_cast<int *>(context);
            (*count)++;
            return 0; // 继续扫描
        };

        err = hs_scan_stream(stream, testData.c_str(), testData.length(), 0,
                             scratch, matchCallback, &matchCount);
        if (err != HS_SUCCESS && err != HS_SCAN_TERMINATED) {
            stats.error("hs_scan_stream", err);
            detailErr() << "Stream scan failed: " << err << std::endl;
        }

        // 压缩流
        size_t requiredSpace = 0;
        err = hs_compress_stream(stream, nullptr, 0, &requiredSpace);
        if (err != HS_INSUFFICIENT_SPACE) {
            stats.error("hs_compress_stream", err);
            detailErr() << "Failed to get required space: " << err << std::endl;
            hs_close_stream(stream, scratch, matchCallback, &matchCount);
            return true;
        }

        // 分配缓冲区并压缩
        std::vector<char> buffer(requiredSpace);
        size_t usedSpace = 0;
        err = hs_compress_stream(stream, buffer.data(), buffer.size(),
                                 &usedSpace);
        if (err != HS_SUCCESS) {
            stats.error("hs_compress_stream", err);
            detailErr() << "Compress stream failed: " << err << std::endl;
        } else {
            stats.ok("hs_compress_stream");
            detailOut() << "Compress stream succeeded, used " << usedSpace
                        << " bytes" << std::endl;
        }

        // 关闭流
        hs_close_stream(stream, scratch, matchCallback, &matchCount);

        return true;
    }

    bool expandStream() override {
        if (!db) {
            stats.skipped("hs_expand_stream");
            return true;
        }

        // 打开流并写入数据
        hs_stream_t *stream1 = nullptr;
        hs_error_t err = hs_open_stream(db, 0, &stream1);
        if (err != HS_SUCCESS) {
            stats.error("hs_open_stream", err);
            detailErr() << "Failed to open stream 1: " << err << std::endl;
            return true;
        }

        std::string testData = "test data for expand stream";
        int matchCount = 0;
        auto matchCallback = [](unsigned int id, unsigned long long from,
                                unsigned long long to, unsigned int flags,
                                void *context) {
            int *count = static_cast<int *>(context);
            (*count)++;
            return 0; // 继续扫描
        };

        err = hs_scan_stream(stream1, testData.c_str(), testData.length(), 0,
                             scratch, matchCallback, &matchCount);
        if (err != HS_SUCCESS && err != HS_SCAN_TERMINATED) {
            stats.error("hs_scan_stream", err);
            detailErr() << "Stream scan failed: " << err << std::endl;
        }

        // 压缩流
        size_t requiredSpace = 0;
        err = hs_compress_stream(stream1, nullptr, 0, &requiredSpace);
        if (err != HS_INSUFFICIENT_SPACE) {
            stats.error("hs_compress_stream", err);
            detailErr() << "Failed to get required space: " << err << std::endl;
            hs_close_stream(stream1, scratch, matchCallback, &matchCount);
            return true;
        }

        std::vector<char> buffer(requiredSpace);
        size_t usedSpace = 0;
        err = hs_compress_stream(stream1, buffer.data(), buffer.size(),
                                 &usedSpace);
        if (err != HS_SUCCESS) {
            stats.error("hs_compress_stream", err);
            detailErr() << "Compress stream failed: " << err << std::endl;
            hs_close_stream(stream1, scratch, matchCallback, &matchCount);
            return true;
        }

        // 解压流
        hs_stream_t *stream2 = nullptr;
        err = hs_expand_stream(db, &stream2, buffer.data(), usedSpace);
        if (err != HS_SUCCESS) {
            stats.error("hs_expand_stream", err);
            detailErr() << "Expand stream failed: " << err << std::endl;
        } else {
            stats.ok("hs_expand_stream");
            detailOut() << "Expand stream succeeded" << std::endl;
        }

        // 关闭流
        hs_close_stream(stream1, scratch, matchCallback, &matchCount);
        if (stream2) {
            hs_close_stream(stream2, scratch, matchCallback, &matchCount);
        }

        return true;
    }

    bool resetAndExpandStream() override {
        if (!db || !scratch) {
            stats.skipped("hs_reset_and_expand_stream");
            return true;
        }

        // 打开流并写入数据
        hs_stream_t *stream1 = nullptr;
        hs_stream_t *stream2 = nullptr;

        hs_error_t err = hs_open_stream(db, 0, &stream1);
        if (err != HS_SUCCESS) {
            stats.error("hs_open_stream", err);
            detailErr() << "Failed to open stream 1: " << err << std::endl;
            return true;
        }

        err = hs_open_stream(db, 0, &stream2);
        if (err != HS_SUCCESS) {
            stats.error("hs_open_stream", err);
            detailErr() << "Failed to open stream 2: " << err << std::endl;
            hs_close_stream(stream1, scratch, nullptr, nullptr);
            return true;
        }

        std::string testData = "test data for reset and expand stream";
        int matchCount = 0;
        auto matchCallback = [](unsigned int id, unsigned long long from,
                                unsigned long long to, unsigned int flags,
                                void *context) {
            int *count = static_cast<int *>(context);
            (*count)++;
            return 0; // 继续扫描
        };

        err = hs_scan_stream(stream1, testData.c_str(), testData.length(), 0,
                             scratch, matchCallback, &matchCount);
        if (err != HS_SUCCESS && err != HS_SCAN_TERMINATED) {
            stats.error("hs_scan_stream", err);
            detailErr() << "Stream scan failed: " << err << std::endl;
        }

        // 压缩流
        size_t requiredSpace = 0;
        err = hs_compress_stream(stream1, nullptr, 0, &requiredSpace);
        if (err != HS_INSUFFICIENT_SPACE) {
            stats.error("hs_compress_stream", err);
            detailErr() << "Failed to get required space: " << err << std::endl;
            hs_close_stream(stream1, scratch, matchCallback, &matchCount);
            hs_close_stream(stream2, scratch, matchCallback, &matchCount);
            return true;
        }

        std::vector<char> buffer(requiredSpace);
        size_t usedSpace = 0;
        err = hs_compress_stream(stream1, buffer.data(), buffer.size(),
                                 &usedSpace);
        if (err != HS_SUCCESS) {
            stats.error("hs_compress_stream", err);
            detailErr() << "Compress stream failed: " << err << std::endl;
            hs_close_stream(stream1, scratch, matchCallback, &matchCount);
            hs_close_stream(stream2, scratch, matchCallback, &matchCount);
            return true;
        }

        // 重置并解压流
        err = hs_reset_and_expand_stream(stream2, buffer.data(), usedSpace,
                                         scratch, matchCallback, &matchCount);
        if (err != HS_SUCCESS) {
            stats.error("hs_reset_and_expand_stream", err);
            detailErr() << "Reset and expand stream failed: " << err
                        << std::endl;
        } else {
            stats.ok("hs_reset_and_expand_stream");
            detailOut() << "Reset and expand stream succeeded" << std::endl;
        }

        // 关闭流
        hs_close_stream(stream1, scratch, matchCallback, &matchCount);
        hs_close_stream(stream2, scratch, matchCallback, &matchCount);

        return true;
    }

    bool scanVector(const std::vector<std::string> &data) override {
        if (!db || !scratch) {
            stats.skipped("hs_scan_vector");
            return true;
        }

        if (data.empty()) {
            stats.skipped("hs_scan_vector");
            return true;
        }

        // 准备向量扫描参数
        std::vector<const char *> dataPtrs;
        std::vector<unsigned int> lengths;

        for (const auto &str : data) {
            dataPtrs.push_back(str.c_str());
            lengths.push_back(str.length());
        }

        // 定义匹配回调函数
        int matchCount = 0;
        auto matchCallback = [](unsigned int id, unsigned long long from,
                                unsigned long long to, unsigned int flags,
                                void *context) {
            int *count = static_cast<int *>(context);
            (*count)++;
            return 0; // 继续扫描
        };

        // 执行向量扫描
        hs_error_t err =
            hs_scan_vector(db, dataPtrs.data(), lengths.data(), data.size(), 0,
                           scratch, matchCallback, &matchCount);

        if (err != HS_SUCCESS && err != HS_SCAN_TERMINATED) {
            stats.error("hs_scan_vector", err);
            detailErr() << "Vector scan failed with error: " << err
                        << std::endl;
        } else {
            stats.ok("hs_scan_vector");
            stats.matches("hs_scan_vector", matchCount);
            detailOut() << "Vector scan completed, found " << matchCount
                        << " matches" << std::endl;
        }

        return true;
    }

    bool cloneScratch() override {
        if (!db) {
            stats.skipped("hs_clone_scratch");
            return true;
        }

        // 确保有scratch空间
        if (!scratch) {
            hs_error_t err = hs_alloc_scratch(db, &scratch);
            if (err != HS_SUCCESS) {
                stats.error("hs_alloc_scratch", err);
                detailErr() << "Failed to allocate scratch space" << std::endl;
                return true;
            }
        }

        // 克隆scratch空间
        hs_scratch_t *clonedScratch = nullptr;
        hs_error_t err = hs_clone_scratch(scratch, &clonedScratch);
        if (err != HS_SUCCESS) {
            stats.error("hs_clone_scratch", err);
            detailErr() << "Clone scratch failed: " << err << std::endl;
        } else {
            stats.ok("hs_clone_scratch");
            detailOut() << "Clone scratch succeeded" << std::endl;
            // 释放克隆的scratch空间
            hs_free_scratch(clonedScratch);
        }

        return true;
    }

    bool getScratchSize() override {
        if (!scratch) {
            stats.skipped("hs_scratch_size");
            return true;
        }

        // 获取scratch空间大小
        size_t scratchSize = 0;
        hs_error_t err = hs_scratch_size(scratch, &scratchSize);
        if (err != HS_SUCCESS) {
            stats.error("hs_scratch_size", err);
            detailErr() << "Get scratch size failed: " << err << std::endl;
        } else {
            stats.ok("hs_scratch_size");
            detailOut() << "Scratch size: " << scratchSize << " bytes"
                        << std::endl;
        }

        return true;
    }

    hs_error_t falsePositiveFeedbackCapability() override {
        const char *expressions[] = {"abc"};
        const unsigned int flags[] = {0};
        const unsigned int ids[] = {1};
        hs_database_t *probeDb = nullptr;
        hs_compile_error_t *probeError = nullptr;
        hs_error_t result =
            hs_compile_multi(expressions, flags, ids, 1, HS_MODE_BLOCK, nullptr,
                             &probeDb, &probeError);
        if (probeError) {
            hs_free_compile_error(probeError);
        }
        if (result != HS_SUCCESS || !probeDb) {
            if (result == HS_SUCCESS) {
                result = HS_UNKNOWN_ERROR;
            }
            if (probeDb) {
                const hs_error_t freeErr = hs_free_database(probeDb);
                if (freeErr != HS_SUCCESS) {
                    result = freeErr;
                }
            }
            stats.error("fp_feedback_capability_probe", result);
            return result;
        }

        hs_fp_collector_t *collector = nullptr;
        result = hs_fp_collector_create(probeDb, &collector);

        hs_error_t collectorFreeErr = HS_SUCCESS;
        if (collector) {
            collectorFreeErr = hs_fp_collector_free(collector);
        } else if (result == HS_SUCCESS) {
            result = HS_UNKNOWN_ERROR;
        }
        const hs_error_t databaseFreeErr = hs_free_database(probeDb);

        if (collectorFreeErr != HS_SUCCESS) {
            result = collectorFreeErr;
        } else if (databaseFreeErr != HS_SUCCESS) {
            result = databaseFreeErr;
        }

        if (result == HS_SUCCESS) {
            stats.ok("fp_feedback_capability_probe");
        } else if (result == HS_ARCH_ERROR) {
            stats.skipped("fp_feedback_capability_probe");
        } else {
            stats.error("fp_feedback_capability_probe", result);
        }
        return result;
    }

    bool falsePositiveFeedback(const FuzzTestCase &testCase,
                               const std::vector<std::string> &data,
                               const FuzzProgressCallback &progress) override {
        bool ok = true;
        if (!exerciseFalsePositiveFeedbackMode(testCase, data, HS_MODE_BLOCK,
                                               "block", progress)) {
            ok = false;
        }
        if (!exerciseFalsePositiveFeedbackMode(testCase, data, HS_MODE_STREAM,
                                               "stream", progress)) {
            ok = false;
        }
        if (!exerciseFalsePositiveFeedbackMode(testCase, data, HS_MODE_VECTORED,
                                               "vector", progress)) {
            ok = false;
        }
        return ok;
    }

    bool falsePositiveFeedbackInvalidArgs() override {
        bool ok = true;
        ScopedCollector unexpectedCollector;
        ScopedFeedback unexpectedFeedback;

        ok &= recordExpectedInvalid("hs_fp_collector_create_null_out",
                                    hs_fp_collector_create(nullptr, nullptr));
        ok &= recordExpectedInvalid(
            "hs_fp_collector_create_null_db",
            hs_fp_collector_create(nullptr, &unexpectedCollector.value));
        if (unexpectedCollector.value) {
            stats.error("hs_fp_collector_create_null_db",
                        "invalid call produced a collector");
            ok = false;
        }
        ok &= recordExpectedInvalid("hs_fp_collector_reset_null",
                                    hs_fp_collector_reset(nullptr));

        hs_fp_collector_t *nullCollector = nullptr;
        hs_fp_collector_t *mergeOutput = nullptr;
        ok &= recordExpectedInvalid(
            "hs_fp_collector_merge_null_inputs",
            hs_fp_collector_merge(nullptr, 0, &mergeOutput));
        if (mergeOutput) {
            stats.error("hs_fp_collector_merge_null_inputs",
                        "invalid call produced a collector");
            hs_fp_collector_free(mergeOutput);
            mergeOutput = nullptr;
            ok = false;
        }
        ok &= recordExpectedInvalid(
            "hs_fp_collector_merge_zero_count",
            hs_fp_collector_merge(&nullCollector, 0, &mergeOutput));
        ok &= recordExpectedInvalid(
            "hs_fp_collector_merge_null_member",
            hs_fp_collector_merge(&nullCollector, 1, &mergeOutput));
        ok &= recordExpectedInvalid(
            "hs_fp_collector_merge_null_out",
            hs_fp_collector_merge(&nullCollector, 1, nullptr));

        ok &= recordExpectedInvalid(
            "hs_fp_collector_to_feedback_null_collector",
            hs_fp_collector_to_feedback(nullptr, nullptr,
                                        &unexpectedFeedback.value));
        if (unexpectedFeedback.value) {
            stats.error("hs_fp_collector_to_feedback_null_collector",
                        "invalid call produced feedback");
            ok = false;
        }
        ok &= recordExpectedInvalid(
            "hs_fp_collector_to_feedback_null_out",
            hs_fp_collector_to_feedback(nullptr, nullptr, nullptr));
        ok &= recordExpectedInvalid(
            "hs_fp_collector_to_feedback_with_dump_null_collector",
            hs_fp_collector_to_feedback_with_dump(
                nullptr, nullptr, nullptr, nullptr, &unexpectedFeedback.value));
        ok &= recordExpectedInvalid(
            "hs_fp_collector_to_feedback_with_dump_null_out",
            hs_fp_collector_to_feedback_with_dump(nullptr, nullptr, nullptr,
                                                  nullptr, nullptr));

        ok &= recordExpectedSuccess("hs_fp_collector_free_null",
                                    hs_fp_collector_free(nullptr));
        ok &= recordExpectedSuccess("hs_fp_feedback_free_null",
                                    hs_fp_feedback_free(nullptr));

        ok &= recordExpectedInvalid("hs_scan_with_collector_null_db",
                                    hs_scan_with_collector(nullptr, "", 0, 0,
                                                           nullptr, nullptr,
                                                           nullptr, nullptr));
        ok &= recordExpectedInvalid(
            "hs_scan_stream_with_collector_null_stream",
            hs_scan_stream_with_collector(nullptr, "", 0, 0, nullptr, nullptr,
                                          nullptr, nullptr));
        ok &= recordExpectedInvalid(
            "hs_scan_vector_with_collector_null_db",
            hs_scan_vector_with_collector(nullptr, nullptr, nullptr, 0, 0,
                                          nullptr, nullptr, nullptr, nullptr));

        if (!exerciseFalsePositiveFeedbackParameterInvalidArgs()) {
            ok = false;
        }
        if (!exerciseFalsePositiveFeedbackDatabaseMismatch()) {
            ok = false;
        }
        return ok;
    }

    void reset() override {
        if (scratch) {
            hs_free_scratch(scratch);
            scratch = nullptr;
        }
        if (db) {
            hs_free_database(db);
            db = nullptr;
        }
        if (fatDb) {
            fat_hs_free_database(fatDb);
            fatDb = nullptr;
        }
        if (compileErr) {
            hs_free_compile_error(compileErr);
            compileErr = nullptr;
        }
    }

    void printSummary(std::ostream &os) const override { stats.print(os); }

private:
    bool shouldExerciseFatDatabaseApis(unsigned int mode) const {
        return mode == HS_MODE_BLOCK || mode == HS_MODE_STREAM ||
               mode == HS_MODE_VECTORED;
    }

    bool exerciseFatDatabaseApis(const char *context) {
        if (!fatDb) {
            return true;
        }

        size_t databaseSize = 0;
        hs_error_t err = fat_hs_database_size(fatDb, &databaseSize);
        if (err != HS_SUCCESS) {
            stats.error("fat_hs_database_size", err);
            detailErr() << context << ": fat_hs_database_size failed: " << err
                        << std::endl;
        } else {
            stats.ok("fat_hs_database_size");
            detailOut() << context << ": fat database size is " << databaseSize
                        << " bytes" << std::endl;
        }

        char *info = nullptr;
        err = fat_hs_database_info(fatDb, &info);
        if (err != HS_SUCCESS) {
            stats.error("fat_hs_database_info", err);
            detailErr() << context << ": fat_hs_database_info failed: " << err
                        << std::endl;
        } else {
            stats.ok("fat_hs_database_info");
            detailOut() << context << ": fat database info: " << info
                        << std::endl;
        }
        std::free(info);

        char *serialized = nullptr;
        size_t serializedLength = 0;
        err = fat_hs_serialize_database(fatDb, &serialized, &serializedLength);
        if (err != HS_SUCCESS) {
            stats.error("fat_hs_serialize_database", err);
            detailErr() << context
                        << ": fat_hs_serialize_database failed: " << err
                        << std::endl;
            return true;
        }
        stats.ok("fat_hs_serialize_database");
        detailOut() << context << ": serialized fat database size is "
                    << serializedLength << " bytes" << std::endl;

        size_t deserializedSize = 0;
        err = fat_hs_serialized_database_size(serialized, serializedLength,
                                              &deserializedSize);
        if (err != HS_SUCCESS) {
            stats.error("fat_hs_serialized_database_size", err);
            detailErr() << context
                        << ": fat_hs_serialized_database_size failed: " << err
                        << std::endl;
        } else {
            stats.ok("fat_hs_serialized_database_size");
            detailOut() << context << ": deserialized fat database size is "
                        << deserializedSize << " bytes" << std::endl;
        }

        fat_hs_database_t *deserializedDb = nullptr;
        err = fat_hs_deserialize_database(serialized, serializedLength,
                                          &deserializedDb);
        if (err != HS_SUCCESS) {
            stats.error("fat_hs_deserialize_database", err);
            detailErr() << context
                        << ": fat_hs_deserialize_database failed: " << err
                        << std::endl;
        } else {
            stats.ok("fat_hs_deserialize_database");
            detailOut() << context << ": fat_hs_deserialize_database succeeded"
                        << std::endl;
            fat_hs_free_database(deserializedDb);
        }

        if (deserializedSize > 0) {
            std::vector<unsigned long long> deserializedBuffer(
                (deserializedSize + sizeof(unsigned long long) - 1) /
                sizeof(unsigned long long));
            fat_hs_database_t *deserializedAtDb =
                reinterpret_cast<fat_hs_database_t *>(
                    deserializedBuffer.data());
            err = fat_hs_deserialize_database_at(serialized, serializedLength,
                                                 deserializedAtDb);
            if (err != HS_SUCCESS) {
                stats.error("fat_hs_deserialize_database_at", err);
                detailErr()
                    << context
                    << ": fat_hs_deserialize_database_at failed: " << err
                    << std::endl;
            } else {
                stats.ok("fat_hs_deserialize_database_at");
                detailOut()
                    << context << ": fat_hs_deserialize_database_at succeeded"
                    << std::endl;
            }
        }

        std::free(serialized);
        return true;
    }

    static bool isFeedbackDisabled(hs_error_t err) {
        return err == HS_ARCH_ERROR;
    }

    bool recordExpectedInvalid(const std::string &api, hs_error_t err,
                               bool allowDisabled = true) {
        if (err == HS_INVALID || (allowDisabled && isFeedbackDisabled(err))) {
            stats.expectedFail(api, errString(err));
            return true;
        }
        if (err == HS_SUCCESS) {
            stats.error(api, "unexpected success");
        } else {
            stats.error(api, err);
        }
        return false;
    }

    bool recordExpectedSuccess(const std::string &api, hs_error_t err) {
        if (err == HS_SUCCESS) {
            stats.ok(api);
            return true;
        }
        stats.error(api, err);
        return false;
    }

    bool releaseCollector(const std::string &api, ScopedCollector &collector) {
        hs_fp_collector_t *raw = collector.value;
        collector.value = nullptr;
        return recordExpectedSuccess(api, hs_fp_collector_free(raw));
    }

    bool releaseFeedback(const std::string &api, ScopedFeedback &feedback) {
        hs_fp_feedback_t *raw = feedback.value;
        feedback.value = nullptr;
        return recordExpectedSuccess(api, hs_fp_feedback_free(raw));
    }

    static void markProgress(const FuzzProgressCallback &progress,
                             const std::string &stage) {
        if (progress) {
            progress(stage);
        }
    }

    bool compileLocalDatabase(const FuzzTestCase &testCase, unsigned int mode,
                              const std::string &apiPrefix,
                              LocalDatabase &database, bool &compiled) {
        compiled = false;
        const char *expressions[] = {testCase.pattern.c_str()};
        const unsigned int flags[] = {testCase.flags};
        const unsigned int ids[] = {static_cast<unsigned int>(testCase.id)};
        hs_compile_error_t *localError = nullptr;
        hs_error_t err = hs_compile_multi(expressions, flags, ids, 1, mode,
                                          nullptr, &database.db, &localError);
        if (err != HS_SUCCESS) {
            const std::string message =
                localError ? compileErrorMessage(localError) : errString(err);
            if (err == HS_COMPILER_ERROR) {
                stats.expectedFail(apiPrefix + "_compile", message);
            } else {
                stats.error(apiPrefix + "_compile", message);
            }
            if (database.db) {
                stats.error(apiPrefix + "_compile",
                            "failed compilation produced a database");
                return false;
            }
            return err == HS_COMPILER_ERROR;
        }
        if (localError) {
            hs_free_compile_error(localError);
        }
        stats.ok(apiPrefix + "_compile");

        err = hs_alloc_scratch(database.db, &database.scratch);
        if (err != HS_SUCCESS) {
            stats.error(apiPrefix + "_alloc_scratch", err);
            return false;
        }
        stats.ok(apiPrefix + "_alloc_scratch");
        compiled = true;
        return true;
    }

    static std::vector<std::string>
    sampleData(const std::vector<std::string> &generatedData) {
        std::vector<std::string> data;
        const size_t limit = std::min<size_t>(generatedData.size(), 4);
        data.reserve(limit ? limit : 1);
        for (size_t i = 0; i < limit; i++) {
            data.push_back(generatedData[i]);
        }
        if (data.empty()) {
            data.emplace_back();
        }
        return data;
    }

    hs_error_t scanBlockMatches(const LocalDatabase &database,
                                const std::vector<std::string> &data,
                                hs_fp_collector_t *collector,
                                const std::string &api, MatchCapture &capture) {
        capture.matches.clear();
        capture.vectorEnds.clear();
        for (size_t i = 0; i < data.size(); i++) {
            capture.currentInput = static_cast<unsigned int>(i);
            capture.currentPhase = 0;
            const std::string &block = data[i];
            hs_error_t err;
            if (collector) {
                err = hs_scan_with_collector(
                    database.db, block.c_str(),
                    static_cast<unsigned int>(block.size()), 0,
                    database.scratch, captureMatch, &capture, collector);
            } else {
                err = hs_scan(database.db, block.c_str(),
                              static_cast<unsigned int>(block.size()), 0,
                              database.scratch, captureMatch, &capture);
            }
            if (err != HS_SUCCESS) {
                stats.error(api, err);
                return err;
            }
        }
        stats.ok(api);
        stats.matches(api, capture.matches.size());
        return HS_SUCCESS;
    }

    hs_error_t scanStreamMatches(const LocalDatabase &database,
                                 const std::vector<std::string> &data,
                                 hs_fp_collector_t *collector,
                                 const std::string &api,
                                 MatchCapture &capture) {
        capture.matches.clear();
        capture.vectorEnds.clear();
        hs_stream_t *stream = nullptr;
        hs_error_t err = hs_open_stream(database.db, 0, &stream);
        if (err != HS_SUCCESS) {
            stats.error(api + "_open", err);
            return err;
        }

        for (size_t i = 0; i < data.size(); i++) {
            capture.currentInput = static_cast<unsigned int>(i);
            capture.currentPhase = 0;
            const std::string &block = data[i];
            if (collector) {
                err = hs_scan_stream_with_collector(
                    stream, block.c_str(),
                    static_cast<unsigned int>(block.size()), 0,
                    database.scratch, captureMatch, &capture, collector);
            } else {
                err = hs_scan_stream(stream, block.c_str(),
                                     static_cast<unsigned int>(block.size()), 0,
                                     database.scratch, captureMatch, &capture);
            }
            if (err != HS_SUCCESS) {
                stats.error(api, err);
                const hs_error_t closeErr =
                    hs_close_stream(stream, nullptr, nullptr, nullptr);
                if (closeErr != HS_SUCCESS) {
                    stats.error(api + "_cleanup_close", closeErr);
                }
                return err;
            }
        }

        capture.currentInput = static_cast<unsigned int>(data.size());
        capture.currentPhase = 1;
        err = hs_close_stream(stream, database.scratch, captureMatch, &capture);
        if (err != HS_SUCCESS) {
            stats.error(api + "_close", err);
            return err;
        }
        stats.ok(api);
        stats.matches(api, capture.matches.size());
        return HS_SUCCESS;
    }

    hs_error_t scanVectorMatches(const LocalDatabase &database,
                                 const std::vector<std::string> &data,
                                 hs_fp_collector_t *collector,
                                 const std::string &api,
                                 MatchCapture &capture) {
        capture.matches.clear();
        capture.vectorEnds.clear();
        capture.currentPhase = 0;
        std::vector<const char *> pointers;
        std::vector<unsigned int> lengths;
        pointers.reserve(data.size());
        lengths.reserve(data.size());
        unsigned long long end = 0;
        for (const auto &block : data) {
            pointers.push_back(block.c_str());
            lengths.push_back(static_cast<unsigned int>(block.size()));
            end += block.size();
            capture.vectorEnds.push_back(end);
        }

        hs_error_t err;
        if (collector) {
            err = hs_scan_vector_with_collector(
                database.db, pointers.data(), lengths.data(),
                static_cast<unsigned int>(pointers.size()), 0, database.scratch,
                captureMatch, &capture, collector);
        } else {
            err = hs_scan_vector(database.db, pointers.data(), lengths.data(),
                                 static_cast<unsigned int>(pointers.size()), 0,
                                 database.scratch, captureMatch, &capture);
        }
        if (err != HS_SUCCESS) {
            stats.error(api, err);
            return err;
        }
        stats.ok(api);
        stats.matches(api, capture.matches.size());
        return HS_SUCCESS;
    }

    hs_error_t scanModeMatches(const LocalDatabase &database,
                               const std::vector<std::string> &data,
                               unsigned int mode, hs_fp_collector_t *collector,
                               const std::string &api, MatchCapture &capture) {
        if (mode == HS_MODE_BLOCK) {
            return scanBlockMatches(database, data, collector, api, capture);
        }
        if (mode == HS_MODE_STREAM) {
            return scanStreamMatches(database, data, collector, api, capture);
        }
        return scanVectorMatches(database, data, collector, api, capture);
    }

    bool compareMatches(const std::string &api,
                        const MatchCapture &expectedCapture,
                        const MatchCapture &actualCapture) {
        std::vector<MatchRecord> expected = expectedCapture.matches;
        std::vector<MatchRecord> actual = actualCapture.matches;
        std::sort(expected.begin(), expected.end());
        std::sort(actual.begin(), actual.end());
        if (expected == actual) {
            stats.ok(api);
            return true;
        }

        std::ostringstream out;
        out << "match multiset mismatch, expected=" << expected.size()
            << ", actual=" << actual.size();
        const size_t common = std::min(expected.size(), actual.size());
        size_t mismatch = 0;
        while (mismatch < common && expected[mismatch] == actual[mismatch]) {
            mismatch++;
        }
        if (mismatch < common) {
            out << ", first expected {" << describeMatch(expected[mismatch])
                << "}, actual {" << describeMatch(actual[mismatch]) << "}";
        } else if (mismatch < expected.size()) {
            out << ", first extra expected {"
                << describeMatch(expected[mismatch]) << "}";
        } else if (mismatch < actual.size()) {
            out << ", first extra actual {" << describeMatch(actual[mismatch])
                << "}";
        }
        stats.error(api, out.str());
        return false;
    }

    bool validateFeedbackDump(const std::string &api,
                              const FeedbackDumpStats &dump) {
        if (dump.invalid) {
            stats.error(api, dump.invalidReason);
            return false;
        }
        if (dump.summaryCalls != 1) {
            stats.error(api, "summary callback count mismatch");
            return false;
        }
        if (dump.fragmentCalls != dump.summary.fragment_count) {
            stats.error(api, "fragment callback count mismatch");
            return false;
        }
        if (dump.selectedCalls != dump.summary.bad_fragment_count) {
            stats.error(api, "selected fragment count mismatch");
            return false;
        }
        if (dump.triggerCount != dump.summary.trigger_count ||
            dump.trueTriggerCount != dump.summary.true_trigger_count ||
            dump.falsePositiveCount != dump.summary.false_positive_count) {
            stats.error(api, "fragment counters do not match summary");
            return false;
        }
        stats.ok(api);
        return true;
    }

    bool compileFeedbackVariant(const FuzzTestCase &testCase, unsigned int mode,
                                const std::vector<std::string> &data,
                                const MatchCapture &expectedMatches,
                                const hs_fp_feedback_t *feedback, bool useExt,
                                const std::string &api,
                                const FuzzProgressCallback &progress) {
        markProgress(progress, api);
        const char *expressions[] = {testCase.pattern.c_str()};
        const unsigned int flags[] = {testCase.flags};
        const unsigned int ids[] = {static_cast<unsigned int>(testCase.id)};
        hs_expr_ext_t extValue = {};
        const hs_expr_ext_t *ext[] = {&extValue};
        LocalDatabase compiled;
        hs_compile_error_t *localError = nullptr;
        hs_error_t err;
        if (useExt) {
            err = hs_compile_ext_multi_with_feedback(
                expressions, flags, ids, ext, 1, mode, nullptr, feedback,
                &compiled.db, &localError);
        } else {
            err = hs_compile_multi_with_feedback(expressions, flags, ids, 1,
                                                 mode, nullptr, feedback,
                                                 &compiled.db, &localError);
        }

        if (err != HS_SUCCESS) {
            const std::string message =
                localError ? compileErrorMessage(localError) : errString(err);
            stats.error(api, message);
            return false;
        }
        if (localError) {
            hs_free_compile_error(localError);
        }
        if (!compiled.db) {
            stats.error(api, "successful compilation returned a null database");
            return false;
        }
        stats.ok(api);

        err = hs_alloc_scratch(compiled.db, &compiled.scratch);
        if (err != HS_SUCCESS) {
            stats.error(api + "_alloc_scratch", err);
            return false;
        }
        stats.ok(api + "_alloc_scratch");

        MatchCapture actualMatches;
        markProgress(progress, api + "_scan");
        err = scanModeMatches(compiled, data, mode, nullptr, api + "_scan",
                              actualMatches);
        if (err != HS_SUCCESS) {
            return false;
        }
        return compareMatches(api + "_matches", expectedMatches, actualMatches);
    }

    bool exerciseFalsePositiveFeedbackMode(
        const FuzzTestCase &testCase,
        const std::vector<std::string> &generatedData, unsigned int mode,
        const char *modeName, const FuzzProgressCallback &progress) {
        const std::string prefix = std::string("fp_feedback_") + modeName;
        bool ok = true;
        bool mergedReady = false;
        bool feedbackReady = false;
        bool dumpFeedbackReady = false;
        LocalDatabase database;
        bool compiled = false;
        markProgress(progress, prefix + "_compile");
        if (!compileLocalDatabase(testCase, mode, prefix, database, compiled)) {
            return false;
        }
        if (!compiled) {
            return true;
        }

        const std::vector<std::string> data = sampleData(generatedData);
        ScopedCollector collectorA;
        ScopedCollector collectorB;
        ScopedCollector merged;
        ScopedFeedback feedback;
        ScopedFeedback dumpFeedback;
        ScopedFeedback resetFeedback;

        markProgress(progress, prefix + "_collector_create");
        hs_error_t err = hs_fp_collector_create(database.db, &collectorA.value);
        if (isFeedbackDisabled(err)) {
            stats.expectedFail(prefix + "_collector_create",
                               "false-positive feedback disabled");
            return true;
        }
        if (err != HS_SUCCESS || !collectorA.value) {
            if (err == HS_SUCCESS) {
                stats.error(prefix + "_collector_create",
                            "successful call returned a null collector");
            } else {
                stats.error(prefix + "_collector_create", err);
            }
            return false;
        }
        stats.ok(prefix + "_collector_create");

        markProgress(progress, prefix + "_collector_create_second");
        err = hs_fp_collector_create(database.db, &collectorB.value);
        if (err != HS_SUCCESS || !collectorB.value) {
            if (err == HS_SUCCESS) {
                stats.error(prefix + "_collector_create_second",
                            "successful call returned a null collector");
            } else {
                stats.error(prefix + "_collector_create_second", err);
            }
            return false;
        }
        stats.ok(prefix + "_collector_create_second");

        MatchCapture normalMatches;
        MatchCapture collectorMatches;
        markProgress(progress, prefix + "_normal_scan");
        err = scanModeMatches(database, data, mode, nullptr,
                              prefix + "_normal_scan", normalMatches);
        if (err != HS_SUCCESS) {
            ok = false;
        }
        markProgress(progress, prefix + "_scan_with_collector");
        err =
            scanModeMatches(database, data, mode, collectorA.value,
                            prefix + "_scan_with_collector", collectorMatches);
        if (err != HS_SUCCESS) {
            ok = false;
        } else if (!compareMatches(prefix + "_collector_matches", normalMatches,
                                   collectorMatches)) {
            ok = false;
        }

        MatchCapture secondCollectorMatches;
        markProgress(progress, prefix + "_second_scan_with_collector");
        err = scanModeMatches(database, data, mode, collectorB.value,
                              prefix + "_second_scan_with_collector",
                              secondCollectorMatches);
        if (err != HS_SUCCESS) {
            ok = false;
        } else if (!compareMatches(prefix + "_second_collector_matches",
                                   normalMatches, secondCollectorMatches)) {
            ok = false;
        }

        markProgress(progress, prefix + "_collector_reset");
        err = hs_fp_collector_reset(collectorB.value);
        if (err != HS_SUCCESS) {
            stats.error(prefix + "_collector_reset", err);
            ok = false;
        } else {
            stats.ok(prefix + "_collector_reset");
            FeedbackDumpStats resetDump;
            hs_fp_feedback_dump_callbacks_t resetCallbacks = {};
            resetCallbacks.on_summary = feedbackDumpSummary;
            resetCallbacks.on_fragment = feedbackDumpFragment;
            markProgress(progress, prefix + "_collector_reset_dump");
            err = hs_fp_collector_to_feedback_with_dump(
                collectorB.value, nullptr, &resetCallbacks, &resetDump,
                &resetFeedback.value);
            if (err != HS_SUCCESS || !resetFeedback.value) {
                if (err == HS_SUCCESS) {
                    stats.error(prefix + "_collector_reset_dump",
                                "successful call returned null feedback");
                } else {
                    stats.error(prefix + "_collector_reset_dump", err);
                }
                ok = false;
            } else {
                if (!validateFeedbackDump(prefix + "_collector_reset_dump",
                                          resetDump)) {
                    ok = false;
                }
                if (resetDump.summary.trigger_count ||
                    resetDump.summary.true_trigger_count ||
                    resetDump.summary.false_positive_count) {
                    stats.error(prefix + "_collector_reset_counters",
                                "reset collector retained runtime counters");
                    ok = false;
                } else {
                    stats.ok(prefix + "_collector_reset_counters");
                }
            }
        }

        secondCollectorMatches = MatchCapture();
        markProgress(progress, prefix + "_second_scan_after_reset");
        err = scanModeMatches(database, data, mode, collectorB.value,
                              prefix + "_second_scan_after_reset",
                              secondCollectorMatches);
        if (err != HS_SUCCESS) {
            ok = false;
        } else if (!compareMatches(prefix + "_after_reset_matches",
                                   normalMatches, secondCollectorMatches)) {
            ok = false;
        }

        hs_fp_collector_t *mergeInputs[] = {collectorA.value, collectorB.value};
        markProgress(progress, prefix + "_collector_merge");
        err = hs_fp_collector_merge(mergeInputs, 2, &merged.value);
        if (err != HS_SUCCESS || !merged.value) {
            if (err == HS_SUCCESS) {
                stats.error(prefix + "_collector_merge",
                            "successful call returned a null collector");
            } else {
                stats.error(prefix + "_collector_merge", err);
            }
            ok = false;
        } else {
            stats.ok(prefix + "_collector_merge");
            mergedReady = true;
        }

        if (mergedReady) {
            markProgress(progress, prefix + "_collector_to_feedback");
            err = hs_fp_collector_to_feedback(merged.value, nullptr,
                                              &feedback.value);
            if (err != HS_SUCCESS || !feedback.value) {
                if (err == HS_SUCCESS) {
                    stats.error(prefix + "_collector_to_feedback",
                                "successful call returned null feedback");
                } else {
                    stats.error(prefix + "_collector_to_feedback", err);
                }
                ok = false;
            } else {
                stats.ok(prefix + "_collector_to_feedback");
                feedbackReady = true;
            }

            hs_fp_feedback_params_t params = {};
            params.flags = HS_FP_FEEDBACK_PARAM_MIN_TRIGGER_COUNT |
                           HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_COUNT |
                           HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_RATE |
                           HS_FP_FEEDBACK_PARAM_MIN_WASTE_SHARE |
                           HS_FP_FEEDBACK_PARAM_MAX_BAD_FRAGMENTS;
            params.max_bad_fragments = 8;
            FeedbackDumpStats dump;
            hs_fp_feedback_dump_callbacks_t callbacks = {};
            callbacks.on_summary = feedbackDumpSummary;
            callbacks.on_fragment = feedbackDumpFragment;
            markProgress(progress, prefix + "_collector_to_feedback_with_dump");
            err = hs_fp_collector_to_feedback_with_dump(
                merged.value, &params, &callbacks, &dump, &dumpFeedback.value);
            if (err != HS_SUCCESS || !dumpFeedback.value) {
                if (err == HS_SUCCESS) {
                    stats.error(prefix + "_collector_to_feedback_with_dump",
                                "successful call returned null feedback");
                } else {
                    stats.error(prefix + "_collector_to_feedback_with_dump",
                                err);
                }
                ok = false;
            } else {
                dumpFeedbackReady = validateFeedbackDump(
                    prefix + "_collector_to_feedback_with_dump", dump);
                if (!dumpFeedbackReady) {
                    ok = false;
                }
            }
        }

        if (!compileFeedbackVariant(
                testCase, mode, data, normalMatches, nullptr, false,
                prefix + "_compile_multi_null_feedback", progress)) {
            ok = false;
        }
        if (!compileFeedbackVariant(
                testCase, mode, data, normalMatches, nullptr, true,
                prefix + "_compile_ext_multi_null_feedback", progress)) {
            ok = false;
        }
        if (feedbackReady &&
            !compileFeedbackVariant(
                testCase, mode, data, normalMatches, feedback.value, false,
                prefix + "_compile_multi_with_feedback", progress)) {
            ok = false;
        }
        if (dumpFeedbackReady &&
            !compileFeedbackVariant(
                testCase, mode, data, normalMatches, dumpFeedback.value, true,
                prefix + "_compile_ext_multi_with_feedback", progress)) {
            ok = false;
        }

        if (!releaseFeedback(prefix + "_reset_feedback_free", resetFeedback)) {
            ok = false;
        }
        if (!releaseFeedback(prefix + "_dump_feedback_free", dumpFeedback)) {
            ok = false;
        }
        if (!releaseFeedback(prefix + "_feedback_free", feedback)) {
            ok = false;
        }
        if (!releaseCollector(prefix + "_merged_collector_free", merged)) {
            ok = false;
        }
        if (!releaseCollector(prefix + "_second_collector_free", collectorB)) {
            ok = false;
        }
        if (!releaseCollector(prefix + "_collector_free", collectorA)) {
            ok = false;
        }
        return ok;
    }

    bool exerciseFalsePositiveFeedbackParameterInvalidArgs() {
        FuzzTestCase testCase;
        testCase.pattern = "abc";
        testCase.flags = 0;
        testCase.id = 7001;
        LocalDatabase database;
        bool compiled = false;
        if (!compileLocalDatabase(testCase, HS_MODE_BLOCK,
                                  "fp_feedback_invalid_params", database,
                                  compiled)) {
            return false;
        }
        if (!compiled) {
            stats.error("fp_feedback_invalid_params",
                        "fixed validation expression did not compile");
            return false;
        }

        bool ok = true;
        ScopedCollector collector;
        hs_error_t err = hs_fp_collector_create(database.db, &collector.value);
        if (isFeedbackDisabled(err)) {
            stats.expectedFail("fp_feedback_invalid_params_collector_create",
                               "false-positive feedback disabled");
            return true;
        }
        if (err != HS_SUCCESS || !collector.value) {
            if (err == HS_SUCCESS) {
                stats.error("fp_feedback_invalid_params_collector_create",
                            "successful call returned null collector");
            } else {
                stats.error("fp_feedback_invalid_params_collector_create", err);
            }
            return false;
        }
        stats.ok("fp_feedback_invalid_params_collector_create");

        auto checkInvalidParams = [this, &collector](
                                      const std::string &api,
                                      const hs_fp_feedback_params_t &params,
                                      bool withDump) {
            bool caseOk = true;
            ScopedFeedback output;
            hs_error_t callErr;
            if (withDump) {
                callErr = hs_fp_collector_to_feedback_with_dump(
                    collector.value, &params, nullptr, nullptr, &output.value);
            } else {
                callErr = hs_fp_collector_to_feedback(collector.value, &params,
                                                      &output.value);
            }
            if (!recordExpectedInvalid(api, callErr, false)) {
                caseOk = false;
            }
            if (output.value) {
                stats.error(api, "invalid call produced feedback");
                if (!releaseFeedback(api + "_unexpected_free", output)) {
                    caseOk = false;
                }
                caseOk = false;
            }
            return caseOk;
        };

        hs_fp_feedback_params_t params = {};
        params.flags = 0x80000000U;
        ok &= checkInvalidParams("hs_fp_collector_to_feedback_bad_flags",
                                 params, false);

        params = {};
        params.flags = HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_RATE;
        params.min_false_positive_rate = HS_FP_FEEDBACK_RATE_SCALE + 1;
        ok &= checkInvalidParams("hs_fp_collector_to_feedback_bad_fp_rate",
                                 params, false);

        params = {};
        params.flags = HS_FP_FEEDBACK_PARAM_MIN_WASTE_SHARE;
        params.min_waste_share = HS_FP_FEEDBACK_RATE_SCALE + 1;
        ok &= checkInvalidParams("hs_fp_collector_to_feedback_bad_waste_share",
                                 params, true);

        params = {};
        params.flags = HS_FP_FEEDBACK_PARAM_MAX_BAD_FRAGMENTS;
        params.max_bad_fragments = 0;
        ok &= checkInvalidParams("hs_fp_collector_to_feedback_bad_topk", params,
                                 false);

        ScopedFeedback validFeedback;
        err = hs_fp_collector_to_feedback(collector.value, nullptr,
                                          &validFeedback.value);
        if (err != HS_SUCCESS || !validFeedback.value) {
            if (err == HS_SUCCESS) {
                stats.error("hs_fp_collector_to_feedback_default_params",
                            "successful call returned null feedback");
            } else {
                stats.error("hs_fp_collector_to_feedback_default_params", err);
            }
            ok = false;
        } else {
            stats.ok("hs_fp_collector_to_feedback_default_params");
        }

        if (!releaseFeedback("fp_feedback_invalid_params_feedback_free",
                             validFeedback)) {
            ok = false;
        }
        if (!releaseCollector("fp_feedback_invalid_params_collector_free",
                              collector)) {
            ok = false;
        }
        return ok;
    }

    bool
    exerciseFalsePositiveFeedbackDatabaseMismatchMode(unsigned int mode,
                                                      const char *modeName) {
        const std::string prefix =
            std::string("fp_feedback_mismatch_") + modeName;
        FuzzTestCase caseA;
        caseA.pattern = "abc";
        caseA.flags = 0;
        caseA.id = 7101;
        FuzzTestCase caseB;
        caseB.pattern = "def";
        caseB.flags = 0;
        caseB.id = 7102;
        LocalDatabase databaseA;
        LocalDatabase databaseB;
        bool compiledA = false;
        bool compiledB = false;
        if (!compileLocalDatabase(caseA, mode, prefix + "_a", databaseA,
                                  compiledA) ||
            !compileLocalDatabase(caseB, mode, prefix + "_b", databaseB,
                                  compiledB)) {
            return false;
        }
        if (!compiledA || !compiledB) {
            stats.error(prefix,
                        "fixed mismatch validation expression did not compile");
            return false;
        }

        bool ok = true;
        ScopedCollector collectorA;
        ScopedCollector collectorB;
        hs_error_t err =
            hs_fp_collector_create(databaseA.db, &collectorA.value);
        if (isFeedbackDisabled(err)) {
            stats.expectedFail(prefix + "_collector_create",
                               "false-positive feedback disabled");
            return true;
        }
        if (err != HS_SUCCESS || !collectorA.value) {
            if (err == HS_SUCCESS) {
                stats.error(prefix + "_collector_create",
                            "successful call returned null collector");
            } else {
                stats.error(prefix + "_collector_create", err);
            }
            return false;
        }
        stats.ok(prefix + "_collector_create");

        err = hs_fp_collector_create(databaseB.db, &collectorB.value);
        if (err != HS_SUCCESS || !collectorB.value) {
            if (err == HS_SUCCESS) {
                stats.error(prefix + "_collector_create_second",
                            "successful call returned null collector");
            } else {
                stats.error(prefix + "_collector_create_second", err);
            }
            return false;
        }
        stats.ok(prefix + "_collector_create_second");

        MatchCapture capture;
        if (mode == HS_MODE_BLOCK) {
            err = hs_scan_with_collector(databaseB.db, "def", 3, 0,
                                         databaseB.scratch, captureMatch,
                                         &capture, collectorA.value);
        } else if (mode == HS_MODE_VECTORED) {
            const char *blocks[] = {"def"};
            const unsigned int lengths[] = {3};
            err = hs_scan_vector_with_collector(
                databaseB.db, blocks, lengths, 1, 0, databaseB.scratch,
                captureMatch, &capture, collectorA.value);
        } else {
            hs_stream_t *stream = nullptr;
            err = hs_open_stream(databaseB.db, 0, &stream);
            if (err != HS_SUCCESS) {
                stats.error(prefix + "_open_stream", err);
                ok = false;
            } else {
                err = hs_scan_stream_with_collector(
                    stream, "def", 3, 0, databaseB.scratch, captureMatch,
                    &capture, collectorA.value);
                const hs_error_t closeErr = hs_close_stream(
                    stream, databaseB.scratch, captureMatch, &capture);
                if (closeErr != HS_SUCCESS) {
                    stats.error(prefix + "_close_stream", closeErr);
                    ok = false;
                } else {
                    stats.ok(prefix + "_close_stream");
                }
            }
        }
        if (!recordExpectedInvalid(prefix + "_scan_database_mismatch", err,
                                   false)) {
            ok = false;
        }

        hs_fp_collector_t *mergeInputs[] = {collectorA.value, collectorB.value};
        ScopedCollector merged;
        err = hs_fp_collector_merge(mergeInputs, 2, &merged.value);
        if (!recordExpectedInvalid(prefix + "_merge_database_mismatch", err,
                                   false)) {
            ok = false;
        }
        if (merged.value) {
            stats.error(prefix + "_merge_database_mismatch",
                        "invalid merge produced a collector");
            ok = false;
        }

        if (!releaseCollector(prefix + "_collector_free_second", collectorB)) {
            ok = false;
        }
        if (!releaseCollector(prefix + "_collector_free", collectorA)) {
            ok = false;
        }
        return ok;
    }

    bool exerciseFalsePositiveFeedbackDatabaseMismatch() {
        bool ok = true;
        if (!exerciseFalsePositiveFeedbackDatabaseMismatchMode(HS_MODE_BLOCK,
                                                               "block")) {
            ok = false;
        }
        if (!exerciseFalsePositiveFeedbackDatabaseMismatchMode(HS_MODE_STREAM,
                                                               "stream")) {
            ok = false;
        }
        if (!exerciseFalsePositiveFeedbackDatabaseMismatchMode(HS_MODE_VECTORED,
                                                               "vector")) {
            ok = false;
        }
        return ok;
    }

    hs_database_t *db;
    fat_hs_database_t *fatDb;
    hs_compile_error_t *compileErr;
    hs_scratch_t *scratch;
    FuzzStats stats;
};

std::unique_ptr<Runner> createRunner() {
    return std::make_unique<HyperscanRunner>();
}
