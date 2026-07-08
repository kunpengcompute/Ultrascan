/*
 * Copyright (c) 2026, Huawei Technologies Co., Ltd.
 */

#include "fdr_enhanced.h"
#include "fdr_internal.h"
#include "hao_runtime.h"
#include "hao_runtime_inline.h"
#include "hao_runtime_layout.h"
#include "hao_runtime_stats.h"
#include "util/bitutils.h"
#include "util/simd_utils.h"

#include <stdint.h>
#include <arm_sve.h>

#if defined(__ARM_FEATURE_SVE2)
#define HAO_HAVE_SVE2 1
#endif

#if defined(__ARM_FEATURE_SVE2_BITPERM)
#define HAO_HAVE_SVEBITPERM 1
#endif

#ifndef HAO_PREFETCH_INPUT_DISTANCE
#define HAO_PREFETCH_INPUT_DISTANCE 32U
#endif

#define HAO_PREFETCH_R(addr) __builtin_prefetch((addr), 0, 3)

struct HAOPositionContext {
    size_t endPos;
    u32 validMask32;
};

struct HAOHashRuntime {
    u32 mode;
    u32 keyMask;
    u64a bextMask;
    u64a dotInputMask;
    u16 dotVector[HAO_RUNTIME_DOT_VECTOR_LANES];
};

static really_inline
u32 haoL2MetaRuleCount(const struct HAORuntimeL2Meta *meta) {
    u32 slot;
    u32 count = 0;

    if (!meta) {
        return 0;
    }
    for (slot = 0; slot < HAO_RUNTIME_RULE_SLOTS_PER_ENTRY; slot++) {
        count += meta->ruleIndex[slot] != HAO_RUNTIME_INVALID_RULE_INDEX;
    }
    return count;
}

static really_inline
void haoBuildHashRuntime(const struct HAORuntimeHeader *hdr,
                         struct HAOHashRuntime *hash) {
    u32 i;

    hash->mode = haoRuntimeHeaderHashMode(hdr);
    hash->keyMask = haoPackedKeyMask(haoRuntimeHeaderKeyBits(hdr));
    hash->bextMask = hdr->bextMask;
    hash->dotInputMask = hdr->dotInputMask;
    for (i = 0; i < HAO_RUNTIME_DOT_VECTOR_LANES; i++) {
        hash->dotVector[i] = haoRuntimeHeaderDotVectorLane(hdr, i);
    }
}

static really_inline
u32 haoL2ValidSlots(u32 careBits, u32 validMask32,
                    u32 matchMask) {
    u32 slot;
    u32 out = matchMask;
    const u32 invalidCare = careBits & ~validMask32;

    if (!invalidCare) {
        return out;
    }

    for (slot = 0; slot < HAO_RUNTIME_RULE_SLOTS_PER_ENTRY; slot++) {
        const u32 slotBits =
            0xffU << (slot * HAO_RUNTIME_BYTES_PER_RULE_SLOT);

        if (invalidCare & slotBits) {
            out &= ~(1U << slot);
        }
    }
    return out;
}

static really_inline
u32 haoL2MatchSve(const struct HAORuntimeL2Check *check,
                  const struct HAORuntimeL2Meta *meta,
                  const struct HAOPositionContext *ctx,
                  svuint64_t laneData, svuint64_t vslotBits) {
    const svbool_t pg = svwhilelt_b64((u32)0, HAO_RUNTIME_RULE_SLOTS_PER_ENTRY);
    const svuint64_t rule = svld1_u64(pg, (const uint64_t *)check->rule);
    const svuint64_t mask = svld1_u64(pg, (const uint64_t *)check->mask);
    const svbool_t hit =
        svcmpeq_u64(pg, svand_u64_x(pg, laneData, mask), rule);
    u32 laneMask = (u32)svorv_u64(
        pg, svsel_u64(hit, vslotBits, svdup_n_u64(0U)));

    if (unlikely(ctx->validMask32 != 0xffffffffU)) {
        laneMask = haoL2ValidSlots(
            meta->careBits, ctx->validMask32, laneMask);
    }
    return laneMask;
}

static really_inline
int haoProcessL2Entry(
    const struct HAORuntimeL2Check *check, const struct HAORuntimeL2Meta *meta,
    const struct HAORuntimeRuleMeta *ruleMeta,
    const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct HAOPositionContext *ctx,
    svuint64_t laneData, svuint64_t vslotBits, u32 *lastMatchId
#if HAO_ENABLE_RUNTIME_STATS
    ,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable, u32 l2Offset,
    int *anyReport
#endif
    ) {
    HAO_STATS_ADD(verifierCalls, 1);
    u32 matchMask = haoL2MatchSve(check, meta, ctx, laneData, vslotBits);
#if HAO_ENABLE_RUNTIME_STATS
    haoStatsObserveL2Entry(l2CheckTable, l2MetaTable, ruleMeta,
                           l2Offset, matchMask);
#endif
    if (likely(!matchMask)) {
        return HWLM_SUCCESS;
    }

    HAO_STATS_ADD(verifierEntryHits, 1);
    HAO_STATS_ADD(verifierSlotHits, popcount32(matchMask));

    while (matchMask) {
        const u32 r = ctz32(matchMask);
        const u32 ridx = meta->ruleIndex[r];
        const struct HAORuntimeRuleMeta *rm;

        rm = &ruleMeta[ridx];
        if (!(rm->groups & *control)) {
            HAO_STATS_ADD(encodedGroupRejects, 1);
            matchMask &= matchMask - 1U;
            continue;
        }

        if ((rm->flags & HAO_RULE_FLAG_NORUNS) && *lastMatchId == rm->id) {
            matchMask &= matchMask - 1U;
            continue;
        }
        *lastMatchId = rm->id;

        HAO_STATS_ADD(callbackReports, 1);
#if HAO_ENABLE_RUNTIME_STATS
        *anyReport = 1;
#endif
        *control = a->cb(ctx->endPos, rm->id, a->scratch);
        if (*control == HWLM_TERMINATE_MATCHING) {
            return HWLM_TERMINATED;
        }
        matchMask &= matchMask - 1U;
    }

    return HWLM_SUCCESS;
}

static really_inline
int haoRunL2Range(
    u32 l2EntryCount,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta,
    const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, struct HAOPositionContext *ctx,
    svuint64_t laneData, svuint64_t vslotBits, u32 encoded) {
    u32 offset = encoded & HAO_RUNTIME_L1_OFFSET_MASK;
    u32 count = encoded >> HAO_RUNTIME_L1_COUNT_SHIFT;
    u32 lastMatchId = HAO_RUNTIME_INVALID_RULE_INDEX;

#if !HAO_ENABLE_RUNTIME_STATS
    (void)l2EntryCount;
    if (likely(count == 1U)) {
        HAO_STATS_ADD(encodedRangeCalls, 1);
        return haoProcessL2Entry(
            &l2CheckTable[offset], &l2MetaTable[offset], ruleMeta, a, control,
            ctx, laneData, vslotBits, &lastMatchId);
    }

    HAO_STATS_ADD(encodedRangeCalls, 1);
    for (u32 n = 0; n < count; n++) {
        const u32 off = offset + n;
        int rv;

        rv = haoProcessL2Entry(
            &l2CheckTable[off], &l2MetaTable[off], ruleMeta, a, control, ctx,
            laneData, vslotBits, &lastMatchId);
        if (rv == HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }
#else
    u32 visitedCount = 0;
    u32 bucketRuleCount = 0;
    int anyReport = 0;
    int rangeCounted = 0;

    if (count == 1U) {
        int rv;

        if (!offset || offset >= l2EntryCount) {
            haoStatsObserveRangeShape(0, 0);
            return HWLM_SUCCESS;
        }

        HAO_STATS_ADD(encodedRangeCalls, 1);
        HAO_STATS_IF_ENABLED({
            visitedCount = 1;
            bucketRuleCount = haoL2MetaRuleCount(&l2MetaTable[offset]);
        });
        HAO_STATS_ADD(encodedEntriesVisited, 1);

        rv = haoProcessL2Entry(
            &l2CheckTable[offset], &l2MetaTable[offset], ruleMeta,
            a, control, ctx, laneData, vslotBits, &lastMatchId,
            l2CheckTable, l2MetaTable, offset,
            &anyReport);
        haoStatsObserveL2Bucket(l2CheckTable, l2MetaTable, ruleMeta,
                                offset, count, visitedCount, anyReport);
        haoStatsObserveRangeShape(visitedCount, bucketRuleCount);
        HAO_STATS_ADD(encodedRangeReportCalls, anyReport ? 1 : 0);
        return rv;
    }

    for (u32 n = 0; n < count; n++) {
        const u32 off = offset + n;
        int rv;

        if (!off || off >= l2EntryCount) {
            break;
        }

        if (!rangeCounted) {
            HAO_STATS_ADD(encodedRangeCalls, 1);
            rangeCounted = 1;
        }

        HAO_STATS_IF_ENABLED({
            visitedCount++;
            bucketRuleCount += haoL2MetaRuleCount(&l2MetaTable[off]);
        });
        HAO_STATS_ADD(encodedEntriesVisited, 1);

        rv = haoProcessL2Entry(
            &l2CheckTable[off], &l2MetaTable[off], ruleMeta, a, control, ctx,
            laneData, vslotBits, &lastMatchId,
            l2CheckTable, l2MetaTable, off, &anyReport);
        if (rv == HWLM_TERMINATED) {
            haoStatsObserveL2Bucket(l2CheckTable, l2MetaTable, ruleMeta,
                                    offset, count, visitedCount, 1);
            haoStatsObserveRangeShape(visitedCount, bucketRuleCount);
            HAO_STATS_ADD(encodedRangeReportCalls, 1);
            return HWLM_TERMINATED;
        }
    }

    if (rangeCounted) {
        haoStatsObserveL2Bucket(l2CheckTable, l2MetaTable, ruleMeta,
                                offset, count, visitedCount, anyReport);
        haoStatsObserveRangeShape(visitedCount, bucketRuleCount);
        HAO_STATS_ADD(encodedRangeReportCalls, anyReport ? 1 : 0);
    }
#endif

    return HWLM_SUCCESS;
}

#if defined(__ARM_FEATURE_SVE)

#define HAO_SVE_BATCH32_BYTES HAO_RUNTIME_BLOCK_BYTES
#define HAO_SVE_BATCH64_BYTES 64U
#define HAO_SVE_BATCH32_U32_LANES (HAO_SVE_BATCH32_BYTES / 4U)
#define HAO_SVE_BATCH64_U32_LANES (HAO_SVE_BATCH64_BYTES / 4U)

static really_inline
svbool_t haoPgB8_32(void) {
    return svptrue_b8();
}

static really_inline
svbool_t haoPgB16_16(void) {
    return svptrue_b16();
}

static really_inline
svbool_t haoPgB32_8(void) {
    return svptrue_b32();
}

static really_inline
svbool_t haoPgB8_64(void) {
    return svwhilelt_b8((u32)0, HAO_SVE_BATCH64_BYTES);
}

static really_inline
svbool_t haoPgB16_32(void) {
    return svwhilelt_b16((u32)0, HAO_SVE_BATCH64_BYTES / 2U);
}

static really_inline
svbool_t haoPgB32_16(void) {
    return svwhilelt_b32((u32)0, HAO_SVE_BATCH64_U32_LANES);
}

static really_inline
u32 haoLaneForPairIndex(u32 pair, u32 pairIdx) {
    return ((pairIdx >> 1U) << 3U) | (pair << 1U) | (pairIdx & 1U);
}

static really_inline
void haoLoadRawPrev32(const struct FDR_Runtime_Args *a, size_t blockStart,
                      svuint8_t *vlo) {
    const svbool_t pgb = haoPgB8_32();
    u8 prevBytes[HAO_RUNTIME_BLOCK_BYTES] = {0};
    u32 i;

    if (blockStart >= HAO_RUNTIME_BLOCK_BYTES) {
        *vlo = svld1_u8(
            pgb, (const uint8_t *)(a->buf + blockStart -
                                    HAO_RUNTIME_BLOCK_BYTES));
        return;
    }
    if (!blockStart && (!a->buf_history || !a->len_history)) {
        *vlo = svdup_n_u8(0);
        return;
    }

    for (i = 0; i < HAO_RUNTIME_BLOCK_BYTES; i++) {
        u8 b = 0;
        haoGetByteAt(a,
                     (s64a)blockStart - (s64a)HAO_RUNTIME_BLOCK_BYTES + i,
                     &b);
        prevBytes[i] = b;
    }

    *vlo = svld1_u8(pgb, prevBytes);
}

static really_inline
void haoLoadRawCurr32(const struct FDR_Runtime_Args *a, size_t blockStart,
                      u32 blockLaneCount, svuint8_t *vhi) {
    const svbool_t pgb = haoPgB8_32();
    u8 currBytes[HAO_RUNTIME_BLOCK_BYTES] = {0};
    u32 i;
    if (blockLaneCount == HAO_RUNTIME_BLOCK_BYTES) {
        *vhi = svld1_u8(pgb, (const uint8_t *)(a->buf + blockStart));
        return;
    }

    for (i = 0; i < blockLaneCount; i++) {
        if (blockStart + i < a->len) {
            currBytes[i] = a->buf[blockStart + i];
        }
    }

    *vhi = svld1_u8(pgb, currBytes);
}

static really_inline
void haoLoadRawPrev64(const struct FDR_Runtime_Args *a, size_t blockStart,
                      svuint8_t *vlo) {
    const svbool_t pgb = haoPgB8_64();
    u8 prevBytes[HAO_SVE_BATCH64_BYTES] = {0};
    u32 i;

    if (blockStart >= HAO_SVE_BATCH64_BYTES) {
        *vlo = svld1_u8(
            pgb, (const uint8_t *)(a->buf + blockStart -
                                    HAO_SVE_BATCH64_BYTES));
        return;
    }
    if (!blockStart && (!a->buf_history || !a->len_history)) {
        *vlo = svdup_n_u8(0);
        return;
    }

    for (i = 0; i < HAO_SVE_BATCH64_BYTES; i++) {
        u8 b = 0;
        haoGetByteAt(a,
                     (s64a)blockStart - (s64a)HAO_SVE_BATCH64_BYTES + i,
                     &b);
        prevBytes[i] = b;
    }

    *vlo = svld1_u8(pgb, prevBytes);
}

static really_inline
void haoLoadRawCurr64(const struct FDR_Runtime_Args *a, size_t blockStart,
                      u32 blockLaneCount, svuint8_t *vhi) {
    const svbool_t pgb = haoPgB8_64();
    u8 currBytes[HAO_SVE_BATCH64_BYTES] = {0};
    u32 i;
    if (blockLaneCount == HAO_SVE_BATCH64_BYTES) {
        *vhi = svld1_u8(pgb, (const uint8_t *)(a->buf + blockStart));
        return;
    }

    for (i = 0; i < blockLaneCount; i++) {
        if (blockStart + i < a->len) {
            currBytes[i] = a->buf[blockStart + i];
        }
    }

    *vhi = svld1_u8(pgb, currBytes);
}

static really_inline
svuint16_t haoLoadDotVector(const struct HAOHashRuntime *hash) {
    const svbool_t pg16 = haoPgB16_16();
    u16 coeffs[HAO_BATCH_MAX_WIDTH / 2U];
    u32 i;

    for (i = 0; i < HAO_BATCH_MAX_WIDTH / 2U; i++) {
        coeffs[i] = hash->dotVector[i & (HAO_RUNTIME_DOT_VECTOR_LANES - 1U)];
    }
    return svld1_u16(pg16, coeffs);
}

static really_inline
svuint16_t haoLoadDotVector64(const struct HAOHashRuntime *hash) {
    const svbool_t pg16 = haoPgB16_32();
    u16 coeffs[HAO_SVE_BATCH64_BYTES / 2U];
    u32 i;

    for (i = 0; i < HAO_SVE_BATCH64_BYTES / 2U; i++) {
        coeffs[i] = hash->dotVector[i & (HAO_RUNTIME_DOT_VECTOR_LANES - 1U)];
    }
    return svld1_u16(pg16, coeffs);
}

static really_inline
svuint32_t haoPackU64KeysToU32(svuint64_t key0, svuint64_t key1) {
#if defined(HAO_HAVE_SVE2)
    return svqxtnt_u64(svqxtnb_u64(key0), key1);
#else
    const svuint32_t k0 = svreinterpret_u32_u64(key0);
    const svuint32_t k1 = svreinterpret_u32_u64(key1);
    const svuint32_t lo0 = svuzp1_u32(k0, k0);
    const svuint32_t lo1 = svuzp1_u32(k1, k1);

    return svzip1_u32(lo0, lo1);
#endif
}

static really_inline
svuint8_t haoTbl2U8_32(svuint8_t vlo, svuint8_t vhi, svuint8_t idx) {
#if defined(HAO_HAVE_SVE2)
    const svuint8x2_t tbl = svcreate2_u8(vlo, vhi);

    return svtbl2_u8(tbl, idx);
#else
    const svbool_t pg = svptrue_b8();
    const svuint8_t lo = svtbl_u8(vlo, idx);
    const svuint8_t hiIdx = svsub_n_u8_x(pg, idx, HAO_RUNTIME_BLOCK_BYTES);
    const svuint8_t hi = svtbl_u8(vhi, hiIdx);

    return svorr_u8_x(pg, lo, hi);
#endif
}

static really_inline
svuint8_t haoTbl2U8_64(svuint8_t vlo, svuint8_t vhi, svuint8_t idx) {
#if defined(HAO_HAVE_SVE2)
    const svuint8x2_t tbl = svcreate2_u8(vlo, vhi);

    return svtbl2_u8(tbl, idx);
#else
    const svbool_t pg = svptrue_b8();
    const svuint8_t lo = svtbl_u8(vlo, idx);
    const svuint8_t hiIdx = svsub_n_u8_x(pg, idx, HAO_SVE_BATCH64_BYTES);
    const svuint8_t hi = svtbl_u8(vhi, hiIdx);

    return svorr_u8_x(pg, lo, hi);
#endif
}

static really_inline
svuint32_t haoRawKeyPairBext(svuint8_t vrow0, svuint8_t vrow1,
                             u64a bextMask) {
#if defined(HAO_HAVE_SVEBITPERM)
    const svuint64_t keys0 = svbext_n_u64(svreinterpret_u64_u8(vrow0), (uint64_t)bextMask);
    const svuint64_t keys1 = svbext_n_u64(svreinterpret_u64_u8(vrow1), (uint64_t)bextMask);

    return haoPackU64KeysToU32(keys0, keys1);
#else
    (void)vrow0;
    (void)vrow1;
    (void)bextMask;
    return svdup_n_u32(0);
#endif
}

static really_inline
svuint32_t haoRawKeyPairDot(svuint8_t vrow0, svuint8_t vrow1,
                            svuint16_t vdot, u64a dotInputMask,
                            u32 keyMask) {
    const svbool_t pg64 = svptrue_b64();
    const svuint64_t zero = svdup_n_u64(0U);
    const svuint64_t words0 = svand_n_u64_x(pg64, svreinterpret_u64_u8(vrow0), dotInputMask);
    const svuint64_t words1 = svand_n_u64_x(pg64, svreinterpret_u64_u8(vrow1), dotInputMask);
    const svuint64_t keys0 = svand_n_u64_x(pg64, svdot_u64(zero, svreinterpret_u16_u64(words0), vdot), keyMask);
    const svuint64_t keys1 = svand_n_u64_x(pg64, svdot_u64(zero, svreinterpret_u16_u64(words1), vdot), keyMask);

    return haoPackU64KeysToU32(keys0, keys1);
}

static really_inline
svuint32_t haoRawKeyPair(svuint8_t vrow0, svuint8_t vrow1,
                         const struct HAOHashRuntime *hash,
                         svuint16_t vdot) {
    if (hash->mode == HAO_RUNTIME_HASH_DOT) {
        return haoRawKeyPairDot(vrow0, vrow1, vdot, hash->dotInputMask,
                                hash->keyMask);
    }
    return haoRawKeyPairBext(vrow0, vrow1, hash->bextMask);
}

static really_inline
void haoPrepRawKeys(const u8 *primaryBitmap, svuint32_t vkeys,
                    svuint32_t *vbitPos, svuint32_t *vbitmapBytes) {
    const svbool_t pg32 = svptrue_b32();
    const u32 *primaryBitmapWords = (const u32 *)primaryBitmap;
    const svuint32_t vwordIdx = svlsr_n_u32_x(pg32, vkeys, 5);

    *vbitPos = svand_n_u32_x(pg32, vkeys, 31U);
    *vbitmapBytes = svld1_gather_u32index_u32(pg32, primaryBitmapWords,
                                              vwordIdx);
}

static really_inline
void haoPrepRawKeysPred(const u8 *primaryBitmap, svbool_t pg,
                              svuint32_t vkeys, svuint32_t *vbitPos,
                              svuint32_t *vbitmapBytes) {
    const u32 *primaryBitmapWords = (const u32 *)primaryBitmap;
    const svuint32_t vwordIdx = svlsr_n_u32_x(pg, vkeys, 5);

    *vbitPos = svand_n_u32_x(pg, vkeys, 31U);
    *vbitmapBytes = svld1_gather_u32index_u32(pg, primaryBitmapWords,
                                              vwordIdx);
}

static really_inline
svuint32_t haoRawLaneIds(u32 pairBase) {
    return svzip1_u32(svindex_u32(pairBase, 8U),
                      svindex_u32(pairBase + 1U, 8U));
}

static really_inline
svuint32_t haoRawLaneBits(u32 pairBase) {
    const svbool_t pg32 = haoPgB32_8();

    return svlsl_u32_x(pg32, svdup_n_u32(1U), haoRawLaneIds(pairBase));
}

static really_inline
u32 haoLaneMaskForCount32(u32 count) {
    if (count >= HAO_RUNTIME_BLOCK_BYTES) {
        return 0xffffffffU;
    }
    if (!count) {
        return 0;
    }
    return (1U << count) - 1U;
}

static really_inline
u64a haoLaneMaskForCount64(u32 count) {
    if (count >= HAO_SVE_BATCH64_BYTES) {
        return ~0ULL;
    }
    if (!count) {
        return 0;
    }
    return ((u64a)1 << count) - 1U;
}

static really_inline
svbool_t haoBitmapProbe32(svbool_t pg32, svuint32_t vbitPos,
                          svuint32_t vbitmapBytes) {
    const svuint32_t vone = svdup_n_u32(1U);
    const svuint32_t vbitMask = svlsl_u32_x(pg32, vone, vbitPos);
    const svuint32_t vhit = svand_u32_x(pg32, vbitmapBytes, vbitMask);

    return svcmpne_n_u32(pg32, vhit, 0U);
}

static really_inline
u32 haoPrimaryGather32(const u32 *primaryHashTable, svbool_t pg32,
                       svbool_t phit, svuint32_t vlaneBits,
                       svuint32_t vkeys, u32 *encodedPair) {
    const svuint32_t vzero = svdup_n_u32(0U);
    const svuint32_t vencoded =
        svld1_gather_u32index_u32(phit, primaryHashTable, vkeys);
    const svuint32_t vhitBits = svsel_u32(phit, vlaneBits, vzero);
    const u32 laneMask = svorv_u32(pg32, vhitBits);

    svst1_u32(phit, encodedPair, vencoded);

    return laneMask;
}

static really_inline
u32 haoRetireEncodedPair(const u32 *primaryHashTable, svuint32_t vlaneBits,
                         svuint32_t vkeys, svuint32_t vbitPos,
                         svuint32_t vbitmapBytes, u32 *encodedPair) {
    const svbool_t pg32 = haoPgB32_8();
    const svbool_t phit = haoBitmapProbe32(pg32, vbitPos, vbitmapBytes);

    if (likely(!svptest_any(pg32, phit))) {
        return 0;
    }

    return haoPrimaryGather32(primaryHashTable, pg32, phit, vlaneBits, vkeys,
                              encodedPair);
}

static really_inline
u32 haoRetireEncodedPairPred(const u32 *primaryHashTable, svbool_t pvalid,
                             svuint32_t vlaneBits, svuint32_t vkeys,
                             svuint32_t vbitPos, svuint32_t vbitmapBytes,
                             u32 *encodedPair) {
    const svbool_t pg32 = haoPgB32_8();
    const svuint32_t vone = svdup_n_u32(1U);
    const svuint32_t vbitMask = svlsl_u32_x(pg32, vone, vbitPos);
    const svuint32_t vrawHit = svand_u32_x(pg32, vbitmapBytes, vbitMask);
    const svuint32_t vhit = svsel_u32(pvalid, vrawHit, svdup_n_u32(0U));
    const svbool_t phit = svcmpne_n_u32(pg32, vhit, 0U);
    const svuint32_t vzero = svdup_n_u32(0U);

    if (likely(!svptest_any(pg32, phit))) {
        return 0;
    }

    const svuint32_t vencoded =
        svld1_gather_u32index_u32(phit, primaryHashTable, vkeys);
    const svuint32_t vhitBits = svsel_u32(phit, vlaneBits, vzero);
    const u32 laneMask = svorv_u32(pg32, vhitBits);

    svst1_u32(phit, encodedPair, vencoded);

    return laneMask;
}

static really_inline
u64a haoRetireEncodedPair64(const u32 *primaryHashTable, u32 pair,
                            svuint32_t vkeys, svuint32_t vbitPos,
                            svuint32_t vbitmapBytes, u32 *encodedPair) {
    const svbool_t pg32 = haoPgB32_16();
    const svuint32_t vone = svdup_n_u32(1U);
    const svuint32_t vzero = svdup_n_u32(0U);
    const svuint32_t vbitMask = svlsl_u32_x(pg32, vone, vbitPos);
    const svuint32_t vhit = svand_u32_x(pg32, vbitmapBytes, vbitMask);
    const svbool_t phit = svcmpne_n_u32(pg32, vhit, 0U);
    u64a laneMask = 0;
    u32 i;

    if (likely(!svptest_any(pg32, phit))) {
        return 0;
    }

    const svuint32_t vencoded =
        svld1_gather_u32index_u32(phit, primaryHashTable, vkeys);
    const svuint32_t vstore = svsel_u32(phit, vencoded, vzero);

    svst1_u32(pg32, encodedPair, vstore);

    for (i = 0; i < HAO_SVE_BATCH64_U32_LANES; i++) {
        if (encodedPair[i]) {
            laneMask |= (u64a)1 << haoLaneForPairIndex(pair, i);
        }
    }

    return laneMask;
}

static really_inline
u64a haoRetireEncodedPairPred64(const u32 *primaryHashTable, u32 pair,
                                svbool_t pvalid, svuint32_t vkeys,
                                svuint32_t vbitPos,
                                svuint32_t vbitmapBytes,
                                u32 *encodedPair) {
    const svbool_t pg32 = haoPgB32_16();
    const svuint32_t vone = svdup_n_u32(1U);
    const svuint32_t vzero = svdup_n_u32(0U);
    const svuint32_t vbitMask = svlsl_u32_x(pg32, vone, vbitPos);
    const svuint32_t vrawHit = svand_u32_x(pg32, vbitmapBytes, vbitMask);
    const svuint32_t vhit = svsel_u32(pvalid, vrawHit, vzero);
    const svbool_t phit = svcmpne_n_u32(pg32, vhit, 0U);
    u64a laneMask = 0;
    u32 i;

    if (likely(!svptest_any(pg32, phit))) {
        return 0;
    }

    const svuint32_t vencoded =
        svld1_gather_u32index_u32(phit, primaryHashTable, vkeys);
    const svuint32_t vstore = svsel_u32(phit, vencoded, vzero);

    svst1_u32(pg32, encodedPair, vstore);

    for (i = 0; i < HAO_SVE_BATCH64_U32_LANES; i++) {
        if (encodedPair[i]) {
            laneMask |= (u64a)1 << haoLaneForPairIndex(pair, i);
        }
    }

    return laneMask;
}

static really_inline
int haoRunEncodedLanes(
    u32 l2EntryCount,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta,
    const struct FDR_Runtime_Args *a, hwlm_group_t *control,
    size_t blockStart, u32 fullValidBlock, const u32 encodedByPair[4][8],
    u32 laneMask, svuint8_t vlo, svuint8_t vhi) {
    const svbool_t pgb = svptrue_b8();
    const svuint8_t baseIdx =
        svadd_n_u8_x(pgb, svand_n_u8_x(pgb, svindex_u8(0, 1), 7U), 25U);
    const svbool_t pg64 =
        svwhilelt_b64((u32)0, HAO_RUNTIME_RULE_SLOTS_PER_ENTRY);
    const svuint64_t vslotBits =
        svlsl_u64_x(pg64, svdup_n_u64(1U), svindex_u64(0, 1));

    while (laneMask) {
        const u32 lane = (u32)__builtin_ctz(laneMask);
        const u32 pair = (lane & 7U) >> 1U;
        const u32 pairIdx = ((lane >> 3U) << 1U) | (lane & 1U);
        const u32 encoded = encodedByPair[pair][pairIdx];

        laneMask &= laneMask - 1U;
        struct HAOPositionContext ctx;
        size_t endPos = blockStart + lane;
        u32 validMask32 = 0xffffffffU;
        svuint8_t laneIdx = svadd_n_u8_x(pgb, baseIdx, (uint8_t)lane);
        svuint8_t laneBytes = haoTbl2U8_32(vlo, vhi, laneIdx);
        svuint64_t laneData = svreinterpret_u64_u8(laneBytes);

        if (unlikely(!fullValidBlock)) {
            validMask32 = haoComputeValidMask8(a, endPos) * 0x01010101U;
        }
        ctx.endPos = endPos;
        ctx.validMask32 = validMask32;
        if (likely(haoRunL2Range(l2EntryCount, l2CheckTable,
                l2MetaTable, ruleMeta, a, control, &ctx, laneData, vslotBits,
                encoded) == HWLM_TERMINATED)) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static really_inline
int haoRunEncodedLanes64(
    u32 l2EntryCount,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta,
    const struct FDR_Runtime_Args *a, hwlm_group_t *control,
    size_t blockStart, u32 fullValidBlock, const u32 encodedByPair[4][16],
    u64a laneMask, svuint8_t vlo, svuint8_t vhi) {
    const svbool_t pgb = svptrue_b8();
    const svuint8_t baseIdx =
        svadd_n_u8_x(pgb, svand_n_u8_x(pgb, svindex_u8(0, 1), 7U), 57U);
    const svbool_t pg64 =
        svwhilelt_b64((u32)0, HAO_RUNTIME_RULE_SLOTS_PER_ENTRY);
    const svuint64_t vslotBits =
        svlsl_u64_x(pg64, svdup_n_u64(1U), svindex_u64(0, 1));

    while (laneMask) {
        const u32 lane = ctz64(laneMask);
        const u32 pair = (lane & 7U) >> 1U;
        const u32 pairIdx = ((lane >> 3U) << 1U) | (lane & 1U);
        const u32 encoded = encodedByPair[pair][pairIdx];

        laneMask &= laneMask - 1U;
        struct HAOPositionContext ctx;
        size_t endPos = blockStart + lane;
        u32 validMask32 = 0xffffffffU;
        svuint8_t laneIdx = svadd_n_u8_x(pgb, baseIdx, (uint8_t)lane);
        svuint8_t laneBytes = haoTbl2U8_64(vlo, vhi, laneIdx);
        svuint64_t laneData = svreinterpret_u64_u8(laneBytes);

        if (unlikely(!fullValidBlock)) {
            validMask32 = haoComputeValidMask8(a, endPos) * 0x01010101U;
        }
        ctx.endPos = endPos;
        ctx.validMask32 = validMask32;
        if (likely(haoRunL2Range(l2EntryCount, l2CheckTable,
                l2MetaTable, ruleMeta, a, control, &ctx, laneData, vslotBits,
                encoded) == HWLM_TERMINATED)) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static really_inline
u32 haoCollectRawEncoded(const u8 *primaryBitmap,
                         const u32 *primaryHashTable,
                         const struct HAOHashRuntime *hash,
                         svuint16_t vdot,
                         svuint8_t vlo, svuint8_t vhi,
                         svuint32_t vlaneBits01,
                         svuint32_t vlaneBits23,
                         svuint32_t vlaneBits45,
                         svuint32_t vlaneBits67,
                         u32 encodedByPair[4][8]) {
    u32 laneMask = 0;
    svuint32_t vbitPos01;
    svuint32_t vbitPos23;
    svuint32_t vbitPos45;
    svuint32_t vbitPos67;
    svuint32_t vbitmapBytes01;
    svuint32_t vbitmapBytes23;
    svuint32_t vbitmapBytes45;
    svuint32_t vbitmapBytes67;
    const svuint8_t vrow0 = svext_u8(vlo, vhi, 25);
    const svuint8_t vrow1 = svext_u8(vlo, vhi, 26);
    const svuint8_t vrow2 = svext_u8(vlo, vhi, 27);
    const svuint8_t vrow3 = svext_u8(vlo, vhi, 28);
    const svuint8_t vrow4 = svext_u8(vlo, vhi, 29);
    const svuint8_t vrow5 = svext_u8(vlo, vhi, 30);
    const svuint8_t vrow6 = svext_u8(vlo, vhi, 31);
    const svuint8_t vrow7 = vhi;

    svuint32_t vkeys01;
    svuint32_t vkeys23;
    svuint32_t vkeys45;
    svuint32_t vkeys67;

    if (hash->mode == HAO_RUNTIME_HASH_DOT) {
        vkeys01 = haoRawKeyPairDot(vrow0, vrow1, vdot, hash->dotInputMask,
                                   hash->keyMask);
        vkeys23 = haoRawKeyPairDot(vrow2, vrow3, vdot, hash->dotInputMask,
                                   hash->keyMask);
        vkeys45 = haoRawKeyPairDot(vrow4, vrow5, vdot, hash->dotInputMask,
                                   hash->keyMask);
        vkeys67 = haoRawKeyPairDot(vrow6, vrow7, vdot, hash->dotInputMask,
                                   hash->keyMask);
    } else {
        vkeys01 = haoRawKeyPairBext(vrow0, vrow1, hash->bextMask);
        vkeys23 = haoRawKeyPairBext(vrow2, vrow3, hash->bextMask);
        vkeys45 = haoRawKeyPairBext(vrow4, vrow5, hash->bextMask);
        vkeys67 = haoRawKeyPairBext(vrow6, vrow7, hash->bextMask);
    }

    haoPrepRawKeys(primaryBitmap, vkeys01, &vbitPos01, &vbitmapBytes01);
    haoPrepRawKeys(primaryBitmap, vkeys23, &vbitPos23, &vbitmapBytes23);
    haoPrepRawKeys(primaryBitmap, vkeys45, &vbitPos45, &vbitmapBytes45);
    haoPrepRawKeys(primaryBitmap, vkeys67, &vbitPos67, &vbitmapBytes67);

    laneMask |= haoRetireEncodedPair(
        primaryHashTable, vlaneBits01, vkeys01, vbitPos01, vbitmapBytes01, encodedByPair[0]);
    laneMask |= haoRetireEncodedPair(
        primaryHashTable, vlaneBits23, vkeys23, vbitPos23, vbitmapBytes23, encodedByPair[1]);
    laneMask |= haoRetireEncodedPair(
        primaryHashTable, vlaneBits45, vkeys45, vbitPos45, vbitmapBytes45, encodedByPair[2]);
    laneMask |= haoRetireEncodedPair(
        primaryHashTable, vlaneBits67, vkeys67, vbitPos67, vbitmapBytes67, encodedByPair[3]);

    HAO_STATS_ADD(primaryActiveLanes, (u32)__builtin_popcount(laneMask));
    return laneMask;
}

static really_inline
int haoRunRaw32(
    const struct HAOHashRuntime *hash, svuint16_t vdot,
    u32 l2EntryCount, const u8 *primaryBitmap,
    const u32 *primaryHashTable,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control,
    size_t blockStart, svuint8_t vlo, svuint8_t vhi,
    svuint32_t vlaneBits01, svuint32_t vlaneBits23,
    svuint32_t vlaneBits45, svuint32_t vlaneBits67) {
    u32 encodedByPair[4][8];
    const u32 laneMask = haoCollectRawEncoded(
        primaryBitmap, primaryHashTable, hash, vdot, vlo, vhi,
        vlaneBits01, vlaneBits23, vlaneBits45, vlaneBits67,
        encodedByPair);
    const u32 fullValidBlock =
        blockStart + a->len_history >= HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U;

    if (likely(laneMask)) {
        if (haoRunEncodedLanes(
                l2EntryCount, l2CheckTable, l2MetaTable, ruleMeta, a,
                control, blockStart, fullValidBlock, encodedByPair, laneMask,
                vlo, vhi) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static really_inline
u64a haoCollectRawEncoded64(const u8 *primaryBitmap,
                          const u32 *primaryHashTable,
                          const struct HAOHashRuntime *hash,
                          svuint16_t vdot, svuint8_t vlo, svuint8_t vhi,
                          u32 encodedByPair[4][16]) {
    u64a laneMask = 0;
    svuint32_t vbitPos01;
    svuint32_t vbitPos23;
    svuint32_t vbitPos45;
    svuint32_t vbitPos67;
    svuint32_t vbitmapBytes01;
    svuint32_t vbitmapBytes23;
    svuint32_t vbitmapBytes45;
    svuint32_t vbitmapBytes67;
    const svuint8_t vrow0 = svext_u8(vlo, vhi, 57);
    const svuint8_t vrow1 = svext_u8(vlo, vhi, 58);
    const svuint8_t vrow2 = svext_u8(vlo, vhi, 59);
    const svuint8_t vrow3 = svext_u8(vlo, vhi, 60);
    const svuint8_t vrow4 = svext_u8(vlo, vhi, 61);
    const svuint8_t vrow5 = svext_u8(vlo, vhi, 62);
    const svuint8_t vrow6 = svext_u8(vlo, vhi, 63);
    const svuint8_t vrow7 = vhi;
    svuint32_t vkeys01;
    svuint32_t vkeys23;
    svuint32_t vkeys45;
    svuint32_t vkeys67;
    const svbool_t pg32 = haoPgB32_16();

    if (hash->mode == HAO_RUNTIME_HASH_DOT) {
        vkeys01 = haoRawKeyPairDot(vrow0, vrow1, vdot, hash->dotInputMask,
                                   hash->keyMask);
        vkeys23 = haoRawKeyPairDot(vrow2, vrow3, vdot, hash->dotInputMask,
                                   hash->keyMask);
        vkeys45 = haoRawKeyPairDot(vrow4, vrow5, vdot, hash->dotInputMask,
                                   hash->keyMask);
        vkeys67 = haoRawKeyPairDot(vrow6, vrow7, vdot, hash->dotInputMask,
                                   hash->keyMask);
    } else {
        vkeys01 = haoRawKeyPairBext(vrow0, vrow1, hash->bextMask);
        vkeys23 = haoRawKeyPairBext(vrow2, vrow3, hash->bextMask);
        vkeys45 = haoRawKeyPairBext(vrow4, vrow5, hash->bextMask);
        vkeys67 = haoRawKeyPairBext(vrow6, vrow7, hash->bextMask);
    }

    haoPrepRawKeysPred(primaryBitmap, pg32, vkeys01, &vbitPos01,
                       &vbitmapBytes01);
    haoPrepRawKeysPred(primaryBitmap, pg32, vkeys23, &vbitPos23,
                       &vbitmapBytes23);
    haoPrepRawKeysPred(primaryBitmap, pg32, vkeys45, &vbitPos45,
                       &vbitmapBytes45);
    haoPrepRawKeysPred(primaryBitmap, pg32, vkeys67, &vbitPos67,
                       &vbitmapBytes67);

    laneMask |= haoRetireEncodedPair64(
        primaryHashTable, 0U, vkeys01, vbitPos01, vbitmapBytes01,
        encodedByPair[0]);
    laneMask |= haoRetireEncodedPair64(
        primaryHashTable, 1U, vkeys23, vbitPos23, vbitmapBytes23,
        encodedByPair[1]);
    laneMask |= haoRetireEncodedPair64(
        primaryHashTable, 2U, vkeys45, vbitPos45, vbitmapBytes45,
        encodedByPair[2]);
    laneMask |= haoRetireEncodedPair64(
        primaryHashTable, 3U, vkeys67, vbitPos67, vbitmapBytes67,
        encodedByPair[3]);

    HAO_STATS_ADD(primaryActiveLanes, popcount64(laneMask));
    return laneMask;
}

static really_inline
int haoRunRaw64(
    const struct HAOHashRuntime *hash, svuint16_t vdot,
    u32 l2EntryCount, const u8 *primaryBitmap,
    const u32 *primaryHashTable,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, size_t blockStart, svuint8_t vlo,
    svuint8_t vhi) {
    u32 encodedByPair[4][16];
    const u64a laneMask = haoCollectRawEncoded64(
        primaryBitmap, primaryHashTable, hash, vdot, vlo, vhi, encodedByPair);
    const u32 fullValidBlock =
        blockStart + a->len_history >= HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U;

    if (likely(laneMask)) {
        if (haoRunEncodedLanes64(
                l2EntryCount, l2CheckTable, l2MetaTable, ruleMeta, a,
                control, blockStart, fullValidBlock, encodedByPair, laneMask,
                vlo, vhi) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static really_inline
u32 haoCollectRawTailEncoded(const u8 *primaryBitmap,
                             const u32 *primaryHashTable,
                             const struct HAOHashRuntime *hash,
                             svuint16_t vdot,
                             u32 blockLaneCount, svuint8_t vlo, svuint8_t vhi,
                             svuint32_t vlaneIds01,
                             svuint32_t vlaneIds23,
                             svuint32_t vlaneIds45,
                             svuint32_t vlaneIds67,
                             svuint32_t vlaneBits01,
                             svuint32_t vlaneBits23,
                             svuint32_t vlaneBits45,
                             svuint32_t vlaneBits67,
                             u32 encodedByPair[4][8]) {
    u32 laneMask = 0;
    svuint32_t vbitPos01;
    svuint32_t vbitPos23;
    svuint32_t vbitPos45;
    svuint32_t vbitPos67;
    svuint32_t vbitmapBytes01;
    svuint32_t vbitmapBytes23;
    svuint32_t vbitmapBytes45;
    svuint32_t vbitmapBytes67;

    const svuint8_t vrow0 = svext_u8(vlo, vhi, 25);
    const svuint8_t vrow1 = svext_u8(vlo, vhi, 26);
    const svuint8_t vrow2 = svext_u8(vlo, vhi, 27);
    const svuint8_t vrow3 = svext_u8(vlo, vhi, 28);
    const svuint8_t vrow4 = svext_u8(vlo, vhi, 29);
    const svuint8_t vrow5 = svext_u8(vlo, vhi, 30);
    const svuint8_t vrow6 = svext_u8(vlo, vhi, 31);
    const svuint8_t vrow7 = vhi;

    svuint32_t vkeys01;
    svuint32_t vkeys23;
    svuint32_t vkeys45;
    svuint32_t vkeys67;

    if (hash->mode == HAO_RUNTIME_HASH_DOT) {
        vkeys01 = haoRawKeyPairDot(vrow0, vrow1, vdot, hash->dotInputMask,
                                   hash->keyMask);
        vkeys23 = haoRawKeyPairDot(vrow2, vrow3, vdot, hash->dotInputMask,
                                   hash->keyMask);
        vkeys45 = haoRawKeyPairDot(vrow4, vrow5, vdot, hash->dotInputMask,
                                   hash->keyMask);
        vkeys67 = haoRawKeyPairDot(vrow6, vrow7, vdot, hash->dotInputMask,
                                   hash->keyMask);
    } else {
        vkeys01 = haoRawKeyPairBext(vrow0, vrow1, hash->bextMask);
        vkeys23 = haoRawKeyPairBext(vrow2, vrow3, hash->bextMask);
        vkeys45 = haoRawKeyPairBext(vrow4, vrow5, hash->bextMask);
        vkeys67 = haoRawKeyPairBext(vrow6, vrow7, hash->bextMask);
    }

    {
        const svbool_t pg32 = haoPgB32_8();
        const svbool_t pvalid01 =
            svcmplt_n_u32(pg32, vlaneIds01, blockLaneCount);
        const svbool_t pvalid23 =
            svcmplt_n_u32(pg32, vlaneIds23, blockLaneCount);
        const svbool_t pvalid45 =
            svcmplt_n_u32(pg32, vlaneIds45, blockLaneCount);
        const svbool_t pvalid67 =
            svcmplt_n_u32(pg32, vlaneIds67, blockLaneCount);

        haoPrepRawKeysPred(primaryBitmap, pvalid01, vkeys01,
                                 &vbitPos01, &vbitmapBytes01);
        haoPrepRawKeysPred(primaryBitmap, pvalid23, vkeys23,
                                 &vbitPos23, &vbitmapBytes23);
        haoPrepRawKeysPred(primaryBitmap, pvalid45, vkeys45,
                                 &vbitPos45, &vbitmapBytes45);
        haoPrepRawKeysPred(primaryBitmap, pvalid67, vkeys67,
                                 &vbitPos67, &vbitmapBytes67);

        laneMask |= haoRetireEncodedPairPred(
            primaryHashTable, pvalid01, vlaneBits01, vkeys01, vbitPos01,
            vbitmapBytes01, encodedByPair[0]);
        laneMask |= haoRetireEncodedPairPred(
            primaryHashTable, pvalid23, vlaneBits23, vkeys23, vbitPos23,
            vbitmapBytes23, encodedByPair[1]);
        laneMask |= haoRetireEncodedPairPred(
            primaryHashTable, pvalid45, vlaneBits45, vkeys45, vbitPos45,
            vbitmapBytes45, encodedByPair[2]);
        laneMask |= haoRetireEncodedPairPred(
            primaryHashTable, pvalid67, vlaneBits67, vkeys67, vbitPos67,
            vbitmapBytes67, encodedByPair[3]);
    }

    laneMask &= haoLaneMaskForCount32(blockLaneCount);
    HAO_STATS_ADD(primaryActiveLanes, (u32)__builtin_popcount(laneMask));
    return laneMask;
}

static really_inline
int haoRunRawTailVec(
    const struct HAOHashRuntime *hash, svuint16_t vdot,
    u32 l2EntryCount, const u8 *primaryBitmap,
    const u32 *primaryHashTable,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control,
    size_t blockStart, u32 blockLaneCount, svuint8_t vlo, svuint8_t vhi,
    svuint32_t vlaneIds01, svuint32_t vlaneIds23,
    svuint32_t vlaneIds45, svuint32_t vlaneIds67,
    svuint32_t vlaneBits01, svuint32_t vlaneBits23,
    svuint32_t vlaneBits45, svuint32_t vlaneBits67) {
    u32 encodedByPair[4][8] = {{0}};
    const u32 laneMask = haoCollectRawTailEncoded(
        primaryBitmap, primaryHashTable, hash, vdot, blockLaneCount,
        vlo, vhi, vlaneIds01, vlaneIds23, vlaneIds45, vlaneIds67,
        vlaneBits01, vlaneBits23, vlaneBits45, vlaneBits67,
        encodedByPair);
    const u32 fullValidBlock =
        blockStart + a->len_history >= HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U;

    if (likely(laneMask)) {
        if (haoRunEncodedLanes(
                l2EntryCount, l2CheckTable, l2MetaTable, ruleMeta, a,
                control, blockStart, fullValidBlock, encodedByPair, laneMask,
                vlo, vhi) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static really_inline
u64a haoCollectRawTailEncoded64(const u8 *primaryBitmap,
                              const u32 *primaryHashTable,
                              const struct HAOHashRuntime *hash,
                              svuint16_t vdot, u32 blockLaneCount,
                              svuint8_t vlo, svuint8_t vhi,
                              svuint32_t vlaneIds01,
                              svuint32_t vlaneIds23,
                              svuint32_t vlaneIds45,
                              svuint32_t vlaneIds67,
                              u32 encodedByPair[4][16]) {
    u64a laneMask = 0;
    svuint32_t vbitPos01;
    svuint32_t vbitPos23;
    svuint32_t vbitPos45;
    svuint32_t vbitPos67;
    svuint32_t vbitmapBytes01;
    svuint32_t vbitmapBytes23;
    svuint32_t vbitmapBytes45;
    svuint32_t vbitmapBytes67;
    const svuint8_t vrow0 = svext_u8(vlo, vhi, 57);
    const svuint8_t vrow1 = svext_u8(vlo, vhi, 58);
    const svuint8_t vrow2 = svext_u8(vlo, vhi, 59);
    const svuint8_t vrow3 = svext_u8(vlo, vhi, 60);
    const svuint8_t vrow4 = svext_u8(vlo, vhi, 61);
    const svuint8_t vrow5 = svext_u8(vlo, vhi, 62);
    const svuint8_t vrow6 = svext_u8(vlo, vhi, 63);
    const svuint8_t vrow7 = vhi;
    svuint32_t vkeys01;
    svuint32_t vkeys23;
    svuint32_t vkeys45;
    svuint32_t vkeys67;
    const svbool_t pg32 = haoPgB32_16();
    const svbool_t pvalid01 = svcmplt_n_u32(pg32, vlaneIds01, blockLaneCount);
    const svbool_t pvalid23 = svcmplt_n_u32(pg32, vlaneIds23, blockLaneCount);
    const svbool_t pvalid45 = svcmplt_n_u32(pg32, vlaneIds45, blockLaneCount);
    const svbool_t pvalid67 = svcmplt_n_u32(pg32, vlaneIds67, blockLaneCount);

    if (hash->mode == HAO_RUNTIME_HASH_DOT) {
        vkeys01 = haoRawKeyPairDot(vrow0, vrow1, vdot, hash->dotInputMask,
                                   hash->keyMask);
        vkeys23 = haoRawKeyPairDot(vrow2, vrow3, vdot, hash->dotInputMask,
                                   hash->keyMask);
        vkeys45 = haoRawKeyPairDot(vrow4, vrow5, vdot, hash->dotInputMask,
                                   hash->keyMask);
        vkeys67 = haoRawKeyPairDot(vrow6, vrow7, vdot, hash->dotInputMask,
                                   hash->keyMask);
    } else {
        vkeys01 = haoRawKeyPairBext(vrow0, vrow1, hash->bextMask);
        vkeys23 = haoRawKeyPairBext(vrow2, vrow3, hash->bextMask);
        vkeys45 = haoRawKeyPairBext(vrow4, vrow5, hash->bextMask);
        vkeys67 = haoRawKeyPairBext(vrow6, vrow7, hash->bextMask);
    }

    haoPrepRawKeysPred(primaryBitmap, pvalid01, vkeys01, &vbitPos01,
                       &vbitmapBytes01);
    haoPrepRawKeysPred(primaryBitmap, pvalid23, vkeys23, &vbitPos23,
                       &vbitmapBytes23);
    haoPrepRawKeysPred(primaryBitmap, pvalid45, vkeys45, &vbitPos45,
                       &vbitmapBytes45);
    haoPrepRawKeysPred(primaryBitmap, pvalid67, vkeys67, &vbitPos67,
                       &vbitmapBytes67);

    laneMask |= haoRetireEncodedPairPred64(
        primaryHashTable, 0U, pvalid01, vkeys01, vbitPos01, vbitmapBytes01,
        encodedByPair[0]);
    laneMask |= haoRetireEncodedPairPred64(
        primaryHashTable, 1U, pvalid23, vkeys23, vbitPos23, vbitmapBytes23,
        encodedByPair[1]);
    laneMask |= haoRetireEncodedPairPred64(
        primaryHashTable, 2U, pvalid45, vkeys45, vbitPos45, vbitmapBytes45,
        encodedByPair[2]);
    laneMask |= haoRetireEncodedPairPred64(
        primaryHashTable, 3U, pvalid67, vkeys67, vbitPos67, vbitmapBytes67,
        encodedByPair[3]);

    laneMask &= haoLaneMaskForCount64(blockLaneCount);
    HAO_STATS_ADD(primaryActiveLanes, popcount64(laneMask));
    return laneMask;
}

static really_inline
int haoRunRawTailVec64(
    const struct HAOHashRuntime *hash, svuint16_t vdot,
    u32 l2EntryCount, const u8 *primaryBitmap,
    const u32 *primaryHashTable,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, size_t blockStart, u32 blockLaneCount,
    svuint8_t vlo, svuint8_t vhi, svuint32_t vlaneIds01,
    svuint32_t vlaneIds23, svuint32_t vlaneIds45,
    svuint32_t vlaneIds67) {
    u32 encodedByPair[4][16] = {{0}};
    const u64a laneMask = haoCollectRawTailEncoded64(
        primaryBitmap, primaryHashTable, hash, vdot, blockLaneCount, vlo, vhi,
        vlaneIds01, vlaneIds23, vlaneIds45, vlaneIds67, encodedByPair);
    const u32 fullValidBlock =
        blockStart + a->len_history >= HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U;

    if (likely(laneMask)) {
        if (haoRunEncodedLanes64(
                l2EntryCount, l2CheckTable, l2MetaTable, ruleMeta, a,
                control, blockStart, fullValidBlock, encodedByPair, laneMask,
                vlo, vhi) ==
            HWLM_TERMINATED) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static really_inline
u64a haoBuildRawWordScalar(const struct FDR_Runtime_Args *a, size_t endPos) {
    u8 bytes[HAO_RUNTIME_BYTES_PER_RULE_SLOT];
    u32 i;

    assert(a);

    for (i = 0; i < HAO_RUNTIME_BYTES_PER_RULE_SLOT; i++) {
        u8 b = 0;
        const s64a pos = (s64a)endPos -
                         (s64a)(HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U) +
                         (s64a)i;

        haoGetByteAt(a, pos, &b);
        bytes[i] = b;
    }

    return unaligned_load_u64a(bytes);
}

static really_inline
int haoRawBitmapHitScalar(const u8 *primaryBitmap, u32 key) {
    const u32 *primaryBitmapWords = (const u32 *)primaryBitmap;
    const u32 word = primaryBitmapWords[key >> 5U];
    return word & (1U << (key & 31U));
}

static really_inline
u32 haoHashRuntimeScalar(const struct HAOHashRuntime *hash, u64a rawWord) {
    if (hash->mode == HAO_RUNTIME_HASH_DOT) {
        const u64a maskedWord = rawWord & hash->dotInputMask;
        u64a dot = 0;
        u32 i;

        for (i = 0; i < HAO_RUNTIME_DOT_VECTOR_LANES; i++) {
            const u64a word = (maskedWord >> (i * 16U)) & 0xffffU;
            dot += word * hash->dotVector[i];
        }
        return (u32)(dot & hash->keyMask);
    }

    return (u32)pext64(rawWord, hash->bextMask);
}

static really_inline
int haoRunRawTailScalar(
    const struct HAOHashRuntime *hash, u32 l2EntryCount,
    const u8 *primaryBitmap,
    const u32 *primaryHashTable,
    const struct HAORuntimeL2Check *l2CheckTable,
    const struct HAORuntimeL2Meta *l2MetaTable,
    const struct HAORuntimeRuleMeta *ruleMeta, const struct FDR_Runtime_Args *a,
    hwlm_group_t *control, size_t blockStart, u32 blockLaneCount) {
    u32 lane;
    u32 activeCount = 0;
    const svbool_t pg64 =
        svwhilelt_b64((u32)0, HAO_RUNTIME_RULE_SLOTS_PER_ENTRY);
    const svuint64_t vslotBits =
        svlsl_u64_x(pg64, svdup_n_u64(1U), svindex_u64(0, 1));

    for (lane = 0; lane < blockLaneCount; lane++) {
        struct HAOPositionContext ctx;
        const size_t endPos = blockStart + lane;
        const u64a rawWord = haoBuildRawWordScalar(a, endPos);
        const u32 key = haoHashRuntimeScalar(hash, rawWord);

        if (haoRawBitmapHitScalar(primaryBitmap, key)) {
            const u32 encoded = primaryHashTable[key];
            const u32 validMask8 = haoComputeValidMask8(a, endPos);
            const svuint64_t laneData = svdup_n_u64(rawWord);

            if (encoded) {
                activeCount++;
                ctx.endPos = endPos;
                ctx.validMask32 = validMask8 * 0x01010101U;
                if (haoRunL2Range(
                        l2EntryCount, l2CheckTable, l2MetaTable,
                        ruleMeta, a, control, &ctx, laneData, vslotBits,
                        encoded) ==
                    HWLM_TERMINATED) {
                    HAO_STATS_ADD(primaryActiveLanes, activeCount);
                    return HWLM_TERMINATED;
                }
            }
        }

    }

    HAO_STATS_ADD(primaryActiveLanes, activeCount);
    return HWLM_SUCCESS;
}

#endif

static int haoRunBatchBlob32(const struct HAORuntimeHeader *hdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t *control) {
    const u8 *primaryBitmap;
    const u32 *primaryHashTable;
    const struct HAORuntimeL2Check *l2CheckTable;
    const struct HAORuntimeL2Meta *l2MetaTable;
    const struct HAORuntimeRuleMeta *ruleMeta;
    struct HAOHashRuntime hash;
    svuint16_t vdot;
    u32 l2EntryCount;
    size_t i = a->start_offset;

    haoRefreshStatsMode();
    HAO_STATS_ADD(scanCalls, 1);
    HAO_STATS_ADD(scanInputBytes, a->len - a->start_offset);

    primaryBitmap = (const u8 *)hdr + hdr->primaryBitmapOffset;
    primaryHashTable =
        (const u32 *)((const u8 *)hdr + hdr->primaryOffset);
    l2CheckTable = (const struct HAORuntimeL2Check *)((const u8 *)hdr +
                                                      hdr->l2CheckOffset);
    l2MetaTable = (const struct HAORuntimeL2Meta *)((const u8 *)hdr +
                                                    hdr->l2MetaOffset);
    ruleMeta = (const struct HAORuntimeRuleMeta *)((const u8 *)hdr +
                                                   hdr->ruleMetaOffset);
    haoBuildHashRuntime(hdr, &hash);
    vdot = haoLoadDotVector(&hash);
    l2EntryCount = hdr->l2EntryCount;

    const svuint32_t vlaneBits01 = haoRawLaneBits(0U);
    const svuint32_t vlaneBits23 = haoRawLaneBits(2U);
    const svuint32_t vlaneBits45 = haoRawLaneBits(4U);
    const svuint32_t vlaneBits67 = haoRawLaneBits(6U);

    svuint8_t rawPrev = svdup_n_u8(0);
    haoLoadRawPrev32(a, i, &rawPrev);
    for ( ; i + HAO_RUNTIME_BLOCK_BYTES <= a->len; i += HAO_RUNTIME_BLOCK_BYTES) {
        svuint8_t rawCurr;

        HAO_PREFETCH_R(a->buf + i + HAO_PREFETCH_INPUT_DISTANCE);

        haoLoadRawCurr32(a, i, HAO_RUNTIME_BLOCK_BYTES, &rawCurr);

        HAO_STATS_ADD(blockCalls, 1);
        HAO_STATS_ADD(blockLanes, HAO_BATCH_MAX_WIDTH);
        HAO_STATS_ADD(primaryProbeLanes, HAO_BATCH_MAX_WIDTH);
        int rt = haoRunRaw32(&hash, vdot, l2EntryCount, primaryBitmap,
                                primaryHashTable, l2CheckTable,
                                l2MetaTable, ruleMeta, a, control, i,
                                rawPrev, rawCurr, vlaneBits01, vlaneBits23,
                                vlaneBits45, vlaneBits67);
        if (likely(rt == HWLM_TERMINATED)) {
            return HWLM_TERMINATED;
        }
        rawPrev = rawCurr;
    }

    if (i < a->len) {
        svuint8_t rawCurr;
        const u32 blockLaneCount = (u32)(a->len - i);
        haoLoadRawCurr32(a, i, blockLaneCount, &rawCurr);

        HAO_STATS_ADD(blockCalls, 1);
        HAO_STATS_ADD(blockLanes, blockLaneCount);
        HAO_STATS_ADD(primaryProbeLanes, blockLaneCount);
        const svuint32_t vlaneIds01 = haoRawLaneIds(0U);
        const svuint32_t vlaneIds23 = haoRawLaneIds(2U);
        const svuint32_t vlaneIds45 = haoRawLaneIds(4U);
        const svuint32_t vlaneIds67 = haoRawLaneIds(6U);
        int rt = haoRunRawTailVec(&hash, vdot, l2EntryCount, primaryBitmap,
                                    primaryHashTable, l2CheckTable,
                                    l2MetaTable, ruleMeta, a, control, i,
                                    blockLaneCount, rawPrev, rawCurr,
                                    vlaneIds01, vlaneIds23, vlaneIds45,
                                    vlaneIds67, vlaneBits01, vlaneBits23,
                                    vlaneBits45, vlaneBits67);
        if (likely(rt == HWLM_TERMINATED)) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static int haoRunBatchBlob64(const struct HAORuntimeHeader *hdr,
                             const struct FDR_Runtime_Args *a,
                             hwlm_group_t *control) {
    const u8 *primaryBitmap;
    const u32 *primaryHashTable;
    const struct HAORuntimeL2Check *l2CheckTable;
    const struct HAORuntimeL2Meta *l2MetaTable;
    const struct HAORuntimeRuleMeta *ruleMeta;
    struct HAOHashRuntime hash;
    svuint16_t vdot;
    u32 l2EntryCount;
    size_t i = a->start_offset;

    haoRefreshStatsMode();
    HAO_STATS_ADD(scanCalls, 1);
    HAO_STATS_ADD(scanInputBytes, a->len - a->start_offset);

    primaryBitmap = (const u8 *)hdr + hdr->primaryBitmapOffset;
    primaryHashTable =
        (const u32 *)((const u8 *)hdr + hdr->primaryOffset);
    l2CheckTable = (const struct HAORuntimeL2Check *)((const u8 *)hdr +
                                                      hdr->l2CheckOffset);
    l2MetaTable = (const struct HAORuntimeL2Meta *)((const u8 *)hdr +
                                                    hdr->l2MetaOffset);
    ruleMeta = (const struct HAORuntimeRuleMeta *)((const u8 *)hdr +
                                                   hdr->ruleMetaOffset);
    haoBuildHashRuntime(hdr, &hash);
    vdot = haoLoadDotVector64(&hash);
    l2EntryCount = hdr->l2EntryCount;

    svuint8_t rawPrev = svdup_n_u8(0);
    haoLoadRawPrev64(a, i, &rawPrev);
    for ( ; i + HAO_SVE_BATCH64_BYTES <= a->len; i += HAO_SVE_BATCH64_BYTES) {
        svuint8_t rawCurr;

        HAO_PREFETCH_R(a->buf + i + HAO_PREFETCH_INPUT_DISTANCE);
        haoLoadRawCurr64(a, i, HAO_SVE_BATCH64_BYTES, &rawCurr);

        HAO_STATS_ADD(blockCalls, 1);
        HAO_STATS_ADD(blockLanes, HAO_SVE_BATCH64_BYTES);
        HAO_STATS_ADD(primaryProbeLanes, HAO_SVE_BATCH64_BYTES);
        int rt = haoRunRaw64(&hash, vdot, l2EntryCount, primaryBitmap,
                             primaryHashTable, l2CheckTable, l2MetaTable,
                             ruleMeta, a, control, i, rawPrev, rawCurr);
        if (likely(rt == HWLM_TERMINATED)) {
            return HWLM_TERMINATED;
        }
        rawPrev = rawCurr;
    }

    if (i < a->len) {
        svuint8_t rawCurr;
        const u32 blockLaneCount = (u32)(a->len - i);
        haoLoadRawCurr64(a, i, blockLaneCount, &rawCurr);

        HAO_STATS_ADD(blockCalls, 1);
        HAO_STATS_ADD(blockLanes, blockLaneCount);
        HAO_STATS_ADD(primaryProbeLanes, blockLaneCount);
        const svuint32_t vlaneIds01 = haoRawLaneIds(0U);
        const svuint32_t vlaneIds23 = haoRawLaneIds(2U);
        const svuint32_t vlaneIds45 = haoRawLaneIds(4U);
        const svuint32_t vlaneIds67 = haoRawLaneIds(6U);
        int rt = haoRunRawTailVec64(
            &hash, vdot, l2EntryCount, primaryBitmap, primaryHashTable,
            l2CheckTable, l2MetaTable, ruleMeta, a, control, i,
            blockLaneCount, rawPrev, rawCurr, vlaneIds01, vlaneIds23,
            vlaneIds45, vlaneIds67);
        if (likely(rt == HWLM_TERMINATED)) {
            return HWLM_TERMINATED;
        }
    }

    return HWLM_SUCCESS;
}

static int haoRunBatchBlobScalar(const struct HAORuntimeHeader *hdr,
                                 const struct FDR_Runtime_Args *a,
                                 hwlm_group_t *control) {
    const u8 *primaryBitmap;
    const u32 *primaryHashTable;
    const struct HAORuntimeL2Check *l2CheckTable;
    const struct HAORuntimeL2Meta *l2MetaTable;
    const struct HAORuntimeRuleMeta *ruleMeta;
    struct HAOHashRuntime hash;
    u32 l2EntryCount;
    size_t i = a->start_offset;

    haoRefreshStatsMode();
    HAO_STATS_ADD(scanCalls, 1);
    HAO_STATS_ADD(scanInputBytes, a->len - a->start_offset);

    primaryBitmap = (const u8 *)hdr + hdr->primaryBitmapOffset;
    primaryHashTable =
        (const u32 *)((const u8 *)hdr + hdr->primaryOffset);
    l2CheckTable = (const struct HAORuntimeL2Check *)((const u8 *)hdr +
                                                      hdr->l2CheckOffset);
    l2MetaTable = (const struct HAORuntimeL2Meta *)((const u8 *)hdr +
                                                    hdr->l2MetaOffset);
    ruleMeta = (const struct HAORuntimeRuleMeta *)((const u8 *)hdr +
                                                   hdr->ruleMetaOffset);
    haoBuildHashRuntime(hdr, &hash);
    l2EntryCount = hdr->l2EntryCount;

    while (i < a->len) {
        const u32 blockLaneCount =
            (u32)((a->len - i) > 0x7fffffffU ? 0x7fffffffU :
                   (a->len - i));
        int rt;

        HAO_STATS_ADD(blockCalls, 1);
        HAO_STATS_ADD(blockLanes, blockLaneCount);
        HAO_STATS_ADD(primaryProbeLanes, blockLaneCount);
        rt = haoRunRawTailScalar(&hash, l2EntryCount, primaryBitmap,
                                 primaryHashTable, l2CheckTable, l2MetaTable,
                                 ruleMeta, a, control, i, blockLaneCount);
        if (likely(rt == HWLM_TERMINATED)) {
            return HWLM_TERMINATED;
        }
        i += blockLaneCount;
    }

    return HWLM_SUCCESS;
}

static int haoRunBatchBlob(const struct HAORuntimeHeader *hdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t *control) {
    const u32 vl = svcntb();

    if (vl == HAO_SVE_BATCH32_BYTES) {
        return haoRunBatchBlob32(hdr, a, control);
    }
    if (vl == HAO_SVE_BATCH64_BYTES) {
        return haoRunBatchBlob64(hdr, a, control);
    }
    return haoRunBatchBlobScalar(hdr, a, control);
}

static hwlm_error_t haoExecBlobWithPath(const void *blob, u32 blobSize,
                                        const struct FDR_Runtime_Args *a,
                                        hwlm_group_t control) {
    const struct HAORuntimeHeader *hdr = NULL;

    if (!haoValidateLayout(blob, blobSize, &hdr)) {
        return HWLM_SUCCESS;
    }
    if (!a) {
        return HWLM_SUCCESS;
    }

    return haoRunBatchBlob(hdr, a, &control);
}

static
hwlm_error_t haoExec(const struct FDR *fdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t control) {
    if (!fdr || !fdrMatcherBlobOffset(fdr) || !fdrMatcherBlobSize(fdr)) {
        return HWLM_SUCCESS;
    }

    {
        const u8 *base = (const u8 *)fdr;
        const void *blob = base + fdrMatcherBlobOffset(fdr);
        return haoExecBlobWithPath(blob, fdrMatcherBlobSize(fdr), a,
                                   control);
    }
}

hwlm_error_t HaoEngineExec(const struct FDR *fdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t control) {
    return haoExec(fdr, a, control);
}
