#ifndef FUZZ_TEST_H
#define FUZZ_TEST_H

#include <iosfwd>
#include <functional>
#include <string>
#include <vector>
#include <memory>
#include "hs.h"

// 测试用例结构
struct FuzzTestCase {
    std::string pattern;    // 正则表达式模式
    unsigned int flags;     // 编译标志
    std::string extParams;  // 扩展参数
    int id;                // 测试用例ID
};

// 测试参数结构
struct FuzzTestParams {
    std::string generatorType; // 生成器类型：aristocrats, completocrats, heuristocrats
    int depth;                 // 生成深度
    int count;                 // 测试用例数量
    bool fullCharset;          // 是否使用完整字符集
};

// 生成器接口
class Generator {
public:
    virtual ~Generator() {}
    virtual void configure(const std::string& type, int depth, int count, bool fullCharset) = 0;
    virtual std::vector<FuzzTestCase> generate() = 0;
    virtual size_t generateTo(const std::function<bool(const FuzzTestCase&)>& consumer) {
        size_t delivered = 0;
        std::vector<FuzzTestCase> testCases = generate();
        for (const auto& testCase : testCases) {
            if (!consumer(testCase)) {
                break;
            }
            delivered++;
        }
        return delivered;
    }
};

// 运行器接口
class Runner {
public:
    virtual ~Runner() {}
    virtual bool compile(const FuzzTestCase& testCase, unsigned int mode = HS_MODE_BLOCK) = 0;
    virtual bool scan(const std::string& data) = 0;
    virtual bool streamScan(const std::string& data) = 0;
    virtual bool compileMulti(const std::vector<FuzzTestCase>& testCases) = 0;
    virtual bool compileExtMulti(const std::vector<FuzzTestCase>& testCases) = 0;
    virtual bool compileLit(const FuzzTestCase& testCase, size_t length) = 0;
    virtual bool compileLitMulti(const std::vector<FuzzTestCase>& testCases, const std::vector<size_t>& lengths) = 0;
    virtual bool fatCompile(const FuzzTestCase& testCase, unsigned int mode = HS_MODE_BLOCK) = 0;
    virtual bool fatCompileMulti(const std::vector<FuzzTestCase>& testCases, unsigned int mode = HS_MODE_BLOCK) = 0;
    virtual bool fatCompileExtMulti(const std::vector<FuzzTestCase>& testCases, unsigned int mode = HS_MODE_BLOCK) = 0;
    virtual bool fatCompileLit(const FuzzTestCase& testCase, size_t length, unsigned int mode = HS_MODE_BLOCK) = 0;
    virtual bool fatCompileLitMulti(const std::vector<FuzzTestCase>& testCases, const std::vector<size_t>& lengths, unsigned int mode = HS_MODE_BLOCK) = 0;
    virtual bool fatCompileInvalidArgs() = 0;
    virtual bool expressionInfo(const FuzzTestCase& testCase) = 0;
    virtual bool expressionExtInfo(const FuzzTestCase& testCase) = 0;
    virtual bool populatePlatform() = 0;
    // 运行时接口测试方法
    virtual bool resetStream() = 0;
    virtual bool copyStream() = 0;
    virtual bool resetAndCopyStream() = 0;
    virtual bool compressStream() = 0;
    virtual bool expandStream() = 0;
    virtual bool resetAndExpandStream() = 0;
    virtual bool scanVector(const std::vector<std::string>& data) = 0;
    virtual bool cloneScratch() = 0;
    virtual bool getScratchSize() = 0;
    virtual void printSummary(std::ostream& os) const = 0;
    virtual void reset() = 0;
};

// 工厂函数
extern std::unique_ptr<Generator> createGenerator();
extern std::unique_ptr<Runner> createRunner();

#endif // FUZZ_TEST_H
