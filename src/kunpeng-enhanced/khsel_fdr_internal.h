/*
 * Copyright (c) 2024 Huawei Technologies Co., Ltd.
 * Redefine some structures
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

#ifndef FDR_INTERNAL_H
#define FDR_INTERNAL_H

#include "ue2common.h"
#include "../hwlm/hwlm.h" // for hwlm_group_t, HWLMCallback

struct hs_scratch;

typedef enum {
    NOT_CAUTIOUS, //!< not near a boundary (quantify?)
    VECTORING     //!< potentially vectoring
} CautionReason;

#define FDR_FLOOD_MAX_IDS 16

struct FDR {
    u32 engineID;
    u32 size;
    u32 maxStringLen;
    u32 numStrings;
    u32 confOffset;
    u32 floodOffset;
    u8 stride;
    u8 domain;
    u16 domainMask;
    u32 tabSize;
    m128 start;
};

struct FDR_Runtime_Args {
    const u8 *buf;
    size_t len;
    const u8 *buf_history;
    size_t len_history;
    size_t start_offset;
    HWLMCallback cb;
    struct hs_scratch *scratch;
    const u8 *firstFloodDetect;
    const u64a histBytes;
};

#endif
