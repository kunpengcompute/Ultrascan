/*
 * Copyright: Copyright (c) Huawei Technologies Co., Ltd. 2020-2022. All rights reserved.
 * Description: get or set cpufeature function
 * Create: 2025-05-15
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
#include "rose/rose_internal.h"

#ifdef __cplusplus
extern "C"
{
#endif


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

// LilyMatchItem相关常量
#define LILY_TO_OFFSET_MAX        (((unsigned long long)1 << LILY_TO_OFFSET_BITS) - 1U)
#define ALL_LILY_MATCH_ITEMS ((u64a)-1) // 标识上报所有Lily匹配项
// Lily缓存匹配项操作函数声明
void initLilyItems(hs_scratch_t *scratch);
int pushLilyItems(hs_scratch_t *scratch, const LilyMatchItem *item);
int flushStoredLilyMatches(hs_scratch_t *scratch, u64a to_offset);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KHSEL_RUNTIME_H_ */
