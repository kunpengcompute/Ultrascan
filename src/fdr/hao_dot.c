/* Copyright (c) 2026, Huawei Technologies Co., Ltd. */

#include "hao_runtime.h"

__attribute__((visibility("hidden"))) hwlm_error_t
HaoDotExecBody(const struct HAORuntimeHeader *, const struct FDR_Runtime_Args *,
               hwlm_group_t);

#define HAO_EXEC HaoDotExecBody
#define HAO_HASH_MODE HAO_RUNTIME_HASH_DOT
#include "hao_engine.c"

u32 HaoVectorLength(void) { return (u32)svcntb(); }
