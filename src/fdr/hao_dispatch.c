/* Copyright (c) 2026, Huawei Technologies Co., Ltd. */

#include "fdr_internal.h"
#include "hao_runtime.h"
#include "hao_runtime_inline.h"
#include "hao_runtime_layout.h"
#include "util/cpuid_flags.h"

u32 haoSelectPath(const struct HAOVariants *variants, u64a features,
                  u64a kernels, u32 vl) {
    const u64a bext =
        HS_CPU_FEATURES_SVE | HS_CPU_FEATURES_SVE2 | HS_CPU_FEATURES_SVEBITPERM;
    const u64a available = features & kernels;
    // Four 64-bit verifier slots require at least 256 bits in this layout.
    if (!variants || vl < 32U) {
        return HAO_PATH_FDR;
    }
    if (variants->bextSize && (available & bext) == bext) {
        return HAO_PATH_BEXT;
    }
    if (variants->dotSize && (available & HS_CPU_FEATURES_SVE)) {
        return HAO_PATH_DOT;
    }
    return HAO_PATH_FDR;
}

static u64a haoKernels(void) {
    u64a features = 0;
#ifdef HS_BUILD_HAVE_SVE
    features |= HS_CPU_FEATURES_SVE;
#endif
#ifdef HS_BUILD_HAVE_SVEBITPERM
    features |=
        HS_CPU_FEATURES_SVE | HS_CPU_FEATURES_SVE2 | HS_CPU_FEATURES_SVEBITPERM;
#endif
    return features;
}

static u32 haoVl(u64a features, u64a kernels) {
#ifdef HS_BUILD_HAVE_SVE
    if (features & kernels & HS_CPU_FEATURES_SVE) {
        return HaoVectorLength();
    }
#else
    (void)features;
    (void)kernels;
#endif
    return 0;
}

static const void *haoBlob(const struct FDR *fdr) {
    if (!fdr || !fdr->matcherBlobOffset || (fdr->matcherBlobOffset & 63U) ||
        (u64a)fdr->matcherBlobOffset + fdr->matcherBlobSize > fdr->size) {
        return NULL;
    }
    return (const u8 *)fdr + fdr->matcherBlobOffset;
}

static hwlm_error_t haoRun(const struct HAORuntimeHeader *hdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t control, u32 path) {
#ifdef HS_BUILD_HAVE_SVE
    if (path == HAO_PATH_DOT) {
        return HaoDotExec(hdr, a, control);
    }
#endif
#ifdef HS_BUILD_HAVE_SVEBITPERM
    if (path == HAO_PATH_BEXT) {
        return HaoBextExec(hdr, a, control);
    }
#endif
    (void)hdr;
    (void)a;
    (void)control;
    (void)path;
    return HWLM_TERMINATED;
}

hwlm_error_t HaoVariantsExec(const struct FDR *fdr,
                             const struct FDR_Runtime_Args *a,
                             hwlm_group_t control) {
    const struct HAOVariants *variants = NULL;
    const void *blob = haoBlob(fdr);
    if (!blob || !a ||
        !haoValidateVariants(blob, fdr->matcherBlobSize, &variants)) {
        return HWLM_TERMINATED;
    }
    const u64a features = cpuid_flags();
    const u64a kernels = haoKernels();
    const u32 path =
        haoSelectPath(variants, features, kernels, haoVl(features, kernels));
    if (path == HAO_PATH_FDR) {
        return FdrEngineExec(fdr, a, control, variants->fallback);
    }
    const u32 offset =
        path == HAO_PATH_DOT ? variants->dotOffset : variants->bextOffset;
    return haoRun((const struct HAORuntimeHeader *)((const u8 *)blob + offset),
                  a, control, path);
}

/* Preserve the standalone HAO entry and its invalid-input behavior. */
hwlm_error_t HaoEngineExec(const struct FDR *fdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t control) {
    const struct HAORuntimeHeader *hdr = NULL;
    const void *blob = haoBlob(fdr);
    if (!blob || !a || !haoValidateLayout(blob, fdr->matcherBlobSize, &hdr)) {
        return HWLM_SUCCESS;
    }
    struct HAOVariants variants = {0};
    if (haoRuntimeHeaderHashMode(hdr) == HAO_RUNTIME_HASH_DOT) {
        variants.dotSize = fdr->matcherBlobSize;
    } else {
        variants.bextSize = fdr->matcherBlobSize;
    }
    const u64a features = cpuid_flags();
    const u64a kernels = haoKernels();
    const u32 path =
        haoSelectPath(&variants, features, kernels, haoVl(features, kernels));
    if (path == HAO_PATH_FDR) {
        return FdrEngineExec(fdr, a, control, 0);
    }
    return haoRun(hdr, a, control, path);
}
