#ifndef HAO_RUNTIME_H
#define HAO_RUNTIME_H

#include "ue2common.h"
#include "hwlm/hwlm.h"

#ifndef HAO_KEY_BITS
#define HAO_KEY_BITS 22U
#endif

#define HAO_RUNTIME_MAGIC 0x48414f30U /* "HAO0" */
#define HAO_RUNTIME_VERSION 28U
#define HAO_RUNTIME_BLOCK_BYTES 32U
#define HAO_RUNTIME_L1_OFFSET_BITS 22U
#define HAO_RUNTIME_L1_OFFSET_MASK ((1U << HAO_RUNTIME_L1_OFFSET_BITS) - 1U)
#define HAO_RUNTIME_L1_COUNT_SHIFT HAO_RUNTIME_L1_OFFSET_BITS
#define HAO_RUNTIME_RULE_SLOTS_PER_ENTRY 4U
#define HAO_RUNTIME_BYTES_PER_RULE_SLOT 8U
#define HAO_RUNTIME_L2_CHECK_ALIGN 64U
#define HAO_RUNTIME_INVALID_RULE_INDEX 0xffffffffU
#define HAO_RUNTIME_MAX_SELECTORS 32U
#define HAO_RUNTIME_HASH_BEXT 0U
#define HAO_RUNTIME_HASH_DOT 1U
#define HAO_RUNTIME_HASH_DOT_GROUP 2U
#define HAO_RUNTIME_DOT_VECTOR_LANES 4U
#define HAO_RUNTIME_DOT_GROUP_COUNT HAO_RUNTIME_DOT_VECTOR_LANES
#define HAO_RUNTIME_KEY_BITS_MASK 0xffU
#define HAO_RUNTIME_HASH_MODE_SHIFT 24U
#define HAO_BATCH_FALLBACK_WIDTH 4U
#define HAO_BATCH_MAX_WIDTH 32U
#define HAO_BITMAP_GROUPED_BYTES 4U

/* Experimental primary bitmap compression. A single bitmap bit represents
 * 2^SHIFT adjacent full primary keys; the full primary table is still probed
 * with the original key to reject compressed-bitmap aliases. */
#ifndef HAO_COMPRESSED_BITMAP
#define HAO_COMPRESSED_BITMAP 0
#endif

#ifndef HAO_COMPRESSED_BITMAP_SHIFT
#define HAO_COMPRESSED_BITMAP_SHIFT 4U
#endif

#if HAO_COMPRESSED_BITMAP && HAO_COMPRESSED_BITMAP_SHIFT >= 31U
#error "HAO_COMPRESSED_BITMAP_SHIFT must fit in a u32 key"
#endif

/* Experimental L1.5 tag filter. This is checked after an L1 hit and before
 * L2 verification, so the secondary BEXT is only paid for candidate lanes. */
#ifndef HAO_L15_TAG
#define HAO_L15_TAG 0
#endif

#ifndef HAO_L15_TAG_BITS
#define HAO_L15_TAG_BITS 8U
#endif

#ifndef HAO_L15_TAG_MAX_OVERLAP_BITS
#define HAO_L15_TAG_MAX_OVERLAP_BITS 2U
#endif

#ifndef HAO_L15_TAG_MIN_NEW_BITS
#define HAO_L15_TAG_MIN_NEW_BITS 6U
#endif

#define HAO_L15_TAG_VALID 0x8000U
#define HAO_L15_TAG_MASK_ID_SHIFT 8U
#define HAO_L15_TAG_MASK_ID_MASK 0x1f00U
#define HAO_L15_TAG_VALUE_MASK 0x00ffU
#define HAO_L15_TAG_MAX_MASKS 32U

/* Shared HAO tuning knobs used by compile-time and runtime code paths. */
/* These remain part of the public HAO contract for the HAO-only path. */
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
    u32 keyBits;
    u32 primaryCount;
    u32 primaryBitmapSize;
    u32 l2EntryCount;
    u32 ruleMetaCount;
    u64a bextMask;
    u32 primaryBitmapOffset;
    u32 primaryOffset;
    u32 l2CheckOffset;
    u32 l2MetaOffset;
    u32 ruleMetaOffset;
    u32 l15TagOffset;
    u32 l15TagCount;
    u32 l15TagBits;
    u32 l15TagOverlapBits;
    u32 l15MaskTableOffset;
    u32 l15MaskCount;
};

struct HAORuntimeDotGroupDesc {
    u32 keyBits;
    u32 primaryCount;
    u32 primaryBitmapSize;
    u32 l2EntryCount;
    u32 knownBytes;
    u32 reserved;
    u64a dotVector;
    u32 primaryBitmapOffset;
    u32 primaryOffset;
    u32 l2CheckOffset;
    u32 l2MetaOffset;
};

struct HAORuntimeRuleMeta {
    u32 id;
    u16 flags;
    u16 reserved;
    hwlm_group_t groups;
};

/* Read-only summary returned by HAO runtime blob inspection helpers. */
struct HAORuntimeInspectSummary {
    u32 keyBits;
    u32 primaryCount;
    u32 primaryBitmapSize;
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
    u64a primaryBitmapHitLanes;
    u64a primaryAliasRejects;
    u64a primaryActiveLanes;
    u64a encodedRangeCalls;
    u64a encodedRangeReportCalls;
    u64a encodedEntriesVisited;
    u64a verifierCalls;
    u64a verifierEntryHits;
    u64a verifierSlotHits;
    u64a encodedGroupRejects;
    u64a l15TagChecks;
    u64a l15TagRejects;
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

hwlm_error_t HaoEngineExec(const struct FDR *fdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t control);

#ifdef __cplusplus
}
#endif

#endif // HAO_RUNTIME_H
