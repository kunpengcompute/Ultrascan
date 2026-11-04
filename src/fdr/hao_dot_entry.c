/* Copyright (c) 2026, Huawei Technologies Co., Ltd. */

#include "hao_runtime.h"
#include <arm_sve.h>

__attribute__((visibility("hidden"))) hwlm_error_t
HaoDotExecBody(const struct HAORuntimeHeader *, const struct FDR_Runtime_Args *,
               hwlm_group_t);

/* Preserve each V register while normalizing caller-saved upper SVE state.
 * V8..V15 keep all low128 bits; omit their clobbers to avoid D-only reloads.
 * Keep this before entry to the DOT body, with no DOT SVE temporaries live. */
static never_inline void haoClearUpper(void) {
    __asm__ volatile("mov v0.16b, v0.16b\n\t"
                     "mov v1.16b, v1.16b\n\t"
                     "mov v2.16b, v2.16b\n\t"
                     "mov v3.16b, v3.16b\n\t"
                     "mov v4.16b, v4.16b\n\t"
                     "mov v5.16b, v5.16b\n\t"
                     "mov v6.16b, v6.16b\n\t"
                     "mov v7.16b, v7.16b\n\t"
                     "mov v8.16b, v8.16b\n\t"
                     "mov v9.16b, v9.16b\n\t"
                     "mov v10.16b, v10.16b\n\t"
                     "mov v11.16b, v11.16b\n\t"
                     "mov v12.16b, v12.16b\n\t"
                     "mov v13.16b, v13.16b\n\t"
                     "mov v14.16b, v14.16b\n\t"
                     "mov v15.16b, v15.16b\n\t"
                     "mov v16.16b, v16.16b\n\t"
                     "mov v17.16b, v17.16b\n\t"
                     "mov v18.16b, v18.16b\n\t"
                     "mov v19.16b, v19.16b\n\t"
                     "mov v20.16b, v20.16b\n\t"
                     "mov v21.16b, v21.16b\n\t"
                     "mov v22.16b, v22.16b\n\t"
                     "mov v23.16b, v23.16b\n\t"
                     "mov v24.16b, v24.16b\n\t"
                     "mov v25.16b, v25.16b\n\t"
                     "mov v26.16b, v26.16b\n\t"
                     "mov v27.16b, v27.16b\n\t"
                     "mov v28.16b, v28.16b\n\t"
                     "mov v29.16b, v29.16b\n\t"
                     "mov v30.16b, v30.16b\n\t"
                     "mov v31.16b, v31.16b\n\t"
                     :
                     :
                     : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16",
                       "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24",
                       "v25", "v26", "v27", "v28", "v29", "v30", "v31",
                       "memory");
}

hwlm_error_t HaoDotExec(const struct HAORuntimeHeader *hdr,
                        const struct FDR_Runtime_Args *a,
                        hwlm_group_t control) {
    if (svcntb() > 16) {
        haoClearUpper();
    }
    return HaoDotExecBody(hdr, a, control);
}
