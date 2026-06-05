#include "fuzz_test.h"
#include "data/data_generator.h"
#include "../gtest/gtest.h"
#include <cstdlib>
#include <iostream>
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

} // namespace

// 测试参数
static const FuzzTestParams testParams[] = {
    {"aristocrats", 10, 100, false},
    {"completocrats", 10, 100, false},
    {"heuristocrats", 10, 100, false}
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

        // 生成测试用例
        testCases = generator->generate();
        detailOut() << "Generated " << testCases.size() << " test cases" << std::endl;

        // 生成测试数据
        testData = dataGenerator->generateTestData(10, 0, 1024);
        detailOut() << "Generated " << testData.size() << " test data items" << std::endl;
    }

    void TearDown() override {
        runner->reset();
    }

    void testAllInterfaces() {
        if (testCases.empty()) {
            std::cout << "\n=== Fuzz Summary ===" << std::endl;
            std::cout << "generator: " << params.generatorType << std::endl;
            std::cout << "test rounds: " << params.count << std::endl;
            std::cout << "test cases: 0" << std::endl;
            std::cout << "test data: " << testData.size() << std::endl;
            runner->printSummary(std::cout);
            return;
        }

        const unsigned int fatModes[] = {
            HS_MODE_BLOCK,
            HS_MODE_STREAM,
            HS_MODE_VECTORED,
            HS_MODE_STREAM | HS_MODE_SOM_HORIZON_LARGE,
            HS_MODE_STREAM | HS_MODE_SOM_HORIZON_MEDIUM,
            HS_MODE_STREAM | HS_MODE_SOM_HORIZON_SMALL
        };

        for (const auto& testCase : testCases) {
            detailOut() << "\n=== 测试用例 " << testCase.id << " ===" << std::endl;

            // 1. 测试hs_compile接口
            detailOut() << "测试 hs_compile..." << std::endl;
            runner->compile(testCase);
            runner->reset();

            // 1.1 测试fat_hs_compile接口
            for (unsigned int mode : fatModes) {
                detailOut() << "测试 fat_hs_compile, mode=" << mode << "..." << std::endl;
                runner->fatCompile(testCase, mode);
                runner->reset();
            }

            // 2. 测试hs_scan接口
            detailOut() << "测试 hs_scan..." << std::endl;
            runner->compile(testCase);
            for (const auto& data : testData) {
                runner->scan(data);
            }
            runner->reset();

            // 3. 测试hs_scan_stream接口
            detailOut() << "测试 hs_scan_stream..." << std::endl;
            runner->compile(testCase, 1); // 1 is HS_MODE_STREAM
            for (const auto& data : testData) {
                runner->streamScan(data);
            }
            runner->reset();

            // 4. 测试hs_compile_lit接口
            detailOut() << "测试 hs_compile_lit..." << std::endl;
            size_t length = testCase.pattern.length();
            runner->compileLit(testCase, length);
            runner->reset();

            // 4.1 测试fat_hs_compile_lit接口
            for (unsigned int mode : fatModes) {
                detailOut() << "测试 fat_hs_compile_lit, mode=" << mode << "..." << std::endl;
                runner->fatCompileLit(testCase, length, mode);
                runner->reset();
            }

            // 5. 测试hs_expression_info接口
            detailOut() << "测试 hs_expression_info..." << std::endl;
            runner->expressionInfo(testCase);
            runner->reset();

            // 6. 测试hs_expression_ext_info接口
            detailOut() << "测试 hs_expression_ext_info..." << std::endl;
            runner->expressionExtInfo(testCase);
            runner->reset();

            // 7. 测试hs_reset_stream接口
            detailOut() << "测试 hs_reset_stream..." << std::endl;
            runner->compile(testCase, 1); // 1 is HS_MODE_STREAM
            runner->resetStream();
            runner->reset();

            // 8. 测试hs_copy_stream接口
            detailOut() << "测试 hs_copy_stream..." << std::endl;
            runner->compile(testCase, 1); // 1 is HS_MODE_STREAM
            runner->copyStream();
            runner->reset();

            // 9. 测试hs_reset_and_copy_stream接口
            detailOut() << "测试 hs_reset_and_copy_stream..." << std::endl;
            runner->compile(testCase, 1); // 1 is HS_MODE_STREAM
            runner->resetAndCopyStream();
            runner->reset();

            // 10. 测试hs_compress_stream接口
            detailOut() << "测试 hs_compress_stream..." << std::endl;
            runner->compile(testCase, 1); // 1 is HS_MODE_STREAM
            runner->compressStream();
            runner->reset();

            // 11. 测试hs_expand_stream接口
            detailOut() << "测试 hs_expand_stream..." << std::endl;
            runner->compile(testCase, 1); // 1 is HS_MODE_STREAM
            runner->expandStream();
            runner->reset();

            // 12. 测试hs_reset_and_expand_stream接口
            detailOut() << "测试 hs_reset_and_expand_stream..." << std::endl;
            runner->compile(testCase, 1); // 1 is HS_MODE_STREAM
            runner->resetAndExpandStream();
            runner->reset();

            // 13. 测试hs_scan_vector接口
            detailOut() << "测试 hs_scan_vector..." << std::endl;
            runner->compile(testCase);
            std::vector<std::string> vectorData;
            for (size_t i = 0; i < 5; i++) {
                vectorData.push_back(testData[i % testData.size()]);
            }
            runner->scanVector(vectorData);
            runner->reset();

            // 14. 测试hs_clone_scratch接口
            detailOut() << "测试 hs_clone_scratch..." << std::endl;
            runner->compile(testCase);
            runner->cloneScratch();
            runner->reset();

            // 15. 测试hs_scratch_size接口
            detailOut() << "测试 hs_scratch_size..." << std::endl;
            runner->compile(testCase);
            runner->getScratchSize();
            runner->reset();
        }

        // 测试多模式接口（只需要测试一次）
        if (testCases.size() >= 2) {
            detailOut() << "\n=== 测试多模式接口 ===" << std::endl;

            // 16. 测试hs_compile_multi接口
            detailOut() << "测试 hs_compile_multi..." << std::endl;
            runner->compileMulti(testCases);
            for (const auto& data : testData) {
                runner->scan(data);
            }
            runner->reset();

            // 16.1 测试fat_hs_compile_multi接口
            for (unsigned int mode : fatModes) {
                detailOut() << "测试 fat_hs_compile_multi, mode=" << mode << "..." << std::endl;
                runner->fatCompileMulti(testCases, mode);
                runner->reset();
            }

            // 17. 测试hs_compile_ext_multi接口
            detailOut() << "测试 hs_compile_ext_multi..." << std::endl;
            runner->compileExtMulti(testCases);
            for (const auto& data : testData) {
                runner->scan(data);
            }
            runner->reset();

            // 17.1 测试fat_hs_compile_ext_multi接口
            for (unsigned int mode : fatModes) {
                detailOut() << "测试 fat_hs_compile_ext_multi, mode=" << mode << "..." << std::endl;
                runner->fatCompileExtMulti(testCases, mode);
                runner->reset();
            }

            // 18. 测试hs_compile_lit_multi接口
            detailOut() << "测试 hs_compile_lit_multi..." << std::endl;
            std::vector<size_t> lengths;
            for (const auto& testCase : testCases) {
                lengths.push_back(testCase.pattern.length());
            }
            runner->compileLitMulti(testCases, lengths);
            for (const auto& data : testData) {
                runner->scan(data);
            }
            runner->reset();

            // 18.1 测试fat_hs_compile_lit_multi接口
            for (unsigned int mode : fatModes) {
                detailOut() << "测试 fat_hs_compile_lit_multi, mode=" << mode << "..." << std::endl;
                runner->fatCompileLitMulti(testCases, lengths, mode);
                runner->reset();
            }
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

        std::cout << "\n=== Fuzz Summary ===" << std::endl;
        std::cout << "generator: " << params.generatorType << std::endl;
        std::cout << "test rounds: " << params.count << std::endl;
        std::cout << "test cases: " << testCases.size() << std::endl;
        std::cout << "test data: " << testData.size() << std::endl;
        runner->printSummary(std::cout);
    }

    FuzzTestParams params;
    std::unique_ptr<Generator> generator;
    std::unique_ptr<Runner> runner;
    std::unique_ptr<DataGenerator> dataGenerator;
    std::vector<FuzzTestCase> testCases;
    std::vector<std::string> testData;
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
