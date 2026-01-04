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
#ifndef KHSEL_TYPEBASE_H_
#define KHSEL_TYPEBASE_H_

#include <arm_neon.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int8_t  re;
    int8_t  im;
} Khsel8sc;

typedef struct {
    int16_t  re;
    int16_t  im;
} Khsel16sc;

typedef struct {
    uint16_t  re;
    uint16_t  im;
} Khsel16uc;

typedef struct {
    int32_t  re;
    int32_t  im;
} Khsel32sc;

typedef struct {
    float  re;
    float  im;
} Khsel32fc;

typedef struct {
    int64_t  re;
    int64_t  im;
} Khsel64sc;

typedef struct {
    double  re;
    double  im;
} Khsel64fc;

typedef enum {
    KHSEL_FALSE = 0,
    KHSEL_TRUE = 1
} KhselBool;

#ifdef __cplusplus
}
#endif

#endif /* KHSEL_TYPEBASE_H__ */

