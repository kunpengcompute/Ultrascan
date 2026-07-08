/*
 * Copyright (c) 2020 Huawei Technologies Co., Ltd.
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
#ifndef CORE_PRECOMP_H_
#define CORE_PRECOMP_H_

#include <stdio.h>
#include <stdint.h>

/* ****************************************************************************
 *  The definition of forceinline
 ******************************************************************************/
#if defined (__GNUC__) || defined(__cplusplus) || defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L   /* C99 */
#  define INLINE inline
#else
#  define INLINE
#endif

# define FORCE_ATTR_INLINE __attribute__((__always_inline__, __gnu_inline__, __artificial__))
#define FORCE_INLINE static INLINE FORCE_ATTR_INLINE
#define KHSEL_API_LOCAL __attribute__((visibility("hidden")))

/* ****************************************************************************
 *  Branch prediction
 ******************************************************************************/
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* **************************************************************************
 *  The definition used for avoiding the warning--"unused parameter"
 ****************************************************************************/
#define UNUSED_PARAM(param) (void)(param)

/* **************************************************************************
 *  The definition used for checking common inputs
 ****************************************************************************/
#define KHSEL_RETURN_IF_NULL(p, ret)                 \
    if ((p) == NULL) {                              \
        return (ret);                               \
    }


// Return if origin >= target
#define KHSEL_RETURN_IF_GE(origin, target, errCode)               \
    if ((origin) >= (target)) {                                  \
        return errCode;                                          \
    }


#ifndef RSHIFT
#define RSHIFT32(a, shift) ((a) >> (shift))     // shift >= 0, shift < 32
#define RSHIFT64(a, shift) ((a) >> (shift))     // shift >= 0, shift < 64
#define RSHIFT(a, shift) RSHIFT32(a, shift) // shift >= 0, shift < 32
#endif

// Return larger of a and b
#define KHSEL_MAX(a, b) (((a) >= (b)) ? (a) : (b))
#define KHSEL_MIN(a, b) (((a) <= (b)) ? (a) : (b))

#endif
