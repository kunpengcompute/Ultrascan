#include "fuzz_test.h"
#include "data/data_generator.h"
#include "../gtest/gtest.h"
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <streambuf>
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
        generator = createGenerator();
        runner = createRunner();
        dataGenerator = std::make_unique<DataGenerator>();

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
                       const unsigned int* fatModes, size_t fatModeCount) {
        detailOut() << "\n=== 测试用例 " << testCase.id << " ===" << std::endl;

        // 1. 测试hs_compile接口
        detailOut() << "测试 hs_compile..." << std::endl;
        activeRunner.compile(testCase);
        activeRunner.reset();

        // 1.1 测试fat_hs_compile接口
        for (size_t i = 0; i < fatModeCount; i++) {
            unsigned int mode = fatModes[i];
            detailOut() << "测试 fat_hs_compile, mode=" << mode << "..." << std::endl;
            activeRunner.fatCompile(testCase, mode);
            activeRunner.reset();
        }

        // 2. 测试hs_scan接口
        detailOut() << "测试 hs_scan..." << std::endl;
        activeRunner.compile(testCase);
        for (const auto& data : testData) {
            activeRunner.scan(data);
        }
        activeRunner.reset();

        // 3. 测试hs_scan_stream接口
        detailOut() << "测试 hs_scan_stream..." << std::endl;
        activeRunner.compile(testCase, HS_MODE_STREAM);
        for (const auto& data : testData) {
            activeRunner.streamScan(data);
        }
        activeRunner.reset();

        // 4. 测试hs_compile_lit接口
        detailOut() << "测试 hs_compile_lit..." << std::endl;
        size_t length = testCase.pattern.length();
        activeRunner.compileLit(testCase, length);
        activeRunner.reset();

        // 4.1 测试fat_hs_compile_lit接口
        for (size_t i = 0; i < fatModeCount; i++) {
            unsigned int mode = fatModes[i];
            detailOut() << "测试 fat_hs_compile_lit, mode=" << mode << "..." << std::endl;
            activeRunner.fatCompileLit(testCase, length, mode);
            activeRunner.reset();
        }

        // 5. 测试hs_expression_info接口
        detailOut() << "测试 hs_expression_info..." << std::endl;
        activeRunner.expressionInfo(testCase);
        activeRunner.reset();

        // 6. 测试hs_expression_ext_info接口
        detailOut() << "测试 hs_expression_ext_info..." << std::endl;
        activeRunner.expressionExtInfo(testCase);
        activeRunner.reset();

        // 7. 测试hs_reset_stream接口
        detailOut() << "测试 hs_reset_stream..." << std::endl;
        activeRunner.compile(testCase, HS_MODE_STREAM);
        activeRunner.resetStream();
        activeRunner.reset();

        // 8. 测试hs_copy_stream接口
        detailOut() << "测试 hs_copy_stream..." << std::endl;
        activeRunner.compile(testCase, HS_MODE_STREAM);
        activeRunner.copyStream();
        activeRunner.reset();

        // 9. 测试hs_reset_and_copy_stream接口
        detailOut() << "测试 hs_reset_and_copy_stream..." << std::endl;
        activeRunner.compile(testCase, HS_MODE_STREAM);
        activeRunner.resetAndCopyStream();
        activeRunner.reset();

        // 10. 测试hs_compress_stream接口
        detailOut() << "测试 hs_compress_stream..." << std::endl;
        activeRunner.compile(testCase, HS_MODE_STREAM);
        activeRunner.compressStream();
        activeRunner.reset();

        // 11. 测试hs_expand_stream接口
        detailOut() << "测试 hs_expand_stream..." << std::endl;
        activeRunner.compile(testCase, HS_MODE_STREAM);
        activeRunner.expandStream();
        activeRunner.reset();

        // 12. 测试hs_reset_and_expand_stream接口
        detailOut() << "测试 hs_reset_and_expand_stream..." << std::endl;
        activeRunner.compile(testCase, HS_MODE_STREAM);
        activeRunner.resetAndExpandStream();
        activeRunner.reset();

        // 13. 测试hs_scan_vector接口
        detailOut() << "测试 hs_scan_vector..." << std::endl;
        activeRunner.compile(testCase);
        std::vector<std::string> vectorData;
        for (size_t i = 0; i < 5; i++) {
            vectorData.push_back(testData[i % testData.size()]);
        }
        activeRunner.scanVector(vectorData);
        activeRunner.reset();

        // 14. 测试hs_clone_scratch接口
        detailOut() << "测试 hs_clone_scratch..." << std::endl;
        activeRunner.compile(testCase);
        activeRunner.cloneScratch();
        activeRunner.reset();

        // 15. 测试hs_scratch_size接口
        detailOut() << "测试 hs_scratch_size..." << std::endl;
        activeRunner.compile(testCase);
        activeRunner.getScratchSize();
        activeRunner.reset();
    }

    void runSingleCasesSerial(const unsigned int* fatModes, size_t fatModeCount) {
        for (const auto& testCase : testCases) {
            runSingleCase(*runner, testCase, fatModes, fatModeCount);
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
                FuzzTestCase testCase;
                while (queue.pop(testCase)) {
                    runSingleCase(workerRunner, testCase, fatModes,
                                  fatModeCount);
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
                return;
            }

            runMultiInterfaces(multiCases, fatModes, fatModeCount);
        }

        // 测试fat_hs_compile参数校验路径（只需要测试一次）
        detailOut() << "\n=== 测试fat编译参数校验接口 ===" << std::endl;
        detailOut() << "测试 fat_hs_compile invalid args..." << std::endl;
        runner->fatCompileInvalidArgs();
        runner->reset();

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
};

// 测试所有接口
TEST_P(HyperscanFuzzTest, AllInterfaces) {
    testAllInterfaces();
}

// 实例化测试
INSTANTIATE_TEST_CASE_P(
    FuzzTests,
    HyperscanFuzzTest,
    ::testing::ValuesIn(testParams)
);
