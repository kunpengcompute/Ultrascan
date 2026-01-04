/*
 * Copyright (c) 2024 Huawei Technologies Co., Ltd.
 * Optimized KHSEL_GetConfirmLitIndex
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
#ifndef FDR_CONFIRM_H
#define FDR_CONFIRM_H

#include "ue2common.h"
#include "khsel_hwlm.h"

static REALLY_INLINE u32 KHSEL_MulHash64(u64a lv, u64a andmsk, u64a mult, u32 nBits) {
    return ((lv & andmsk) * mult) >> (sizeof(u64a)*8 - nBits);
}

#define CONF_TYPE u64a
#define KHSEL_CONF_HASH_CALL KHSEL_MulHash64
#define KHSEL_FDR_LIT_FLAG_NOREPEAT   1

struct LitInfo {
    CONF_TYPE v;
    CONF_TYPE msk;
    hwlm_group_t groups;
    u32 id;
    u8 size;
    u8 flags;
    u8 next;
};

#define FDRC_FLAG_NO_CONFIRM 1
#define FDRC_FLAG_NOREPEAT   2

struct FDRConfirm {
    CONF_TYPE andmsk;
    CONF_TYPE mult;
    u32 nBits;
    hwlm_group_t groups;
};

static REALLY_INLINE const u32 *KHSEL_GetConfirmLitIndex(const struct FDRConfirm *fdrc) {
    const u8 *base = (const u8 *)fdrc;
    const u32 *litIndex =
        (const u32 *)(base + KHSEL_ROUNDUP_N(sizeof(*fdrc), ALIGNOF(u32)));
    return litIndex;
}

#endif // FDR_CONFIRM_H
