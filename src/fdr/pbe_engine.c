/*
 * Copyright (c) 2026, Huawei Technologies Co., Ltd.
 */

#include "fdr_enhanced.h"
#include "fdr_internal.h"
#include "pbe_runtime.h"
#include "util/compare.h"

static u32 pbeExtractKey(const struct PBERuntimeBitSelector *selectors,
                         u32 selectorCount,
                         const struct FDR_Runtime_Args *a, size_t endPos);

static void pbeDecodePrimaryValue(u32 encoded, u32 *offset, u32 *count);
static int pbePrimaryBitmapHasValue(const u8 *bitmap, u32 bitmapSize, u32 idx);

static int pbeEntryMayMatchAtPos(
    const struct PBERuntimeSecondaryHashEntry *entry,
    const struct FDR_Runtime_Args *a, size_t endPos);

static int pbeRunNaive(const struct PBERuntimeHeader *hdr,
                       const struct FDR_Runtime_Args *a,
                       hwlm_group_t *control);

static int pbeGetByteAt(const struct FDR_Runtime_Args *a, s64a pos, u8 *out) {
    if (!a || !out) {
        return 0;
    }
    if (pos >= 0) {
        if ((u64a)pos >= a->len) {
            return 0;
        }
        *out = a->buf[pos];
        return 1;
    }

    if (!a->buf_history || !a->len_history) {
        return 0;
    }

    // pos is negative, index from end of history.
    s64a idx = (s64a)a->len_history + pos;
    if (idx < 0 || (u64a)idx >= a->len_history) {
        return 0;
    }
    *out = a->buf_history[idx];
    return 1;
}

static u8 pbeNormalizeByte(u8 c) {
    return ourisalpha(c) ? (u8)mytoupper((char)c) : c;
}

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

static int pbePrimaryBitmapHasValue(const u8 *bitmap, u32 bitmapSize, u32 idx) {
    if (!bitmap || idx / 8U >= bitmapSize) {
        return 0;
    }
    return !!(bitmap[idx / 8U] & (1U << (idx % 8U)));
}

static int pbeEntryMayMatchAtPos(
    const struct PBERuntimeSecondaryHashEntry *entry,
    const struct FDR_Runtime_Args *a, size_t endPos) {
    u32 slot;
    if (!entry || !entry->ruleCount || !a) {
        return 0;
    }

    for (slot = 0; slot < entry->ruleCount &&
                   slot < PBE_RUNTIME_RULE_SLOTS_PER_ENTRY; slot++) {
        const u32 laneBase = slot * PBE_RUNTIME_BYTES_PER_RULE_SLOT;
        int slotMatched = 1;
        int sawValidByte = 0;
        u32 j;

        for (j = 0; j < PBE_RUNTIME_BYTES_PER_RULE_SLOT; j++) {
            const u32 vecIndex = laneBase + j;
            u8 got = 0;
            if (!entry->tableControl[vecIndex]) {
                continue;
            }
            sawValidByte = 1;
            if (!pbeGetByteAt(a,
                              (s64a)endPos -
                                  (s64a)(PBE_RUNTIME_BYTES_PER_RULE_SLOT - 1U - j),
                              &got)) {
                slotMatched = 0;
                break;
            }
            if (pbeNormalizeByte(got) != entry->ruleVector[vecIndex]) {
                slotMatched = 0;
                break;
            }
        }

        if (sawValidByte && slotMatched) {
            return 1;
        }
    }

    return 0;
}

static int pbeRunNaive(const struct PBERuntimeHeader *hdr,
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

    size_t i;
    for (i = a->start_offset; i < a->len; i++) {
        const u32 key = pbeExtractKey(selectors, hdr->selectorCount, a, i);
        const u32 idx = (key < hdr->primaryCount) ? key : 0;
        const u32 exactValue =
            pbePrimaryBitmapHasValue(primaryBitmap, hdr->primaryBitmapSize, idx)
                ? primaryHashTable[idx]
                : 0;
        const u32 wildcardValue =
            (wildcardIdx < hdr->primaryCount &&
             pbePrimaryBitmapHasValue(primaryBitmap, hdr->primaryBitmapSize,
                                      wildcardIdx))
                ? primaryHashTable[wildcardIdx]
                : 0;

#define PBE_RUN_RANGE(_encoded)                                               \
        do {                                                                  \
            const u32 __encoded = (_encoded);                                 \
            u32 __offset = 0;                                                 \
            u32 __count = 0;                                                  \
            u32 __n;                                                          \
            pbeDecodePrimaryValue(__encoded, &__offset, &__count);            \
            for (__n = 0; __n < __count; __n++) {                             \
                const u32 __off = __offset + __n;                             \
                if (!__off || __off >= hdr->secondaryCount) {                 \
                    break;                                                    \
                }                                                             \
                const struct PBERuntimeSecondaryHashEntry *entry =            \
                    &secondaryHashTable[__off];                               \
                if (!pbeEntryMayMatchAtPos(entry, a, i)) {                    \
                    continue;                                                 \
                }                                                             \
                {                                                             \
                    u32 r;                                                    \
                for (r = 0;                                                   \
                     r < entry->ruleCount &&                                  \
                         r < PBE_RUNTIME_RULE_SLOTS_PER_ENTRY;                \
                     r++) {                                                   \
                    const u16 ridx = entry->ruleIndex[r];                     \
                    const u32 kv = entry->keyValue[r];                        \
                    const u32 km = entry->keyMask[r];                         \
                    const struct PBERuntimeRuleMeta *rm;                      \
                    if (ridx >= hdr->ruleMetaCount) {                         \
                        continue;                                             \
                    }                                                         \
                    if (((key ^ kv) & km) != 0) {                             \
                        continue;                                             \
                    }                                                         \
                    rm = &ruleMeta[ridx];                                     \
                    if (!(rm->groups & *control)) {                           \
                        continue;                                             \
                    }                                                         \
                    if (!pbeRuleExactMatch(rm, a, i, literalBlob,             \
                                           literalBlobSize)) {                \
                        continue;                                             \
                    }                                                         \
                    *control = a->cb(i, rm->id, a->scratch);                  \
                    if (*control == HWLM_TERMINATE_MATCHING) {                \
                        return HWLM_TERMINATED;                               \
                    }                                                         \
                }                                                             \
                }                                                             \
            }                                                                 \
        } while (0)

        PBE_RUN_RANGE(exactValue);
        if (wildcardValue != exactValue) {
            PBE_RUN_RANGE(wildcardValue);
        }
#undef PBE_RUN_RANGE
    }
    return HWLM_SUCCESS;
}

static void pbeDecodePrimaryValue(u32 encoded, u32 *offset, u32 *count) {
    if (offset) {
        *offset = encoded & PBE_RUNTIME_L1_OFFSET_MASK;
    }
    if (count) {
        *count = encoded >> PBE_RUNTIME_L1_COUNT_SHIFT;
    }
}
static u32 pbeExtractKey(const struct PBERuntimeBitSelector *selectors,
                         u32 selectorCount,
                         const struct FDR_Runtime_Args *a, size_t endPos) {
    u32 key = 0;
    u32 i;

    for (i = 0; i < selectorCount && i < 32; i++) {
        const struct PBERuntimeBitSelector *s = &selectors[i];
        const s64a pos = (s64a)endPos - (s64a)s->byteOffset;
        u8 b = 0;
        if (!pbeGetByteAt(a, pos, &b)) {
            continue;
        }
        if ((b >> s->bitOffset) & 0x1U) {
            key |= (1U << i);
        }
    }

    return key;
}

hwlm_error_t PbeEngineExec(const struct FDR *fdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t control) {
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

    return pbeRunNaive(hdr, a, &control);
}
