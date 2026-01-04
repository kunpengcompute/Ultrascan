 /*
 * Copyright (c) 2020-2022 Huawei Technologies Co., Ltd.
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
#ifndef KHSEL_CORE_H_
#define KHSEL_CORE_H_

#include "khsel_typebase.h"
#include "khsel_type.h"

#ifdef __cplusplus
extern "C" {
#endif


#define GCCVERSION_LTE(majar, minor, plvl) (__GNUC__ < (majar) || (__GNUC__ == (majar) && __GNUC_MINOR__ < (minor)) \
            || (__GNUC__ == (majar) && __GNUC_MINOR__ == (minor) && __GNUC_PATCHLEVEL__ <= (plvl)))

#ifdef __GNUC__
#if GCCVERSION_LTE(4, 8, 5)
    typedef int16_t   float16_t;
#else
#endif
#endif

typedef KhselResult(*function)(int32_t i, void *arg);

#ifdef __cplusplus
}
#endif


#endif /* KHSEL_CORE_H__ */
