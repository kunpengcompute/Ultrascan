#ifndef HAO_RUNTIME_H
#define HAO_RUNTIME_H

#include "ue2common.h"
#include "hwlm/hwlm.h"

#define HAO_RUNTIME_MAGIC 0x48414f30U /* "HAO0" */
#define HAO_RUNTIME_VERSION 18U
#define HAO_RUNTIME_BLOCK_BYTES 32U
#define HAO_RUNTIME_FLAG_PARTIAL_COVERAGE (1U << 0)
#define HAO_RUNTIME_KEY_BITS 22U
#define HAO_RUNTIME_EXTRACT_MODE_SCALAR 0U
#define HAO_RUNTIME_EXTRACT_MODE_BEXT 1U
#define HAO_RUNTIME_L1_OFFSET_BITS 22U
#define HAO_RUNTIME_L1_OFFSET_MASK ((1U << HAO_RUNTIME_L1_OFFSET_BITS) - 1U)
#define HAO_RUNTIME_L1_COUNT_SHIFT HAO_RUNTIME_L1_OFFSET_BITS
#define HAO_RUNTIME_RULE_SLOTS_PER_ENTRY 4U
#define HAO_RUNTIME_BYTES_PER_RULE_SLOT 8U
#define HAO_RUNTIME_INVALID_RULE_INDEX 0xffffffffU
#define HAO_RUNTIME_MAX_SELECTORS 32U
#define HAO_BATCH_FALLBACK_WIDTH 4U
#define HAO_BATCH_MAX_WIDTH 32U
#define HAO_BITMAP_GROUPED_BYTES 4U
#define HAO_RUNTIME_PRIMARY_COARSE_KEY_SHIFT 3U
#define HAO_RUNTIME_PRIMARY_COARSE_KEY_GROUP \
    (1U << HAO_RUNTIME_PRIMARY_COARSE_KEY_SHIFT)

/* Shared HAO tuning knobs used by compile-time and runtime code paths. */
/* These remain part of the public HAO contract for the HAO-only path. */
#ifndef HAO_KEY_BITS
#define HAO_KEY_BITS 22U
#endif
#ifndef HAO_MAX_KEY_AMBIG_BITS
#define HAO_MAX_KEY_AMBIG_BITS HAO_KEY_BITS
#endif
#ifndef HAO_MAX_KEY_EXPANSION
#define HAO_MAX_KEY_EXPANSION (1U << HAO_MAX_KEY_AMBIG_BITS)
#endif
#ifndef HAO_MAX_SMALL_CLASS_EXPANSION
#define HAO_MAX_SMALL_CLASS_EXPANSION 16U
#endif
#ifndef HAO_MAX_LITERALS
#define HAO_MAX_LITERALS (1U << 18)
#endif
#define HAO_RULE_FLAG_NOCASE (1U << 0)
#define HAO_RULE_FLAG_NORUNS (1U << 1)
#define HAO_RULE_FLAG_HAS_MASK (1U << 2)

#define HAO_RUNTIME_RULE_EXACT 0U
#define HAO_RUNTIME_RULE_NOCASE 1U
#define HAO_RUNTIME_RULE_SMALL_CLASS_EXPAND 2U
#define HAO_RUNTIME_RULE_MASK_CONFIRM 3U
#define HAO_RUNTIME_RULE_UNSUPPORTED 4U
#define HAO_RUNTIME_PLAN_FLAG_DIRECT_REPORT_SAFE (1U << 7)
struct HAORuntimeL2Check {
    u64a rule[HAO_RUNTIME_RULE_SLOTS_PER_ENTRY];
    u64a mask[HAO_RUNTIME_RULE_SLOTS_PER_ENTRY];
};

struct HAORuntimeL2Meta {
    u32 ruleIndex[HAO_RUNTIME_RULE_SLOTS_PER_ENTRY];
    u32 careBits;
};

struct HAORuntimeHeader {
    u32 magic;
    u32 version;
    u32 flags;
    u32 keyBits;
    u32 selectorCount;
    u32 primaryCount;
    u32 primaryBitmapSize;
    u32 primaryCoarseBitmapSize;
    u32 l2EntryCount;
    u32 ruleMetaCount;
    u32 reservedLiteralBlobSize;
    u32 extractMode;
    u32 windowBytes;
    u64a bextMask;
    u64a bextMaskRaw;
    u32 selectorsOffset;
    u32 primaryBitmapOffset;
    u32 primaryBitmapCoarseOffset;
    u32 primaryOffset;
    u32 primaryBitmapRawOffset;
    u32 primaryBitmapRawCoarseOffset;
    u32 primaryRawOffset;
    u32 l2CheckOffset;
    u32 l2MetaOffset;
    u32 ruleMetaOffset;
    u32 reservedLiteralBlobOffset;
    u32 reserved1;
    u32 reserved2;
};

struct HAORuntimeBitSelector {
    u8 byteOffset;
    u8 bitOffset;
    u16 reserved;
};

struct HAORuntimeRuleMeta {
    u32 id;
    hwlm_group_t groups;
    u16 len;
    u16 flags;
    u8 category;
    u8 verifierValidByteMask;
    u8 reserved1;
    u8 reserved2;
    u8 verifierFlags;
    u8 maskLen;
    u16 reserved0;
    u32 planFlags;
    u32 reserved3;
    u64a maskWord;
    u64a cmpWord;
    u8 msk[8];
    u8 cmp[8];
};

/* Read-only summary returned by HAO runtime blob inspection helpers. */
struct HAORuntimeInspectSummary {
    u32 selectorCount;
    u32 primaryCount;
    u32 primaryBitmapSize;
    u32 primaryCoarseBitmapSize;
    u32 l2EntryCount;
    u32 ruleMetaCount;
    u32 nonEmptyPrimary;
    u32 multiEntryBucketCount;
    u32 maxEntriesPerKey;
    u32 totalRulesInL2;
};

struct HAORuntimeStats {
    u64a scanCalls;
    u64a scanInputBytes;
    u64a blockCalls;
    u64a blockLanes;
    u64a primaryProbeLanes;
    u64a primaryActiveLanes;
    u64a encodedRangeCalls;
    u64a encodedRangeReportCalls;
    u64a encodedEntriesVisited;
    u64a verifierCalls;
    u64a verifierEntryHits;
    u64a verifierSlotHits;
    u64a encodedGroupRejects;
    u64a directReports;
    u64a encodedConfirmCalls;
    u64a encodedConfirmMatches;
    u64a encodedConfirmRejects;
    u64a callbackReports;
    u64a l2RangeEntryBucketsEq1;
    u64a l2RangeEntryBuckets2To4;
    u64a l2RangeEntryBucketsGt4;
    u64a l2RangeRuleBucketsEq1;
    u64a l2RangeRuleBuckets2To4;
    u64a l2RangeRuleBucketsGt4;
    u64a l2RangeCollisionBuckets;
    u64a l2RangeTotalEntries;
    u64a l2RangeTotalRules;
    u64a l2RangeMinEntries;
    u64a l2RangeMaxEntries;
    u64a l2RangeMinRules;
    u64a l2RangeMaxRules;
};

struct FDR;
struct FDR_Runtime_Args;

#ifdef __cplusplus
extern "C" {
#endif

u64a haoExtractPackedBitsSveBitPerm(u64a window, u64a mask);
void haoExtractPackedBitsSveBitPermBatch(const u64a *windows, u32 count,
                                         u64a mask, u64a *packedOut);
void haoExtractPackedBitsSveBitPermBatchToKeys(const u64a *windows, u32 count,
                                               u64a mask, u32 keyMask,
                                               u32 *keysOut);
u32 haoExtractPackedBitsSveBitPermLaneCount(void);
hwlm_error_t HaoEngineExec(const struct FDR *fdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t control);
hwlm_error_t HaoEngineExecNaiveForTest(const struct FDR *fdr,
                                       const struct FDR_Runtime_Args *a,
                                       hwlm_group_t control);
u32 HaoRuntimeBitmapProbeMaskForTest(const u8 *bitmap, u32 bitmapSize,
                                     const u32 *primaryIdx, u32 laneCount,
                                     int usePacked);
u64a HaoRuntimeRawLaneWordForTest(const u8 *prev32, const u8 *curr32,
                                  u32 lane);
int HaoRuntimeValidateLayoutForTest(const void *blob, u32 blobSize);
int HaoRuntimeInspectBlobForTest(const void *blob, u32 blobSize,
                                 struct HAORuntimeInspectSummary *summary);
void HaoRuntimeResetStatsForTest(void);
void HaoRuntimeGetStatsForTest(struct HAORuntimeStats *summary);
int HaoRuntimeStatsEnabledForTest(void);
hwlm_error_t HaoEngineExecBlobNaiveForTest(const void *blob, u32 blobSize,
                                           const struct FDR_Runtime_Args *a,
                                           hwlm_group_t control);
hwlm_error_t HaoEngineExecBlobBatchForTest(const void *blob, u32 blobSize,
                                           const struct FDR_Runtime_Args *a,
                                           hwlm_group_t control);

#ifdef __cplusplus
}
#endif

#endif // HAO_RUNTIME_H
