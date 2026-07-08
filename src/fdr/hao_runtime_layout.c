/*
 * Copyright (c) 2026, Huawei Technologies Co., Ltd.
 */

#include "hao_runtime_layout.h"
#include "hao_runtime_inline.h"

#if defined(__ARM_FEATURE_SVE2_BITPERM)
#define HAO_HAVE_SVEBITPERM 1
#endif

/* Validate the HAO blob layout before entering execution. */
int haoValidateLayout(const void *blob, u32 blobSize,
                             const struct HAORuntimeHeader **outHdr) {
    const struct HAORuntimeHeader *hdr;
    u32 keyBits;
    u32 hashMode;

    if (!blob || blobSize < sizeof(struct HAORuntimeHeader)) {
        return 0;
    }

    hdr = (const struct HAORuntimeHeader *)blob;
    if (hdr->magic != HAO_RUNTIME_MAGIC ||
        hdr->version != HAO_RUNTIME_VERSION) {
        return 0;
    }
    keyBits = haoRuntimeHeaderKeyBits(hdr);
    hashMode = haoRuntimeHeaderHashMode(hdr);

    if (!keyBits || !hdr->primaryCount || !hdr->l2EntryCount) {
        return 0;
    }
    if (keyBits > HAO_RUNTIME_MAX_SELECTORS) {
        return 0;
    }
    if (hashMode != HAO_RUNTIME_HASH_BEXT &&
        hashMode != HAO_RUNTIME_HASH_DOT) {
        return 0;
    }
    if (hashMode == HAO_RUNTIME_HASH_BEXT && !hdr->bextMask) {
        return 0;
    }
#if !defined(HAO_HAVE_SVEBITPERM)
    if (hashMode == HAO_RUNTIME_HASH_BEXT) {
        return 0;
    }
#endif
    if ((u64a)hdr->ruleMetaOffset + (u64a)hdr->ruleMetaCount *
            sizeof(struct HAORuntimeRuleMeta) >
        (u64a)blobSize) {
        return 0;
    }
    if ((u64a)hdr->primaryBitmapOffset + (u64a)hdr->primaryBitmapSize >
        (u64a)blobSize) {
        return 0;
    }
    if ((u64a)hdr->primaryOffset + (u64a)hdr->primaryCount * sizeof(u32) >
        (u64a)blobSize) {
        return 0;
    }
    if (!hdr->l2CheckOffset || !hdr->l2MetaOffset ||
        (hdr->l2CheckOffset & (HAO_RUNTIME_L2_CHECK_ALIGN - 1U))) {
        return 0;
    }
    if ((u64a)hdr->l2CheckOffset + (u64a)hdr->l2EntryCount *
            sizeof(struct HAORuntimeL2Check) >
        (u64a)blobSize) {
        return 0;
    }
    if ((u64a)hdr->l2MetaOffset + (u64a)hdr->l2EntryCount *
            sizeof(struct HAORuntimeL2Meta) >
        (u64a)blobSize) {
        return 0;
    }
    if (outHdr) {
        *outHdr = hdr;
    }
    return 1;
}


