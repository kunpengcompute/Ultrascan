/*
 * Copyright (c) 2024 Huawei Technologies Co., Ltd.
 * Redefine macros
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

#ifndef HWLM_H
#define HWLM_H

#include "ue2common.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef int hwlm_error_t;
typedef u64a hwlm_group_t;
typedef hwlm_group_t hwlmcb_rv_t;

#define KHSEL_HWLM_ALL_GROUPS         ((hwlm_group_t)~0ULL)
#define KHSEL_HWLM_CONTINUE_MATCHING  KHSEL_HWLM_ALL_GROUPS
#define KHSEL_HWLM_TERMINATE_MATCHING 0
#define KHSEL_HWLM_SUCCESS       0
#define KHSEL_HWLM_TERMINATED    1
#define KHSEL_HWLM_ERROR_UNKNOWN 2
#define KHSEL_HWLM_LITERAL_MAX_LEN 8

struct hs_scratch;
struct HWLM;

typedef hwlmcb_rv_t (*HWLMCallback)(size_t end, u32 id, struct hs_scratch *scratch);

#ifdef __cplusplus
}       /* extern "C" */
#endif

#endif
