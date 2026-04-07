#include "../fuzz_test.h"
#include "hs.h"
#include <iostream>

class HyperscanRunner : public Runner {
public:
    HyperscanRunner() : db(nullptr), compileErr(nullptr), scratch(nullptr) {}
    
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
                std::cerr << "Compilation failed for pattern " << testCase.id << ": " 
                          << compileErr->message << std::endl;
                hs_free_compile_error(compileErr);
                compileErr = nullptr;
            }
            return true;
        }
        
        // 编译成功
        std::cout << "Compilation succeeded for pattern " << testCase.id << std::endl;
        
        // 分配scratch空间
        err = hs_alloc_scratch(db, &scratch);
        if (err != HS_SUCCESS) {
            std::cerr << "Failed to allocate scratch space" << std::endl;
            return true;
        }
        
        return true;
    }

    bool scan(const std::string& data) override {
        if (!db || !scratch) {
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
            std::cerr << "Scan failed with error: " << err << std::endl;
        }
        
        std::cout << "Scan completed, found " << matchCount << " matches" << std::endl;
        return true;
    }

    bool streamScan(const std::string& data) override {
        if (!db || !scratch) {
            return false;
        }
        
        // 打开流
        hs_stream_t* stream = nullptr;
        hs_error_t err = hs_open_stream(db, 0, &stream);
        if (err != HS_SUCCESS) {
            std::cerr << "Failed to open stream: " << err << std::endl;
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
                std::cerr << "Stream scan failed with error: " << err << std::endl;
                break;
            }
            offset += chunk;
        }
        
        // 关闭流
        err = hs_close_stream(stream, scratch, matchCallback, &matchCount);
        if (err != HS_SUCCESS) {
            std::cerr << "Failed to close stream: " << err << std::endl;
        }
        
        std::cout << "Stream scan completed, found " << matchCount << " matches" << std::endl;
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
                std::cerr << "Multi compilation failed: " << compileErr->message << std::endl;
                hs_free_compile_error(compileErr);
                compileErr = nullptr;
            }
            return true;
        }
        
        // 编译成功
        std::cout << "Multi compilation succeeded for " << testCases.size() << " patterns" << std::endl;
        
        // 分配scratch空间
        err = hs_alloc_scratch(db, &scratch);
        if (err != HS_SUCCESS) {
            std::cerr << "Failed to allocate scratch space" << std::endl;
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
        std::vector<hs_expr_ext_t> extParams;
        std::vector<const hs_expr_ext_t*> extParamsPtrs;
        
        for (const auto& testCase : testCases) {
            patterns.push_back(testCase.pattern.c_str());
            flags.push_back(testCase.flags);
            ids.push_back(testCase.id);
            
            // 创建扩展参数
            hs_expr_ext_t ext;
            ext.flags = 0;
            ext.min_offset = 0;
            ext.max_offset = 0;
            ext.min_length = 0;
            ext.edit_distance = 0;
            ext.hamming_distance = 0;
            extParams.push_back(ext);
            extParamsPtrs.push_back(&extParams.back());
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
                std::cerr << "Ext multi compilation failed: " << compileErr->message << std::endl;
                hs_free_compile_error(compileErr);
                compileErr = nullptr;
            }
            return true;
        }
        
        // 编译成功
        std::cout << "Ext multi compilation succeeded for " << testCases.size() << " patterns" << std::endl;
        
        // 分配scratch空间
        err = hs_alloc_scratch(db, &scratch);
        if (err != HS_SUCCESS) {
            std::cerr << "Failed to allocate scratch space" << std::endl;
            return true;
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
                std::cerr << "Literal compilation failed for pattern " << testCase.id << ": " 
                          << compileErr->message << std::endl;
                hs_free_compile_error(compileErr);
                compileErr = nullptr;
            }
            return true;
        }
        
        // 编译成功
        std::cout << "Literal compilation succeeded for pattern " << testCase.id << std::endl;
        
        // 分配scratch空间
        err = hs_alloc_scratch(db, &scratch);
        if (err != HS_SUCCESS) {
            std::cerr << "Failed to allocate scratch space" << std::endl;
            return true;
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
                std::cerr << "Multi literal compilation failed: " << compileErr->message << std::endl;
                hs_free_compile_error(compileErr);
                compileErr = nullptr;
            }
            return true;
        }
        
        // 编译成功
        std::cout << "Multi literal compilation succeeded for " << testCases.size() << " patterns" << std::endl;
        
        // 分配scratch空间
        err = hs_alloc_scratch(db, &scratch);
        if (err != HS_SUCCESS) {
            std::cerr << "Failed to allocate scratch space" << std::endl;
            return true;
        }
        
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
                std::cerr << "Expression info failed for pattern " << testCase.id << ": " 
                          << error->message << std::endl;
                hs_free_compile_error(error);
            }
            return true;
        }
        
        // 获取成功
        if (info) {
            std::cout << "Expression info succeeded for pattern " << testCase.id << ": " 
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
                std::cerr << "Expression ext info failed for pattern " << testCase.id << ": " 
                          << error->message << std::endl;
                hs_free_compile_error(error);
            }
            return true;
        }
        
        // 获取成功
        if (info) {
            std::cout << "Expression ext info succeeded for pattern " << testCase.id << ": " 
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
            std::cerr << "Populate platform failed: " << err << std::endl;
            return true;
        }
        
        // 填充成功
        std::cout << "Populate platform succeeded: " 
                  << "tune=" << platform.tune << ", " 
                  << "cpu_features=" << platform.cpu_features << std::endl;
        
        return true;
    }

    bool resetStream() override {
        if (!db || !scratch) {
            return true;
        }
        
        // 打开流
        hs_stream_t* stream = nullptr;
        hs_error_t err = hs_open_stream(db, 0, &stream);
        if (err != HS_SUCCESS) {
            std::cerr << "Failed to open stream for reset: " << err << std::endl;
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
            std::cerr << "Stream scan failed: " << err << std::endl;
        }
        
        // 重置流
        err = hs_reset_stream(stream, 0, scratch, matchCallback, &matchCount);
        if (err != HS_SUCCESS) {
            std::cerr << "Reset stream failed: " << err << std::endl;
        } else {
            std::cout << "Reset stream succeeded" << std::endl;
        }
        
        // 关闭流
        hs_close_stream(stream, scratch, matchCallback, &matchCount);
        
        return true;
    }

    bool copyStream() override {
        if (!db) {
            return true;
        }
        
        // 打开流
        hs_stream_t* stream1 = nullptr;
        hs_error_t err = hs_open_stream(db, 0, &stream1);
        if (err != HS_SUCCESS) {
            std::cerr << "Failed to open stream 1: " << err << std::endl;
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
            std::cerr << "Stream scan failed: " << err << std::endl;
        }
        
        // 复制流
        hs_stream_t* stream2 = nullptr;
        err = hs_copy_stream(&stream2, stream1);
        if (err != HS_SUCCESS) {
            std::cerr << "Copy stream failed: " << err << std::endl;
        } else {
            std::cout << "Copy stream succeeded" << std::endl;
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
            return true;
        }
        
        // 打开两个流
        hs_stream_t* stream1 = nullptr;
        hs_stream_t* stream2 = nullptr;
        
        hs_error_t err = hs_open_stream(db, 0, &stream1);
        if (err != HS_SUCCESS) {
            std::cerr << "Failed to open stream 1: " << err << std::endl;
            return true;
        }
        
        err = hs_open_stream(db, 0, &stream2);
        if (err != HS_SUCCESS) {
            std::cerr << "Failed to open stream 2: " << err << std::endl;
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
            std::cerr << "Stream scan failed: " << err << std::endl;
        }
        
        // 重置并复制流
        err = hs_reset_and_copy_stream(stream2, stream1, scratch, matchCallback, &matchCount);
        if (err != HS_SUCCESS) {
            std::cerr << "Reset and copy stream failed: " << err << std::endl;
        } else {
            std::cout << "Reset and copy stream succeeded" << std::endl;
        }
        
        // 关闭流
        hs_close_stream(stream1, scratch, matchCallback, &matchCount);
        hs_close_stream(stream2, scratch, matchCallback, &matchCount);
        
        return true;
    }

    bool compressStream() override {
        if (!db || !scratch) {
            return true;
        }
        
        // 打开流
        hs_stream_t* stream = nullptr;
        hs_error_t err = hs_open_stream(db, 0, &stream);
        if (err != HS_SUCCESS) {
            std::cerr << "Failed to open stream: " << err << std::endl;
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
            std::cerr << "Stream scan failed: " << err << std::endl;
        }
        
        // 压缩流
        size_t requiredSpace = 0;
        err = hs_compress_stream(stream, nullptr, 0, &requiredSpace);
        if (err != HS_INSUFFICIENT_SPACE) {
            std::cerr << "Failed to get required space: " << err << std::endl;
            hs_close_stream(stream, scratch, matchCallback, &matchCount);
            return true;
        }
        
        // 分配缓冲区并压缩
        std::vector<char> buffer(requiredSpace);
        size_t usedSpace = 0;
        err = hs_compress_stream(stream, buffer.data(), buffer.size(), &usedSpace);
        if (err != HS_SUCCESS) {
            std::cerr << "Compress stream failed: " << err << std::endl;
        } else {
            std::cout << "Compress stream succeeded, used " << usedSpace << " bytes" << std::endl;
        }
        
        // 关闭流
        hs_close_stream(stream, scratch, matchCallback, &matchCount);
        
        return true;
    }

    bool expandStream() override {
        if (!db) {
            return true;
        }
        
        // 打开流并写入数据
        hs_stream_t* stream1 = nullptr;
        hs_error_t err = hs_open_stream(db, 0, &stream1);
        if (err != HS_SUCCESS) {
            std::cerr << "Failed to open stream 1: " << err << std::endl;
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
            std::cerr << "Stream scan failed: " << err << std::endl;
        }
        
        // 压缩流
        size_t requiredSpace = 0;
        err = hs_compress_stream(stream1, nullptr, 0, &requiredSpace);
        if (err != HS_INSUFFICIENT_SPACE) {
            std::cerr << "Failed to get required space: " << err << std::endl;
            hs_close_stream(stream1, scratch, matchCallback, &matchCount);
            return true;
        }
        
        std::vector<char> buffer(requiredSpace);
        size_t usedSpace = 0;
        err = hs_compress_stream(stream1, buffer.data(), buffer.size(), &usedSpace);
        if (err != HS_SUCCESS) {
            std::cerr << "Compress stream failed: " << err << std::endl;
            hs_close_stream(stream1, scratch, matchCallback, &matchCount);
            return true;
        }
        
        // 解压流
        hs_stream_t* stream2 = nullptr;
        err = hs_expand_stream(db, &stream2, buffer.data(), usedSpace);
        if (err != HS_SUCCESS) {
            std::cerr << "Expand stream failed: " << err << std::endl;
        } else {
            std::cout << "Expand stream succeeded" << std::endl;
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
            return true;
        }
        
        // 打开流并写入数据
        hs_stream_t* stream1 = nullptr;
        hs_stream_t* stream2 = nullptr;
        
        hs_error_t err = hs_open_stream(db, 0, &stream1);
        if (err != HS_SUCCESS) {
            std::cerr << "Failed to open stream 1: " << err << std::endl;
            return true;
        }
        
        err = hs_open_stream(db, 0, &stream2);
        if (err != HS_SUCCESS) {
            std::cerr << "Failed to open stream 2: " << err << std::endl;
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
            std::cerr << "Stream scan failed: " << err << std::endl;
        }
        
        // 压缩流
        size_t requiredSpace = 0;
        err = hs_compress_stream(stream1, nullptr, 0, &requiredSpace);
        if (err != HS_INSUFFICIENT_SPACE) {
            std::cerr << "Failed to get required space: " << err << std::endl;
            hs_close_stream(stream1, scratch, matchCallback, &matchCount);
            hs_close_stream(stream2, scratch, matchCallback, &matchCount);
            return true;
        }
        
        std::vector<char> buffer(requiredSpace);
        size_t usedSpace = 0;
        err = hs_compress_stream(stream1, buffer.data(), buffer.size(), &usedSpace);
        if (err != HS_SUCCESS) {
            std::cerr << "Compress stream failed: " << err << std::endl;
            hs_close_stream(stream1, scratch, matchCallback, &matchCount);
            hs_close_stream(stream2, scratch, matchCallback, &matchCount);
            return true;
        }
        
        // 重置并解压流
        err = hs_reset_and_expand_stream(stream2, buffer.data(), usedSpace, scratch, matchCallback, &matchCount);
        if (err != HS_SUCCESS) {
            std::cerr << "Reset and expand stream failed: " << err << std::endl;
        } else {
            std::cout << "Reset and expand stream succeeded" << std::endl;
        }
        
        // 关闭流
        hs_close_stream(stream1, scratch, matchCallback, &matchCount);
        hs_close_stream(stream2, scratch, matchCallback, &matchCount);
        
        return true;
    }

    bool scanVector(const std::vector<std::string>& data) override {
        if (!db || !scratch) {
            return true;
        }
        
        if (data.empty()) {
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
            std::cerr << "Vector scan failed with error: " << err << std::endl;
        } else {
            std::cout << "Vector scan completed, found " << matchCount << " matches" << std::endl;
        }
        
        return true;
    }

    bool cloneScratch() override {
        if (!db) {
            return true;
        }
        
        // 确保有scratch空间
        if (!scratch) {
            hs_error_t err = hs_alloc_scratch(db, &scratch);
            if (err != HS_SUCCESS) {
                std::cerr << "Failed to allocate scratch space" << std::endl;
                return true;
            }
        }
        
        // 克隆scratch空间
        hs_scratch_t* clonedScratch = nullptr;
        hs_error_t err = hs_clone_scratch(scratch, &clonedScratch);
        if (err != HS_SUCCESS) {
            std::cerr << "Clone scratch failed: " << err << std::endl;
        } else {
            std::cout << "Clone scratch succeeded" << std::endl;
            // 释放克隆的scratch空间
            hs_free_scratch(clonedScratch);
        }
        
        return true;
    }

    bool getScratchSize() override {
        if (!scratch) {
            return true;
        }
        
        // 获取scratch空间大小
        size_t scratchSize = 0;
        hs_error_t err = hs_scratch_size(scratch, &scratchSize);
        if (err != HS_SUCCESS) {
            std::cerr << "Get scratch size failed: " << err << std::endl;
        } else {
            std::cout << "Scratch size: " << scratchSize << " bytes" << std::endl;
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
        if (compileErr) {
            hs_free_compile_error(compileErr);
            compileErr = nullptr;
        }
    }

private:
    hs_database_t* db;
    hs_compile_error_t* compileErr;
    hs_scratch_t* scratch;
};

std::unique_ptr<Runner> createRunner() {
    return std::make_unique<HyperscanRunner>();
}
