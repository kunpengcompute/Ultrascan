/*
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * rose-related modifications
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

#ifndef ROSE_INTERNAL_H
#define ROSE_INTERNAL_H

#include "ue2common.h"

typedef u64a rose_group;
#define KHSEL_ROSE_CONTINUE_MATCHING_NO_EXHAUST 2

struct RoseStateOffsets {
    u32 history;
    u32 exhausted;
    u32 exhausted_size;
    u32 logicalVec;
    u32 logicalVec_size;
    u32 combVec;
    u32 combVec_size;
    u32 activeLeafArray;
    u32 activeLeafArray_size;
    u32 activeLeftArray;
    u32 activeLeftArray_size;
    u32 leftfixLagTable;
    u32 anchorState;
    u32 groups;
    u32 groups_size;
    u32 longLitState;
    u32 longLitState_size;
    u32 somLocation;
    u32 somValid;
    u32 somWritable;
    u32 somMultibit_size;
    u32 nfaStateBegin;
    u32 end;
};

struct RoseBoundaryReports {
    u32 reportEodOffset;
    u32 reportZeroOffset;
    u32 reportZeroEodOffset;
};

struct scatter_full_plan {
    u32 s_u64a_offset;
    u32 s_u64a_count;
    u32 s_u32_offset;
    u32 s_u32_count;
    u32 s_u16_offset;
    u32 s_u16_count;
    u32 s_u8_count;
    u32 s_u8_offset;
};

struct RoseEngine {
    u8 pureLiteral;
    u8 noFloatingRoots;
    u8 requiresEodCheck;
    u8 hasOutfixesInSmallBlock;
    u8 runtimeImpl;
    u8 mpvTriggeredByLeaf;
    u8 canExhaust;
    u8 hasSom;
    u8 somHorizon;
    u32 mode;
    u32 historyRequired;
    u32 ekeyCount;
    u32 lkeyCount;
    u32 lopCount;
    u32 ckeyCount;
    u32 logicalTreeOffset;
    u32 combInfoMapOffset;
    u32 dkeyCount;
    u32 dkeyLogSize;
    u32 invDkeyOffset;
    u32 somLocationCount;
    u32 somLocationFatbitSize;
    u32 rolesWithStateCount;
    u32 stateSize;
    u32 anchorStateSize;
    u32 tStateSize;
    u32 scratchStateSize;
    u32 smallWriteOffset;
    u32 lilyOffset;
    u32 amatcherOffset;
    u32 ematcherOffset;
    u32 fmatcherOffset;
    u32 drmatcherOffset;
    u32 sbmatcherOffset;
    u32 longLitTableOffset;
    u32 amatcherMinWidth;
    u32 fmatcherMinWidth;
    u32 eodmatcherMinWidth;
    u32 amatcherMaxBiAnchoredWidth;
    u32 fmatcherMaxBiAnchoredWidth;
    u32 reportProgramOffset;
    u32 reportProgramCount;
    u32 delayProgramOffset;
    u32 anchoredProgramOffset;
    u32 activeArrayCount;
    u32 activeLeftCount;
    u32 queueCount;
    u32 activeQueueArraySize;
    u32 eagerIterOffset;
    u32 handledKeyCount;
    u32 handledKeyFatbitSize;
    u32 leftOffset;
    u32 roseCount;
    u32 eodProgramOffset;
    u32 flushCombProgramOffset;
    u32 lastFlushCombProgramOffset;
    u32 lastByteHistoryIterOffset;
    u32 minWidth;
    u32 minWidthExcludingBoundaries;
    u32 maxBiAnchoredWidth;
    u32 anchoredDistance;
    u32 anchoredMinDistance;
    u32 floatingDistance;
    u32 floatingMinDistance;
    u32 smallBlockDistance;
    u32 floatingMinLiteralMatchOffset;
    u32 nfaInfoOffset;
    rose_group initialGroups;
    rose_group floating_group_mask;
    u32 size;
    u32 delay_count;
    u32 delay_fatbit_size;
    u32 anchored_count;
    u32 anchored_fatbit_size;
    u32 maxFloatingDelayedMatch;
    u32 delayRebuildLength;
    struct RoseStateOffsets stateOffsets;
    struct RoseBoundaryReports boundary;
    u32 totalNumLiterals;
    u32 asize;
    u32 outfixBeginQueue;
    u32 outfixEndQueue;
    u32 leftfixBeginQueue;
    u32 initMpvNfa;
    u32 rosePrefixCount;
    u32 activeLeftIterOffset;
    u32 ematcherRegionSize;
    u32 somRevCount;
    u32 somRevOffsetOffset;
    u32 longLitStreamState;
    struct scatter_full_plan state_init;
};
#endif