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

#include "gtest/gtest.h"
#include <unistd.h>
#include <iostream>
#include <vector>
#include <regex>
#include <string>
#include "random_string_gen.h"
#include "khsel_ops.h"

using namespace std;

#define TEST_LEN 100000

#ifdef __aarch64__

static int ut_replace_all(
    const std::vector<std::string> &test_data, const std::string &regex, const std::string &replacement)
{
    int passed_num = 0;
    std::regex pattern(regex);
    std::string failed_example = "";
    std::string stl_example = "";

    for (const auto &str : test_data) {
        std::string res = ReplaceAllAcc(str, "_");
        std::string stl_res = std::regex_replace(str, pattern, replacement);
        if (!res.compare(stl_res)) {
            passed_num++;
        } else if (failed_example.length() == 0) {
            failed_example = res;
            stl_example = stl_res;
        }
    }
    return passed_num;
}

TEST(ReplaceAllAcc, ExecMatch1) {
    std::string regex = "[^A-Za-z0-9_/.]+";
    std::string replacement = "_";
    const size_t max_length = 64;
    RandomStringGenerator gen;

    auto test_data = gen.generate_dataset(TEST_LEN, max_length);

    int passed = ut_replace_all(test_data, regex, replacement);
    test_data.clear();
    test_data.shrink_to_fit();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    EXPECT_EQ(passed, TEST_LEN);
}

#endif