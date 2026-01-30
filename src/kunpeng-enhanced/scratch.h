/*
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * hs_scratch-related modifications
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

#ifndef SCRATCH_H
#define SCRATCH_H

#include "ue2common.h"

#ifdef __cplusplus
extern "C"
{
#endif


#define SCRATCH_STATUS_TERMINATED   (1U << 0)
#define SCRATCH_STATUS_EXHAUSTED    (1U << 1)
#define MAX_MQE_LEN 10
#define MIN_FAT_SIZE 32
struct fatbit {
    union {
        u64a flat[MIN_FAT_SIZE / sizeof(u64a)];
        u8 raw[MIN_FAT_SIZE];
    } fb_int;
    u64a tail[];
};

/** Queue item */
struct mq_item {
    u32 type;
    s64a location;
    u64a som;
};

struct fatbit;
struct hs_scratch;
struct RoseEngine;
struct mq;

struct queue_match {
    size_t loc;

    u32 queue; /**< queue index. */
};

#define LILY_REPORT_INDEX_BITS    3U
#define LILY_TO_OFFSET_BITS       61U
// LilyMatchItem结构体（8字节位域）

struct LilyMatchItem {
    unsigned long long onmatch_index : LILY_REPORT_INDEX_BITS; // 低3bit：ReportID索引(0~7)
    unsigned long long toOffset      : LILY_TO_OFFSET_BITS;    // 剩余61bit：toOffset值
};
typedef struct LilyMatchItem LilyMatchItem;

struct catchup_pq {
    struct queue_match *qm;
    u32 qm_size; /**< current size of the priority queue */
};

typedef int (*NfaCallback)(u64a start, u64a end, ReportID id, void *context);

struct mq {
    const struct NFA *nfa;
    u32 cur;
    u32 end;
    char *state;
    char *streamState;
    u64a offset;
    const u8 *buffer;
    size_t length;
    const u8 *history;
    size_t hlength;
    struct hs_scratch *scratch;
    char report_current;
    void *context;
    struct mq_item items[MAX_MQE_LEN];
};


struct core_info {
    void *userContext;
    int (HS_CDECL *userCallback)(unsigned int id, unsigned long long from,
                                 unsigned long long to, unsigned int flags,
                                 void *ctx);

    const struct RoseEngine *rose;
    char *state;
    char *exhaustionVector;
    char *logicalVector;
    char *combVector;
    const u8 *buf;
    size_t len;
    const u8 *hbuf;
    size_t hlen;
    u64a buf_offset;
    u8 status;
};

/** \brief Rose state information. */
struct RoseContext {
    u8 mpv_inactive;
    u64a groups;
    u64a lit_offset_adjust;
    u64a delayLastEndOffset;
    u64a lastEndOffset;
    u64a lastMatchOffset;
    u64a lastCombMatchOffset;
    u64a minMatchOffset;
    u64a minNonMpvMatchOffset;
    u64a next_mpv_offset;
    u32 filledDelayedSlots;
    u32 curr_qi;
    const u8 *ll_buf;
    size_t ll_len;
    const u8 *ll_buf_nocase;
    size_t ll_len_nocase;
};

struct match_deduper {
    struct fatbit *log[2];
    struct fatbit *som_log[2];
    u64a *som_start_log[2];
    u32 dkey_count;
    u32 log_size;
    u64a current_report_offset;
    u8 som_log_dirty;
};

struct KHSEL_ALIGN_CL_DIRECTIVE hs_scratch {
    u32 magic;
    u8 in_use;
    u32 queueCount;
    u32 activeQueueArraySize;
    u32 bStateSize;
    u32 tStateSize;
    u32 fullStateSize;
    struct RoseContext tctxt;
    char *bstate;
    char *tstate;
    char *fullState;
    struct mq *queues;
    struct fatbit *aqa;
    struct fatbit **delay_slots;
    struct fatbit **al_log;
    u64a al_log_sum;
    struct catchup_pq catchup_pq;
    struct core_info core_info;
    struct match_deduper deduper;
    u32 anchored_literal_fatbit_size;
    u32 anchored_literal_region_len;
    struct fatbit *handled_roles;
    u64a *som_store;
    u64a *som_attempted_store;
    struct fatbit *som_set_now;
    struct fatbit *som_attempted_set;
    u64a som_set_now_offset;
    u32 som_store_count;
    u32 som_fatbit_size;
    u32 handledKeyFatbitSize;
    u32 delay_fatbit_size;
    u32 scratchSize;
    char *scratch_alloc;
    u64a *fdr_conf;
    u8 fdr_conf_offset;
    struct LilyMatchItem *lily_items;  // 数组用于暂存lily匹配项
    size_t lily_items_start; // 未上报的起始下标，初始为0
    size_t lily_items_size;          // 当前元素数
    size_t lily_items_capacity;      // 容量（预分配）
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SCRATCH_H */

