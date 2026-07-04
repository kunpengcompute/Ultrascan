/*
 * Copyright (c) 2015-2020, Intel Corporation
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

/** \file
 * \brief Runtime code for hs_database common manipulation.
 */

#ifndef DATABASE_COMMMON_H_D467FD6F343DDA
#define DATABASE_COMMMON_H_D467FD6F343DDA

#ifdef __cplusplus
extern "C"
{
#endif

#include "hs_compile.h" // for HS_MODE_ flags
#include "hs_version.h"
#include "ue2common.h"
#include "util/arch.h"

#define HS_DB_VERSION HS_VERSION_32BIT
#define HS_DB_MAGIC   (0xdbdbdbdbU)

// Values in here cannot (easily) change - add new ones!

// CPU type is the low 6 bits (we can't need more than 64, surely!)

#define HS_PLATFORM_INTEL           1
#define HS_PLATFORM_CPU_MASK        0x3F

#define HS_PLATFORM_NOAVX2          (4<<13)
#define HS_PLATFORM_NOAVX512        (8<<13)
#define HS_PLATFORM_NOAVX512VBMI    (0x10<<13)

/** \brief Platform features bitmask. */
typedef u64a platform_t;

static UNUSED
const platform_t hs_current_platform = {
#if !defined(HAVE_AVX2)
    HS_PLATFORM_NOAVX2 |
#endif
#if !defined(HAVE_AVX512)
    HS_PLATFORM_NOAVX512 |
#endif
#if !defined(HAVE_AVX512VBMI)
    HS_PLATFORM_NOAVX512VBMI |
#endif
    0,
};

static UNUSED
const platform_t hs_current_platform_no_avx2 = {
    HS_PLATFORM_NOAVX2 |
    HS_PLATFORM_NOAVX512 |
    HS_PLATFORM_NOAVX512VBMI |
    0,
};

static UNUSED
const platform_t hs_current_platform_no_avx512 = {
    HS_PLATFORM_NOAVX512 |
    HS_PLATFORM_NOAVX512VBMI |
    0,
};

static UNUSED
const platform_t hs_current_platform_no_avx512vbmi = {
    HS_PLATFORM_NOAVX512VBMI |
    0,
};


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DATABASE_H_D467FD6F343DDE */
