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
            sizeof(struct PBERuntimeBitSelector) >
        (u64a)fdr->pbeSize) {
        return 0;
    }
    if ((u64a)hdr->primaryBitmapOffset + (u64a)hdr->primaryBitmapSize >
        (u64a)fdr->pbeSize) {
        return 0;
    }
    if ((u64a)hdr->primaryOffset + (u64a)hdr->primaryCount * sizeof(u32) >
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
    return 1;
}

static int pbeProcessEncodedRange(
    const struct PBERuntimeHeader *hdr,
    const struct PBERuntimeSecondaryHashEntry *secondaryHashTable,
    const struct PBERuntimeRuleMeta *ruleMeta, const u8 *literalBlob,
    u32 literalBlobSize, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, const struct PBEPositionContext *ctx, u32 encoded) {
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
            if (((ctx->key ^ kv) & km) != 0) {
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
    const u8 *primaryBitmap = (const u8 *)hdr + hdr->primaryBitmapOffset;
    const u32 *primaryHashTable =
        (const u32 *)((const u8 *)hdr + hdr->primaryOffset);
    const struct PBERuntimeSecondaryHashEntry *secondaryHashTable =
        (const struct PBERuntimeSecondaryHashEntry *)((const u8 *)hdr +
                                                      hdr->secondaryOffset);
    const struct PBERuntimeRuleMeta *ruleMeta =
        (const struct PBERuntimeRuleMeta *)((const u8 *)hdr +
                                            hdr->ruleMetaOffset);
    const u8 *literalBlob = (const u8 *)hdr + hdr->literalBlobOffset;
    const u32 literalBlobSize = hdr->literalBlobSize;
    const u32 wildcardIdx = 0;
    const u32 wildcardValue =
        (wildcardIdx < hdr->primaryCount &&
         pbePrimaryBitmapHasValue(primaryBitmap, hdr->primaryBitmapSize,
                                  wildcardIdx))
            ? primaryHashTable[wildcardIdx]
            : 0;

    size_t i;
    for (i = a->start_offset; i < a->len; i++) {
        struct PBEPositionContext ctx;

        pbeBuildPositionContext(hdr, selectors, a, i, wildcardValue, &ctx, 1);
        ctx.exactEncoded =
            pbePrimaryBitmapHasValue(primaryBitmap, hdr->primaryBitmapSize,
                                     ctx.primaryIdx)
                ? primaryHashTable[ctx.primaryIdx]
                : 0;

        if (pbeProcessEncodedRange(hdr, secondaryHashTable, ruleMeta,
                                   literalBlob, literalBlobSize, a, control,
                                   &ctx, ctx.exactEncoded) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
        if (ctx.wildcardEncoded != ctx.exactEncoded &&
            pbeProcessEncodedRange(hdr, secondaryHashTable, ruleMeta,
                                   literalBlob, literalBlobSize, a, control,
                                   &ctx, ctx.wildcardEncoded) ==
                HWLM_TERMINATED) {
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
    const u8 *primaryBitmap = (const u8 *)hdr + hdr->primaryBitmapOffset;
    const u32 *primaryHashTable =
        (const u32 *)((const u8 *)hdr + hdr->primaryOffset);
    const struct PBERuntimeSecondaryHashEntry *secondaryHashTable =
        (const struct PBERuntimeSecondaryHashEntry *)((const u8 *)hdr +
                                                      hdr->secondaryOffset);
    const struct PBERuntimeRuleMeta *ruleMeta =
        (const struct PBERuntimeRuleMeta *)((const u8 *)hdr +
                                            hdr->ruleMetaOffset);
    const u8 *literalBlob = (const u8 *)hdr + hdr->literalBlobOffset;
    const u32 literalBlobSize = hdr->literalBlobSize;
    const u32 wildcardIdx = 0;
    const u32 wildcardValue =
        (wildcardIdx < hdr->primaryCount &&
         pbePrimaryBitmapHasValue(primaryBitmap, hdr->primaryBitmapSize,
                                  wildcardIdx))
            ? primaryHashTable[wildcardIdx]
            : 0;

    const u32 batchWidth = pbeSuggestedBatchWidth(hdr);
    size_t i;
    for (i = a->start_offset; i < a->len; i += batchWidth) {
        struct PBEPositionContext ctxs[PBE_BATCH_MAX_WIDTH];
        u32 laneCount = 0;
        u32 lane;

        memset(ctxs, 0, sizeof(ctxs));
        laneCount = pbeBuildBatchContexts(hdr, selectors, a, i, wildcardValue,
                                          batchWidth, ctxs);
        pbeFillBatchEncodedValues(primaryBitmap, hdr->primaryBitmapSize,
                                  primaryHashTable, ctxs, laneCount,
                                  batchWidth);

        for (lane = 0; lane < laneCount; lane++) {
            if (ctxs[lane].exactEncoded &&
                pbeProcessEncodedRange(hdr, secondaryHashTable, ruleMeta,
                                       literalBlob, literalBlobSize, a, control,
                                       &ctxs[lane], ctxs[lane].exactEncoded) ==
                    HWLM_TERMINATED) {
                return HWLM_TERMINATED;
            }

            if (!ctxs[lane].wildcardEncoded ||
                ctxs[lane].wildcardEncoded == ctxs[lane].exactEncoded) {
                continue;
            }
            if (pbeProcessEncodedRange(hdr, secondaryHashTable, ruleMeta,
                                       literalBlob, literalBlobSize, a, control,
                                       &ctxs[lane], ctxs[lane].wildcardEncoded) ==
                HWLM_TERMINATED) {
                return HWLM_TERMINATED;
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
    pbeBuildLaneWindow32FromWindow(ctx.window64, pbeComputeValidMask8(a, endPos),
                                   ctx.laneWindow32, &ctx.validMask32);

    return useVector ? pbeEntryMatchMaskFromContextVector(entry, &ctx)
                     : pbeEntryMatchMaskFromContextScalar(entry, &ctx);
}

hwlm_error_t PbeEngineExec(const struct FDR *fdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t control) {
    return pbeExecWithPath(fdr, a, control, 1);
}
