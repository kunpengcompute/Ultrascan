/*
 * Copyright (c) 2026, Huawei Technologies Co., Ltd.
 */

#include "hao_runtime_test.h"

#include "fdr/hao_runtime_inline.h"

#include <cstring>

#if defined(__ARM_FEATURE_SVE2_BITPERM)
#define HAO_HAVE_SVEBITPERM 1
#endif

static u32 haoL2MetaRuleCountForTest(const struct HAORuntimeL2Meta *meta) {
    u32 count = 0;

    if (!meta) {
        return 0;
    }
    for (u32 slot = 0; slot < HAO_RUNTIME_RULE_SLOTS_PER_ENTRY; slot++) {
        count += meta->ruleIndex[slot] != HAO_RUNTIME_INVALID_RULE_INDEX;
    }
    return count;
}

static int haoValidateLayoutForTest(const void *blob, u32 blobSize,
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

static void haoInspectLayoutForTest(const struct HAORuntimeHeader *hdr,
                                    struct HAORuntimeInspectSummary *summary) {
    if (!hdr || !summary) {
        return;
    }

    std::memset(summary, 0, sizeof(*summary));
    summary->keyBits = haoRuntimeHeaderKeyBits(hdr);
    summary->primaryCount = hdr->primaryCount;
    summary->primaryBitmapSize = hdr->primaryBitmapSize;
    summary->l2EntryCount = hdr->l2EntryCount;
    summary->ruleMetaCount = hdr->ruleMetaCount;

    const u32 *primary = (const u32 *)((const u8 *)hdr + hdr->primaryOffset);
    const struct HAORuntimeL2Meta *l2MetaTable =
        (const struct HAORuntimeL2Meta *)((const u8 *)hdr + hdr->l2MetaOffset);
    for (u32 i = 0; i < hdr->primaryCount; i++) {
        if (!primary[i]) {
            continue;
        }
        const u32 entryCount = primary[i] >> HAO_RUNTIME_L1_COUNT_SHIFT;
        summary->nonEmptyPrimary++;
        summary->multiEntryBucketCount += entryCount > 1;
        if (summary->maxEntriesPerKey < entryCount) {
            summary->maxEntriesPerKey = entryCount;
        }
    }
    for (u32 i = 0; i < hdr->l2EntryCount; i++) {
        summary->totalRulesInL2 += haoL2MetaRuleCountForTest(&l2MetaTable[i]);
    }
}

extern "C" {

hwlm_error_t HaoEngineExecNaiveForTest(const struct FDR *fdr,
                                       const struct FDR_Runtime_Args *a,
                                       hwlm_group_t control) {
    return HaoEngineExec(fdr, a, control);
}

u32 HaoRuntimeBitmapProbeMaskForTest(const u8 *bitmap, u32 bitmapSize,
                                     const u32 *primaryIdx, u32 laneCount,
                                     int usePacked) {
    u32 activeMask = 0;

    (void)usePacked;
    if (!bitmap || !primaryIdx || laneCount > HAO_BATCH_MAX_WIDTH) {
        return 0;
    }

    for (u32 lane = 0; lane < laneCount; lane++) {
        const u32 idx = primaryIdx[lane];
        if (idx / 8U < bitmapSize &&
            (bitmap[idx / 8U] & (1U << (idx & 7U)))) {
            activeMask |= 1U << lane;
        }
    }
    return activeMask;
}

u64a HaoRuntimeRawLaneWordForTest(const u8 *prev32, const u8 *curr32,
                                  u32 lane) {
    u64a word = 0;

    if (!prev32 || !curr32 || lane >= HAO_RUNTIME_BLOCK_BYTES) {
        return 0;
    }

    for (u32 i = 0; i < HAO_RUNTIME_BYTES_PER_RULE_SLOT; i++) {
        const u32 idx = lane + 25U + i;
        const u8 byte = idx < HAO_RUNTIME_BLOCK_BYTES
                            ? prev32[idx]
                            : curr32[idx - HAO_RUNTIME_BLOCK_BYTES];
        word |= (u64a)byte << (i * 8U);
    }
    return word;
}

int HaoRuntimeValidateLayoutForTest(const void *blob, u32 blobSize) {
    return haoValidateLayoutForTest(blob, blobSize, nullptr);
}

int HaoRuntimeInspectBlobForTest(const void *blob, u32 blobSize,
                                 struct HAORuntimeInspectSummary *summary) {
    const struct HAORuntimeHeader *hdr = nullptr;

    if (!summary) {
        return 0;
    }
    if (!haoValidateLayoutForTest(blob, blobSize, &hdr)) {
        return 0;
    }
    haoInspectLayoutForTest(hdr, summary);
    return 1;
}

} // extern "C"
