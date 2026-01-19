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
#ifndef REPLACE_ALL_ACC_H_
#define REPLACE_ALL_ACC_H_
#include <string>
#include <unistd.h>
#include "arm_neon.h"
#include "khsel_ops.h"

#define ALIGN_ATTR(x) __attribute__((aligned((x))))

#if defined(HS_OPTIMIZE)
#define REALLY_INLINE inline __attribute__((always_inline, unused))
#else
#define REALLY_INLINE __attribute__((unused))
#endif

typedef signed char s8;
typedef unsigned char u8;
typedef signed short s16;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed int s32;
typedef unsigned long long ALIGN_ATTR(8) u64a;
typedef signed long long ALIGN_ATTR(8) s64a;

typedef union {
    int8x16_t vect_s8;
    int16x8_t vect_s16;
    int32x4_t vect_s32;
    int64x2_t vect_s64;
    uint8x16_t vect_u8;
    uint16x8_t vect_u16;
    uint32x4_t vect_u32;
    uint64x2_t vect_u64;
} __m128i;
typedef float32x4_t __m128;
typedef float64x2_t __m128d;

typedef __m128i m128;

#ifdef __cplusplus
#ifdef HAVE_CXX_BUILTIN_ASSUME_ALIGNED
#define assume_aligned(a, b) __builtin_assume_aligned((a), (b))
#endif
#else
#ifdef HAVE_CC_BUILTIN_ASSUME_ALIGNED
#define assume_aligned(a, b) __builtin_assume_aligned((a), (b))
#endif
#endif

#ifndef assume_aligned
#define assume_aligned(a, b) (a)
#endif

#endif
