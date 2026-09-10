#include "fuzz_test.h"
#include "../gtest/gtest.h"
#include "data/data_generator.h"
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

    bool push(const FuzzTestCase &testCase) {
        std::unique_lock<std::mutex> lock(mutex);
        notFull.wait(lock,
                     [this]() { return closed || queue.size() < capacity; });

        if (closed) {
            return false;
        }

        queue.push_back(testCase);
        notEmpty.notify_one();
        return true;
    }

    bool pop(FuzzTestCase &testCase) {
        std::unique_lock<std::mutex> lock(mutex);
        notEmpty.wait(lock, [this]() { return closed || !queue.empty(); });

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
    const char *value = std::getenv("HS_FUZZ_VERBOSE");
    return value && value[0] != '\0' && value[0] != '0';
}

std::ostream &detailOut() {
    thread_local NullBuffer nullBuffer;
    thread_local std::ostream nullStream(&nullBuffer);
    return fuzzVerbose() ? std::cout : nullStream;
}

size_t readSizeEnv(const char *name, size_t defaultValue) {
    const char *value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return defaultValue;
    }

    char *end = nullptr;
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

void writeCurrentCase(const char *dir, size_t workerId, const char *laneName,
                      const FuzzTestCase &testCase, const std::string &stage) {
    if (!dir) {
        return;
    }

    std::ofstream out(traceCaseFile(dir, workerId).c_str(),
                      std::ios::out | std::ios::trunc);
    if (!out) {
        return;
    }

    out << "worker=" << workerId << '\n';
    if (laneName) {
        out << "lane=" << laneName << '\n';
    }
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

void traceCaseEvent(size_t workerId, const char *laneName, const char *phase,
                    const FuzzTestCase &testCase) {
    static std::mutex traceMutex;
    if (!traceCases()) {
        return;
    }

    std::lock_guard<std::mutex> lock(traceMutex);
    std::cout << "[case] worker=" << workerId;
    if (laneName) {
        std::cout << " lane=" << laneName;
    }
    std::cout << " " << phase << " id=" << testCase.id
              << " flags=" << testCase.flags << " pattern=\""
              << escapeTraceString(testCase.pattern) << "\"" << std::endl;
}

void reportFpFeedbackFailure(size_t workerId, const FuzzTestCase &testCase,
                             const std::vector<std::string> &data) {
    static std::mutex failureMutex;
    std::lock_guard<std::mutex> lock(failureMutex);
    std::cerr << "[fp-feedback-failure] worker=" << workerId
              << " id=" << testCase.id << " flags=" << testCase.flags
              << " pattern=\"" << escapeTraceString(testCase.pattern) << "\""
              << std::endl;
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

// fat 编译模式全集
const unsigned int kFatModes[] = {HS_MODE_BLOCK,
                                  HS_MODE_STREAM,
                                  HS_MODE_VECTORED,
                                  HS_MODE_STREAM | HS_MODE_SOM_HORIZON_LARGE,
                                  HS_MODE_STREAM | HS_MODE_SOM_HORIZON_MEDIUM,
                                  HS_MODE_STREAM | HS_MODE_SOM_HORIZON_SMALL};
const size_t kFatModeCount = sizeof(kFatModes) / sizeof(kFatModes[0]);

// 单用例内的 API 测试阶段。串行模式按枚举顺序全部执行；
// 并行 lane 模式下按阶段分组，交给不同 lane 的线程并行执行。
enum FuzzStageId {
    STAGE_HS_COMPILE = 0,
    STAGE_FAT_HS_COMPILE,
    STAGE_HS_SCAN,
    STAGE_HS_SCAN_STREAM,
    STAGE_HS_COMPILE_LIT,
    STAGE_FAT_HS_COMPILE_LIT,
    STAGE_HS_EXPRESSION_INFO,
    STAGE_HS_EXPRESSION_EXT_INFO,
    STAGE_HS_RESET_STREAM,
    STAGE_HS_COPY_STREAM,
    STAGE_HS_RESET_AND_COPY_STREAM,
    STAGE_HS_COMPRESS_STREAM,
    STAGE_HS_EXPAND_STREAM,
    STAGE_HS_RESET_AND_EXPAND_STREAM,
    STAGE_HS_SCAN_VECTOR,
    STAGE_HS_CLONE_SCRATCH,
    STAGE_HS_SCRATCH_SIZE,
    STAGE_FP_FEEDBACK,
    STAGE_COUNT
};

size_t readApiSampleRate() {
    size_t rate = readSizeEnv("HS_FUZZ_API_SAMPLE", 10);
    return rate ? rate : 1;
}

size_t readFeedbackThreads(size_t defaultValue) {
    return readSizeEnv("HS_FUZZ_FEEDBACK_THREADS", defaultValue);
}

// 最大余数法：把 threads 个线程按 weights 权重分配给各 lane，
// 线程不足时权重小的 lane 可能分到 0（表示跳过）。
std::vector<size_t> distributeThreads(const std::vector<size_t> &weights,
                                      size_t threads) {
    std::vector<size_t> alloc(weights.size(), 0);
    size_t totalWeight = 0;
    for (size_t w : weights) {
        totalWeight += w;
    }
    if (totalWeight == 0 || threads == 0) {
        return alloc;
    }

    std::vector<double> remainders(weights.size(), 0.0);
    size_t assigned = 0;
    for (size_t i = 0; i < weights.size(); i++) {
        const double quota =
            static_cast<double>(threads) * weights[i] / totalWeight;
        alloc[i] = static_cast<size_t>(quota);
        remainders[i] = quota - static_cast<double>(alloc[i]);
        assigned += alloc[i];
    }
    while (assigned < threads) {
        size_t best = 0;
        for (size_t i = 1; i < remainders.size(); i++) {
            if (remainders[i] > remainders[best]) {
                best = i;
            }
        }
        alloc[best]++;
        remainders[best] -= 1.0;
        assigned++;
    }
    return alloc;
}

} // namespace

// 测试参数
static const FuzzTestParams testParams[] = {
    {"aristocrats", 10, 10000000, false},
    {"completocrats", 10, 10000000, false},
    {"heuristocrats", 10, 10000000, false}};

class HyperscanFuzzTest : public ::testing::TestWithParam<FuzzTestParams> {
protected:
    void SetUp() override {
        params = GetParam();
        params.count = readFuzzCount(params.count);
        fpFeedbackLimit = readFpFeedbackLimit();
        generator = createGenerator();
        runner = createRunner();
        dataGenerator = std::make_unique<DataGenerator>();

        const hs_error_t capability = runner->falsePositiveFeedbackCapability();
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
        generator->configure(params.generatorType, params.depth, params.count,
                             params.fullCharset);

        // 生成测试数据
        testData = dataGenerator->generateTestData(10, 0, 1024);
        detailOut() << "Generated " << testData.size() << " test data items"
                    << std::endl;
    }

    void TearDown() override { runner->reset(); }

    // 执行单个 API 测试阶段。阶段之间自包含（各自 compile/reset），
    // 因此既可以按原顺序串行执行，也可以拆给不同 lane 并行执行。
    void runStage(FuzzStageId stage, Runner &activeRunner,
                  const FuzzTestCase &testCase, const char *traceDir,
                  size_t workerId, const char *laneName) {
        const FuzzProgressCallback markStage =
            [&](const std::string &stageName) {
                writeCurrentCase(traceDir, workerId, laneName, testCase,
                                 stageName);
            };

        switch (stage) {
        case STAGE_HS_COMPILE:
            // 1. 测试hs_compile接口
            detailOut() << "测试 hs_compile..." << std::endl;
            markStage("hs_compile");
            activeRunner.compile(testCase);
            activeRunner.reset();
            break;

        case STAGE_FAT_HS_COMPILE:
            // 1.1 测试fat_hs_compile接口
            for (size_t i = 0; i < kFatModeCount; i++) {
                unsigned int mode = kFatModes[i];
                detailOut() << "测试 fat_hs_compile, mode=" << mode << "..."
                            << std::endl;
                markStage(stageWithMode("fat_hs_compile", mode));
                activeRunner.fatCompile(testCase, mode);
                activeRunner.reset();
            }
            break;

        case STAGE_HS_SCAN:
            // 2. 测试hs_scan接口
            detailOut() << "测试 hs_scan..." << std::endl;
            markStage("hs_scan_compile");
            activeRunner.compile(testCase);
            for (size_t i = 0; i < testData.size(); i++) {
                markStage(stageWithData("hs_scan", i, testData[i]));
                activeRunner.scan(testData[i]);
            }
            activeRunner.reset();
            break;

        case STAGE_HS_SCAN_STREAM:
            // 3. 测试hs_scan_stream接口
            detailOut() << "测试 hs_scan_stream..." << std::endl;
            markStage("hs_scan_stream_compile");
            activeRunner.compile(testCase, HS_MODE_STREAM);
            for (size_t i = 0; i < testData.size(); i++) {
                markStage(stageWithData("hs_scan_stream", i, testData[i]));
                activeRunner.streamScan(testData[i]);
            }
            activeRunner.reset();
            break;

        case STAGE_HS_COMPILE_LIT:
            // 4. 测试hs_compile_lit接口
            detailOut() << "测试 hs_compile_lit..." << std::endl;
            markStage("hs_compile_lit");
            activeRunner.compileLit(testCase, testCase.pattern.length());
            activeRunner.reset();
            break;

        case STAGE_FAT_HS_COMPILE_LIT: {
            // 4.1 测试fat_hs_compile_lit接口
            const size_t length = testCase.pattern.length();
            for (size_t i = 0; i < kFatModeCount; i++) {
                unsigned int mode = kFatModes[i];
                detailOut() << "测试 fat_hs_compile_lit, mode=" << mode << "..."
                            << std::endl;
                markStage(stageWithMode("fat_hs_compile_lit", mode));
                activeRunner.fatCompileLit(testCase, length, mode);
                activeRunner.reset();
            }
            break;
        }

        case STAGE_HS_EXPRESSION_INFO:
            // 5. 测试hs_expression_info接口
            detailOut() << "测试 hs_expression_info..." << std::endl;
            markStage("hs_expression_info");
            activeRunner.expressionInfo(testCase);
            activeRunner.reset();
            break;

        case STAGE_HS_EXPRESSION_EXT_INFO:
            // 6. 测试hs_expression_ext_info接口
            detailOut() << "测试 hs_expression_ext_info..." << std::endl;
            markStage("hs_expression_ext_info");
            activeRunner.expressionExtInfo(testCase);
            activeRunner.reset();
            break;

        case STAGE_HS_RESET_STREAM:
            // 7. 测试hs_reset_stream接口
            detailOut() << "测试 hs_reset_stream..." << std::endl;
            markStage("hs_reset_stream_compile");
            activeRunner.compile(testCase, HS_MODE_STREAM);
            markStage("hs_reset_stream");
            activeRunner.resetStream();
            activeRunner.reset();
            break;

        case STAGE_HS_COPY_STREAM:
            // 8. 测试hs_copy_stream接口
            detailOut() << "测试 hs_copy_stream..." << std::endl;
            markStage("hs_copy_stream_compile");
            activeRunner.compile(testCase, HS_MODE_STREAM);
            markStage("hs_copy_stream");
            activeRunner.copyStream();
            activeRunner.reset();
            break;

        case STAGE_HS_RESET_AND_COPY_STREAM:
            // 9. 测试hs_reset_and_copy_stream接口
            detailOut() << "测试 hs_reset_and_copy_stream..." << std::endl;
            markStage("hs_reset_and_copy_stream_compile");
            activeRunner.compile(testCase, HS_MODE_STREAM);
            markStage("hs_reset_and_copy_stream");
            activeRunner.resetAndCopyStream();
            activeRunner.reset();
            break;

        case STAGE_HS_COMPRESS_STREAM:
            // 10. 测试hs_compress_stream接口
            detailOut() << "测试 hs_compress_stream..." << std::endl;
            markStage("hs_compress_stream_compile");
            activeRunner.compile(testCase, HS_MODE_STREAM);
            markStage("hs_compress_stream");
            activeRunner.compressStream();
            activeRunner.reset();
            break;

        case STAGE_HS_EXPAND_STREAM:
            // 11. 测试hs_expand_stream接口
            detailOut() << "测试 hs_expand_stream..." << std::endl;
            markStage("hs_expand_stream_compile");
            activeRunner.compile(testCase, HS_MODE_STREAM);
            markStage("hs_expand_stream");
            activeRunner.expandStream();
            activeRunner.reset();
            break;

        case STAGE_HS_RESET_AND_EXPAND_STREAM:
            // 12. 测试hs_reset_and_expand_stream接口
            detailOut() << "测试 hs_reset_and_expand_stream..." << std::endl;
            markStage("hs_reset_and_expand_stream_compile");
            activeRunner.compile(testCase, HS_MODE_STREAM);
            markStage("hs_reset_and_expand_stream");
            activeRunner.resetAndExpandStream();
            activeRunner.reset();
            break;

        case STAGE_HS_SCAN_VECTOR: {
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
            break;
        }

        case STAGE_HS_CLONE_SCRATCH:
            // 14. 测试hs_clone_scratch接口
            detailOut() << "测试 hs_clone_scratch..." << std::endl;
            markStage("hs_clone_scratch_compile");
            activeRunner.compile(testCase);
            markStage("hs_clone_scratch");
            activeRunner.cloneScratch();
            activeRunner.reset();
            break;

        case STAGE_HS_SCRATCH_SIZE:
            // 15. 测试hs_scratch_size接口
            detailOut() << "测试 hs_scratch_size..." << std::endl;
            markStage("hs_scratch_size_compile");
            activeRunner.compile(testCase);
            markStage("hs_scratch_size");
            activeRunner.getScratchSize();
            activeRunner.reset();
            break;

        case STAGE_FP_FEEDBACK:
            // 16. 测试假阳性反馈相关接口（受 HS_FUZZ_FP_LIMIT 总量限制）
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
            break;

        case STAGE_COUNT:
            break;
        }
    }

    // 在一条 lane 上按给定阶段集合执行一个用例（lane 内阶段串行）
    void runLaneCase(Runner &activeRunner,
                     const std::vector<FuzzStageId> &stages,
                     const FuzzTestCase &testCase, const char *traceDir,
                     size_t workerId, const char *laneName) {
        const FuzzProgressCallback markStage =
            [&](const std::string &stageName) {
                writeCurrentCase(traceDir, workerId, laneName, testCase,
                                 stageName);
            };
        markStage("case_start");
        detailOut() << "\n=== 测试用例 " << testCase.id << " (lane " << laneName
                    << ") ===" << std::endl;
        for (FuzzStageId stage : stages) {
            runStage(stage, activeRunner, testCase, traceDir, workerId,
                     laneName);
        }
        markStage("case_done");
    }

    // 串行模式：一个用例按原有顺序跑全部阶段
    void runSingleCase(Runner &activeRunner, const FuzzTestCase &testCase,
                       const char *traceDir, size_t workerId) {
        std::vector<FuzzStageId> allStages;
        for (size_t s = 0; s < STAGE_COUNT; s++) {
            allStages.push_back(static_cast<FuzzStageId>(s));
        }
        runLaneCase(activeRunner, allStages, testCase, traceDir, workerId,
                    "serial");
    }

    bool claimFpFeedbackCase() {
        if (!fpFeedbackAvailable || fpFeedbackLimit == 0) {
            return false;
        }

        size_t current = fpFeedbackCaseCount.load(std::memory_order_relaxed);
        while (current < fpFeedbackLimit) {
            if (fpFeedbackCaseCount.compare_exchange_weak(
                    current, current + 1, std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void runSingleCasesSerial() {
        const char *traceDir = traceCaseDir();
        for (const auto &testCase : testCases) {
            writeCurrentCase(traceDir, 0, "serial", testCase, "queued");
            traceCaseEvent(0, "serial", "begin", testCase);
            runSingleCase(*runner, testCase, traceDir, 0);
            traceCaseEvent(0, "serial", "end", testCase);
            clearCurrentCase(traceDir, 0);
        }
    }

    // lane 模式：不同 API 组分配到不同 lane 并行执行。
    // feedback lane 全量采样（受 HS_FUZZ_FP_LIMIT 限制总量），
    // 其余 lane 按 HS_FUZZ_API_SAMPLE 降采样。
    std::vector<std::unique_ptr<Runner>>
    runSingleCasesLaned(size_t threadCount, size_t queueSize, size_t multiLimit,
                        std::vector<FuzzTestCase> &multiCases,
                        std::vector<std::string> &workerLabels) {
        struct LaneDef {
            const char *name;
            std::vector<FuzzStageId> stages;
            size_t weight;
            size_t sampleRate;
        };

        const bool feedbackActive = fpFeedbackAvailable && fpFeedbackLimit > 0;
        const size_t apiSample = readApiSampleRate();

        std::vector<LaneDef> defs;
        if (feedbackActive) {
            // feedback lane 的线程数不走权重分配，由 HS_FUZZ_FEEDBACK_THREADS
            // 决定
            defs.push_back(LaneDef{"feedback", {STAGE_FP_FEEDBACK}, 0, 1});
        }
        defs.push_back(
            LaneDef{"compile",
                    {STAGE_HS_COMPILE, STAGE_FAT_HS_COMPILE,
                     STAGE_HS_COMPILE_LIT, STAGE_FAT_HS_COMPILE_LIT,
                     STAGE_HS_EXPRESSION_INFO, STAGE_HS_EXPRESSION_EXT_INFO},
                    5,
                    apiSample});
        defs.push_back(
            LaneDef{"scan",
                    {STAGE_HS_SCAN, STAGE_HS_SCAN_STREAM, STAGE_HS_SCAN_VECTOR},
                    3,
                    apiSample});
        defs.push_back(
            LaneDef{"stream",
                    {STAGE_HS_RESET_STREAM, STAGE_HS_COPY_STREAM,
                     STAGE_HS_RESET_AND_COPY_STREAM, STAGE_HS_COMPRESS_STREAM,
                     STAGE_HS_EXPAND_STREAM, STAGE_HS_RESET_AND_EXPAND_STREAM,
                     STAGE_HS_CLONE_SCRATCH, STAGE_HS_SCRATCH_SIZE},
                    2,
                    apiSample});

        // 线程分配：feedback lane 默认占一半（HS_FUZZ_FEEDBACK_THREADS 覆盖），
        // 剩余线程按 5:3:2 分给 compile/scan/stream。
        const size_t otherStart = feedbackActive ? 1 : 0;
        std::vector<size_t> laneThreads(defs.size(), 0);
        if (feedbackActive) {
            const size_t defaultFeedback =
                threadCount / 2 ? threadCount / 2 : 1;
            size_t feedbackThreads = readFeedbackThreads(defaultFeedback);
            if (feedbackThreads == 0) {
                feedbackThreads = 1;
            }
            if (feedbackThreads > threadCount) {
                feedbackThreads = threadCount;
            }
            laneThreads[0] = feedbackThreads;
        }
        std::vector<size_t> otherWeights;
        for (size_t i = otherStart; i < defs.size(); i++) {
            otherWeights.push_back(defs[i].weight);
        }
        const std::vector<size_t> otherAlloc =
            distributeThreads(otherWeights, threadCount - laneThreads[0]);
        for (size_t i = 0; i < otherAlloc.size(); i++) {
            laneThreads[otherStart + i] = otherAlloc[i];
        }

        struct LaneRuntime {
            const char *name;
            std::vector<FuzzStageId> stages;
            size_t threads;
            size_t sampleRate;
            std::unique_ptr<FuzzCaseQueue> queue;
        };
        std::vector<LaneRuntime> lanes;
        for (size_t i = 0; i < defs.size(); i++) {
            if (laneThreads[i] == 0) {
                std::cout << "[lane 设置] lane " << defs[i].name
                          << ": 0 线程，跳过该 lane 覆盖的接口" << std::endl;
                continue;
            }
            LaneRuntime lane;
            lane.name = defs[i].name;
            lane.stages = defs[i].stages;
            lane.threads = laneThreads[i];
            lane.sampleRate = defs[i].sampleRate;
            lane.queue = std::make_unique<FuzzCaseQueue>(queueSize);
            lanes.push_back(std::move(lane));
            std::cout << "[lane 设置] lane " << defs[i].name
                      << ": threads=" << laneThreads[i] << ", sample=1/"
                      << defs[i].sampleRate
                      << ", stages=" << defs[i].stages.size() << std::endl;
        }

        std::vector<std::unique_ptr<Runner>> workerRunners;
        workerRunners.reserve(threadCount);
        workerLabels.clear();
        std::vector<std::thread> workers;
        workers.reserve(threadCount);

        for (size_t laneIdx = 0; laneIdx < lanes.size(); laneIdx++) {
            for (size_t w = 0; w < lanes[laneIdx].threads; w++) {
                const size_t workerId = workerRunners.size();
                workerRunners.push_back(createRunner());
                workerLabels.push_back(std::string(lanes[laneIdx].name) + "/" +
                                       std::to_string(w));
                workers.emplace_back(
                    [this, &lanes, &workerRunners, laneIdx, workerId]() {
                        Runner &workerRunner = *workerRunners[workerId];
                        const char *traceDir = traceCaseDir();
                        FuzzTestCase testCase;
                        while (lanes[laneIdx].queue->pop(testCase)) {
                            writeCurrentCase(traceDir, workerId,
                                             lanes[laneIdx].name, testCase,
                                             "queued");
                            traceCaseEvent(workerId, lanes[laneIdx].name,
                                           "begin", testCase);
                            runLaneCase(workerRunner, lanes[laneIdx].stages,
                                        testCase, traceDir, workerId,
                                        lanes[laneIdx].name);
                            traceCaseEvent(workerId, lanes[laneIdx].name, "end",
                                           testCase);
                            clearCurrentCase(traceDir, workerId);
                        }
                        workerRunner.reset();
                    });
            }
        }

        // 生产者：按采样率把用例路由到各 lane 的独立队列。
        // 队列满时阻塞形成背压，慢 lane 会拖住生产者，但不会死锁。
        size_t seq = 0;
        generatedCaseCount =
            generator->generateTo([&](const FuzzTestCase &testCase) {
                if (multiLimit && multiCases.size() < multiLimit) {
                    multiCases.push_back(testCase);
                }
                for (LaneRuntime &lane : lanes) {
                    if (lane.sampleRate > 1 && (seq % lane.sampleRate) != 0) {
                        continue;
                    }
                    lane.queue->push(testCase);
                }
                seq++;
                return true;
            });

        for (LaneRuntime &lane : lanes) {
            lane.queue->close();
        }
        for (auto &worker : workers) {
            worker.join();
        }

        return workerRunners;
    }

    const std::vector<FuzzTestCase> &
    selectMultiCases(size_t threadCount,
                     std::vector<FuzzTestCase> &selected) const {
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

    void runMultiInterfaces(const std::vector<FuzzTestCase> &multiCases) {
        if (multiCases.size() < 2) {
            return;
        }

        detailOut() << "\n=== 测试多模式接口 ===" << std::endl;

        // 16. 测试hs_compile_multi接口
        detailOut() << "测试 hs_compile_multi..." << std::endl;
        runner->compileMulti(multiCases);
        for (const auto &data : testData) {
            runner->scan(data);
        }
        runner->reset();

        // 16.1 测试fat_hs_compile_multi接口
        for (size_t i = 0; i < kFatModeCount; i++) {
            unsigned int mode = kFatModes[i];
            detailOut() << "测试 fat_hs_compile_multi, mode=" << mode << "..."
                        << std::endl;
            runner->fatCompileMulti(multiCases, mode);
            runner->reset();
        }

        // 17. 测试hs_compile_ext_multi接口
        detailOut() << "测试 hs_compile_ext_multi..." << std::endl;
        runner->compileExtMulti(multiCases);
        for (const auto &data : testData) {
            runner->scan(data);
        }
        runner->reset();

        // 17.1 测试fat_hs_compile_ext_multi接口
        for (size_t i = 0; i < kFatModeCount; i++) {
            unsigned int mode = kFatModes[i];
            detailOut() << "测试 fat_hs_compile_ext_multi, mode=" << mode
                        << "..." << std::endl;
            runner->fatCompileExtMulti(multiCases, mode);
            runner->reset();
        }

        // 18. 测试hs_compile_lit_multi接口
        detailOut() << "测试 hs_compile_lit_multi..." << std::endl;
        std::vector<size_t> lengths;
        for (const auto &testCase : multiCases) {
            lengths.push_back(testCase.pattern.length());
        }
        runner->compileLitMulti(multiCases, lengths);
        for (const auto &data : testData) {
            runner->scan(data);
        }
        runner->reset();

        // 18.1 测试fat_hs_compile_lit_multi接口
        for (size_t i = 0; i < kFatModeCount; i++) {
            unsigned int mode = kFatModes[i];
            detailOut() << "测试 fat_hs_compile_lit_multi, mode=" << mode
                        << "..." << std::endl;
            runner->fatCompileLitMulti(multiCases, lengths, mode);
            runner->reset();
        }
    }

    void printSummary(size_t threadCount,
                      const std::vector<std::unique_ptr<Runner>> &workerRunners,
                      const std::vector<std::string> &workerLabels) {
        std::cout << "\n=== Fuzz Summary ===" << std::endl;
        std::cout << "generator: " << params.generatorType << std::endl;
        std::cout << "test rounds: " << params.count << std::endl;
        std::cout << "test cases: " << generatedCaseCount << std::endl;
        std::cout << "test data: " << testData.size() << std::endl;
        std::cout << "threads: " << threadCount << std::endl;
        runner->printSummary(std::cout);
        for (size_t i = 0; i < workerRunners.size(); i++) {
            if (i < workerLabels.size()) {
                std::cout << "worker " << i << " (lane " << workerLabels[i]
                          << ") summary:" << std::endl;
            } else {
                std::cout << "worker " << i << " summary:" << std::endl;
            }
            workerRunners[i]->printSummary(std::cout);
        }
    }

    void testAllInterfaces() {
        size_t threadCount = readThreadCount();
        std::vector<std::unique_ptr<Runner>> workerRunners;
        std::vector<std::string> workerLabels;
        std::vector<FuzzTestCase> multiCases;

        if (threadCount <= 1) {
            threadCount = 1;

            // 单线程保留原先行为，生成全部用例后顺序执行。
            testCases = generator->generate();
            generatedCaseCount = testCases.size();
            detailOut() << "Generated " << testCases.size() << " test cases"
                        << std::endl;

            if (testCases.empty()) {
                printSummary(threadCount, workerRunners, workerLabels);
                ADD_FAILURE() << "fuzz generator produced no test cases";
                return;
            }

            runSingleCasesSerial();
            const std::vector<FuzzTestCase> &selectedMultiCases =
                selectMultiCases(threadCount, multiCases);
            runMultiInterfaces(selectedMultiCases);
        } else {
            const size_t queueSize = readQueueSize();
            const size_t multiLimit = readMultiLimit(1024);
            std::cout << "[并行设置] HS_FUZZ_THREADS=" << threadCount
                      << ", HS_FUZZ_QUEUE_SIZE=" << queueSize
                      << ", lane 模式（不同 API 组并行执行）" << std::endl;
            std::cout << "[并行设置] HS_FUZZ_API_SAMPLE=" << readApiSampleRate()
                      << "（非 feedback lane 每 N 个用例取 1）" << std::endl;
            if (fpFeedbackAvailable && fpFeedbackLimit > 0) {
                std::cout << "[并行设置] feedback lane 全量采样, "
                          << "HS_FUZZ_FP_LIMIT=" << fpFeedbackLimit
                          << " 限制反馈用例总量" << std::endl;
            }
            if (multiLimit == 0) {
                std::cout << "[并行设置] multi interfaces disabled by "
                          << "HS_FUZZ_MULTI_LIMIT=0" << std::endl;
            } else {
                std::cout << "[并行设置] multi interfaces use first "
                          << multiLimit << " streamed cases; override with "
                          << "HS_FUZZ_MULTI_LIMIT" << std::endl;
            }

            workerRunners = runSingleCasesLaned(
                threadCount, queueSize, multiLimit, multiCases, workerLabels);

            if (generatedCaseCount == 0) {
                printSummary(threadCount, workerRunners, workerLabels);
                ADD_FAILURE() << "fuzz generator produced no test cases";
                return;
            }

            runMultiInterfaces(multiCases);
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
                           "args ==="
                        << std::endl;
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

        printSummary(threadCount, workerRunners, workerLabels);
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
TEST_P(HyperscanFuzzTest, AllInterfaces) { testAllInterfaces(); }

TEST(QuietOnlyFuzzRegression, BlockStreamVectoredNoCallbacks) {
    const char *expression = "Q";
    const unsigned int flags = HS_FLAG_QUIET | HS_FLAG_PREFILTER |
                               HS_FLAG_ALLOWEMPTY | HS_FLAG_MULTILINE;
    const unsigned int id = 2001;
    const unsigned int modes[] = {HS_MODE_BLOCK, HS_MODE_STREAM,
                                  HS_MODE_VECTORED};
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
INSTANTIATE_TEST_CASE_P(FuzzTests, HyperscanFuzzTest,
                        ::testing::ValuesIn(testParams));
