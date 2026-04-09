#ifndef HAO_RUNTIME_H
#define HAO_RUNTIME_H

#include "ue2common.h"
#include "hwlm/hwlm.h"

#define PBE_RUNTIME_MAGIC 0x50424530U /* "PBE0" */
#define PBE_RUNTIME_VERSION 8U
#define PBE_RUNTIME_FLAG_PARTIAL_COVERAGE (1U << 0)
#define PBE_RUNTIME_KEY_BITS 22U
#define PBE_RUNTIME_L1_OFFSET_BITS 18U
#define PBE_RUNTIME_L1_OFFSET_MASK ((1U << PBE_RUNTIME_L1_OFFSET_BITS) - 1U)
#define PBE_RUNTIME_L1_COUNT_SHIFT PBE_RUNTIME_L1_OFFSET_BITS
#define PBE_RUNTIME_RULE_SLOTS_PER_ENTRY 4U
#define PBE_RUNTIME_BYTES_PER_RULE_SLOT 8U
#define PBE_RUNTIME_RULE_VECTOR_BYTES \
    (PBE_RUNTIME_RULE_SLOTS_PER_ENTRY * PBE_RUNTIME_BYTES_PER_RULE_SLOT)
#define PBE_RUNTIME_TBL_CONTROL_BYTES \
    (PBE_RUNTIME_RULE_SLOTS_PER_ENTRY * PBE_RUNTIME_BYTES_PER_RULE_SLOT)
#define PBE_RUNTIME_RULE_SLOT_MASK_WORDS 1U
#define PBE_RUNTIME_MAX_SELECTORS 32U
#define PBE_RUNTIME_MAX_MASK_CLASSES 32U
#define PBE_RUNTIME_EXTRACT_MODE_SCALAR 0U
#define PBE_RUNTIME_EXTRACT_MODE_BEXT 1U

#define HAO_COMPAT_RUNTIME_MAGIC PBE_RUNTIME_MAGIC
#define HAO_COMPAT_RUNTIME_VERSION PBE_RUNTIME_VERSION
#define HAO_COMPAT_RUNTIME_FLAG_PARTIAL_COVERAGE \
    PBE_RUNTIME_FLAG_PARTIAL_COVERAGE
#define HAO_COMPAT_RUNTIME_KEY_BITS PBE_RUNTIME_KEY_BITS
#define HAO_COMPAT_RUNTIME_L1_OFFSET_BITS PBE_RUNTIME_L1_OFFSET_BITS
#define HAO_COMPAT_RUNTIME_L1_OFFSET_MASK PBE_RUNTIME_L1_OFFSET_MASK
#define HAO_COMPAT_RUNTIME_L1_COUNT_SHIFT PBE_RUNTIME_L1_COUNT_SHIFT
#define HAO_COMPAT_RUNTIME_RULE_SLOTS_PER_ENTRY \
    PBE_RUNTIME_RULE_SLOTS_PER_ENTRY
#define HAO_COMPAT_RUNTIME_BYTES_PER_RULE_SLOT \
    PBE_RUNTIME_BYTES_PER_RULE_SLOT
#define HAO_COMPAT_RUNTIME_RULE_VECTOR_BYTES PBE_RUNTIME_RULE_VECTOR_BYTES
#define HAO_COMPAT_RUNTIME_TBL_CONTROL_BYTES PBE_RUNTIME_TBL_CONTROL_BYTES
#define HAO_COMPAT_RUNTIME_RULE_SLOT_MASK_WORDS \
    PBE_RUNTIME_RULE_SLOT_MASK_WORDS
#define HAO_COMPAT_RUNTIME_MAX_SELECTORS PBE_RUNTIME_MAX_SELECTORS
#define HAO_COMPAT_RUNTIME_MAX_MASK_CLASSES PBE_RUNTIME_MAX_MASK_CLASSES
#define HAO_COMPAT_RUNTIME_EXTRACT_MODE_SCALAR PBE_RUNTIME_EXTRACT_MODE_SCALAR
#define HAO_COMPAT_RUNTIME_EXTRACT_MODE_BEXT PBE_RUNTIME_EXTRACT_MODE_BEXT

#define HAO_RUNTIME_MAGIC 0x48414f30U /* "HAO0" */
#define HAO_RUNTIME_VERSION 5U
#define HAO_RUNTIME_BLOCK_BYTES 32U
#define HAO_RUNTIME_EXTRACT_MODE_SCALAR 0U
#define HAO_RUNTIME_EXTRACT_MODE_BEXT 1U
#define HAO_RUNTIME_L1_OFFSET_BITS HAO_COMPAT_RUNTIME_L1_OFFSET_BITS
#define HAO_RUNTIME_L1_OFFSET_MASK HAO_COMPAT_RUNTIME_L1_OFFSET_MASK
#define HAO_RUNTIME_L1_COUNT_SHIFT HAO_RUNTIME_L1_OFFSET_BITS
#define HAO_RUNTIME_RULE_SLOTS_PER_ENTRY \
    HAO_COMPAT_RUNTIME_RULE_SLOTS_PER_ENTRY
#define HAO_RUNTIME_BYTES_PER_RULE_SLOT \
    HAO_COMPAT_RUNTIME_BYTES_PER_RULE_SLOT
#define HAO_RUNTIME_RULE_VECTOR_BYTES HAO_COMPAT_RUNTIME_RULE_VECTOR_BYTES
#define HAO_RUNTIME_TBL_CONTROL_BYTES HAO_COMPAT_RUNTIME_TBL_CONTROL_BYTES
#define HAO_RUNTIME_MAX_SELECTORS HAO_COMPAT_RUNTIME_MAX_SELECTORS

#define PBE_RUNTIME_MASK_CLASS_FLAG_HOT (1U << 0)
#define HAO_COMPAT_RUNTIME_MASK_CLASS_FLAG_HOT PBE_RUNTIME_MASK_CLASS_FLAG_HOT

/* Shared HAO tuning knobs used by compile-time and runtime code paths. */
/* These remain part of the public HAO contract even while compat layout stays alive. */
#ifndef HAO_KEY_BITS
#define HAO_KEY_BITS 22U
#endif
#ifndef HAO_MAX_KEY_AMBIG_BITS
#define HAO_MAX_KEY_AMBIG_BITS 10U
#endif
#ifndef HAO_MAX_KEY_EXPANSION
#define HAO_MAX_KEY_EXPANSION (1U << HAO_MAX_KEY_AMBIG_BITS)
#endif
#ifndef HAO_MAX_SMALL_CLASS_EXPANSION
#define HAO_MAX_SMALL_CLASS_EXPANSION 16U
#endif
#ifndef HAO_MAX_TOTAL_EXPANDED_KEYS
#define HAO_MAX_TOTAL_EXPANDED_KEYS (1U << 20)
#endif
#ifndef HAO_MIN_FAST_RULE_COVERAGE_PCT
#define HAO_MIN_FAST_RULE_COVERAGE_PCT 80U
#endif

#define PBE_RULE_FLAG_NOCASE (1U << 0)
#define PBE_RULE_FLAG_NORUNS (1U << 1)
#define PBE_RULE_FLAG_HAS_MASK (1U << 2)
#define HAO_COMPAT_RULE_FLAG_NOCASE PBE_RULE_FLAG_NOCASE
#define HAO_COMPAT_RULE_FLAG_NORUNS PBE_RULE_FLAG_NORUNS
#define HAO_COMPAT_RULE_FLAG_HAS_MASK PBE_RULE_FLAG_HAS_MASK

#define HAO_RUNTIME_RULE_EXACT 0U
#define HAO_RUNTIME_RULE_NOCASE 1U
#define HAO_RUNTIME_RULE_SMALL_CLASS_EXPAND 2U
#define HAO_RUNTIME_RULE_ANCHOR_CONFIRM 3U
#define HAO_RUNTIME_RULE_UNSUPPORTED 4U
#define HAO_RUNTIME_PLAN_FLAG_DIRECT_REPORT_SAFE (1U << 7)

/* Legacy HAO compat runtime layout that still carries the v1/class-table format. */
struct PBERuntimeHeader {
    u32 magic;
    u32 version;
    u32 flags;
    u32 keyBits;
    u32 selectorCount;
    u32 classCount;
    u32 primaryCount;
    u32 primaryBitmapSize;
    u32 secondaryCount;
    u32 ruleMetaCount;
    u32 literalBlobSize;
    u32 extractMode;
    u32 windowBytes;
    u64a bextMask;
    u32 selectorsOffset;
    u32 classTableOffset;
    u32 primaryBitmapOffset;
    u32 primaryOffset;
    u32 secondaryOffset;
    u32 ruleMetaOffset;
    u32 literalBlobOffset;
};

struct PBERuntimeBitSelector {
    u8 byteOffset;
    u8 bitOffset;
    u16 reserved;
};

struct PBERuntimeMaskClass {
    u32 classMask;
    u32 classKeyBits;
    u32 flags;
    u32 primaryCount;
    u32 primaryBitmapSize;
    u32 primaryBitmapOffset;
    u32 primaryOffset;
    u32 secondaryOffset;
    u32 secondaryCount;
};

struct PBERuntimeSecondaryHashEntry {
    u8 ruleVector[PBE_RUNTIME_RULE_VECTOR_BYTES];
    u8 tableControl[PBE_RUNTIME_TBL_CONTROL_BYTES];
    u16 ruleIndex[PBE_RUNTIME_RULE_SLOTS_PER_ENTRY];
    u32 keyValue[PBE_RUNTIME_RULE_SLOTS_PER_ENTRY];
    u32 keyMask[PBE_RUNTIME_RULE_SLOTS_PER_ENTRY];
    u32 headMask;
    u32 tailMask;
    u16 ruleCount;
    u16 reserved;
};

struct HAORuntimeSecondaryHashEntry {
    u8 ruleVector[HAO_RUNTIME_RULE_VECTOR_BYTES];
    u8 tableControl[HAO_RUNTIME_TBL_CONTROL_BYTES];
    u16 ruleIndex[HAO_RUNTIME_RULE_SLOTS_PER_ENTRY];
    u32 headMask;
    u32 tailMask;
    u8 slotMask;
    u8 reserved[3];
};

typedef struct PBERuntimeHeader HAOCompatRuntimeHeader;
typedef struct PBERuntimeBitSelector HAOCompatRuntimeBitSelector;
typedef struct PBERuntimeMaskClass HAOCompatRuntimeMaskClass;
typedef struct PBERuntimeSecondaryHashEntry HAOCompatRuntimeSecondaryHashEntry;

struct PBERuntimeRuleMeta {
    u32 id;
    hwlm_group_t groups;
    u16 len;
    u16 flags;
    u8 maskLen;
    u32 litOffset;
    u8 lit[8];
    u8 msk[8];
    u8 cmp[8];
};

/* HAO v2 runtime layout: class table removed, global single-table retained. */
struct HAORuntimeHeader {
    u32 magic;
    u32 version;
    u32 flags;
    u32 keyBits;
    u32 selectorCount;
    u32 primaryCount;
    u32 primaryBitmapSize;
    u32 secondaryCount;
    u32 ruleMetaCount;
    u32 literalBlobSize;
    u32 extractMode;
    u32 windowBytes;
    u64a bextMask;
    u32 selectorsOffset;
    u32 primaryBitmapOffset;
    u32 primaryOffset;
    u32 secondaryOffset;
    u32 ruleMetaOffset;
    u32 literalBlobOffset;
    u32 residualRuleCount;
    u32 residualRuleIndexOffset;
};

struct HAORuntimeBitSelector {
    u8 byteOffset;
    u8 bitOffset;
    u16 reserved;
};

/* Compat rule metadata keeps the original literal-confirm payload. */
typedef struct PBERuntimeRuleMeta HAOCompatRuntimeRuleMeta;

struct HAORuntimeRuleMeta {
    u32 id;
    hwlm_group_t groups;
    u16 len;
    u16 flags;
    u8 category;
    u8 verifierValidByteMask;
    u8 anchorOffset;
    u8 anchorLength;
    u8 verifierFlags;
    u8 maskLen;
    u16 reserved0;
    u32 planFlags;
    u32 litOffset;
    u8 lit[8];
    u8 msk[8];
    u8 cmp[8];
};

/* Read-only summary returned by HAO runtime blob inspection helpers. */
struct HAORuntimeInspectSummary {
    u32 selectorCount;
    u32 primaryCount;
    u32 primaryBitmapSize;
    u32 secondaryCount;
    u32 ruleMetaCount;
    u32 residualRuleCount;
    u32 nonEmptyPrimary;
    u32 multiEntryBucketCount;
    u32 maxEntriesPerKey;
    u32 totalRulesInL2;
};

struct HAORuntimeStats {
    u64a blockCalls;
    u64a blockLanes;
    u64a primaryProbeLanes;
    u64a primaryActiveLanes;
    u64a encodedRangeCalls;
    u64a encodedEntriesVisited;
    u64a verifierCalls;
    u64a verifierEntryHits;
    u64a verifierSlotHits;
    u64a directReports;
    u64a encodedConfirmCalls;
    u64a encodedConfirmMatches;
    u64a residualPosCalls;
    u64a residualRuleChecks;
    u64a residualConfirmCalls;
    u64a residualConfirmMatches;
};

struct FDR;
struct FDR_Runtime_Args;

#ifdef __cplusplus
extern "C" {
#endif

u64a haoExtractPackedBitsSveBitPerm(u64a window, u64a mask);
void haoExtractPackedBitsSveBitPermBatch(const u64a *windows, u32 count,
                                         u64a mask, u64a *packedOut);
u32 haoExtractPackedBitsSveBitPermLaneCount(void);
u64a pbeExtractPackedBitsSveBitPerm(u64a window, u64a mask);
void pbeExtractPackedBitsSveBitPermBatch(const u64a *windows, u32 count,
                                         u64a mask, u64a *packedOut);
u32 pbeExtractPackedBitsSveBitPermLaneCount(void);
hwlm_error_t HaoEngineExec(const struct FDR *fdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t control);
hwlm_error_t PbeEngineExec(const struct FDR *fdr,
                          const struct FDR_Runtime_Args *a,
                          hwlm_group_t control);
hwlm_error_t HaoCompatEngineExecNaiveForTest(const struct FDR *fdr,
                                       const struct FDR_Runtime_Args *a,
                                       hwlm_group_t control);
u32 HaoCompatRuntimeEntryMatchMaskForTest(
    const HAOCompatRuntimeSecondaryHashEntry *entry,
    const struct FDR_Runtime_Args *a, size_t endPos, int useVector);
u32 HaoCompatRuntimeBitmapProbeMaskForTest(const u8 *bitmap, u32 bitmapSize,
                                     const u32 *primaryIdx, u32 laneCount,
                                     int usePacked);
int HaoRuntimeValidateLayoutForTest(const void *blob, u32 blobSize);
int HaoRuntimeInspectBlobForTest(const void *blob, u32 blobSize,
                                 struct HAORuntimeInspectSummary *summary);
void HaoRuntimeResetStatsForTest(void);
void HaoRuntimeGetStatsForTest(struct HAORuntimeStats *summary);
hwlm_error_t PbeEngineExecNaiveForTest(const struct FDR *fdr,
                                       const struct FDR_Runtime_Args *a,
                                       hwlm_group_t control);
u32 PbeRuntimeEntryMatchMaskForTest(
    const HAOCompatRuntimeSecondaryHashEntry *entry,
    const struct FDR_Runtime_Args *a, size_t endPos, int useVector);
u32 PbeRuntimeBitmapProbeMaskForTest(const u8 *bitmap, u32 bitmapSize,
                                     const u32 *primaryIdx, u32 laneCount,
                                     int usePacked);
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
