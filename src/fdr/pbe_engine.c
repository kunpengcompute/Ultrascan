/*
 * Copyright (c) 2026, Huawei Technologies Co., Ltd.
 */

#include "fdr_enhanced.h"
#include "fdr_internal.h"
#include "pbe_runtime.h"
#include "util/compare.h"

static u32 pbeExtractKey(const struct PBERuntimeBitSelector *selectors,
                         u32 selectorCount, const u8 *cur,
                         const u8 *bufStart);

static int pbeEntryMayMatchAtPos(
    const struct PBERuntimeSecondaryHashEntry *entry, size_t endPos,
    size_t lenHistory, const u8 *cur);

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
        if (mlen > len || mlen > sizeof(rm->msk)) {
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

static int pbeEntryMayMatchAtPos(
    const struct PBERuntimeSecondaryHashEntry *entry, size_t endPos,
    size_t lenHistory, const u8 *cur) {
    if (!entry || !entry->ruleCount || !cur) {
        return 0;
    }

    const size_t avail = endPos + 1 + lenHistory;
    const u8 c = *cur;
    const u8 cUpper = (u8)mytoupper((char)c);
    u32 i;

    for (i = 0; i < entry->ruleCount && i < 32; i++) {
        const u8 len = entry->tableControl[i];
        const u8 tail = entry->ruleVector[i];
        if (!len || avail < len) {
            continue;
        }

        if (c == tail || cUpper == tail) {
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
    u32 lastMatchId = ~0U;

    size_t i;
    for (i = a->start_offset; i < a->len; i++) {
        const u8 *cur = a->buf + i;
        const u32 key =
            pbeExtractKey(selectors, hdr->selectorCount, cur, a->buf);
        const u32 idx = (hdr->primaryCount > 1) ? (key % hdr->primaryCount) : 0;
        const u32 secOff = primaryHashTable[idx];

        if (secOff && secOff < hdr->secondaryCount) {
            const struct PBERuntimeSecondaryHashEntry *entry =
                &secondaryHashTable[secOff];
            if (pbeEntryMayMatchAtPos(entry, i, a->len_history, cur)) {
                u32 r;
                for (r = 0; r < entry->ruleCount && r < 32; r++) {
                    const u8 len = entry->tableControl[r];
                    const u16 ridx = entry->ruleIndex[r];
                    const struct PBERuntimeRuleMeta *rm;

                    if (!len || ridx >= hdr->ruleMetaCount) {
                        continue;
                    }
                    rm = &ruleMeta[ridx];
                    if (!(rm->groups & *control)) {
                        continue;
                    }
                    if (rm->id == lastMatchId &&
                        (rm->flags & PBE_RULE_FLAG_NORUNS)) {
                        continue;
                    }

                    if (!pbeRuleExactMatch(rm, a, i, literalBlob,
                                           literalBlobSize)) {
                        continue;
                    }

                    lastMatchId = rm->id;
                    *control = a->cb(i, rm->id, a->scratch);
                    if (*control == HWLM_TERMINATE_MATCHING) {
                        return HWLM_TERMINATED;
                    }
                }
            }
        }
    }
    return HWLM_SUCCESS;
}
static u32 pbeExtractKey(const struct PBERuntimeBitSelector *selectors,
                         u32 selectorCount, const u8 *cur, const u8 *bufStart) {
    u32 key = 0;
    u32 i;

    for (i = 0; i < selectorCount && i < 32; i++) {
        const struct PBERuntimeBitSelector *s = &selectors[i];
        const u8 *p = cur - s->byteOffset;
        if (p < bufStart) {
            continue;
        }
        if ((*p >> s->bitOffset) & 0x1U) {
            key |= (1U << i);
        }
    }

    return key;
}

hwlm_error_t PbeEngineExec(const struct FDR *fdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t control) {
    if (!fdr || !fdr->pbeOffset || !fdr->pbeSize) {
        return KHSEL_NeoFdrEngineExec(fdr, a, control);
    }

    const u8 *base = (const u8 *)fdr;
    const struct PBERuntimeHeader *hdr =
        (const struct PBERuntimeHeader *)(base + fdr->pbeOffset);

    if (!pbeValidateLayout(fdr, hdr)) {
        return KHSEL_NeoFdrEngineExec(fdr, a, control);
    }

    if (!a || (hdr->flags & (PBE_RUNTIME_FLAG_PARTIAL_COVERAGE |
                       PBE_RUNTIME_FLAG_NEEDS_NEO_FALLBACK))) {
        return KHSEL_NeoFdrEngineExec(fdr, a, control);
    }

    return pbeRunNaive(hdr, a, &control);
}
