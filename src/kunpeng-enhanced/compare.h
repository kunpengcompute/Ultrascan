/*
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * Optimized compare
 * Copyright (c) 2015-2016, Intel Corporation
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

#ifndef COMPARE_H
#define COMPARE_H

#include "ue2common.h"

#define ALIGN_ATTR(x) __attribute__((aligned((x))))
#define PACKED__MAY_ALIAS __attribute__((__packed__, __may_alias__))

typedef unsigned long long ALIGN_ATTR(8) u64a;
typedef uint8_t u8;

#define CMP_T        u64a
#define CMP_SIZE_NEW sizeof(CMP_T)
#define NEO_SIZE 16

static inline u64a toupper64(const u64a input)
{
    u64a mask = 0x8080808080808080uLL | input;
    u64a c = mask - 0x6161616161616161uLL;
    u64a d = ~(mask - 0x7b7b7b7b7b7b7b7buLL);
    u64a e = (c & d) & (~input & 0x8080808080808080uLL);
    u64a v = input - (e >> 2);
    return v;
}

static inline u64a unaligned_load_u64a(const void *ptr)
{
    struct unaligned { u64a u; } PACKED__MAY_ALIAS;
    const struct unaligned *uptr = (const struct unaligned *)ptr;
    return uptr->u;
}

#define ULOAD(x)     unaligned_load_u64a(x)
#define TOUPPER(x)   toupper64(x)

static inline char mytoupper(const char c)
{
    if (c >= 'a' && c <= 'z') {
        return c - 0x20;
    }
    return c;
}

static inline int cmpNocaseNaiveNew(const u8 *p1, const u8 *p2, size_t len)
{
    const u8 *pEnd = p1 + len;
    for (; p1 < pEnd; p1++, p2++) {
        if ((u8)mytoupper(*p1) != *p2) {
            return 1;
        }
    }
    return 0;
}

static inline int cmpCaseNaiveNew(const u8 *p1, const u8 *p2, size_t len)
{
    const u8 *pEnd = p1 + len;
    for (; p1 < pEnd; p1++, p2++) {
        if (*p1 != *p2) {
            return 1;
        }
    }
    return 0;
}

static inline int cmpForward_small(const u8 *p1, const u8 *p2, size_t len, char nocase)
{
    if (len < CMP_SIZE_NEW) {
        return nocase ? cmpNocaseNaiveNew(p1, p2, len)
                      : cmpCaseNaiveNew(p1, p2, len);
    }

    const u8 *p1_end = p1 + len - CMP_SIZE_NEW;
    const u8 *p2_end = p2 + len - CMP_SIZE_NEW;

    if (nocase) {
        for (; p1 < p1_end; p1 += CMP_SIZE_NEW, p2 += CMP_SIZE_NEW) {
            if (TOUPPER(ULOAD(p1)) != ULOAD(p2)) {
                return 1;
            }
        }
        if (TOUPPER(ULOAD(p1_end)) != ULOAD(p2_end)) {
            return 1;
        }
    } else {
        for (; p1 < p1_end; p1 += CMP_SIZE_NEW, p2 += CMP_SIZE_NEW) {
            if (ULOAD(p1) != ULOAD(p2)) {
                return 1;
            }
        }
        if (ULOAD(p1_end) != ULOAD(p2_end)) {
            return 1;
        }
    }

    return 0;
}

// 大字符串（≥96字节）：混合策略（标量前缀 + NEON主体）
static inline int cmpForward_large(const u8 *p1, const u8 *p2, size_t len, char nocase)
{
    // 前32字节用标量（快速退出）
    const size_t SCALAR_PREFIX = 32;
    if (nocase) {
        for (size_t i = 0; i < SCALAR_PREFIX && i + CMP_SIZE_NEW <= len; i += CMP_SIZE_NEW) {
            if (TOUPPER(ULOAD(p1 + i)) != ULOAD(p2 + i)) {
                return 1;
            }
        }
    } else {
        for (size_t i = 0; i < SCALAR_PREFIX && i + CMP_SIZE_NEW <= len; i += CMP_SIZE_NEW) {
            if (ULOAD(p1 + i) != ULOAD(p2 + i)) {
                return 1;
            }
        }
    }
    // 中间部分用NEON并行
    const u8 *p1_cur = p1 + SCALAR_PREFIX;
    const u8 *p2_cur = p2 + SCALAR_PREFIX;
    const u8 *p1_neon_end = p1 + len - 16;
    if (nocase) {
        uint8x16_t lower = vdupq_n_u8('a');
        uint8x16_t upper = vdupq_n_u8('z');
        uint8x16_t mask = vdupq_n_u8(0x20);
        while (p1_cur <= p1_neon_end) {
            uint8x16_t d1 = vld1q_u8(p1_cur);
            uint8x16_t d2 = vld1q_u8(p2_cur);
            // 检测并转换小写字母
            uint8x16_t is_lower = vandq_u8(
                vcgeq_u8(d1, lower),
                vcleq_u8(d1, upper)
            );
            uint8x16_t toggled = veorq_u8(d1, mask);
            d1 = vbslq_u8(is_lower, toggled, d1);
            // 比较
            uint8x16_t cmp = vceqq_u8(d1, d2);
            uint64x2_t cmp64 = vreinterpretq_u64_u8(cmp);
            if (vgetq_lane_u64(cmp64, 0) != ~0uLL || vgetq_lane_u64(cmp64, 1) != ~0uLL) {
                return 1;
            }
            p1_cur += NEO_SIZE;
            p2_cur += NEO_SIZE;
        }
    } else {
        while (p1_cur <= p1_neon_end) {
            uint8x16_t d1 = vld1q_u8(p1_cur);
            uint8x16_t d2 = vld1q_u8(p2_cur);
            uint8x16_t cmp = vceqq_u8(d1, d2);
            uint64x2_t cmp64 = vreinterpretq_u64_u8(cmp);
            if (vgetq_lane_u64(cmp64, 0) != ~0uLL || vgetq_lane_u64(cmp64, 1) != ~0uLL) {
                return 1;
            }
            p1_cur += NEO_SIZE;
            p2_cur += NEO_SIZE;
        }
    }
    
    // 剩余尾部用标量
    size_t processed = p1_cur - p1;
    return cmpForward_small(p1 + processed, p2 + processed, len - processed, nocase);
}


static inline int cmpForward_PRO(const u8 *p1, const u8 *p2, size_t len, char nocase)
{
    size_t adaptiveSize = 96;
    if (len < adaptiveSize) {
        return cmpForward_small(p1, p2, len, nocase);
    } else {
        return cmpForward_large(p1, p2, len, nocase);
    }
}

#endif // COMPARE_H