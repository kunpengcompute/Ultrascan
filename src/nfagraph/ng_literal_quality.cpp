/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 *
 * Licensed under the BSD License.
 */

#include "nfagraph/ng_literal_quality.h"

#include <string>

namespace ue2 {

size_t neoFdrZeroCount(const ue2_literal &literal, size_t *window_out) {
    static const size_t MAX_WINDOW = 8;

    const std::string &s = literal.get_string();
    const size_t window = s.size() < MAX_WINDOW ? s.size() : MAX_WINDOW;
    size_t zero_count = 0;
    for (size_t i = s.size() - window; i < s.size(); i++) {
        if (s[i] == '\0') {
            zero_count++;
        }
    }

    *window_out = window;
    return zero_count;
}

bool hasAllZeroNeoFdrTail(const ue2_literal &literal) {
    size_t window = 0;
    const size_t zero_count = neoFdrZeroCount(literal, &window);
    return window && zero_count == window;
}

bool isZeroDenseNeoFdrLiteral(const ue2_literal &literal) {
    static const size_t MIN_WINDOW = 4;

    size_t window = 0;
    const size_t zero_count = neoFdrZeroCount(literal, &window);
    if (window < MIN_WINDOW) {
        return false;
    }

    return zero_count * 4 >= window * 3;
}

bool isLowQualityNeoFdrLiteral(const ue2_literal &literal) {
    return hasAllZeroNeoFdrTail(literal) ||
           isZeroDenseNeoFdrLiteral(literal);
}

} // namespace ue2
