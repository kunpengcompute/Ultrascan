/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 */

#ifndef FDR_ENHANCED
#define FDR_ENHANCED

#include <stdio.h>
#include "ue2common.h"

#ifdef __aarch64__
#define NO_ASM
#endif

// C linkage in the API
#ifdef __cplusplus
extern "C" {
#endif

hwlm_error_t KHSEL_FdrEngineExec(const struct FDR *fdr,
                             const struct FDR_Runtime_Args *a,
                             hwlm_group_t control);


#ifdef __cplusplus
}
#endif // __cplusplus

#endif // FDR_ENHANCED