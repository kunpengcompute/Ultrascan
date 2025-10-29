/*
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

#include "unaligned.h"
#include "ue2common.h"

/* Our own definitions of tolower, toupper and isalpha are provided to prevent
 * us from going out to libc for these tests. */

static really_inline
char myisupper(const char c) {
    return ((c >= 'A') && (c <= 'Z'));
}

static really_inline
char myislower(const char c) {
    return ((c >= 'a') && (c <= 'z'));
}

static really_inline
char mytolower(const char c) {
    if (myisupper(c)) {
        return c + 0x20;
    }
    return c;
}

static really_inline
char mytoupper(const char c) {
    if (myislower(c)) {
        return c - 0x20;
    }
    return c;
}

/* this is a slightly warped definition of `alpha'. What we really
 * mean is: does this character have different uppercase and lowercase forms?
 */
static really_inline char ourisalpha(const char c) {
    return mytolower(c) != mytoupper(c);
}

static really_inline char ourisprint(const char c) {
    return c >= 0x20 && c <= 0x7e;
}

// Paul Hsieh's SWAR toupper; used because it doesn't
// matter whether we go toupper or tolower. We should
// probably change the other one
static really_inline
u32 theirtoupper32(const u32 x) {
    u32 b = 0x80808080ul | x;
    u32 c = b - 0x61616161ul;
    u32 d = ~(b - 0x7b7b7b7bul);
    u32 e = (c & d) & (~x & 0x80808080ul);
    return x - (e >> 2);
}

// 64-bit variant.
static really_inline
u64a theirtoupper64(const u64a x) {
    u64a b = 0x8080808080808080ull | x;
    u64a c = b - 0x6161616161616161ull;
    u64a d = ~(b - 0x7b7b7b7b7b7b7b7bull);
    u64a e = (c & d) & (~x & 0x8080808080808080ull);
    u64a v = x - (e >> 2);
    return v;
}

static really_inline
int cmpNocaseNaive(const u8 *p1, const u8 *p2, size_t len) {
    const u8 *pEnd = p1 + len;
    for (; p1 < pEnd; p1++, p2++) {
        assert(!ourisalpha(*p2) || myisupper(*p2)); // Already upper-case.
        if ((u8)mytoupper(*p1) != *p2) {
            return 1;
        }
    }
    return 0;
}

static really_inline
int cmpCaseNaive(const u8 *p1, const u8 *p2, size_t len) {
    const u8 *pEnd = p1 + len;
    for (; p1 < pEnd; p1++, p2++) {
        if (*p1 != *p2) {
            return 1;
        }
    }
    return 0;
}

#ifdef ARCH_64_BIT
#  define CMP_T        u64a
#  define ULOAD(x)     unaligned_load_u64a(x)
#  define TOUPPER(x)   theirtoupper64(x)
#else
#  define CMP_T        u32
#  define ULOAD(x)     unaligned_load_u32(x)
#  define TOUPPER(x)   theirtoupper32(x)
#endif

#define CMP_SIZE sizeof(CMP_T)

/**
 * \brief Compare two strings, optionally caselessly.
 *
 * Note: If nocase is true, p2 is assumed to be already upper-case.
 */
#if defined(ARCH_IA32)
static UNUSED never_inline
#else
static really_inline
#endif
int cmpForward(const u8 *p1, const u8 *p2, size_t len, char nocase) {
    if (len < CMP_SIZE) {
        return nocase ? cmpNocaseNaive(p1, p2, len)
                      : cmpCaseNaive(p1, p2, len);
    }

    const u8 *p1_end = p1 + len - CMP_SIZE;
    const u8 *p2_end = p2 + len - CMP_SIZE;

    if (nocase) { // Case-insensitive version.
        for (; p1 < p1_end; p1 += CMP_SIZE, p2 += CMP_SIZE) {
            assert(ULOAD(p2) == TOUPPER(ULOAD(p2))); // Already upper-case.
            if (TOUPPER(ULOAD(p1)) != ULOAD(p2)) {
                return 1;
            }
        }
        assert(ULOAD(p2_end) == TOUPPER(ULOAD(p2_end))); // Already upper-case.
        if (TOUPPER(ULOAD(p1_end)) != ULOAD(p2_end)) {
            return 1;
        }
    } else { // Case-sensitive version.
        for (; p1 < p1_end; p1 += CMP_SIZE, p2 += CMP_SIZE) {
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

#if defined(HAVE_NEON)
#define NEON_SIZE 16
#define UNROLL_FACTOR 4

static really_inline
int cmpForward_optimized_unaligned(const uint8_t *p1, const uint8_t *p2, size_t len, char nocase) {
    
    // 小数据使用朴素算法
    if (len < 4) {
        return nocase ? cmpNocaseNaive(p1, p2, len)
                     : cmpCaseNaive(p1, p2, len);
    }

    const size_t step = UNROLL_FACTOR * CMP_SIZE;
    const u8 *p1_end = p1 + len - step;
    const u8 *p1_end_raw = p1 + len - CMP_SIZE;
    const u8 *p2_end_raw = p2 + len - CMP_SIZE;


    // Case-sensitive
    if (!nocase) {
        for (; p1 < p1_end; p1 += step, p2 += step) {
            if (unlikely(ULOAD(p1) != ULOAD(p2))) return 1;
            if (unlikely(ULOAD(p1 + CMP_SIZE) != ULOAD(p2 + CMP_SIZE))) return 1;
            if (unlikely(ULOAD(p1 + 2*CMP_SIZE) != ULOAD(p2 + 2*CMP_SIZE))) return 1;
            if (unlikely(ULOAD(p1 + 3*CMP_SIZE) != ULOAD(p2 + 3*CMP_SIZE))) return 1;
        }

        
        for (; p1 < p1_end_raw; p1 += CMP_SIZE, p2 += CMP_SIZE) {
            if (unlikely(ULOAD(p1) != ULOAD(p2))) {
                return 1;
            }
        }

        if (unlikely(ULOAD(p1_end_raw) != ULOAD(p2_end_raw))) {
            return 1;
        }
    } else {
        for (; p1 < p1_end_raw; p1 += CMP_SIZE, p2 += CMP_SIZE) {
            if (TOUPPER(ULOAD(p1)) != ULOAD(p2)) {
                return 1;
            }
        }
        if (TOUPPER(ULOAD(p1_end_raw)) != ULOAD(p2_end_raw)) {
            return 1;
        }
    }

    return 0;
}

#endif

static really_inline
int cmpForward_small(const u8 *p1, const u8 *p2, size_t len, char nocase) {
    if (len < CMP_SIZE) {
        return nocase ? cmpNocaseNaive(p1, p2, len)
                      : cmpCaseNaive(p1, p2, len);
    }

    const u8 *p1_end = p1 + len - CMP_SIZE;
    const u8 *p2_end = p2 + len - CMP_SIZE;

    if (nocase) {
        for (; p1 < p1_end; p1 += CMP_SIZE, p2 += CMP_SIZE) {
            if (TOUPPER(ULOAD(p1)) != ULOAD(p2)) return 1;
        }
        if (TOUPPER(ULOAD(p1_end)) != ULOAD(p2_end)) return 1;
    } else {
        for (; p1 < p1_end; p1 += CMP_SIZE, p2 += CMP_SIZE) {
            if (ULOAD(p1) != ULOAD(p2)) return 1;
        }
        if (ULOAD(p1_end) != ULOAD(p2_end)) return 1;
    }

    return 0;
}

// 大字符串（≥96字节）：混合策略（标量前缀 + NEON主体）
static really_inline
int cmpForward_large(const u8 *p1, const u8 *p2, size_t len, char nocase) {
    // 前32字节用标量（快速退出）
    const size_t SCALAR_PREFIX = 32;
    
    if (nocase) {
        for (size_t i = 0; i < SCALAR_PREFIX && i + CMP_SIZE <= len; i += CMP_SIZE) {
            if (TOUPPER(ULOAD(p1 + i)) != ULOAD(p2 + i)) return 1;
        }
    } else {
        for (size_t i = 0; i < SCALAR_PREFIX && i + CMP_SIZE <= len; i += CMP_SIZE) {
            if (ULOAD(p1 + i) != ULOAD(p2 + i)) return 1;
        }
    }
    
    // 中间部分用NEON
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
            
            if (vgetq_lane_u64(cmp64, 0) != ~0ULL || 
                vgetq_lane_u64(cmp64, 1) != ~0ULL) {
                return 1;
            }
            
            p1_cur += 16;
            p2_cur += 16;
        }
    } else {
        while (p1_cur <= p1_neon_end) {
            uint8x16_t d1 = vld1q_u8(p1_cur);
            uint8x16_t d2 = vld1q_u8(p2_cur);
            uint8x16_t cmp = vceqq_u8(d1, d2);
            uint64x2_t cmp64 = vreinterpretq_u64_u8(cmp);
            
            if (vgetq_lane_u64(cmp64, 0) != ~0ULL || 
                vgetq_lane_u64(cmp64, 1) != ~0ULL) {
                return 1;
            }
            
            p1_cur += 16;
            p2_cur += 16;
        }
    }
    
    // 剩余尾部用标量
    size_t processed = p1_cur - p1;
    return cmpForward_small(p1 + processed, p2 + processed, len - processed, nocase);
}

static really_inline
int cmpForward_PRO(const u8 *p1, const u8 *p2, size_t len, char nocase) {
    if (len < 96) {
        return cmpForward_small(p1, p2, len, nocase);
    } else {
        return cmpForward_large(p1, p2, len, nocase);
    }
}


#undef CMP_T
#undef ULOAD
#undef TOUPPER
#undef CMP_SIZE

#endif

