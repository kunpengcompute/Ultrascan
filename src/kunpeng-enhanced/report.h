/*
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * report-related modifications
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

#ifndef REPORT_H
#define REPORT_H

#include "ue2common.h"
#include "../util/multibit.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define SCRATCH_STATUS_TERMINATED   (1U << 0)
#define SCRATCH_STATUS_EXHAUSTED    (1U << 1)
#define MAX_MQE_LEN 10
#define MIN_FAT_SIZE 32


static REALLY_INLINE
int IsExhausted(const struct RoseEngine *rose, const char *evec, u32 ekey)
{
    return mmbit_isset((const u8 *)evec, rose->ekeyCount, ekey);
}

static REALLY_INLINE
void MarkAsMatched(const struct RoseEngine *rose, char *evec, u32 ekey)
{
    mmbit_set((u8 *)evec, rose->ekeyCount, ekey);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* REPORT_H */

