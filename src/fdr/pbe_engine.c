/*
 * Copyright (c) 2026, Huawei Technologies Co., Ltd.
 */

#include "fdr_enhanced.h"
#include "fdr_internal.h"
#include "pbe_runtime.h"
#include "util/compare.h"
#include <stdio.h>
#include <stdlib.h>

struct PBEProbeStats {
    u64a scannedPositions;
    u64a primaryHits;
    u64a secondaryHits;
    u64a ruleWindowChecks;
};

static u32 pbeExtractKey(const struct PBERuntimeBitSelector *selectors,
                         u32 selectorCount, const u8 *cur,
                         const u8 *bufStart);

static int pbeEntryMayMatchAtPos(
    const struct PBERuntimeSecondaryHashEntry *entry, const u8 *cur,
    const u8 *bufStart, struct PBEProbeStats *stats);

static int pbeHasAnyCandidate(const struct PBERuntimeHeader *hdr,
                              const struct FDR_Runtime_Args *a,
                              struct PBEProbeStats *stats);

static int pbeStatsEnabled(void) {
    static int init = 0;
    static int enabled = 0;
    if (!init) {
        const char *v = getenv("HS_PBE_STATS");
        enabled = (v && v[0] == '1') ? 1 : 0;
        init = 1;
    }
    return enabled;
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
    return 1;
}

static int pbeEntryMayMatchAtPos(
    const struct PBERuntimeSecondaryHashEntry *entry, const u8 *cur,
    const u8 *bufStart, struct PBEProbeStats *stats) {
    if (!entry || !entry->ruleCount || !cur || !bufStart || cur < bufStart) {
        return 0;
    }

    const size_t avail = (size_t)(cur - bufStart) + 1;
    const u8 c = *cur;
    const u8 cUpper = (u8)mytoupper((char)c);
    u32 i;

    for (i = 0; i < entry->ruleCount && i < 32; i++) {
        if (stats) {
            stats->ruleWindowChecks++;
        }
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

static int pbeHasAnyCandidate(const struct PBERuntimeHeader *hdr,
                              const struct FDR_Runtime_Args *a,
                              struct PBEProbeStats *stats) {
    if (!hdr || !a || !a->buf || !a->len || a->start_offset >= a->len) {
        return 0;
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

    size_t i;
    for (i = a->start_offset; i < a->len; i++) {
        if (stats) {
            stats->scannedPositions++;
        }
        const u8 *cur = a->buf + i;
        const u32 key =
            pbeExtractKey(selectors, hdr->selectorCount, cur, a->buf);
        const u32 idx = (hdr->primaryCount > 1) ? (key % hdr->primaryCount) : 0;
        const u32 secOff = primaryHashTable[idx];

        if (secOff && secOff < hdr->secondaryCount) {
            if (stats) {
                stats->primaryHits++;
            }
            const struct PBERuntimeSecondaryHashEntry *entry =
                &secondaryHashTable[secOff];
            if ((u32)entry->ruleBase + (u32)entry->ruleCount >
                hdr->ruleMetaCount) {
                continue;
            }
            (void)ruleMeta;
            if (stats) {
                stats->secondaryHits++;
            }
            if (pbeEntryMayMatchAtPos(entry, cur, a->buf, stats)) {
                return 1;
            }
        }
    }
    return 0;
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

    if (!(hdr->flags & PBE_RUNTIME_FLAG_PARTIAL_COVERAGE) && a &&
        !a->len_history) {
        struct PBEProbeStats stats = {0, 0, 0, 0};
        int hasCandidate = pbeHasAnyCandidate(hdr, a, &stats);
        if (pbeStatsEnabled()) {
            fprintf(stderr,
                    "[PBE] candidate=%d scan=%llu primary=%llu secondary=%llu "
                    "rule_checks=%llu\n",
                    hasCandidate, (unsigned long long)stats.scannedPositions,
                    (unsigned long long)stats.primaryHits,
                    (unsigned long long)stats.secondaryHits,
                    (unsigned long long)stats.ruleWindowChecks);
        }
    }

    /* Phase-2 step 2: use PBE as a safe pre-check fast path. Matching and
     * callback behavior remains on the Neo path for candidate cases.
     */
    return KHSEL_NeoFdrEngineExec(fdr, a, control);
}
