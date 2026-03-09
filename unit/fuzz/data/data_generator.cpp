#include "data_generator.h"
#include <random>
#include <cstdlib>
#include <ctime>

DataGenerator::DataGenerator() : seed(0) {
    // 初始化随机种子
    seed = static_cast<unsigned int>(time(nullptr));
    srand(seed);
}

std::string DataGenerator::generateRandomText(size_t length, bool includeSpecialChars) {
    std::string result;
    result.reserve(length);
    
    for (size_t i = 0; i < length; ++i) {
        if (includeSpecialChars) {
            // 包含特殊字符
            char c = static_cast<char>(32 + rand() % 95); // ASCII 32-126
            result += c;
        } else {
            // 只包含字母和数字
            int type = rand() % 3;
            if (type == 0) {
                result += 'a' + rand() % 26; // 小写字母
            } else if (type == 1) {
                result += 'A' + rand() % 26; // 大写字母
            } else {
                result += '0' + rand() % 10; // 数字
            }
        }
    }
    
    return result;
}

std::string DataGenerator::generateBinaryData(size_t length) {
    std::string result;
    result.reserve(length);
    
    for (size_t i = 0; i < length; ++i) {
        char c = static_cast<char>(rand() % 256); // 0-255
        result += c;
    }
    
    return result;
}

std::string DataGenerator::generateSpecialChars(size_t length) {
    std::string result;
    result.reserve(length);
    
    // 特殊字符集合
    const char specialChars[] = "!@#$%^&*()_+-=[]{}|;:,.<>?\"'";
    size_t specialCount = sizeof(specialChars) - 1;
    
    for (size_t i = 0; i < length; ++i) {
        result += specialChars[rand() % specialCount];
    }
    
    return result;
}

std::string DataGenerator::generateBoundaryData(size_t length) {
    std::string result;
    result.reserve(length);
    
    // 生成边界情况的数据
    if (length == 0) {
        return result; // 空数据
    } else if (length == 1) {
        result += static_cast<char>(rand() % 256); // 单字节数据
    } else {
        // 生成包含各种边界字符的数据
        result += '\0'; // 空字符
        result += '\n'; // 换行符
        result += '\r'; // 回车符
        result += '\t'; // 制表符
        
        // 填充剩余长度
        for (size_t i = 4; i < length; ++i) {
            result += static_cast<char>(rand() % 256);
        }
    }
    
    return result;
}

std::vector<std::string> DataGenerator::generateTestData(size_t count, size_t minLength, size_t maxLength) {
    std::vector<std::string> testData;
    testData.reserve(count);
    
    for (size_t i = 0; i < count; ++i) {
        // 随机长度
        size_t length = minLength + rand() % (maxLength - minLength + 1);
        
        // 随机选择数据类型
        int type = rand() % 4;
        switch (type) {
            case 0:
                testData.push_back(generateRandomText(length));
                break;
            case 1:
                testData.push_back(generateBinaryData(length));
                break;
            case 2:
                testData.push_back(generateSpecialChars(length));
                break;
            case 3:
                testData.push_back(generateBoundaryData(length));
                break;
            default:
                break;
        }
    }
    
    return testData;
}