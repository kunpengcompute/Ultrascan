/*
 * Copyright (c) 2026, Huawei Technologies Co., Ltd.
 */

#include "fdr_enhanced.h"
#include "fdr_internal.h"
#include "pbe_runtime.h"
#include "pbe_runtime_inline.h"

#include <string.h>

static int pbeRuleExactMatch(const struct PBERuntimeRuleMeta *rm,
                             const struct FDR_Runtime_Args *a, size_t endPos,
                             const u8 *literalBlob, u32 literalBlobSize) {
    if (!rm || !a || !literalBlob) {
        return 0;
    }
    const u16 len = rm->len;
    if (!len) {
        return 0;
    }
    if (rm->litOffset > literalBlobSize ||
        (u64a)rm->litOffset + len > literalBlobSize) {
        return 0;
    }
    if ((u64a)len > (u64a)endPos + 1 + a->len_history) {
        return 0;
    }

    const s64a startPos = (s64a)endPos - (s64a)len + 1;
    const u8 *pat = literalBlob + rm->litOffset;
    const int nocase = (rm->flags & PBE_RULE_FLAG_NOCASE) ? 1 : 0;
    u16 i;
    for (i = 0; i < len; i++) {
        u8 got;
        if (!pbeGetByteAt(a, startPos + i, &got)) {
            return 0;
        }
        const u8 expect = pat[i];
        if (nocase) {
            if ((u8)mytoupper((char)got) != expect) {
                return 0;
            }
        } else if (got != expect) {
            return 0;
        }
    }

    if ((rm->flags & PBE_RULE_FLAG_HAS_MASK) && rm->maskLen) {
        const u8 mlen = rm->maskLen;
        if (mlen > sizeof(rm->msk)) {
            return 0;
        }
        const s64a maskStart = startPos + (s64a)len - (s64a)mlen;
        for (i = 0; i < mlen; i++) {
            u8 got;
            if (!pbeGetByteAt(a, maskStart + i, &got)) {
                return 0;
            }
            if ((got & rm->msk[i]) != rm->cmp[i]) {
                return 0;
            }
        }
    }

    return 1;
}

static int pbeValidateLayout(const struct FDR *fdr,
                             const struct PBERuntimeHeader *hdr) {
    if (!fdr || !hdr) {
        return 0;
    }
    if (hdr->magic != PBE_RUNTIME_MAGIC ||
        hdr->version != PBE_RUNTIME_VERSION) {
        return 0;
    }
    if (!hdr->selectorCount || !hdr->classCount || !hdr->secondaryCount) {
        return 0;
    }
    if (hdr->classCount > PBE_RUNTIME_MAX_MASK_CLASSES) {
        return 0;
    }
    if (hdr->selectorCount > PBE_RUNTIME_MAX_SELECTORS) {
        return 0;
    }
    if (!hdr->windowBytes || hdr->windowBytes > PBE_RUNTIME_BYTES_PER_RULE_SLOT) {
        return 0;
    }
    if (hdr->extractMode > PBE_RUNTIME_EXTRACT_MODE_BEXT) {
        return 0;
    }
    if ((u64a)hdr->selectorsOffset + (u64a)hdr->selectorCount *
            sizeof(struct PBERuntimeBitSelector) >
        (u64a)fdr->pbeSize) {
        return 0;
    }
    if ((u64a)hdr->classTableOffset + (u64a)hdr->classCount *
            sizeof(struct PBERuntimeMaskClass) >
        (u64a)fdr->pbeSize) {
        return 0;
    }
    if ((u64a)hdr->secondaryOffset + (u64a)hdr->secondaryCount *
            sizeof(struct PBERuntimeSecondaryHashEntry) >
        (u64a)fdr->pbeSize) {
        return 0;
    }
    if ((u64a)hdr->ruleMetaOffset + (u64a)hdr->ruleMetaCount *
            sizeof(struct PBERuntimeRuleMeta) >
        (u64a)fdr->pbeSize) {
        return 0;
    }
    if ((u64a)hdr->literalBlobOffset + (u64a)hdr->literalBlobSize >
        (u64a)fdr->pbeSize) {
        return 0;
    }
    {
        const struct PBERuntimeMaskClass *classes =
            (const struct PBERuntimeMaskClass *)((const u8 *)hdr +
                                                 hdr->classTableOffset);
        u32 i;
        for (i = 0; i < hdr->classCount; i++) {
            const struct PBERuntimeMaskClass *klass = &classes[i];
            if (klass->classKeyBits > hdr->keyBits) {
                return 0;
            }
            if ((u64a)klass->primaryBitmapOffset + (u64a)klass->primaryBitmapSize >
                (u64a)fdr->pbeSize) {
                return 0;
            }
            if ((u64a)klass->primaryOffset + (u64a)klass->primaryCount * sizeof(u32) >
                (u64a)fdr->pbeSize) {
                return 0;
            }
            if (klass->secondaryOffset >= hdr->secondaryCount) {
                return 0;
            }
            if ((u64a)klass->secondaryOffset + (u64a)klass->secondaryCount >
                (u64a)hdr->secondaryCount) {
                return 0;
            }
        }
    }
    return 1;
}

/* HAO v2 当前还未切到正式执行路径，这里先提供 runtime 侧的只读布局校验。
 * 后续切换执行主路径时，可以直接复用这层边界检查。 */
static int haoValidateLayout(const void *blob, u32 blobSize,
                             const struct HAORuntimeHeader **outHdr) {
    const struct HAORuntimeHeader *hdr;

    if (!blob || blobSize < sizeof(struct HAORuntimeHeader)) {
        return 0;
    }

    hdr = (const struct HAORuntimeHeader *)blob;
    if (hdr->magic != HAO_RUNTIME_MAGIC ||
        hdr->version != HAO_RUNTIME_VERSION) {
        return 0;
    }
    if (!hdr->selectorCount || !hdr->primaryCount || !hdr->secondaryCount) {
        return 0;
    }
    if (hdr->selectorCount > PBE_RUNTIME_MAX_SELECTORS) {
        return 0;
    }
    if (!hdr->windowBytes || hdr->windowBytes > PBE_RUNTIME_BYTES_PER_RULE_SLOT) {
        return 0;
    }
    if (hdr->extractMode > PBE_RUNTIME_EXTRACT_MODE_BEXT) {
        return 0;
    }
    if ((u64a)hdr->selectorsOffset + (u64a)hdr->selectorCount *
            sizeof(struct HAORuntimeBitSelector) >
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
    if ((u64a)hdr->secondaryOffset + (u64a)hdr->secondaryCount *
            sizeof(struct PBERuntimeSecondaryHashEntry) >
        (u64a)blobSize) {
        return 0;
    }
    if ((u64a)hdr->ruleMetaOffset + (u64a)hdr->ruleMetaCount *
            sizeof(struct HAORuntimeRuleMeta) >
        (u64a)blobSize) {
        return 0;
    }
    if ((u64a)hdr->literalBlobOffset + (u64a)hdr->literalBlobSize >
        (u64a)blobSize) {
        return 0;
    }

    if (outHdr) {
        *outHdr = hdr;
    }
    return 1;
}

/* 先提供一个 runtime 侧摘要统计，帮助我们在不切执行链的前提下校验 HAO v2 blob。 */
static void haoInspectLayout(const struct HAORuntimeHeader *hdr,
                             struct HAORuntimeInspectSummary *summary) {
    const u32 *primary;
    const struct PBERuntimeSecondaryHashEntry *secondary;
    u32 i;

    if (!hdr || !summary) {
        return;
    }

    memset(summary, 0, sizeof(*summary));
    summary->selectorCount = hdr->selectorCount;
    summary->primaryCount = hdr->primaryCount;
    summary->primaryBitmapSize = hdr->primaryBitmapSize;
    summary->secondaryCount = hdr->secondaryCount;
    summary->ruleMetaCount = hdr->ruleMetaCount;

    primary = (const u32 *)((const u8 *)hdr + hdr->primaryOffset);
    secondary = (const struct PBERuntimeSecondaryHashEntry *)(
        (const u8 *)hdr + hdr->secondaryOffset);

    for (i = 0; i < hdr->primaryCount; i++) {
        if (!primary[i]) {
            continue;
        }
        {
            const u32 entryCount = primary[i] >> PBE_RUNTIME_L1_COUNT_SHIFT;
            summary->nonEmptyPrimary++;
            if (entryCount > 1) {
                summary->multiEntryBucketCount++;
            }
            if (summary->maxEntriesPerKey < entryCount) {
                summary->maxEntriesPerKey = entryCount;
            }
        }
    }

    for (i = 0; i < hdr->secondaryCount; i++) {
        summary->totalRulesInL2 += secondary[i].ruleCount;
    }
}

/* HAO v2 运行期当前先复用 PBE 的位置上下文与 L2 预筛 helper，
 * 通过一个轻量 shim 头把全局单表 layout 接入现有公共小函数。 */
static void haoBuildShimHeader(const struct HAORuntimeHeader *hdr,
                               struct PBERuntimeHeader *shim) {
    memset(shim, 0, sizeof(*shim));
    shim->keyBits = hdr->keyBits;
    shim->selectorCount = hdr->selectorCount;
    shim->primaryCount = hdr->primaryCount;
    shim->primaryBitmapSize = hdr->primaryBitmapSize;
    shim->secondaryCount = hdr->secondaryCount;
    shim->ruleMetaCount = hdr->ruleMetaCount;
    shim->literalBlobSize = hdr->literalBlobSize;
    shim->extractMode = hdr->extractMode;
    shim->windowBytes = hdr->windowBytes;
    shim->bextMask = hdr->bextMask;
}

/* HAO v2 的 confirm 当前仍然沿用原始 literal + mask/cmp 做最终确认。
 * 后续真正的 HAO confirm 优化会在 block kernel 稳定后再推进。 */
static int haoRuleExactMatch(const struct HAORuntimeRuleMeta *rm,
                             const struct FDR_Runtime_Args *a, size_t endPos,
                             const u8 *literalBlob, u32 literalBlobSize) {
    if (!rm || !a || !literalBlob) {
        return 0;
    }
    if (!rm->len) {
        return 0;
    }
    if (rm->litOffset > literalBlobSize ||
        (u64a)rm->litOffset + rm->len > literalBlobSize) {
        return 0;
    }
    if ((u64a)rm->len > (u64a)endPos + 1 + a->len_history) {
        return 0;
    }

    {
        const s64a startPos = (s64a)endPos - (s64a)rm->len + 1;
        const u8 *pat = literalBlob + rm->litOffset;
        const int nocase = (rm->flags & PBE_RULE_FLAG_NOCASE) ? 1 : 0;
        u16 i;

        for (i = 0; i < rm->len; i++) {
            u8 got;
            if (!pbeGetByteAt(a, startPos + i, &got)) {
                return 0;
            }
            if (nocase) {
                if ((u8)mytoupper((char)got) != pat[i]) {
                    return 0;
                }
            } else if (got != pat[i]) {
                return 0;
            }
        }

        if ((rm->flags & PBE_RULE_FLAG_HAS_MASK) && rm->maskLen) {
            const u8 mlen = rm->maskLen;
            const s64a maskStart = startPos + (s64a)rm->len - (s64a)mlen;
            for (i = 0; i < mlen && i < sizeof(rm->msk); i++) {
                u8 got;
                if (!pbeGetByteAt(a, maskStart + i, &got)) {
                    return 0;
                }
                if ((got & rm->msk[i]) != rm->cmp[i]) {
                    return 0;
                }
            }
        }
    }

    return 1;
}

static int haoProcessEncodedRange(
    const struct HAORuntimeHeader *hdr,
    const struct PBERuntimeSecondaryHashEntry *secondaryHashTable,
    const struct HAORuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct PBEPositionContext *ctx, u32 fullKey,
    u32 encoded) {
    u32 offset = 0;
    u32 count = 0;
    u32 n;

    if (!encoded || !ctx) {
        return HWLM_SUCCESS;
    }

    pbeDecodePrimaryValue(encoded, &offset, &count);
    for (n = 0; n < count; n++) {
        const u32 off = offset + n;
        const struct PBERuntimeSecondaryHashEntry *entry;
        u32 laneMask;
        u32 r;

        if (!off || off >= hdr->secondaryCount) {
            break;
        }

        entry = &secondaryHashTable[off];
        laneMask = pbeEntryMatchMaskFromContextVector(entry, ctx);
        if (!laneMask) {
            continue;
        }

        for (r = 0; r < entry->ruleCount &&
                    r < PBE_RUNTIME_RULE_SLOTS_PER_ENTRY; r++) {
            const u16 ridx = entry->ruleIndex[r];
            const u32 kv = entry->keyValue[r];
            const u32 km = entry->keyMask[r];
            const struct HAORuntimeRuleMeta *rm;

            if (ridx >= hdr->ruleMetaCount) {
                continue;
            }
            if (!(laneMask & (1U << r))) {
                continue;
            }
            if (((fullKey ^ kv) & km) != 0) {
                continue;
            }

            rm = &ruleMeta[ridx];
            if (!(rm->groups & *control)) {
                continue;
            }
            if (!haoRuleExactMatch(rm, a, ctx->endPos, literalBlob,
                                   literalBlobSize)) {
                continue;
            }

            *control = a->cb(ctx->endPos, rm->id, a->scratch);
            if (*control == HWLM_TERMINATE_MATCHING) {
                return HWLM_TERMINATED;
            }
        }
    }

    return HWLM_SUCCESS;
}

/* HAO v2 执行骨架先走全局单表 naive 路径。
 * 这里的目标是先把新 layout 真正跑起来，后续再替换成 32B block kernel。 */
static int haoRunNaiveBlob(const struct HAORuntimeHeader *hdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t *control) {
    struct PBERuntimeHeader shim;
    const struct PBERuntimeBitSelector *selectors;
    const u8 *primaryBitmap;
    const u32 *primaryHashTable;
    const struct PBERuntimeSecondaryHashEntry *secondaryHashTable;
    const struct HAORuntimeRuleMeta *ruleMeta;
    const u8 *literalBlob;
    size_t i;

    if (!hdr || !a || !a->buf || !a->len || a->start_offset >= a->len) {
        return HWLM_SUCCESS;
    }

    haoBuildShimHeader(hdr, &shim);
    selectors = (const struct PBERuntimeBitSelector *)((const u8 *)hdr +
                                                       hdr->selectorsOffset);
    primaryBitmap = (const u8 *)hdr + hdr->primaryBitmapOffset;
    primaryHashTable = (const u32 *)((const u8 *)hdr + hdr->primaryOffset);
    secondaryHashTable = (const struct PBERuntimeSecondaryHashEntry *)(
        (const u8 *)hdr + hdr->secondaryOffset);
    ruleMeta = (const struct HAORuntimeRuleMeta *)((const u8 *)hdr +
                                                   hdr->ruleMetaOffset);
    literalBlob = (const u8 *)hdr + hdr->literalBlobOffset;

    for (i = a->start_offset; i < a->len; i++) {
        struct PBEPositionContext ctx;

        pbeBuildPositionContext(&shim, selectors, a, i, &ctx, 1);
        if (ctx.key >= hdr->primaryCount) {
            continue;
        }
        if (!pbePrimaryBitmapHasValue(primaryBitmap, hdr->primaryBitmapSize,
                                      ctx.key)) {
            continue;
        }
        if (haoProcessEncodedRange(hdr, secondaryHashTable, ruleMeta,
                                   literalBlob, hdr->literalBlobSize, a,
                                   control, &ctx, ctx.key,
                                   primaryHashTable[ctx.key]) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static hwlm_error_t haoExecBlobWithPath(const void *blob, u32 blobSize,
                                        const struct FDR_Runtime_Args *a,
                                        hwlm_group_t control, int useBatch4) {
    const struct HAORuntimeHeader *hdr = NULL;

    (void)useBatch4;
    if (!haoValidateLayout(blob, blobSize, &hdr)) {
        return HWLM_SUCCESS;
    }
    if (!a) {
        return HWLM_SUCCESS;
    }

    return haoRunNaiveBlob(hdr, a, &control);
}

/* 统一用安全拷贝读取 blob magic，避免直接解引用未对齐地址。 */
static int pbeReadBlobMagic(const void *blob, u32 blobSize, u32 *magic) {
    if (!blob || !magic || blobSize < sizeof(*magic)) {
        return 0;
    }

    memcpy(magic, blob, sizeof(*magic));
    return 1;
}

static int pbeProcessEncodedRange(
    const struct PBERuntimeHeader *hdr,
    const struct PBERuntimeSecondaryHashEntry *secondaryHashTable,
    const struct PBERuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct PBEPositionContext *ctx, u32 classKey,
    u32 encoded) {
    u32 offset = 0;
    u32 count = 0;
    u32 n;

    if (!encoded || !ctx) {
        return HWLM_SUCCESS;
    }

    pbeDecodePrimaryValue(encoded, &offset, &count);
    for (n = 0; n < count; n++) {
        const u32 off = offset + n;
        const struct PBERuntimeSecondaryHashEntry *entry;
        u32 laneMask;
        u32 r;

        if (!off || off >= hdr->secondaryCount) {
            break;
        }

        entry = &secondaryHashTable[off];
        laneMask = pbeEntryMatchMaskFromContextVector(entry, ctx);
        if (!laneMask) {
            continue;
        }

        for (r = 0; r < entry->ruleCount &&
                    r < PBE_RUNTIME_RULE_SLOTS_PER_ENTRY; r++) {
            const u16 ridx = entry->ruleIndex[r];
            const u32 kv = entry->keyValue[r];
            const u32 km = entry->keyMask[r];
            const struct PBERuntimeRuleMeta *rm;

            if (ridx >= hdr->ruleMetaCount) {
                continue;
            }
            if (!(laneMask & (1U << r))) {
                continue;
            }
            if (((classKey ^ kv) & km) != 0) {
                continue;
            }

            rm = &ruleMeta[ridx];
            if (!(rm->groups & *control)) {
                continue;
            }
            if (!pbeRuleExactMatch(rm, a, ctx->endPos, literalBlob,
                                   literalBlobSize)) {
                continue;
            }

            *control = a->cb(ctx->endPos, rm->id, a->scratch);
            if (*control == HWLM_TERMINATE_MATCHING) {
                return HWLM_TERMINATED;
            }
        }
    }

    return HWLM_SUCCESS;
}

static int pbeProcessMaskClassesForContext(
    const struct PBERuntimeHeader *hdr,
    const struct PBERuntimeMaskClass *classes,
    const struct PBERuntimeSecondaryHashEntry *secondaryHashTable,
    const struct PBERuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct PBEPositionContext *ctx) {
    u32 classIdx;

    for (classIdx = 0; classIdx < hdr->classCount; classIdx++) {
        const struct PBERuntimeMaskClass *klass = &classes[classIdx];
        const u8 *primaryBitmap =
            (const u8 *)hdr + klass->primaryBitmapOffset;
        const u32 *primaryHashTable =
            (const u32 *)((const u8 *)hdr + klass->primaryOffset);
        const u32 classKey = pbeProjectKeyToClass(ctx->key, klass->classMask);

        if (classKey >= klass->primaryCount) {
            continue;
        }
        if (!pbePrimaryBitmapHasValue(primaryBitmap, klass->primaryBitmapSize,
                                      classKey)) {
            continue;
        }
        if (pbeProcessEncodedRange(hdr, secondaryHashTable, ruleMeta,
                                   literalBlob, literalBlobSize, a, control,
                                   ctx, classKey,
                                   primaryHashTable[classKey]) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static int pbeProcessMaskClassesForLaneWithBatchState(
    const struct PBERuntimeHeader *hdr,
    const struct PBERuntimeMaskClass *classes,
    const struct PBERuntimeSecondaryHashEntry *secondaryHashTable,
    const struct PBERuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct PBEPositionContext *ctx,
    const struct PBEMaskClassBatchState *batchState, u32 lane) {
    u32 classIdx;

    for (classIdx = 0; classIdx < hdr->classCount; classIdx++) {
        const struct PBERuntimeMaskClass *klass = &classes[classIdx];
        u32 classKey = 0;
        u32 encoded = 0;

        if (batchState && lane < batchState->laneCount && pbeMaskClassIsHot(klass)) {
            encoded = batchState->classEncoded[classIdx][lane];
            if (!encoded) {
                continue;
            }
            classKey = batchState->classKeys[classIdx][lane];
        } else {
            const u8 *primaryBitmap =
                (const u8 *)hdr + klass->primaryBitmapOffset;
            const u32 *primaryHashTable =
                (const u32 *)((const u8 *)hdr + klass->primaryOffset);

            classKey = pbeProjectKeyToClass(ctx->key, klass->classMask);

            if (classKey >= klass->primaryCount) {
                continue;
            }
            if (!pbePrimaryBitmapHasValue(primaryBitmap, klass->primaryBitmapSize,
                                          classKey)) {
                continue;
            }
            encoded = primaryHashTable[classKey];
        }

        if (pbeProcessEncodedRange(hdr, secondaryHashTable, ruleMeta,
                                   literalBlob, literalBlobSize, a, control,
                                   ctx, classKey,
                                   encoded) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static UNUSED
int pbeRunNaive(const struct PBERuntimeHeader *hdr,
                       const struct FDR_Runtime_Args *a,
                       hwlm_group_t *control) {
    if (!hdr || !a || !a->buf || !a->len || a->start_offset >= a->len) {
        return HWLM_SUCCESS;
    }

    const struct PBERuntimeBitSelector *selectors =
        (const struct PBERuntimeBitSelector *)((const u8 *)hdr +
                                               hdr->selectorsOffset);
    const struct PBERuntimeMaskClass *classes =
        (const struct PBERuntimeMaskClass *)((const u8 *)hdr +
                                             hdr->classTableOffset);
    const struct PBERuntimeSecondaryHashEntry *secondaryHashTable =
        (const struct PBERuntimeSecondaryHashEntry *)((const u8 *)hdr +
                                                      hdr->secondaryOffset);
    const struct PBERuntimeRuleMeta *ruleMeta =
        (const struct PBERuntimeRuleMeta *)((const u8 *)hdr +
                                            hdr->ruleMetaOffset);
    const u8 *literalBlob = (const u8 *)hdr + hdr->literalBlobOffset;
    const u32 literalBlobSize = hdr->literalBlobSize;

    size_t i;
    for (i = a->start_offset; i < a->len; i++) {
        struct PBEPositionContext ctx;

        pbeBuildPositionContext(hdr, selectors, a, i, &ctx, 1);
        if (pbeProcessMaskClassesForContext(hdr, classes, secondaryHashTable,
                                            ruleMeta, literalBlob,
                                            literalBlobSize, a, control,
                                            &ctx) == HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }
    return HWLM_SUCCESS;
}

static int pbeRunBatch4(const struct PBERuntimeHeader *hdr,
                        const struct FDR_Runtime_Args *a,
                        hwlm_group_t *control) {
    const struct PBERuntimeBitSelector *selectors =
        (const struct PBERuntimeBitSelector *)((const u8 *)hdr +
                                               hdr->selectorsOffset);
    const struct PBERuntimeMaskClass *classes =
        (const struct PBERuntimeMaskClass *)((const u8 *)hdr +
                                             hdr->classTableOffset);
    const struct PBERuntimeSecondaryHashEntry *secondaryHashTable =
        (const struct PBERuntimeSecondaryHashEntry *)((const u8 *)hdr +
                                                      hdr->secondaryOffset);
    const struct PBERuntimeRuleMeta *ruleMeta =
        (const struct PBERuntimeRuleMeta *)((const u8 *)hdr +
                                            hdr->ruleMetaOffset);
    const u8 *literalBlob = (const u8 *)hdr + hdr->literalBlobOffset;
    const u32 literalBlobSize = hdr->literalBlobSize;

    const u32 batchWidth = pbeSuggestedBatchWidth(hdr);
    size_t i;
    for (i = a->start_offset; i < a->len; i += batchWidth) {
        struct PBEPositionContext ctxs[PBE_BATCH_MAX_WIDTH];
        u32 laneCount = 0;

        laneCount = pbeBuildBatchContexts(hdr, selectors, a, i, batchWidth,
                                          ctxs);

        if (hdr->classCount <= 2) {
            u32 lane;
            for (lane = 0; lane < laneCount; lane++) {
                if (pbeProcessMaskClassesForContext(hdr, classes,
                                                    secondaryHashTable,
                                                    ruleMeta, literalBlob,
                                                    literalBlobSize, a, control,
                                                    &ctxs[lane]) ==
                    HWLM_TERMINATED) {
                    return HWLM_TERMINATED;
                }
            }
        } else {
            struct PBEMaskClassBatchState batchState;
            u32 lane;
            pbeBuildMaskClassBatchState(hdr, classes, ctxs, laneCount,
                                        &batchState);

            for (lane = 0; lane < laneCount; lane++) {
                if (pbeProcessMaskClassesForLaneWithBatchState(
                        hdr, classes, secondaryHashTable, ruleMeta,
                        literalBlob, literalBlobSize, a, control, &ctxs[lane],
                        &batchState, lane) == HWLM_TERMINATED) {
                    return HWLM_TERMINATED;
                }
            }
        }
    }

    return HWLM_SUCCESS;
}

static
hwlm_error_t pbeExecWithPath(const struct FDR *fdr,
                             const struct FDR_Runtime_Args *a,
                             hwlm_group_t control, int useBatch4) {
    if (!fdr || !fdr->pbeOffset || !fdr->pbeSize) {
        return HWLM_SUCCESS;
    }

    const u8 *base = (const u8 *)fdr;
    const struct PBERuntimeHeader *hdr =
        (const struct PBERuntimeHeader *)(base + fdr->pbeOffset);

    if (!pbeValidateLayout(fdr, hdr)) {
        return HWLM_SUCCESS;
    }

    if (!a || (hdr->flags & PBE_RUNTIME_FLAG_PARTIAL_COVERAGE)) {
        return HWLM_SUCCESS;
    }

    return useBatch4 ? pbeRunBatch4(hdr, a, &control)
                     : pbeRunNaive(hdr, a, &control);
}

hwlm_error_t PbeEngineExecNaiveForTest(const struct FDR *fdr,
                                       const struct FDR_Runtime_Args *a,
                                       hwlm_group_t control) {
    if (!fdr || !fdr->pbeOffset || !fdr->pbeSize) {
        return HWLM_SUCCESS;
    }

    {
        const u8 *base = (const u8 *)fdr;
        const void *blob = base + fdr->pbeOffset;
        u32 magic = 0;

        if (pbeReadBlobMagic(blob, fdr->pbeSize, &magic) &&
            magic == HAO_RUNTIME_MAGIC) {
            return haoExecBlobWithPath(blob, fdr->pbeSize, a, control, 0);
        }
    }

    return pbeExecWithPath(fdr, a, control, 0);
}

u32 PbeRuntimeEntryMatchMaskForTest(
    const struct PBERuntimeSecondaryHashEntry *entry,
    const struct FDR_Runtime_Args *a, size_t endPos, int useVector) {
    struct PBEPositionContext ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.endPos = endPos;
    ctx.window64 = pbeLoadWindow64Normalized(a, endPos,
                                             PBE_RUNTIME_BYTES_PER_RULE_SLOT);
    ctx.validMask8 = pbeComputeValidMask8(a, endPos);

    return useVector ? pbeEntryMatchMaskFromContextVector(entry, &ctx)
                     : pbeEntryMatchMaskFromContextScalar(entry, &ctx);
}

u32 PbeRuntimeBitmapProbeMaskForTest(const u8 *bitmap, u32 bitmapSize,
                                     const u32 *primaryIdx, u32 laneCount,
                                     int usePacked) {
    struct PBEBitmapProbeState probe;

    memset(&probe, 0, sizeof(probe));
    pbePrepareBitmapProbeStateFromPrimaryIdx(primaryIdx, laneCount, &probe);

    return usePacked ? pbeProbeBitmapPacked(bitmap, bitmapSize, &probe)
                     : pbeProbeBitmapScalar(bitmap, bitmapSize, &probe);
}

int HaoRuntimeValidateLayoutForTest(const void *blob, u32 blobSize) {
    return haoValidateLayout(blob, blobSize, NULL);
}

int HaoRuntimeInspectBlobForTest(const void *blob, u32 blobSize,
                                 struct HAORuntimeInspectSummary *summary) {
    const struct HAORuntimeHeader *hdr = NULL;

    if (!summary) {
        return 0;
    }
    if (!haoValidateLayout(blob, blobSize, &hdr)) {
        return 0;
    }
    haoInspectLayout(hdr, summary);
    return 1;
}

hwlm_error_t HaoEngineExecBlobNaiveForTest(const void *blob, u32 blobSize,
                                           const struct FDR_Runtime_Args *a,
                                           hwlm_group_t control) {
    return haoExecBlobWithPath(blob, blobSize, a, control, 0);
}

hwlm_error_t PbeEngineExec(const struct FDR *fdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t control) {
    if (!fdr || !fdr->pbeOffset || !fdr->pbeSize) {
        return HWLM_SUCCESS;
    }

    {
        const u8 *base = (const u8 *)fdr;
        const void *blob = base + fdr->pbeOffset;
        u32 magic = 0;

        if (pbeReadBlobMagic(blob, fdr->pbeSize, &magic) &&
            magic == HAO_RUNTIME_MAGIC) {
            return haoExecBlobWithPath(blob, fdr->pbeSize, a,
                                       control, 1);
        }
    }

    return pbeExecWithPath(fdr, a, control, 1);
}
