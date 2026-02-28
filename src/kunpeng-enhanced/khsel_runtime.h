/*
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * new lily runtime function
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

#ifndef KHSEL_RUNTIME_H_
#define KHSEL_RUNTIME_H_

#include <stdlib.h>

/**
 * @file
 * @brief The Hyperscan runtime API definition.
 *
 * Hyperscan is a high speed regular expression engine.
 *
 * This header contains functions for using compiled Hyperscan databases for
 * scanning data at runtime.
 */
#include "rose_internal.h"
#include "scratch.h"
#include "simd_types.h"
#include "simd_arm.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef int hs_error_t;
typedef struct hs_scratch hs_scratch_t;

// 引擎枚举：仅2个值，适配1bit位域，贴合Hyperscan引擎命名
enum HsEngine {
    HS_ENGINE_LILY = 0,  // lily引擎标识（0）
    HS_ENGINE_LILY_FOR_TEDDY  = 1   // lilyForTeddy引擎标识（1）
};

#define BYTE_SIZE_FOUR 4
#define LILY_VEC_LEN 8
// LilyMatchItem相关常量
#define LILY_TO_OFFSET_MAX        (((unsigned long long)1 << LILY_TO_OFFSET_BITS) - 1U)
#define INVALID_EKEY    (~(u32)0)
#define KHSEL_MO_HALT_MATCHING 0
#define KHSEL_MO_CONTINUE_MATCHING 1
#define KHSEL_MATCHING_TERMINATED 1
#define KHSEL_MATCHING_SUCCESS 0
/**
 * The block / streaming regular expression scanner.
 *
 * This is the function call in which the actual pattern matching takes place
 * for block-mode pattern databases.
 *
 * @param rose
 *      ROSE.
 *
 *
 * @param scratch
 *      A per-thread scratch space allocated by @ref hs_alloc_scratch() for this
 *      database.
 *
 * @return
 *      Returns @ref HS_SUCCESS on success; @ref HS_SCAN_TERMINATED if the
 *      match callback indicated that scanning should stop; other values on
 *      error.
 */
hs_error_t KHSEL_LilyRunExec(const struct RoseEngine *rose, hs_scratch_t *scratch);
hs_error_t KHSEL_LilyForTeddyRunExec(const struct RoseEngine *rose, hs_scratch_t *scratch);


#define ALL_LILY_MATCH_ITEMS ((u64a)-1) // 标识上报所有Lily匹配项
// Lily缓存匹配项操作函数声明
void initLilyItems(hs_scratch_t *scratch);
int pushLilyItems(const LilyMatchItem *item, LilyEngineCtx *ctx);
int flushStoredLilyMatches(hs_scratch_t *scratch, u64a to_offset);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KHSEL_RUNTIME_H_ */
