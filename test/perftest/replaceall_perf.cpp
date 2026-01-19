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
#include <iostream>
#include <vector>
#include <string>
#include <random>
#include <algorithm>
#include <regex>
#include <iomanip>
#include <thread>
#include <unistd.h>
#include <chrono>
#include "random_string_gen.h"
#include "khsel_ops.h"

long long get_nanoseconds()
{
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::time_point_cast<std::chrono::nanoseconds>(now).time_since_epoch().count();
}

class RegexBenchmark {
public:
    static uint64_t benchmark_replace_all(
        const std::vector<std::string> &test_data, const std::string &replacement)
    {
        const uint64_t start = get_nanoseconds();
        for (const auto &str : test_data) {
            std::string res = ReplaceAllAcc(str, replacement);
        }
        return get_nanoseconds() - start;
    }

    static void print_stats(const std::vector<long long> &durations, size_t dataset_size)
    {
        uint64_t total = 0;
        for (auto dur : durations)
            total += dur;

        const double mean_total = total / static_cast<double>(durations.size());
        const double mean_per_op = total / (durations.size() * dataset_size);

        std::cout << "\n--- Summary ---\n"
                  << "Total runs: " << durations.size() << "\n"
                  << std::fixed << std::setprecision(2) << "Mean duration (total): " << mean_total / 1e6 << " ms\n"
                  << "Mean duration per operation: " << mean_per_op << " ns\n";
    }

    static void display_results(size_t size, const std::vector<long long> &durations)
    {
        std::cout << "\nResults for " << size << " strings:\n"
                  << "-------------------------------------------------\n"
                  << std::left << std::setw(10) << "Run#" << std::setw(20) << "Duration (ms)" << std::setw(20)
                  << "Duration per op (ns)\n"
                  << "-------------------------------------------------\n";

        for (size_t i = 0; i < durations.size(); ++i) {
            std::cout << std::setw(10) << i + 1 << std::fixed << std::setprecision(2) << std::setw(20)
                      << durations[i] / 1e6 << std::setw(20) << durations[i] / static_cast<double>(size) << "\n";
        }
        print_stats(durations, size);
    }
};

TEST(ReplaceAllAcc, BASIC_PERF001)
{
    // std::string regex = "[^A-Za-z0-9_/.]+";
    std::string replacement = "_";

    const std::vector<size_t> data_sizes = {100000, 500000};
    const size_t max_length = 64;

    bool warmup_done = false;
    RandomStringGenerator gen;

    for (auto size : data_sizes) {
        std::cout << "\nTesting with " << size << " strings:\n";

        if (!warmup_done) {
            std::cout << "Running warm-up phase (3 rounds)...\n";
            for (int i = 0; i < 3; ++i) {
                auto warm_data = gen.generate_dataset(size, max_length);
                RegexBenchmark::benchmark_replace_all(warm_data, replacement);
                warm_data.clear();
                warm_data.shrink_to_fit();
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            warmup_done = true;
        }

        std::vector<long long> durations;
        for (int i = 0; i < 5; ++i) {
            auto test_data = gen.generate_dataset(size, max_length);

            auto duration = RegexBenchmark::benchmark_replace_all(test_data, replacement);
            durations.push_back(duration);

            test_data.clear();
            test_data.shrink_to_fit();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        RegexBenchmark::display_results(size, durations);
    }
}
