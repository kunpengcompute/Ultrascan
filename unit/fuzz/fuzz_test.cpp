#include "fuzz_test.h"
#include "data/data_generator.h"
#include "../gtest/gtest.h"
#include <iostream>

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
        
        // 生成测试用例
        testCases = generator->generate();
        std::cout << "Generated " << testCases.size() << " test cases" << std::endl;
        
        // 生成测试数据
        testData = dataGenerator->generateTestData(10, 0, 1024);
        std::cout << "Generated " << testData.size() << " test data items" << std::endl;
    }
    
    void TearDown() override {
        runner->reset();
    }

    void testAllInterfaces() {
        if (testCases.empty()) return;
        
        for (const auto& testCase : testCases) {
            std::cout << "\n=== 测试用例 " << testCase.id << " ===" << std::endl;
            
            // 1. 测试hs_compile接口
            std::cout << "测试 hs_compile..." << std::endl;
            runner->compile(testCase);
            runner->reset();
            
            // 2. 测试hs_scan接口
            std::cout << "测试 hs_scan..." << std::endl;
            runner->compile(testCase);
            for (const auto& data : testData) {
                runner->scan(data);
            }
            runner->reset();
            
            // 3. 测试hs_scan_stream接口
            std::cout << "测试 hs_scan_stream..." << std::endl;
            runner->compile(testCase, 1); // 1 is HS_MODE_STREAM
            for (const auto& data : testData) {
                runner->streamScan(data);
            }
            runner->reset();
            
            // 4. 测试hs_compile_lit接口
            std::cout << "测试 hs_compile_lit..." << std::endl;
            size_t length = testCase.pattern.length();
            runner->compileLit(testCase, length);
            runner->reset();
            
            // 5. 测试hs_expression_info接口
            std::cout << "测试 hs_expression_info..." << std::endl;
            runner->expressionInfo(testCase);
            runner->reset();
            
            // 6. 测试hs_expression_ext_info接口
            std::cout << "测试 hs_expression_ext_info..." << std::endl;
            runner->expressionExtInfo(testCase);
            runner->reset();
            
            // 7. 测试hs_reset_stream接口
            std::cout << "测试 hs_reset_stream..." << std::endl;
            runner->compile(testCase, 1); // 1 is HS_MODE_STREAM
            runner->resetStream();
            runner->reset();
            
            // 8. 测试hs_copy_stream接口
            std::cout << "测试 hs_copy_stream..." << std::endl;
            runner->compile(testCase, 1); // 1 is HS_MODE_STREAM
            runner->copyStream();
            runner->reset();
            
            // 9. 测试hs_reset_and_copy_stream接口
            std::cout << "测试 hs_reset_and_copy_stream..." << std::endl;
            runner->compile(testCase, 1); // 1 is HS_MODE_STREAM
            runner->resetAndCopyStream();
            runner->reset();
            
            // 10. 测试hs_compress_stream接口
            std::cout << "测试 hs_compress_stream..." << std::endl;
            runner->compile(testCase, 1); // 1 is HS_MODE_STREAM
            runner->compressStream();
            runner->reset();
            
            // 11. 测试hs_expand_stream接口
            std::cout << "测试 hs_expand_stream..." << std::endl;
            runner->compile(testCase, 1); // 1 is HS_MODE_STREAM
            runner->expandStream();
            runner->reset();
            
            // 12. 测试hs_reset_and_expand_stream接口
            std::cout << "测试 hs_reset_and_expand_stream..." << std::endl;
            runner->compile(testCase, 1); // 1 is HS_MODE_STREAM
            runner->resetAndExpandStream();
            runner->reset();
            
            // 13. 测试hs_scan_vector接口
            std::cout << "测试 hs_scan_vector..." << std::endl;
            runner->compile(testCase);
            std::vector<std::string> vectorData;
            for (size_t i = 0; i < 5; i++) {
                vectorData.push_back(testData[i % testData.size()]);
            }
            runner->scanVector(vectorData);
            runner->reset();
            
            // 14. 测试hs_clone_scratch接口
            std::cout << "测试 hs_clone_scratch..." << std::endl;
            runner->compile(testCase);
            runner->cloneScratch();
            runner->reset();
            
            // 15. 测试hs_scratch_size接口
            std::cout << "测试 hs_scratch_size..." << std::endl;
            runner->compile(testCase);
            runner->getScratchSize();
            runner->reset();
        }
        
        // 测试多模式接口（只需要测试一次）
        if (testCases.size() >= 2) {
            std::cout << "\n=== 测试多模式接口 ===" << std::endl;
            
            // 16. 测试hs_compile_multi接口
            std::cout << "测试 hs_compile_multi..." << std::endl;
            runner->compileMulti(testCases);
            for (const auto& data : testData) {
                runner->scan(data);
            }
            runner->reset();
            
            // 17. 测试hs_compile_ext_multi接口
            std::cout << "测试 hs_compile_ext_multi..." << std::endl;
            runner->compileExtMulti(testCases);
            for (const auto& data : testData) {
                runner->scan(data);
            }
            runner->reset();
            
            // 18. 测试hs_compile_lit_multi接口
            std::cout << "测试 hs_compile_lit_multi..." << std::endl;
            std::vector<size_t> lengths;
            for (const auto& testCase : testCases) {
                lengths.push_back(testCase.pattern.length());
            }
            runner->compileLitMulti(testCases, lengths);
            for (const auto& data : testData) {
                runner->scan(data);
            }
            runner->reset();
        }
        
        // 测试平台接口（只需要测试一次）
        std::cout << "\n=== 测试平台接口 ===" << std::endl;
        std::cout << "测试 hs_populate_platform..." << std::endl;
        runner->populatePlatform();
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