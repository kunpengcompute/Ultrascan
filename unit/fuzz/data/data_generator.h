#ifndef DATA_GENERATOR_H
#define DATA_GENERATOR_H

#include <string>
#include <vector>

class DataGenerator {
public:
    DataGenerator();
    
    // 生成随机文本数据
    std::string generateRandomText(size_t length, bool includeSpecialChars = false);
    
    // 生成二进制数据
    std::string generateBinaryData(size_t length);
    
    // 生成特殊字符数据
    std::string generateSpecialChars(size_t length);
    
    // 生成边界长度数据
    std::string generateBoundaryData(size_t length);
    
    // 生成多种类型的数据集合
    std::vector<std::string> generateTestData(size_t count, size_t minLength, size_t maxLength);
    
private:
    unsigned int seed;
};

#endif // DATA_GENERATOR_H