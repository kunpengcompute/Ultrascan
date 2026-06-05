#include "../fuzz_test.h"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <regex>

namespace {

bool fuzzVerbose() {
    const char* value = std::getenv("HS_FUZZ_VERBOSE");
    return value && value[0] != '\0' && value[0] != '0';
}

} // namespace

class PythonGenerator : public Generator {
public:
    void configure(const std::string& p_type, int p_depth, int p_count, bool p_fullCharset) override {
        generatorType = p_type;
        depth = p_depth;
        count = p_count;
        fullCharset = p_fullCharset;
    }

    std::vector<FuzzTestCase> generate() override {
        std::vector<FuzzTestCase> testCases;
        
        // 构建Python命令
        std::stringstream cmd;
        // 使用相对于项目根目录的路径
        cmd << "python ../../../tools/fuzz/" << generatorType << ".py";
        cmd << " --depth " << depth;
        cmd << " --count " << count;
        if (fullCharset) {
            cmd << " --full";
        }
        
        // 执行命令并读取输出
        FILE* pipe = popen(cmd.str().c_str(), "r");
        if (!pipe) {
            std::cerr << "Failed to open pipe" << std::endl;
            return testCases;
        }
        
        char buffer[1024];
        int lineCount = 0;
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            lineCount++;
            std::string line(buffer);
            FuzzTestCase testCase = parseGeneratorOutput(line);
            if (!testCase.pattern.empty()) {
                testCases.push_back(testCase);
            }
        }
        
        int status = pclose(pipe);
        if (fuzzVerbose()) {
            std::cout << "Command exit status: " << status << std::endl;
            std::cout << "Total lines read: " << lineCount << std::endl;
        }
        return testCases;
    }

private:
    FuzzTestCase parseGeneratorOutput(const std::string& line) {
        FuzzTestCase testCase;
        
        // 简单解析：找到第一个冒号，然后找到第一个和第二个斜杠
        size_t colonPos = line.find(':');
        if (colonPos == std::string::npos) {
            return testCase;
        }
        
        // 解析ID
        std::string idStr = line.substr(0, colonPos);
        try {
            testCase.id = std::stoi(idStr);
        } catch (...) {
            return testCase;
        }
        
        // 找到第一个斜杠
        size_t firstSlash = line.find('/', colonPos + 1);
        if (firstSlash == std::string::npos) {
            return testCase;
        }
        
        // 找到第二个斜杠
        size_t secondSlash = line.find('/', firstSlash + 1);
        if (secondSlash == std::string::npos) {
            return testCase;
        }
        
        // 解析模式
        testCase.pattern = line.substr(firstSlash + 1, secondSlash - firstSlash - 1);
        
        // 解析标志（从第二个斜杠到行尾）
        std::string flagsStr = line.substr(secondSlash + 1);
        // 移除末尾的换行符
        flagsStr.erase(std::remove(flagsStr.begin(), flagsStr.end(), '\n'), flagsStr.end());
        flagsStr.erase(std::remove(flagsStr.begin(), flagsStr.end(), '\r'), flagsStr.end());
        testCase.flags = parseFlags(flagsStr);
        
        return testCase;
    }

    unsigned int parseFlags(const std::string& flagsStr) {
        unsigned int flags = 0;
        
        // 映射标志字符到HS_FLAG_*常量
        for (char c : flagsStr) {
            switch (c) {
                case 's': flags |= 4; break; // HS_FLAG_MULTILINE
                case 'm': flags |= 4; break; // HS_FLAG_MULTILINE
                case 'i': flags |= 1; break; // HS_FLAG_CASELESS
                case 'H': flags |= 128; break; // HS_FLAG_PREFILTER
                case 'V': flags |= 256; break; // HS_FLAG_SOM_LEFTMOST
                case '8': flags |= 32; break; // HS_FLAG_UTF8
                case 'W': flags |= 64; break; // HS_FLAG_UCP
                case 'L': flags |= 16; break; // HS_FLAG_ALLOWEMPTY
                case 'P': break; // 暂时忽略
                case 'Q': flags |= 1024; break; // HS_FLAG_QUIET
            }
        }
        
        return flags;
    }

    std::string generatorType;
    int depth;
    int count;
    bool fullCharset;
};

std::unique_ptr<Generator> createGenerator() {
    return std::make_unique<PythonGenerator>();
}
