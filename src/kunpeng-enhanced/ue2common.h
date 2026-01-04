/*
 * Copyright (c) 2024 Huawei Technologies Co., Ltd.
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

#ifndef UE2COMMON_H
#define UE2COMMON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __aarch64__
#define HAVE_NEON
#endif

/* ick */
#define KHSEL_ALIGN_ATTR(x) __attribute__((aligned((x))))

#define KHSEL_ALIGN_DIRECTIVE KHSEL_ALIGN_ATTR(16)
#define KHSEL_ALIGN_AVX_DIRECTIVE KHSEL_ALIGN_ATTR(32)
#define KHSEL_ALIGN_CL_DIRECTIVE KHSEL_ALIGN_ATTR(64)

#define KHSEL_ISALIGNED_N(ptr, n) (((uintptr_t)(ptr) & ((n) - 1)) == 0)
#define KHSEL_ISALIGNED_16(ptr)   KHSEL_ISALIGNED_N((ptr), 16)
#define KHSEL_ISALIGNED_CL(ptr)   KHSEL_ISALIGNED_N((ptr), 64)
// Align to N-byte boundary
#define KHSEL_ROUNDUP_N(a, n) (((a) + ((n)-1)) & ~((n)-1))
#define KHSEL_ROUNDDOWN_N(a, n) ((a) & ~((n)-1))
// Align to a cacheline - assumed to be 64 bytes
#define KHSEL_ROUNDUP_CL(a) KHSEL_ROUNDUP_N(a, 64)

#define UNUSED __attribute__ ((unused))
#define HS_CDECL

typedef signed int s32;
typedef unsigned int u32;
typedef signed short s16;
typedef unsigned short u16;
typedef signed char s8;
typedef unsigned char u8;

typedef unsigned long long KHSEL_ALIGN_ATTR(8) u64a;
typedef signed long long KHSEL_ALIGN_ATTR(8) s64a;

/* get the SIMD types */
#include "../src/core/include/simd_types.h"

typedef u32 ReportID;
// define HS_OPTIMIZE in cmakefile
#if defined(HS_OPTIMIZE)
#define REALLY_INLINE inline __attribute__ ((always_inline, unused))
#else
#define REALLY_INLINE __attribute__ ((unused))
#endif

#define REALLY_REALLY_INLINE inline __attribute__ ((always_inline, unused))
#define NEVER_INLINE __attribute__ ((noinline))
#define ALIGNOF __alignof
#define HAVE_TYPEOF 1

#include <assert.h>

#endif
