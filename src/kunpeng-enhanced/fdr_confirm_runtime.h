/*
 * Copyright (c) 2024 Huawei Technologies Co., Ltd.
 * Optimized KHSEL_ConfWithBit
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

#ifndef FDR_CONFIRM_RUNTIME_H
#define FDR_CONFIRM_RUNTIME_H

#include "scratch.h"
#include "khsel_fdr_internal.h"
#include "../fdr/fdr_confirm.h"
#include "khsel_hwlm.h"
#include "../ue2common.h"

static REALLY_INLINE void KHSEL_ConfWithBit(const struct FDRConfirm *fdrc, const struct FDR_Runtime_Args *a,
    size_t i, hwlmcb_rv_t *control, u32 *last_match_in, u64a conf_key, u64a *conf, u8 bit)
{
    const u8 * buf = a->buf;
    u32 c = CONF_HASH_CALL(conf_key, fdrc->andmsk, fdrc->mult,
                           fdrc->nBits);
    u32 start = getConfirmLitIndex(fdrc)[c];
    if (likely(!start)) {
        return;
    }
    const struct LitInfo *li
        = (const struct LitInfo *)((const u8 *)fdrc + start);

    struct hs_scratch *scratchT = a->scratch;
    scratchT->fdr_conf = conf;
    scratchT->fdr_conf_offset = bit;
    u8 oldNext; // initialized in loop
    do {
        if ((*last_match_in == li->id) && (li->flags & FDR_LIT_FLAG_NOREPEAT)) {
            goto out;
        }
        if (unlikely((conf_key & li->msk) != li->v)) {
            goto out;
        }
        const u8 *locT = buf + i - li->size + 1;
        if (locT < buf) {
            u32 fullOverhang = buf - locT;
            size_t lenHistory = a->len_history;

            if (fullOverhang > lenHistory) {
                goto out;
            }
        }
        if (unlikely(!(li->groups & *control))) {
            goto out;
        }
        *last_match_in = li->id;
        *control = a->cb(i, li->id, scratchT);
    out:
        oldNext = li->next; // oldNext is either 0 or an 'adjust' value
        li++;
    } while (oldNext);
    scratchT->fdr_conf = NULL;
}

#endif
