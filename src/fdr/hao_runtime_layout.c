/*
 * Copyright (c) 2026, Huawei Technologies Co., Ltd.
 */

#include "hao_runtime_layout.h"
#include "hao_runtime_inline.h"

#include "hwlm/hwlm_internal.h"
#include "rose/rose_internal.h"

/* Validate the HAO blob layout before entering execution. */
int haoValidateLayout(const void *blob, u32 blobSize,
                      const struct HAORuntimeHeader **outHdr) {
    const struct HAORuntimeHeader *hdr;
    u32 keyBits;
    u32 hashMode;

    if (!blob || !ISALIGNED_N(blob, 8) ||
        blobSize < sizeof(struct HAORuntimeHeader)) {
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
    if (keyBits > HAO_RUNTIME_L1_OFFSET_BITS ||
        hdr->primaryCount != (1U << keyBits) ||
        hdr->primaryBitmapSize !=
            ROUNDUP_N((hdr->primaryCount + 7U) / 8U, 4U) ||
        !hdr->ruleMetaCount || (hdr->keyBits & 0x00ffff00U)) {
        return 0;
    }
    if (hashMode != HAO_RUNTIME_HASH_BEXT && hashMode != HAO_RUNTIME_HASH_DOT) {
        return 0;
    }
    if (hashMode == HAO_RUNTIME_HASH_BEXT && !hdr->bextMask) {
        return 0;
    }
    if (hashMode == HAO_RUNTIME_HASH_BEXT &&
        popcount64(hdr->bextMask) != keyBits) {
        return 0;
    }
    if (hdr->primaryBitmapOffset < sizeof(*hdr) ||
        (hdr->primaryBitmapOffset & 3U) || (hdr->primaryOffset & 3U) ||
        (hdr->l2MetaOffset & 3U) || (hdr->ruleMetaOffset & 7U) ||
        (u64a)hdr->primaryBitmapOffset + hdr->primaryBitmapSize >
            hdr->primaryOffset ||
        (u64a)hdr->primaryOffset + (u64a)hdr->primaryCount * sizeof(u32) >
            hdr->l2CheckOffset ||
        (u64a)hdr->l2CheckOffset +
                (u64a)hdr->l2EntryCount * sizeof(struct HAORuntimeL2Check) >
            hdr->l2MetaOffset ||
        (u64a)hdr->l2MetaOffset +
                (u64a)hdr->l2EntryCount * sizeof(struct HAORuntimeL2Meta) >
            hdr->ruleMetaOffset) {
        return 0;
    }
    if ((u64a)hdr->ruleMetaOffset +
            (u64a)hdr->ruleMetaCount * sizeof(struct HAORuntimeRuleMeta) >
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
    if ((u64a)hdr->l2CheckOffset +
            (u64a)hdr->l2EntryCount * sizeof(struct HAORuntimeL2Check) >
        (u64a)blobSize) {
        return 0;
    }
    if ((u64a)hdr->l2MetaOffset +
            (u64a)hdr->l2EntryCount * sizeof(struct HAORuntimeL2Meta) >
        (u64a)blobSize) {
        return 0;
    }
    if (outHdr) {
        *outHdr = hdr;
    }
    return 1;
}

int haoValidateVariants(const void *blob, u32 blobSize,
                        const struct HAOVariants **out) {
    if (!blob || !ISALIGNED_N(blob, 64) ||
        blobSize < sizeof(struct HAOVariants)) {
        return 0;
    }
    const struct HAOVariants *v = (const struct HAOVariants *)blob;
    if (v->magic != HAO_VARIANTS_MAGIC || v->version != HAO_VARIANTS_VERSION ||
        v->size != blobSize || v->fallback > 1U ||
        (!v->dotSize && !v->bextSize)) {
        return 0;
    }
    const u32 offsets[] = {v->dotOffset, v->bextOffset};
    const u32 sizes[] = {v->dotSize, v->bextSize};
    u64a end = ROUNDUP_CL(sizeof(*v));
    for (u32 i = 0; i < 2U; i++) {
        if (!sizes[i]) {
            if (offsets[i]) {
                return 0;
            }
            continue;
        }
        const struct HAORuntimeHeader *hdr = NULL;
        if ((offsets[i] & 63U) || offsets[i] < end ||
            (u64a)offsets[i] + sizes[i] > blobSize ||
            !haoValidateLayout((const u8 *)blob + offsets[i], sizes[i], &hdr) ||
            haoRuntimeHeaderHashMode(hdr) !=
                (i ? HAO_RUNTIME_HASH_BEXT : HAO_RUNTIME_HASH_DOT)) {
            return 0;
        }
        end = (u64a)offsets[i] + sizes[i];
    }
    if (out) {
        *out = v;
    }
    return 1;
}

int haoValidateTables(const struct HAORuntimeHeader *hdr) {
    const u8 *base = (const u8 *)hdr;
    const u32 *primary = (const u32 *)(base + hdr->primaryOffset);
    const u8 *bitmap = base + hdr->primaryBitmapOffset;
    const struct HAORuntimeL2Check *checks =
        (const struct HAORuntimeL2Check *)(base + hdr->l2CheckOffset);
    const struct HAORuntimeL2Meta *meta =
        (const struct HAORuntimeL2Meta *)(base + hdr->l2MetaOffset);
    for (u32 i = 0; i < hdr->primaryCount; i++) {
        const u32 value = primary[i];
        const u32 offset = value & HAO_RUNTIME_L1_OFFSET_MASK;
        const u32 count = value >> HAO_RUNTIME_L1_COUNT_SHIFT;
        if (!!value != !!(bitmap[i / 8U] & (1U << (i & 7U))) ||
            (value &&
             (!offset || !count || (u64a)offset + count > hdr->l2EntryCount))) {
            return 0;
        }
    }
    for (u32 i = 0; i < hdr->l2EntryCount; i++) {
        for (u32 slot = 0; slot < HAO_RUNTIME_RULE_SLOTS_PER_ENTRY; slot++) {
            const u32 index = meta[i].ruleIndex[slot];
            const u64a mask = checks[i].mask[slot];
            const u64a rule = checks[i].rule[slot];
            u32 care = 0;
            for (u32 byte = 0; byte < 8U; byte++) {
                care |= !!((mask >> (byte * 8U)) & 0xffU) << byte;
            }
            if (((meta[i].careBits >> (slot * 8U)) & 0xffU) != care ||
                (index == HAO_RUNTIME_INVALID_RULE_INDEX
                     ? (mask != 0 || rule == 0)
                     : (!i || index >= hdr->ruleMetaCount || !mask ||
                        (rule & ~mask)))) {
                return 0;
            }
        }
    }
    return 1;
}

int haoValidateDatabase(const void *blob, u32 size) {
#if defined(__aarch64__)
    if (!blob || size < sizeof(struct RoseEngine)) {
        return 1; // Existing non-Rose test payloads have no matcher variants.
    }
    const struct RoseEngine *rose = (const struct RoseEngine *)blob;
    const u32 offsets[] = {rose->fmatcherOffset, rose->ematcherOffset,
                           rose->drmatcherOffset, rose->sbmatcherOffset};
    for (u32 i = 0; i < 4U; i++) {
        if (!offsets[i]) {
            continue;
        }
        const u32 headerSize = ROUNDUP_CL(sizeof(struct HWLM));
        if ((u64a)offsets[i] + headerSize > size || (offsets[i] & 63U)) {
            return 0;
        }
        const struct HWLM *hwlm =
            (const struct HWLM *)((const u8 *)blob + offsets[i]);
        if (hwlm->type != HWLM_ENGINE_FDR) {
            continue;
        }
        if ((u64a)offsets[i] + headerSize + sizeof(struct FDR) > size) {
            return 0;
        }
        const struct FDR *fdr = (const struct FDR *)HWLM_C_DATA(hwlm);
        if (fdr->engineID != HAO_VARIANTS_ENGINE_ID) {
            continue;
        }
        const struct HAOVariants *v = NULL;
        if ((u64a)offsets[i] + headerSize + fdr->size > size ||
            fdr->matcherBlobOffset <
                ROUNDUP_CL(sizeof(*fdr)) + (u64a)fdr->tabSize ||
            (u64a)fdr->matcherBlobOffset + fdr->matcherBlobSize >
                fdr->confOffset ||
            fdr->confOffset >= fdr->size ||
            !haoValidateVariants((const u8 *)fdr + fdr->matcherBlobOffset,
                                 fdr->matcherBlobSize, &v)) {
            return 0;
        }
        if ((v->dotSize &&
             !haoValidateTables(
                 (const struct HAORuntimeHeader *)((const u8 *)v +
                                                   v->dotOffset))) ||
            (v->bextSize &&
             !haoValidateTables(
                 (const struct HAORuntimeHeader *)((const u8 *)v +
                                                   v->bextOffset)))) {
            return 0;
        }
    }
#else
    (void)blob;
    (void)size;
#endif
    return 1;
}
