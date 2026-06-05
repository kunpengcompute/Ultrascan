#include "../fuzz_test.h"
#include "hs.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <streambuf>

namespace {

class NullBuffer : public std::streambuf {
public:
    int overflow(int c) override {
        return c == traits_type::eof() ? traits_type::not_eof(c) : c;
    }
};

bool fuzzVerbose() {
    const char* value = std::getenv("HS_FUZZ_VERBOSE");
    return value && value[0] != '\0' && value[0] != '0';
}

std::ostream& detailOut() {
    static NullBuffer nullBuffer;
    static std::ostream nullStream(&nullBuffer);
    return fuzzVerbose() ? std::cout : nullStream;
}

std::ostream& detailErr() {
    static NullBuffer nullBuffer;
    static std::ostream nullStream(&nullBuffer);
    return fuzzVerbose() ? std::cerr : nullStream;
}

std::string errString(hs_error_t err) {
    std::ostringstream out;
    out << err;
    return out.str();
}

std::string compileErrorMessage(hs_compile_error_t*& err) {
    if (!err) {
        return "no compile error detail";
    }

    std::string message = err->message ? err->message : "no compile error detail";
    hs_free_compile_error(err);
    err = nullptr;
    return message;
}

size_t maxUniqueErrors() {
    const char* value = std::getenv("HS_FUZZ_MAX_UNIQUE_ERRORS");
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
    void ok(const std::string& api) {
        stats[api].ok++;
    }

    void expectedFail(const std::string& api, const std::string& message) {
        stats[api].expectedFail++;
        recordMessage(api, message);
    }

    void error(const std::string& api, hs_error_t err) {
        error(api, "error " + errString(err));
    }

    void error(const std::string& api, const std::string& message) {
        stats[api].errors++;
        recordMessage(api, message);
    }

    void skipped(const std::string& api) {
        stats[api].skipped++;
    }

    void matches(const std::string& api, unsigned long long count) {
        stats[api].matches += count;
    }

    void print(std::ostream& os) const {
        os << "api summary:" << std::endl;
        if (stats.empty()) {
            os << "  no api calls recorded" << std::endl;
        }

        for (const auto& item : stats) {
            const ApiStats& value = item.second;
            os << "  " << item.first
               << ": ok=" << value.ok
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
            for (const auto& item : messages) {
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
    void recordMessage(const std::string& api, const std::string& message) {
        std::string text = api + ": " + message;
        messages[text]++;
    }

    std::map<std::string, ApiStats> stats;
    std::map<std::string, unsigned long long> messages;
};

} // namespace

class HyperscanRunner : public Runner {
public:
    HyperscanRunner() : db(nullptr), fatDb(nullptr), compileErr(nullptr), scratch(nullptr) {}

    ~HyperscanRunner() {
        reset();
    }

    bool compile(const FuzzTestCase& testCase, unsigned int mode = HS_MODE_BLOCK) override {
        // 重置之前的状态
        reset();

        // 编译正则表达式
        hs_error_t err = hs_compile(
            testCase.pattern.c_str(),
            testCase.flags,
            mode,
            nullptr, // 使用默认平台
            &db,
            &compileErr
        );

        if (err != HS_SUCCESS) {
            // 编译失败，但这是正常的fuzz测试行为
            // 记录失败信息但返回true，表示测试执行成功
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("hs_compile", message);
                detailErr() << "Compilation failed for pattern " << testCase.id << ": "
                          << message << std::endl;
            } else {
                stats.expectedFail("hs_compile", errString(err));
            }
            return true;
        }

        // 编译成功
        stats.ok("hs_compile");
        detailOut() << "Compilation succeeded for pattern " << testCase.id << std::endl;

        // 分配scratch空间
        err = hs_alloc_scratch(db, &scratch);
        if (err != HS_SUCCESS) {
            stats.error("hs_alloc_scratch", err);
            detailErr() << "Failed to allocate scratch space" << std::endl;
            return true;
        }

        return true;
    }

    bool fatCompile(const FuzzTestCase& testCase, unsigned int mode = HS_MODE_BLOCK) override {
        // 重置之前的状态
        reset();

        // 编译通用字节码正则表达式
        hs_error_t err = fat_hs_compile(
            testCase.pattern.c_str(),
            testCase.flags,
            mode,
            nullptr, // 使用默认平台
            &fatDb,
            &compileErr
        );

        if (err != HS_SUCCESS) {
            // 编译失败是正常的fuzz测试行为
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("fat_hs_compile", message);
                detailErr() << "Fat compilation failed for pattern " << testCase.id << ": "
                          << message << std::endl;
            } else {
                stats.expectedFail("fat_hs_compile", errString(err));
            }
            return true;
        }

        stats.ok("fat_hs_compile");
        detailOut() << "Fat compilation succeeded for pattern " << testCase.id << std::endl;
        if (shouldExerciseFatDatabaseApis(mode)) {
            exerciseFatDatabaseApis("fat_hs_compile");
        }
        return true;
    }

    bool scan(const std::string& data) override {
        if (!db || !scratch) {
            stats.skipped("hs_scan");
            return false;
        }

        // 定义匹配回调函数
        int matchCount = 0;
        auto matchCallback = [](unsigned int id, unsigned long long from, unsigned long long to, unsigned int flags, void* context) {
            int* count = static_cast<int*>(context);
            (*count)++;
            return 0; // 继续扫描
        };

        // 执行扫描
        hs_error_t err = hs_scan(
            db,
            data.c_str(),
            data.length(),
            0,
            scratch,
            matchCallback,
            &matchCount
        );

        if (err != HS_SUCCESS && err != HS_SCAN_TERMINATED) {
            stats.error("hs_scan", err);
            detailErr() << "Scan failed with error: " << err << std::endl;
        } else {
            stats.ok("hs_scan");
        }

        stats.matches("hs_scan", matchCount);
        detailOut() << "Scan completed, found " << matchCount << " matches" << std::endl;
        return true;
    }

    bool streamScan(const std::string& data) override {
        if (!db || !scratch) {
            stats.skipped("hs_scan_stream");
            return false;
        }

        // 打开流
        hs_stream_t* stream = nullptr;
        hs_error_t err = hs_open_stream(db, 0, &stream);
        if (err != HS_SUCCESS) {
            stats.error("hs_open_stream", err);
            detailErr() << "Failed to open stream: " << err << std::endl;
            return true;
        }

        // 定义匹配回调函数
        int matchCount = 0;
        auto matchCallback = [](unsigned int id, unsigned long long from, unsigned long long to, unsigned int flags, void* context) {
            int* count = static_cast<int*>(context);
            (*count)++;
            return 0; // 继续扫描
        };

        // 分块扫描
        const size_t chunkSize = 1024;
        size_t offset = 0;
        while (offset < data.length()) {
            size_t chunk = std::min(chunkSize, data.length() - offset);
            err = hs_scan_stream(
                stream,
                data.c_str() + offset,
                chunk,
                0,
                scratch,
                matchCallback,
                &matchCount
            );
            if (err != HS_SUCCESS && err != HS_SCAN_TERMINATED) {
                stats.error("hs_scan_stream", err);
                detailErr() << "Stream scan failed with error: " << err << std::endl;
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
        detailOut() << "Stream scan completed, found " << matchCount << " matches" << std::endl;
        return true;
    }

    bool compileMulti(const std::vector<FuzzTestCase>& testCases) override {
        if (testCases.empty()) {
            return true;
        }

        // 重置之前的状态
        reset();

        // 准备参数
        std::vector<const char*> patterns;
        std::vector<unsigned int> flags;
        std::vector<unsigned int> ids;

        for (const auto& testCase : testCases) {
            patterns.push_back(testCase.pattern.c_str());
            flags.push_back(testCase.flags);
            ids.push_back(testCase.id);
        }

        // 编译多模式
        hs_error_t err = hs_compile_multi(
            patterns.data(),
            flags.data(),
            ids.data(),
            testCases.size(),
            HS_MODE_BLOCK,
            nullptr,
            &db,
            &compileErr
        );

        if (err != HS_SUCCESS) {
            // 编译失败，但这是正常的fuzz测试行为
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("hs_compile_multi", message);
                detailErr() << "Multi compilation failed: " << message << std::endl;
            } else {
                stats.expectedFail("hs_compile_multi", errString(err));
            }
            return true;
        }

        // 编译成功
        stats.ok("hs_compile_multi");
        detailOut() << "Multi compilation succeeded for " << testCases.size() << " patterns" << std::endl;

        // 分配scratch空间
        err = hs_alloc_scratch(db, &scratch);
        if (err != HS_SUCCESS) {
            stats.error("hs_alloc_scratch", err);
            detailErr() << "Failed to allocate scratch space" << std::endl;
            return true;
        }

        return true;
    }

    bool compileExtMulti(const std::vector<FuzzTestCase>& testCases) override {
        if (testCases.empty()) {
            return true;
        }

        // 重置之前的状态
        reset();

        // 准备参数
        std::vector<const char*> patterns;
        std::vector<unsigned int> flags;
        std::vector<unsigned int> ids;
        std::vector<hs_expr_ext_t> extParams(testCases.size());
        std::vector<const hs_expr_ext_t*> extParamsPtrs;
        extParamsPtrs.reserve(testCases.size());

        for (size_t i = 0; i < testCases.size(); i++) {
            const auto& testCase = testCases[i];
            patterns.push_back(testCase.pattern.c_str());
            flags.push_back(testCase.flags);
            ids.push_back(testCase.id);

            // 创建扩展参数
            hs_expr_ext_t& ext = extParams[i];
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
            patterns.data(),
            flags.data(),
            ids.data(),
            extParamsPtrs.data(),
            testCases.size(),
            HS_MODE_BLOCK,
            nullptr,
            &db,
            &compileErr
        );

        if (err != HS_SUCCESS) {
            // 编译失败，但这是正常的fuzz测试行为
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("hs_compile_ext_multi", message);
                detailErr() << "Ext multi compilation failed: " << message << std::endl;
            } else {
                stats.expectedFail("hs_compile_ext_multi", errString(err));
            }
            return true;
        }

        // 编译成功
        stats.ok("hs_compile_ext_multi");
        detailOut() << "Ext multi compilation succeeded for " << testCases.size() << " patterns" << std::endl;

        // 分配scratch空间
        err = hs_alloc_scratch(db, &scratch);
        if (err != HS_SUCCESS) {
            stats.error("hs_alloc_scratch", err);
            detailErr() << "Failed to allocate scratch space" << std::endl;
            return true;
        }

        return true;
    }

    bool fatCompileMulti(const std::vector<FuzzTestCase>& testCases, unsigned int mode = HS_MODE_BLOCK) override {
        if (testCases.empty()) {
            return true;
        }

        // 重置之前的状态
        reset();

        // 准备参数
        std::vector<const char*> patterns;
        std::vector<unsigned int> flags;
        std::vector<unsigned int> ids;

        for (const auto& testCase : testCases) {
            patterns.push_back(testCase.pattern.c_str());
            flags.push_back(testCase.flags);
            ids.push_back(testCase.id);
        }

        // 编译通用字节码多模式
        hs_error_t err = fat_hs_compile_multi(
            patterns.data(),
            flags.data(),
            ids.data(),
            testCases.size(),
            mode,
            nullptr,
            &fatDb,
            &compileErr
        );

        if (err != HS_SUCCESS) {
            // 编译失败是正常的fuzz测试行为
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("fat_hs_compile_multi", message);
                detailErr() << "Fat multi compilation failed: " << message << std::endl;
            } else {
                stats.expectedFail("fat_hs_compile_multi", errString(err));
            }
            return true;
        }

        stats.ok("fat_hs_compile_multi");
        detailOut() << "Fat multi compilation succeeded for " << testCases.size() << " patterns" << std::endl;
        if (shouldExerciseFatDatabaseApis(mode)) {
            exerciseFatDatabaseApis("fat_hs_compile_multi");
        }
        return true;
    }

    bool fatCompileExtMulti(const std::vector<FuzzTestCase>& testCases, unsigned int mode = HS_MODE_BLOCK) override {
        if (testCases.empty()) {
            return true;
        }

        // 重置之前的状态
        reset();

        // 准备参数
        std::vector<const char*> patterns;
        std::vector<unsigned int> flags;
        std::vector<unsigned int> ids;
        std::vector<hs_expr_ext_t> extParams(testCases.size());
        std::vector<const hs_expr_ext_t*> extParamsPtrs;
        extParamsPtrs.reserve(testCases.size());

        for (size_t i = 0; i < testCases.size(); i++) {
            const auto& testCase = testCases[i];
            patterns.push_back(testCase.pattern.c_str());
            flags.push_back(testCase.flags);
            ids.push_back(testCase.id);

            // 创建扩展参数
            hs_expr_ext_t& ext = extParams[i];
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
            patterns.data(),
            flags.data(),
            ids.data(),
            extParamsPtrs.data(),
            testCases.size(),
            mode,
            nullptr,
            &fatDb,
            &compileErr
        );

        if (err != HS_SUCCESS) {
            // 编译失败是正常的fuzz测试行为
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("fat_hs_compile_ext_multi", message);
                detailErr() << "Fat ext multi compilation failed: " << message << std::endl;
            } else {
                stats.expectedFail("fat_hs_compile_ext_multi", errString(err));
            }
            return true;
        }

        stats.ok("fat_hs_compile_ext_multi");
        detailOut() << "Fat ext multi compilation succeeded for " << testCases.size() << " patterns" << std::endl;
        if (shouldExerciseFatDatabaseApis(mode)) {
            exerciseFatDatabaseApis("fat_hs_compile_ext_multi");
        }
        return true;
    }

    bool compileLit(const FuzzTestCase& testCase, size_t length) override {
        // 重置之前的状态
        reset();

        // 编译纯字面表达式
        hs_error_t err = hs_compile_lit(
            testCase.pattern.c_str(),
            testCase.flags,
            length,
            HS_MODE_BLOCK,
            nullptr,
            &db,
            &compileErr
        );

        if (err != HS_SUCCESS) {
            // 编译失败，但这是正常的fuzz测试行为
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("hs_compile_lit", message);
                detailErr() << "Literal compilation failed for pattern " << testCase.id << ": "
                          << message << std::endl;
            } else {
                stats.expectedFail("hs_compile_lit", errString(err));
            }
            return true;
        }

        // 编译成功
        stats.ok("hs_compile_lit");
        detailOut() << "Literal compilation succeeded for pattern " << testCase.id << std::endl;

        // 分配scratch空间
        err = hs_alloc_scratch(db, &scratch);
        if (err != HS_SUCCESS) {
            stats.error("hs_alloc_scratch", err);
            detailErr() << "Failed to allocate scratch space" << std::endl;
            return true;
        }

        return true;
    }

    bool fatCompileLit(const FuzzTestCase& testCase, size_t length, unsigned int mode = HS_MODE_BLOCK) override {
        // 重置之前的状态
        reset();

        // 编译通用字节码纯字面表达式
        hs_error_t err = fat_hs_compile_lit(
            testCase.pattern.c_str(),
            testCase.flags,
            length,
            mode,
            nullptr,
            &fatDb,
            &compileErr
        );

        if (err != HS_SUCCESS) {
            // 编译失败是正常的fuzz测试行为
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("fat_hs_compile_lit", message);
                detailErr() << "Fat literal compilation failed for pattern " << testCase.id << ": "
                          << message << std::endl;
            } else {
                stats.expectedFail("fat_hs_compile_lit", errString(err));
            }
            return true;
        }

        stats.ok("fat_hs_compile_lit");
        detailOut() << "Fat literal compilation succeeded for pattern " << testCase.id << std::endl;
        if (shouldExerciseFatDatabaseApis(mode)) {
            exerciseFatDatabaseApis("fat_hs_compile_lit");
        }
        return true;
    }

    bool compileLitMulti(const std::vector<FuzzTestCase>& testCases, const std::vector<size_t>& lengths) override {
        if (testCases.empty() || testCases.size() != lengths.size()) {
            return true;
        }

        // 重置之前的状态
        reset();

        // 准备参数
        std::vector<const char*> patterns;
        std::vector<unsigned int> flags;
        std::vector<unsigned int> ids;

        for (const auto& testCase : testCases) {
            patterns.push_back(testCase.pattern.c_str());
            flags.push_back(testCase.flags);
            ids.push_back(testCase.id);
        }

        // 编译多纯字面表达式
        hs_error_t err = hs_compile_lit_multi(
            patterns.data(),
            flags.data(),
            ids.data(),
            lengths.data(),
            testCases.size(),
            HS_MODE_BLOCK,
            nullptr,
            &db,
            &compileErr
        );

        if (err != HS_SUCCESS) {
            // 编译失败，但这是正常的fuzz测试行为
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("hs_compile_lit_multi", message);
                detailErr() << "Multi literal compilation failed: " << message << std::endl;
            } else {
                stats.expectedFail("hs_compile_lit_multi", errString(err));
            }
            return true;
        }

        // 编译成功
        stats.ok("hs_compile_lit_multi");
        detailOut() << "Multi literal compilation succeeded for " << testCases.size() << " patterns" << std::endl;

        // 分配scratch空间
        err = hs_alloc_scratch(db, &scratch);
        if (err != HS_SUCCESS) {
            stats.error("hs_alloc_scratch", err);
            detailErr() << "Failed to allocate scratch space" << std::endl;
            return true;
        }

        return true;
    }

    bool fatCompileLitMulti(const std::vector<FuzzTestCase>& testCases, const std::vector<size_t>& lengths, unsigned int mode = HS_MODE_BLOCK) override {
        if (testCases.empty() || testCases.size() != lengths.size()) {
            return true;
        }

        // 重置之前的状态
        reset();

        // 准备参数
        std::vector<const char*> patterns;
        std::vector<unsigned int> flags;
        std::vector<unsigned int> ids;

        for (const auto& testCase : testCases) {
            patterns.push_back(testCase.pattern.c_str());
            flags.push_back(testCase.flags);
            ids.push_back(testCase.id);
        }

        // 编译通用字节码多纯字面表达式
        hs_error_t err = fat_hs_compile_lit_multi(
            patterns.data(),
            flags.data(),
            ids.data(),
            lengths.data(),
            testCases.size(),
            mode,
            nullptr,
            &fatDb,
            &compileErr
        );

        if (err != HS_SUCCESS) {
            // 编译失败是正常的fuzz测试行为
            if (compileErr) {
                std::string message = compileErrorMessage(compileErr);
                stats.expectedFail("fat_hs_compile_lit_multi", message);
                detailErr() << "Fat multi literal compilation failed: " << message << std::endl;
            } else {
                stats.expectedFail("fat_hs_compile_lit_multi", errString(err));
            }
            return true;
        }

        stats.ok("fat_hs_compile_lit_multi");
        detailOut() << "Fat multi literal compilation succeeded for " << testCases.size() << " patterns" << std::endl;
        if (shouldExerciseFatDatabaseApis(mode)) {
            exerciseFatDatabaseApis("fat_hs_compile_lit_multi");
        }
        return true;
    }

    bool fatCompileInvalidArgs() override {
        detailOut() << "Testing fat_hs_compile invalid argument paths..." << std::endl;

        const char* patterns[] = {"abc"};
        unsigned int flags[] = {0};
        unsigned int ids[] = {0};
        size_t lengths[] = {3};
        const hs_expr_ext_t* ext[] = {nullptr};
        const unsigned int tooManyElements = 8000001U;

        auto finishCase = [this](const char* name, hs_error_t err,
                                 fat_hs_database_t* localDb,
                                 hs_compile_error_t* localErr) {
            detailOut() << name << " returned " << err << std::endl;
            if (err != HS_SUCCESS) {
                std::string message = localErr ? compileErrorMessage(localErr)
                                               : errString(err);
                stats.expectedFail("fat_invalid_args", std::string(name) + ": " + message);
                detailErr() << name << " compile error: " << message << std::endl;
            } else {
                stats.error("fat_invalid_args", std::string(name) + ": unexpected success");
            }
            if (localErr) {
                hs_free_compile_error(localErr);
            }
            if (localDb) {
                stats.error("fat_invalid_args", std::string(name) + ": unexpectedly produced a database");
                detailErr() << name << " unexpectedly produced a database" << std::endl;
                fat_hs_free_database(localDb);
            }
        };

        fat_hs_database_t* localDb = nullptr;
        hs_compile_error_t* localErr = nullptr;

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
                                        HS_MODE_BLOCK, nullptr, &localDb, &localErr),
                   localDb, localErr);

        // Exercise the same validator through the ext and literal multi entry points.
        localDb = nullptr;
        localErr = nullptr;
        finishCase("fat_hs_compile_ext_multi zero elements",
                   fat_hs_compile_ext_multi(patterns, flags, ids, ext, 0,
                                            HS_MODE_BLOCK, nullptr, &localDb, &localErr),
                   localDb, localErr);

        localDb = nullptr;
        localErr = nullptr;
        finishCase("fat_hs_compile_lit_multi zero elements",
                   fat_hs_compile_lit_multi(patterns, flags, ids, lengths, 0,
                                            HS_MODE_BLOCK, nullptr, &localDb, &localErr),
                   localDb, localErr);

        // Adjacent fat compile error paths after validation.
        localDb = nullptr;
        localErr = nullptr;
        finishCase("fat_hs_compile_multi invalid mode none",
                   fat_hs_compile_multi(patterns, flags, ids, 1, 0,
                                        nullptr, &localDb, &localErr),
                   localDb, localErr);

        localDb = nullptr;
        localErr = nullptr;
        finishCase("fat_hs_compile_multi invalid mode multiple",
                   fat_hs_compile_multi(patterns, flags, ids, 1,
                                        HS_MODE_BLOCK | HS_MODE_STREAM,
                                        nullptr, &localDb, &localErr),
                   localDb, localErr);

        localDb = nullptr;
        localErr = nullptr;
        finishCase("fat_hs_compile_multi invalid som mode",
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

    bool expressionInfo(const FuzzTestCase& testCase) override {
        hs_expr_info_t* info = nullptr;
        hs_compile_error_t* error = nullptr;

        // 获取表达式信息
        hs_error_t err = hs_expression_info(
            testCase.pattern.c_str(),
            testCase.flags,
            &info,
            &error
        );

        if (err != HS_SUCCESS) {
            // 获取失败，但这是正常的fuzz测试行为
            if (error) {
                std::string message = compileErrorMessage(error);
                stats.expectedFail("hs_expression_info", message);
                detailErr() << "Expression info failed for pattern " << testCase.id << ": "
                          << message << std::endl;
            } else {
                stats.expectedFail("hs_expression_info", errString(err));
            }
            return true;
        }

        // 获取成功
        stats.ok("hs_expression_info");
        if (info) {
            detailOut() << "Expression info succeeded for pattern " << testCase.id << ": "
                      << "min_width=" << info->min_width << ", "
                      << "max_width=" << info->max_width << ", "
                      << "unordered_matches=" << (int)info->unordered_matches << ", "
                      << "matches_at_eod=" << (int)info->matches_at_eod << ", "
                      << "matches_only_at_eod=" << (int)info->matches_only_at_eod << std::endl;
            free(info); // hs_expression_info使用malloc分配内存
        }

        return true;
    }

    bool expressionExtInfo(const FuzzTestCase& testCase) override {
        hs_expr_info_t* info = nullptr;
        hs_compile_error_t* error = nullptr;

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
            testCase.pattern.c_str(),
            testCase.flags,
            &ext,
            &info,
            &error
        );

        if (err != HS_SUCCESS) {
            // 获取失败，但这是正常的fuzz测试行为
            if (error) {
                std::string message = compileErrorMessage(error);
                stats.expectedFail("hs_expression_ext_info", message);
                detailErr() << "Expression ext info failed for pattern " << testCase.id << ": "
                          << message << std::endl;
            } else {
                stats.expectedFail("hs_expression_ext_info", errString(err));
            }
            return true;
        }

        // 获取成功
        stats.ok("hs_expression_ext_info");
        if (info) {
            detailOut() << "Expression ext info succeeded for pattern " << testCase.id << ": "
                      << "min_width=" << info->min_width << ", "
                      << "max_width=" << info->max_width << ", "
                      << "unordered_matches=" << (int)info->unordered_matches << ", "
                      << "matches_at_eod=" << (int)info->matches_at_eod << ", "
                      << "matches_only_at_eod=" << (int)info->matches_only_at_eod << std::endl;
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
        hs_stream_t* stream = nullptr;
        hs_error_t err = hs_open_stream(db, 0, &stream);
        if (err != HS_SUCCESS) {
            stats.error("hs_open_stream", err);
            detailErr() << "Failed to open stream for reset: " << err << std::endl;
            return true;
        }

        // 定义匹配回调函数
        int matchCount = 0;
        auto matchCallback = [](unsigned int id, unsigned long long from, unsigned long long to, unsigned int flags, void* context) {
            int* count = static_cast<int*>(context);
            (*count)++;
            return 0; // 继续扫描
        };

        // 写入一些数据
        std::string testData = "test data for reset stream";
        err = hs_scan_stream(stream, testData.c_str(), testData.length(), 0, scratch, matchCallback, &matchCount);
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
        hs_stream_t* stream1 = nullptr;
        hs_error_t err = hs_open_stream(db, 0, &stream1);
        if (err != HS_SUCCESS) {
            stats.error("hs_open_stream", err);
            detailErr() << "Failed to open stream 1: " << err << std::endl;
            return true;
        }

        // 写入一些数据
        std::string testData = "test data for copy stream";
        int matchCount = 0;
        auto matchCallback = [](unsigned int id, unsigned long long from, unsigned long long to, unsigned int flags, void* context) {
            int* count = static_cast<int*>(context);
            (*count)++;
            return 0; // 继续扫描
        };

        err = hs_scan_stream(stream1, testData.c_str(), testData.length(), 0, scratch, matchCallback, &matchCount);
        if (err != HS_SUCCESS && err != HS_SCAN_TERMINATED) {
            stats.error("hs_scan_stream", err);
            detailErr() << "Stream scan failed: " << err << std::endl;
        }

        // 复制流
        hs_stream_t* stream2 = nullptr;
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
        hs_stream_t* stream1 = nullptr;
        hs_stream_t* stream2 = nullptr;

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
        auto matchCallback = [](unsigned int id, unsigned long long from, unsigned long long to, unsigned int flags, void* context) {
            int* count = static_cast<int*>(context);
            (*count)++;
            return 0; // 继续扫描
        };

        err = hs_scan_stream(stream1, testData.c_str(), testData.length(), 0, scratch, matchCallback, &matchCount);
        if (err != HS_SUCCESS && err != HS_SCAN_TERMINATED) {
            stats.error("hs_scan_stream", err);
            detailErr() << "Stream scan failed: " << err << std::endl;
        }

        // 重置并复制流
        err = hs_reset_and_copy_stream(stream2, stream1, scratch, matchCallback, &matchCount);
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
        hs_stream_t* stream = nullptr;
        hs_error_t err = hs_open_stream(db, 0, &stream);
        if (err != HS_SUCCESS) {
            stats.error("hs_open_stream", err);
            detailErr() << "Failed to open stream: " << err << std::endl;
            return true;
        }

        // 写入一些数据
        std::string testData = "test data for compress stream";
        int matchCount = 0;
        auto matchCallback = [](unsigned int id, unsigned long long from, unsigned long long to, unsigned int flags, void* context) {
            int* count = static_cast<int*>(context);
            (*count)++;
            return 0; // 继续扫描
        };

        err = hs_scan_stream(stream, testData.c_str(), testData.length(), 0, scratch, matchCallback, &matchCount);
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
        err = hs_compress_stream(stream, buffer.data(), buffer.size(), &usedSpace);
        if (err != HS_SUCCESS) {
            stats.error("hs_compress_stream", err);
            detailErr() << "Compress stream failed: " << err << std::endl;
        } else {
            stats.ok("hs_compress_stream");
            detailOut() << "Compress stream succeeded, used " << usedSpace << " bytes" << std::endl;
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
        hs_stream_t* stream1 = nullptr;
        hs_error_t err = hs_open_stream(db, 0, &stream1);
        if (err != HS_SUCCESS) {
            stats.error("hs_open_stream", err);
            detailErr() << "Failed to open stream 1: " << err << std::endl;
            return true;
        }

        std::string testData = "test data for expand stream";
        int matchCount = 0;
        auto matchCallback = [](unsigned int id, unsigned long long from, unsigned long long to, unsigned int flags, void* context) {
            int* count = static_cast<int*>(context);
            (*count)++;
            return 0; // 继续扫描
        };

        err = hs_scan_stream(stream1, testData.c_str(), testData.length(), 0, scratch, matchCallback, &matchCount);
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
        err = hs_compress_stream(stream1, buffer.data(), buffer.size(), &usedSpace);
        if (err != HS_SUCCESS) {
            stats.error("hs_compress_stream", err);
            detailErr() << "Compress stream failed: " << err << std::endl;
            hs_close_stream(stream1, scratch, matchCallback, &matchCount);
            return true;
        }

        // 解压流
        hs_stream_t* stream2 = nullptr;
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
        hs_stream_t* stream1 = nullptr;
        hs_stream_t* stream2 = nullptr;

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
        auto matchCallback = [](unsigned int id, unsigned long long from, unsigned long long to, unsigned int flags, void* context) {
            int* count = static_cast<int*>(context);
            (*count)++;
            return 0; // 继续扫描
        };

        err = hs_scan_stream(stream1, testData.c_str(), testData.length(), 0, scratch, matchCallback, &matchCount);
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
        err = hs_compress_stream(stream1, buffer.data(), buffer.size(), &usedSpace);
        if (err != HS_SUCCESS) {
            stats.error("hs_compress_stream", err);
            detailErr() << "Compress stream failed: " << err << std::endl;
            hs_close_stream(stream1, scratch, matchCallback, &matchCount);
            hs_close_stream(stream2, scratch, matchCallback, &matchCount);
            return true;
        }

        // 重置并解压流
        err = hs_reset_and_expand_stream(stream2, buffer.data(), usedSpace, scratch, matchCallback, &matchCount);
        if (err != HS_SUCCESS) {
            stats.error("hs_reset_and_expand_stream", err);
            detailErr() << "Reset and expand stream failed: " << err << std::endl;
        } else {
            stats.ok("hs_reset_and_expand_stream");
            detailOut() << "Reset and expand stream succeeded" << std::endl;
        }

        // 关闭流
        hs_close_stream(stream1, scratch, matchCallback, &matchCount);
        hs_close_stream(stream2, scratch, matchCallback, &matchCount);

        return true;
    }

    bool scanVector(const std::vector<std::string>& data) override {
        if (!db || !scratch) {
            stats.skipped("hs_scan_vector");
            return true;
        }

        if (data.empty()) {
            stats.skipped("hs_scan_vector");
            return true;
        }

        // 准备向量扫描参数
        std::vector<const char*> dataPtrs;
        std::vector<unsigned int> lengths;

        for (const auto& str : data) {
            dataPtrs.push_back(str.c_str());
            lengths.push_back(str.length());
        }

        // 定义匹配回调函数
        int matchCount = 0;
        auto matchCallback = [](unsigned int id, unsigned long long from, unsigned long long to, unsigned int flags, void* context) {
            int* count = static_cast<int*>(context);
            (*count)++;
            return 0; // 继续扫描
        };

        // 执行向量扫描
        hs_error_t err = hs_scan_vector(
            db,
            dataPtrs.data(),
            lengths.data(),
            data.size(),
            0,
            scratch,
            matchCallback,
            &matchCount
        );

        if (err != HS_SUCCESS && err != HS_SCAN_TERMINATED) {
            stats.error("hs_scan_vector", err);
            detailErr() << "Vector scan failed with error: " << err << std::endl;
        } else {
            stats.ok("hs_scan_vector");
            stats.matches("hs_scan_vector", matchCount);
            detailOut() << "Vector scan completed, found " << matchCount << " matches" << std::endl;
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
        hs_scratch_t* clonedScratch = nullptr;
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
            detailOut() << "Scratch size: " << scratchSize << " bytes" << std::endl;
        }

        return true;
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

    void printSummary(std::ostream& os) const override {
        stats.print(os);
    }

private:
    bool shouldExerciseFatDatabaseApis(unsigned int mode) const {
        return mode == HS_MODE_BLOCK || mode == HS_MODE_STREAM || mode == HS_MODE_VECTORED;
    }

    bool exerciseFatDatabaseApis(const char* context) {
        if (!fatDb) {
            return true;
        }

        size_t databaseSize = 0;
        hs_error_t err = fat_hs_database_size(fatDb, &databaseSize);
        if (err != HS_SUCCESS) {
            stats.error("fat_hs_database_size", err);
            detailErr() << context << ": fat_hs_database_size failed: " << err << std::endl;
        } else {
            stats.ok("fat_hs_database_size");
            detailOut() << context << ": fat database size is " << databaseSize << " bytes" << std::endl;
        }

        char* info = nullptr;
        err = fat_hs_database_info(fatDb, &info);
        if (err != HS_SUCCESS) {
            stats.error("fat_hs_database_info", err);
            detailErr() << context << ": fat_hs_database_info failed: " << err << std::endl;
        } else {
            stats.ok("fat_hs_database_info");
            detailOut() << context << ": fat database info: " << info << std::endl;
        }
        std::free(info);

        char* serialized = nullptr;
        size_t serializedLength = 0;
        err = fat_hs_serialize_database(fatDb, &serialized, &serializedLength);
        if (err != HS_SUCCESS) {
            stats.error("fat_hs_serialize_database", err);
            detailErr() << context << ": fat_hs_serialize_database failed: " << err << std::endl;
            return true;
        }
        stats.ok("fat_hs_serialize_database");
        detailOut() << context << ": serialized fat database size is "
                  << serializedLength << " bytes" << std::endl;

        size_t deserializedSize = 0;
        err = fat_hs_serialized_database_size(serialized, serializedLength, &deserializedSize);
        if (err != HS_SUCCESS) {
            stats.error("fat_hs_serialized_database_size", err);
            detailErr() << context << ": fat_hs_serialized_database_size failed: " << err << std::endl;
        } else {
            stats.ok("fat_hs_serialized_database_size");
            detailOut() << context << ": deserialized fat database size is "
                      << deserializedSize << " bytes" << std::endl;
        }

        fat_hs_database_t* deserializedDb = nullptr;
        err = fat_hs_deserialize_database(serialized, serializedLength, &deserializedDb);
        if (err != HS_SUCCESS) {
            stats.error("fat_hs_deserialize_database", err);
            detailErr() << context << ": fat_hs_deserialize_database failed: " << err << std::endl;
        } else {
            stats.ok("fat_hs_deserialize_database");
            detailOut() << context << ": fat_hs_deserialize_database succeeded" << std::endl;
            fat_hs_free_database(deserializedDb);
        }

        if (deserializedSize > 0) {
            std::vector<unsigned long long> deserializedBuffer(
                (deserializedSize + sizeof(unsigned long long) - 1) / sizeof(unsigned long long));
            fat_hs_database_t* deserializedAtDb =
                reinterpret_cast<fat_hs_database_t*>(deserializedBuffer.data());
            err = fat_hs_deserialize_database_at(serialized, serializedLength, deserializedAtDb);
            if (err != HS_SUCCESS) {
                stats.error("fat_hs_deserialize_database_at", err);
                detailErr() << context << ": fat_hs_deserialize_database_at failed: " << err << std::endl;
            } else {
                stats.ok("fat_hs_deserialize_database_at");
                detailOut() << context << ": fat_hs_deserialize_database_at succeeded" << std::endl;
            }
        }

        std::free(serialized);
        return true;
    }

    hs_database_t* db;
    fat_hs_database_t* fatDb;
    hs_compile_error_t* compileErr;
    hs_scratch_t* scratch;
    FuzzStats stats;
};

std::unique_ptr<Runner> createRunner() {
    return std::make_unique<HyperscanRunner>();
}
