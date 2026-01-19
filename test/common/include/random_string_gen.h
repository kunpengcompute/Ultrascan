/*
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef RANDOM_STRING_GEN_H_
#define RANDOM_STRING_GEN_H_

#include <string>
#include <random>
#include <algorithm>
#include <iomanip>
#include <thread>

class RandomStringGenerator {
public:
    RandomStringGenerator() : engine(std::random_device{}()), type_dist(WEIGHTS.begin(), WEIGHTS.end())
    {}

    char generate_char()
    {
        switch (type_dist(engine)) {
            case 0: {
                bool is_upper = (case_dist(engine) == 0);
                const std::string &charset = is_upper ? UPPERCASE : LOWERCASE;
                return charset[uniform<size_t>(0, charset.size() - 1)];
            }
            case 1:
                return DIGITS[uniform<size_t>(0, DIGITS.size() - 1)];
            default:
                return SPECIAL_CHARS[uniform<size_t>(0, SPECIAL_CHARS.size() - 1)];
        }
    }

    std::string generate_string(size_t length)
    {
        std::string s;
        s.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            s += generate_char();
        }
        return s;
    }

    std::vector<std::string> generate_dataset(size_t num_strs, size_t max_length)
    {
        std::vector<std::string> dataset;
        dataset.reserve(num_strs);

        std::uniform_int_distribution<size_t> len_dist(1, max_length);

        for (size_t i = 0; i < num_strs; ++i) {
            dataset.emplace_back(generate_string(len_dist(engine)));
        }
        return dataset;
    }

private:
    const std::string UPPERCASE = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const std::string LOWERCASE = "abcdefghijklmnopqrstuvwxyz";
    const std::string DIGITS = "0123456789";
    const std::string SPECIAL_CHARS = "$-./,@#&_*";

    // 权重分布：字母60%，数字20%，特殊字符20%
    const std::vector<double> WEIGHTS = {0.6, 0.2, 0.2};

    // 随机数引擎和分布
    std::mt19937 engine;
    std::discrete_distribution<int> type_dist;
    std::uniform_int_distribution<int> case_dist{0, 1};

private:
    template <typename T>
    T uniform(T min, T max)
    {
        return std::uniform_int_distribution<T>{min, max}(engine);
    }
};

#endif