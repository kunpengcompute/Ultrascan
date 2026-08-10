#include "fuzz_test.h"
#include "data/data_generator.h"
#include "../gtest/gtest.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class NullBuffer : public std::streambuf {
public:
    int overflow(int c) override {
        return c == traits_type::eof() ? traits_type::not_eof(c) : c;
    }
};

class FuzzCaseQueue {
public:
    explicit FuzzCaseQueue(size_t capacity)
        : capacity(std::max<size_t>(1, capacity)) {}

    bool push(const FuzzTestCase& testCase) {
        std::unique_lock<std::mutex> lock(mutex);
        notFull.wait(lock, [this]() {
            return closed || queue.size() < capacity;
        });

        if (closed) {
            return false;
        }

        queue.push_back(testCase);
        notEmpty.notify_one();
        return true;
    }

    bool pop(FuzzTestCase& testCase) {
        std::unique_lock<std::mutex> lock(mutex);
        notEmpty.wait(lock, [this]() {
            return closed || !queue.empty();
        });

        if (queue.empty()) {
            return false;
        }

        testCase = std::move(queue.front());
        queue.pop_front();
        notFull.notify_one();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            closed = true;
        }
        notEmpty.notify_all();
        notFull.notify_all();
    }

private:
    std::mutex mutex;
    std::condition_variable notEmpty;
    std::condition_variable notFull;
    std::deque<FuzzTestCase> queue;
    size_t capacity;
    bool closed = false;
};

bool fuzzVerbose() {
    const char* value = std::getenv("HS_FUZZ_VERBOSE");
    return value && value[0] != '\0' && value[0] != '0';
}

std::ostream& detailOut() {
    thread_local NullBuffer nullBuffer;
    thread_local std::ostream nullStream(&nullBuffer);
    return fuzzVerbose() ? std::cout : nullStream;
}

size_t readSizeEnv(const char* name, size_t defaultValue) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return defaultValue;
    }

    char* end = nullptr;
    unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) {
        return defaultValue;
    }

    return static_cast<size_t>(parsed);
}

size_t readThreadCount() {
    size_t count = readSizeEnv("HS_FUZZ_THREADS", 1);
    return count ? count : 1;
}

size_t readQueueSize() {
    size_t size = readSizeEnv("HS_FUZZ_QUEUE_SIZE", 4096);
    return size ? size : 4096;
}

size_t readMultiLimit(size_t defaultValue) {
    return readSizeEnv("HS_FUZZ_MULTI_LIMIT", defaultValue);
}

int readFuzzCount(int defaultValue) {
    const char *value = std::getenv("HS_FUZZ_COUNT");
    if (!value || value[0] == '\0' || value[0] == '-') {
        return defaultValue;
    }

    errno = 0;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' || parsed == 0 ||
        parsed > static_cast<unsigned long>(INT_MAX)) {
        return defaultValue;
    }
    return static_cast<int>(parsed);
}

size_t readFpFeedbackLimit() {
    const char *value = std::getenv("HS_FUZZ_FP_LIMIT");
    if (!value || value[0] == '\0' || value[0] == '-') {
        return 256;
    }

    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' ||
        parsed > static_cast<unsigned long long>(
                     std::numeric_limits<size_t>::max())) {
        return 256;
    }
    return static_cast<size_t>(parsed);
}

bool traceCases() {
    const char *value = std::getenv("HS_FUZZ_TRACE_CASE");
    return value && value[0] != '\0' && value[0] != '0';
}

const char *traceCaseDir() {
    const char *value = std::getenv("HS_FUZZ_TRACE_DIR");
    return value && value[0] != '\0' ? value : nullptr;
}

bool isTracePrintable(unsigned char c) {
    return c >= 0x20 && c <= 0x7e && c != '\\' && c != '"';
}

std::string escapeTraceString(const std::string &value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned char c : value) {
        if (isTracePrintable(c)) {
            out << static_cast<char>(c);
        } else if (c == '\\') {
            out << "\\\\";
        } else if (c == '"') {
            out << "\\\"";
        } else {
            out << "\\x" << std::setw(2) << static_cast<unsigned int>(c);
        }
    }
    return out.str();
}

std::string traceCaseFile(const char *dir, size_t workerId) {
    std::string path(dir);
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path += '/';
    }
    path += "worker_" + std::to_string(workerId) + ".current";
    return path;
}

void writeCurrentCase(const char *dir, size_t workerId,
                      const FuzzTestCase &testCase,
                      const std::string &stage) {
    if (!dir) {
        return;
    }

    std::ofstream out(traceCaseFile(dir, workerId).c_str(),
                      std::ios::out | std::ios::trunc);
    if (!out) {
        return;
    }

    out << "worker=" << workerId << '\n';
    out << "thread_id=" << std::this_thread::get_id() << '\n';
    out << "case_id=" << testCase.id << '\n';
    out << "stage=" << stage << '\n';
    out << "flags=" << testCase.flags << '\n';
    out << "pattern_length=" << testCase.pattern.size() << '\n';
    out << "pattern=\"" << escapeTraceString(testCase.pattern) << "\"\n";
}

void clearCurrentCase(const char *dir, size_t workerId) {
    if (dir) {
        std::remove(traceCaseFile(dir, workerId).c_str());
    }
}

void traceCaseEvent(size_t workerId, const char *phase,
                    const FuzzTestCase &testCase) {
    static std::mutex traceMutex;
    if (!traceCases()) {
        return;
    }

    std::lock_guard<std::mutex> lock(traceMutex);
    std::cout << "[case] worker=" << workerId << " " << phase
              << " id=" << testCase.id << " flags=" << testCase.flags
              << " pattern=\"" << escapeTraceString(testCase.pattern)
              << "\"" << std::endl;
}

void reportFpFeedbackFailure(size_t workerId,
                             const FuzzTestCase &testCase,
                             const std::vector<std::string> &data) {
    static std::mutex failureMutex;
    std::lock_guard<std::mutex> lock(failureMutex);
    std::cerr << "[fp-feedback-failure] worker=" << workerId
              << " id=" << testCase.id << " flags=" << testCase.flags
              << " pattern=\"" << escapeTraceString(testCase.pattern)
              << "\"" << std::endl;
    const size_t limit = std::min<size_t>(data.size(), 4);
    for (size_t i = 0; i < limit; i++) {
        std::cerr << "  data[" << i << "] length=" << data[i].size()
                  << " value=\"" << escapeTraceString(data[i]) << "\""
                  << std::endl;
    }
}

std::string stageWithMode(const char *stage, unsigned int mode) {
    std::ostringstream out;
    out << stage << "(mode=0x" << std::hex << mode << ")";
    return out.str();
}

std::string stageWithData(const char *stage, size_t index,
                          const std::string &data) {
    std::ostringstream out;
    out << stage << "(data_index=" << index << ", length=" << data.size()
        << ", data=\"" << escapeTraceString(data) << "\")";
    return out.str();
}

int HS_CDECL quietMatchCallback(unsigned int, unsigned long long,
                                unsigned long long, unsigned int,
                                void *context) {
    size_t *matchCount = static_cast<size_t *>(context);
    ++*matchCount;
    return 0;
}

} // namespace

// 测试参数
static const FuzzTestParams testParams[] = {
    {"aristocrats", 10, 10000000, false},
    {"completocrats", 10, 10000000, false},
    {"heuristocrats", 10, 10000000, false}
};

class HyperscanFuzzTest : public ::testing::TestWithParam<FuzzTestParams> {
protected:
    void SetUp() override {
        params = GetParam();
        params.count = readFuzzCount(params.count);
        fpFeedbackLimit = readFpFeedbackLimit();
        generator = createGenerator();
        runner = createRunner();
        dataGenerator = std::make_unique<DataGenerator>();

        const hs_error_t capability =
            runner->falsePositiveFeedbackCapability();
        if (capability == HS_SUCCESS) {
            fpFeedbackAvailable = true;
        } else if (capability == HS_ARCH_ERROR) {
            fpFeedbackAvailable = false;
        } else {
            fpFeedbackAvailable = false;
            FAIL() << "false-positive feedback capability probe failed: "
                   << capability;
        }

        // 配置生成器
        generator->configure(
            params.generatorType,
            params.depth,
            params.count,
            params.fullCharset
        );

        // 生成测试数据
        testData = dataGenerator->generateTestData(10, 0, 1024);
        detailOut() << "Generated " << testData.size() << " test data items" << std::endl;
    }

    void TearDown() override {
        runner->reset();
    }

    void runSingleCase(Runner& activeRunner, const FuzzTestCase& testCase,
                       const unsigned int* fatModes, size_t fatModeCount,
                       const char* traceDir, size_t workerId) {
        const FuzzProgressCallback markStage = [&](const std::string& stage) {
            writeCurrentCase(traceDir, workerId, testCase, stage);
        };
        markStage("case_start");
        detailOut() << "\n=== 测试用例 " << testCase.id << " ===" << std::endl;

        // 1. 测试hs_compile接口
        detailOut() << "测试 hs_compile..." << std::endl;
        markStage("hs_compile");
        activeRunner.compile(testCase);
        activeRunner.reset();

        // 1.1 测试fat_hs_compile接口
        for (size_t i = 0; i < fatModeCount; i++) {
            unsigned int mode = fatModes[i];
            detailOut() << "测试 fat_hs_compile, mode=" << mode << "..." << std::endl;
            markStage(stageWithMode("fat_hs_compile", mode));
            activeRunner.fatCompile(testCase, mode);
            activeRunner.reset();
        }

        // 2. 测试hs_scan接口
        detailOut() << "测试 hs_scan..." << std::endl;
        markStage("hs_scan_compile");
        activeRunner.compile(testCase);
        for (size_t i = 0; i < testData.size(); i++) {
            markStage(stageWithData("hs_scan", i, testData[i]));
            activeRunner.scan(testData[i]);
        }
        activeRunner.reset();

        // 3. 测试hs_scan_stream接口
        detailOut() << "测试 hs_scan_stream..." << std::endl;
        markStage("hs_scan_stream_compile");
        activeRunner.compile(testCase, HS_MODE_STREAM);
        for (size_t i = 0; i < testData.size(); i++) {
            markStage(stageWithData("hs_scan_stream", i, testData[i]));
            activeRunner.streamScan(testData[i]);
        }
        activeRunner.reset();

        // 4. 测试hs_compile_lit接口
        detailOut() << "测试 hs_compile_lit..." << std::endl;
        size_t length = testCase.pattern.length();
        markStage("hs_compile_lit");
        activeRunner.compileLit(testCase, length);
        activeRunner.reset();

        // 4.1 测试fat_hs_compile_lit接口
        for (size_t i = 0; i < fatModeCount; i++) {
            unsigned int mode = fatModes[i];
            detailOut() << "测试 fat_hs_compile_lit, mode=" << mode << "..." << std::endl;
            markStage(stageWithMode("fat_hs_compile_lit", mode));
            activeRunner.fatCompileLit(testCase, length, mode);
            activeRunner.reset();
        }

        // 5. 测试hs_expression_info接口
        detailOut() << "测试 hs_expression_info..." << std::endl;
        markStage("hs_expression_info");
        activeRunner.expressionInfo(testCase);
        activeRunner.reset();

        // 6. 测试hs_expression_ext_info接口
        detailOut() << "测试 hs_expression_ext_info..." << std::endl;
        markStage("hs_expression_ext_info");
        activeRunner.expressionExtInfo(testCase);
        activeRunner.reset();

        // 7. 测试hs_reset_stream接口
        detailOut() << "测试 hs_reset_stream..." << std::endl;
        markStage("hs_reset_stream_compile");
        activeRunner.compile(testCase, HS_MODE_STREAM);
        markStage("hs_reset_stream");
        activeRunner.resetStream();
        activeRunner.reset();

        // 8. 测试hs_copy_stream接口
        detailOut() << "测试 hs_copy_stream..." << std::endl;
        markStage("hs_copy_stream_compile");
        activeRunner.compile(testCase, HS_MODE_STREAM);
        markStage("hs_copy_stream");
        activeRunner.copyStream();
        activeRunner.reset();

        // 9. 测试hs_reset_and_copy_stream接口
        detailOut() << "测试 hs_reset_and_copy_stream..." << std::endl;
        markStage("hs_reset_and_copy_stream_compile");
        activeRunner.compile(testCase, HS_MODE_STREAM);
        markStage("hs_reset_and_copy_stream");
        activeRunner.resetAndCopyStream();
        activeRunner.reset();

        // 10. 测试hs_compress_stream接口
        detailOut() << "测试 hs_compress_stream..." << std::endl;
        markStage("hs_compress_stream_compile");
        activeRunner.compile(testCase, HS_MODE_STREAM);
        markStage("hs_compress_stream");
        activeRunner.compressStream();
        activeRunner.reset();

        // 11. 测试hs_expand_stream接口
        detailOut() << "测试 hs_expand_stream..." << std::endl;
        markStage("hs_expand_stream_compile");
        activeRunner.compile(testCase, HS_MODE_STREAM);
        markStage("hs_expand_stream");
        activeRunner.expandStream();
        activeRunner.reset();

        // 12. 测试hs_reset_and_expand_stream接口
        detailOut() << "测试 hs_reset_and_expand_stream..." << std::endl;
        markStage("hs_reset_and_expand_stream_compile");
        activeRunner.compile(testCase, HS_MODE_STREAM);
        markStage("hs_reset_and_expand_stream");
        activeRunner.resetAndExpandStream();
        activeRunner.reset();

        // 13. 测试hs_scan_vector接口
        detailOut() << "测试 hs_scan_vector..." << std::endl;
        markStage("hs_scan_vector_compile");
        activeRunner.compile(testCase, HS_MODE_VECTORED);
        std::vector<std::string> vectorData;
        for (size_t i = 0; i < 5; i++) {
            vectorData.push_back(testData[i % testData.size()]);
        }
        markStage("hs_scan_vector");
        activeRunner.scanVector(vectorData);
        activeRunner.reset();

        // 14. 测试hs_clone_scratch接口
        detailOut() << "测试 hs_clone_scratch..." << std::endl;
        markStage("hs_clone_scratch_compile");
        activeRunner.compile(testCase);
        markStage("hs_clone_scratch");
        activeRunner.cloneScratch();
        activeRunner.reset();

        // 15. 测试hs_scratch_size接口
        detailOut() << "测试 hs_scratch_size..." << std::endl;
        markStage("hs_scratch_size_compile");
        activeRunner.compile(testCase);
        markStage("hs_scratch_size");
        activeRunner.getScratchSize();
        activeRunner.reset();

        if (claimFpFeedbackCase()) {
            markStage("fp_feedback");
            if (!activeRunner.falsePositiveFeedback(testCase, testData,
                                                    markStage)) {
                reportFpFeedbackFailure(workerId, testCase, testData);
                fpFeedbackFailureCount.fetch_add(1,
                                                 std::memory_order_relaxed);
            }
            activeRunner.reset();
        }

        markStage("case_done");
    }

    bool claimFpFeedbackCase() {
        if (!fpFeedbackAvailable || fpFeedbackLimit == 0) {
            return false;
        }

        size_t current =
            fpFeedbackCaseCount.load(std::memory_order_relaxed);
        while (current < fpFeedbackLimit) {
            if (fpFeedbackCaseCount.compare_exchange_weak(
                    current, current + 1, std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void runSingleCasesSerial(const unsigned int* fatModes, size_t fatModeCount) {
        const char* traceDir = traceCaseDir();
        for (const auto& testCase : testCases) {
            writeCurrentCase(traceDir, 0, testCase, "queued");
            traceCaseEvent(0, "begin", testCase);
            runSingleCase(*runner, testCase, fatModes, fatModeCount,
                          traceDir, 0);
            traceCaseEvent(0, "end", testCase);
            clearCurrentCase(traceDir, 0);
        }
    }

    std::vector<std::unique_ptr<Runner>> runSingleCasesStreaming(
            size_t threadCount, size_t queueSize, size_t multiLimit,
            const unsigned int* fatModes, size_t fatModeCount,
            std::vector<FuzzTestCase>& multiCases) {
        std::vector<std::unique_ptr<Runner>> workerRunners;
        workerRunners.reserve(threadCount);
        for (size_t i = 0; i < threadCount; i++) {
            workerRunners.push_back(createRunner());
        }

        FuzzCaseQueue queue(queueSize);
        std::vector<std::thread> workers;
        workers.reserve(threadCount);

        for (size_t workerId = 0; workerId < threadCount; workerId++) {
            workers.emplace_back([&, workerId]() {
                Runner& workerRunner = *workerRunners[workerId];
                const char* traceDir = traceCaseDir();
                FuzzTestCase testCase;
                while (queue.pop(testCase)) {
                    writeCurrentCase(traceDir, workerId, testCase, "queued");
                    traceCaseEvent(workerId, "begin", testCase);
                    runSingleCase(workerRunner, testCase, fatModes,
                                  fatModeCount, traceDir, workerId);
                    traceCaseEvent(workerId, "end", testCase);
                    clearCurrentCase(traceDir, workerId);
                }
                workerRunner.reset();
            });
        }

        generatedCaseCount = generator->generateTo(
            [&](const FuzzTestCase& testCase) {
                if (multiLimit && multiCases.size() < multiLimit) {
                    multiCases.push_back(testCase);
                }
                return queue.push(testCase);
            });

        queue.close();
        for (auto& worker : workers) {
            worker.join();
        }

        return workerRunners;
    }

    const std::vector<FuzzTestCase>& selectMultiCases(
            size_t threadCount, std::vector<FuzzTestCase>& selected) const {
        if (threadCount <= 1) {
            return testCases;
        }

        size_t limit = readMultiLimit(1024);
        if (limit == 0) {
            selected.clear();
            return selected;
        }
        if (limit >= testCases.size()) {
            return testCases;
        }

        selected.assign(testCases.begin(), testCases.begin() + limit);
        return selected;
    }

    void runMultiInterfaces(const std::vector<FuzzTestCase>& multiCases,
                            const unsigned int* fatModes,
                            size_t fatModeCount) {
        if (multiCases.size() < 2) {
            return;
        }

        detailOut() << "\n=== 测试多模式接口 ===" << std::endl;

        // 16. 测试hs_compile_multi接口
        detailOut() << "测试 hs_compile_multi..." << std::endl;
        runner->compileMulti(multiCases);
        for (const auto& data : testData) {
            runner->scan(data);
        }
        runner->reset();

        // 16.1 测试fat_hs_compile_multi接口
        for (size_t i = 0; i < fatModeCount; i++) {
            unsigned int mode = fatModes[i];
            detailOut() << "测试 fat_hs_compile_multi, mode=" << mode << "..." << std::endl;
            runner->fatCompileMulti(multiCases, mode);
            runner->reset();
        }

        // 17. 测试hs_compile_ext_multi接口
        detailOut() << "测试 hs_compile_ext_multi..." << std::endl;
        runner->compileExtMulti(multiCases);
        for (const auto& data : testData) {
            runner->scan(data);
        }
        runner->reset();

        // 17.1 测试fat_hs_compile_ext_multi接口
        for (size_t i = 0; i < fatModeCount; i++) {
            unsigned int mode = fatModes[i];
            detailOut() << "测试 fat_hs_compile_ext_multi, mode=" << mode << "..." << std::endl;
            runner->fatCompileExtMulti(multiCases, mode);
            runner->reset();
        }

        // 18. 测试hs_compile_lit_multi接口
        detailOut() << "测试 hs_compile_lit_multi..." << std::endl;
        std::vector<size_t> lengths;
        for (const auto& testCase : multiCases) {
            lengths.push_back(testCase.pattern.length());
        }
        runner->compileLitMulti(multiCases, lengths);
        for (const auto& data : testData) {
            runner->scan(data);
        }
        runner->reset();

        // 18.1 测试fat_hs_compile_lit_multi接口
        for (size_t i = 0; i < fatModeCount; i++) {
            unsigned int mode = fatModes[i];
            detailOut() << "测试 fat_hs_compile_lit_multi, mode=" << mode << "..." << std::endl;
            runner->fatCompileLitMulti(multiCases, lengths, mode);
            runner->reset();
        }
    }

    void printSummary(size_t threadCount,
                      const std::vector<std::unique_ptr<Runner>>& workerRunners) {
        std::cout << "\n=== Fuzz Summary ===" << std::endl;
        std::cout << "generator: " << params.generatorType << std::endl;
        std::cout << "test rounds: " << params.count << std::endl;
        std::cout << "test cases: " << generatedCaseCount << std::endl;
        std::cout << "test data: " << testData.size() << std::endl;
        std::cout << "threads: " << threadCount << std::endl;
        runner->printSummary(std::cout);
        for (size_t i = 0; i < workerRunners.size(); i++) {
            std::cout << "worker " << i << " summary:" << std::endl;
            workerRunners[i]->printSummary(std::cout);
        }
    }

    void testAllInterfaces() {
        const unsigned int fatModes[] = {
            HS_MODE_BLOCK,
            HS_MODE_STREAM,
            HS_MODE_VECTORED,
            HS_MODE_STREAM | HS_MODE_SOM_HORIZON_LARGE,
            HS_MODE_STREAM | HS_MODE_SOM_HORIZON_MEDIUM,
            HS_MODE_STREAM | HS_MODE_SOM_HORIZON_SMALL
        };
        const size_t fatModeCount = sizeof(fatModes) / sizeof(fatModes[0]);

        size_t threadCount = readThreadCount();
        std::vector<std::unique_ptr<Runner>> workerRunners;
        std::vector<FuzzTestCase> multiCases;

        if (threadCount <= 1) {
            threadCount = 1;

            // 单线程保留原先行为，生成全部用例后顺序执行。
            testCases = generator->generate();
            generatedCaseCount = testCases.size();
            detailOut() << "Generated " << testCases.size()
                        << " test cases" << std::endl;

            if (testCases.empty()) {
                printSummary(threadCount, workerRunners);
                ADD_FAILURE() << "fuzz generator produced no test cases";
                return;
            }

            runSingleCasesSerial(fatModes, fatModeCount);
            const std::vector<FuzzTestCase>& selectedMultiCases =
                selectMultiCases(threadCount, multiCases);
            runMultiInterfaces(selectedMultiCases, fatModes, fatModeCount);
        } else {
            const size_t queueSize = readQueueSize();
            const size_t multiLimit = readMultiLimit(1024);
            std::cout << "[并行设置] HS_FUZZ_THREADS=" << threadCount
                      << ", HS_FUZZ_QUEUE_SIZE=" << queueSize
                      << ", streaming producer-consumer enabled"
                      << std::endl;
            if (multiLimit == 0) {
                std::cout << "[并行设置] multi interfaces disabled by "
                          << "HS_FUZZ_MULTI_LIMIT=0" << std::endl;
            } else {
                std::cout << "[并行设置] multi interfaces use first "
                          << multiLimit
                          << " streamed cases; override with "
                          << "HS_FUZZ_MULTI_LIMIT" << std::endl;
            }

            workerRunners = runSingleCasesStreaming(threadCount, queueSize,
                                                    multiLimit, fatModes,
                                                    fatModeCount, multiCases);

            if (generatedCaseCount == 0) {
                printSummary(threadCount, workerRunners);
                ADD_FAILURE() << "fuzz generator produced no test cases";
                return;
            }

            runMultiInterfaces(multiCases, fatModes, fatModeCount);
        }

        const size_t fpFailures =
            fpFeedbackFailureCount.load(std::memory_order_relaxed);
        if (fpFailures != 0) {
            ADD_FAILURE() << fpFailures
                          << " false-positive feedback fuzz case(s) failed";
        }

        // 测试fat_hs_compile参数校验路径（只需要测试一次）
        detailOut() << "\n=== 测试fat编译参数校验接口 ===" << std::endl;
        detailOut() << "测试 fat_hs_compile invalid args..." << std::endl;
        runner->fatCompileInvalidArgs();
        runner->reset();

        if (fpFeedbackAvailable) {
            detailOut() << "\n=== Testing false-positive feedback invalid "
                           "args ===" << std::endl;
            if (!runner->falsePositiveFeedbackInvalidArgs()) {
                ADD_FAILURE() << "false-positive feedback invalid-argument "
                                 "checks failed";
            }
            runner->reset();
        }

        // 测试平台接口（只需要测试一次）
        detailOut() << "\n=== 测试平台接口 ===" << std::endl;
        detailOut() << "测试 hs_populate_platform..." << std::endl;
        runner->populatePlatform();

        printSummary(threadCount, workerRunners);
    }

    FuzzTestParams params;
    std::unique_ptr<Generator> generator;
    std::unique_ptr<Runner> runner;
    std::unique_ptr<DataGenerator> dataGenerator;
    std::vector<FuzzTestCase> testCases;
    std::vector<std::string> testData;
    size_t generatedCaseCount = 0;
    bool fpFeedbackAvailable = false;
    size_t fpFeedbackLimit = 256;
    std::atomic<size_t> fpFeedbackCaseCount{0};
    std::atomic<size_t> fpFeedbackFailureCount{0};
};

// 测试所有接口
TEST_P(HyperscanFuzzTest, AllInterfaces) {
    testAllInterfaces();
}

TEST(QuietOnlyFuzzRegression, BlockStreamVectoredNoCallbacks) {
    const char *expression = "Q";
    const unsigned int flags = HS_FLAG_QUIET | HS_FLAG_PREFILTER |
                               HS_FLAG_ALLOWEMPTY | HS_FLAG_MULTILINE;
    const unsigned int id = 2001;
    const unsigned int modes[] = {
        HS_MODE_BLOCK, HS_MODE_STREAM, HS_MODE_VECTORED
    };
    const char data[] = "QQQQQQQQQQ";
    const unsigned int dataLength = sizeof(data) - 1;

    for (unsigned int mode : modes) {
        SCOPED_TRACE(::testing::Message() << "mode=" << mode);

        hs_database_t *database = nullptr;
        hs_compile_error_t *compileError = nullptr;
        hs_error_t err = hs_compile_multi(&expression, &flags, &id, 1, mode,
                                          nullptr, &database, &compileError);
        if (err != HS_SUCCESS) {
            const std::string message = compileError && compileError->message
                                            ? compileError->message
                                            : "no compile error message";
            if (compileError) {
                hs_free_compile_error(compileError);
            }
            FAIL() << "quiet-only compile failed: " << message
                   << " (error=" << err << ")";
        }
        if (compileError) {
            hs_free_compile_error(compileError);
        }
        ASSERT_NE(nullptr, database);

        hs_scratch_t *scratch = nullptr;
        err = hs_alloc_scratch(database, &scratch);
        ASSERT_EQ(HS_SUCCESS, err);
        ASSERT_NE(nullptr, scratch);

        size_t matchCount = 0;
        if (mode == HS_MODE_BLOCK) {
            err = hs_scan(database, data, dataLength, 0, scratch,
                          quietMatchCallback, &matchCount);
            EXPECT_EQ(HS_SUCCESS, err);
        } else if (mode == HS_MODE_STREAM) {
            hs_stream_t *stream = nullptr;
            err = hs_open_stream(database, 0, &stream);
            ASSERT_EQ(HS_SUCCESS, err);
            ASSERT_NE(nullptr, stream);
            err = hs_scan_stream(stream, data, dataLength, 0, scratch,
                                 quietMatchCallback, &matchCount);
            EXPECT_EQ(HS_SUCCESS, err);
            err = hs_close_stream(stream, scratch, quietMatchCallback,
                                  &matchCount);
            EXPECT_EQ(HS_SUCCESS, err);
        } else {
            const char *vectors[] = {data, data + 5};
            const unsigned int lengths[] = {5, dataLength - 5};
            err = hs_scan_vector(database, vectors, lengths, 2, 0, scratch,
                                 quietMatchCallback, &matchCount);
            EXPECT_EQ(HS_SUCCESS, err);
        }

        EXPECT_EQ(0U, matchCount);
        EXPECT_EQ(HS_SUCCESS, hs_free_scratch(scratch));
        EXPECT_EQ(HS_SUCCESS, hs_free_database(database));
    }
}

// 实例化测试
INSTANTIATE_TEST_CASE_P(
    FuzzTests,
    HyperscanFuzzTest,
    ::testing::ValuesIn(testParams)
);
