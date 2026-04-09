#ifndef HAO_COMPILE_H
#define HAO_COMPILE_H

#include "ue2common.h"
#include "hwlm/hwlm_literal.h"
#include "util/bytecode_ptr.h"

#include <array>
#include <vector>

/* Top-level HAO tuning knobs used by compile-time expansion and admission. */
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

namespace ue2 {

static constexpr u32 HAO_COMPAT_ARTIFACT_FLAG_PARTIAL_COVERAGE = 1U << 0;
static constexpr u32 HAO_COMPAT_ARTIFACT_FLAG_PARTIAL_SECONDARY_CAPACITY =
    1U << 1;
static constexpr u32 HAO_COMPAT_ARTIFACT_FLAG_PARTIAL_ENTRY_OVERFLOW =
    1U << 2;
static constexpr u32 HAO_COMPAT_KEY_BITS = 22U;
static constexpr u32 HAO_COMPAT_L1_OFFSET_BITS = 18U;
static constexpr u32 HAO_COMPAT_L1_OFFSET_MASK =
    (1U << HAO_COMPAT_L1_OFFSET_BITS) - 1U;
static constexpr u32 HAO_COMPAT_L1_COUNT_SHIFT = HAO_COMPAT_L1_OFFSET_BITS;
static constexpr u32 HAO_COMPAT_RULE_SLOTS_PER_ENTRY = 4U;
static constexpr u32 HAO_COMPAT_BYTES_PER_RULE_SLOT = 8U;
static constexpr u32 HAO_COMPAT_RULE_VECTOR_BYTES =
    HAO_COMPAT_RULE_SLOTS_PER_ENTRY * HAO_COMPAT_BYTES_PER_RULE_SLOT;
static constexpr u32 HAO_COMPAT_TBL_CONTROL_BYTES =
    HAO_COMPAT_RULE_SLOTS_PER_ENTRY * HAO_COMPAT_BYTES_PER_RULE_SLOT;
static constexpr u32 HAO_COMPAT_RULE_SLOT_MASK_WORDS = 1U;
static constexpr u32 HAO_COMPAT_MAX_SELECTORS = 32U;
static constexpr u32 HAO_COMPAT_MAX_MASK_CLASSES = 32U;
static constexpr u32 HAO_COMPAT_MAX_HOT_MASK_CLASSES = 4U;
static constexpr u32 HAO_COMPAT_HOT_MASK_CLASS_COVERAGE_PCT = 80U;
static constexpr u32 HAO_COMPAT_EXTRACT_MODE_SCALAR = 0U;
static constexpr u32 HAO_COMPAT_EXTRACT_MODE_BEXT = 1U;
static constexpr u32 HAO_FAMILY_ENGINE_ID = 2U;
static constexpr u32 HAO_ENGINE_ID = HAO_FAMILY_ENGINE_ID;

static constexpr u32 HAO_COMPAT_MASK_CLASS_FLAG_HOT = 1U << 0;

static constexpr u16 HAO_COMPAT_RULE_FLAG_NOCASE = 1U << 0;
static constexpr u16 HAO_COMPAT_RULE_FLAG_NORUNS = 1U << 1;
static constexpr u16 HAO_COMPAT_RULE_FLAG_HAS_MASK = 1U << 2;

/* Legacy PBE names remain as compatibility aliases. */
static constexpr u32 PBE_ARTIFACT_FLAG_PARTIAL_COVERAGE =
    HAO_COMPAT_ARTIFACT_FLAG_PARTIAL_COVERAGE;
static constexpr u32 PBE_ARTIFACT_FLAG_PARTIAL_SECONDARY_CAPACITY =
    HAO_COMPAT_ARTIFACT_FLAG_PARTIAL_SECONDARY_CAPACITY;
static constexpr u32 PBE_ARTIFACT_FLAG_PARTIAL_ENTRY_OVERFLOW =
    HAO_COMPAT_ARTIFACT_FLAG_PARTIAL_ENTRY_OVERFLOW;
static constexpr u32 PBE_KEY_BITS = HAO_COMPAT_KEY_BITS;
static constexpr u32 PBE_L1_OFFSET_BITS = HAO_COMPAT_L1_OFFSET_BITS;
static constexpr u32 PBE_L1_OFFSET_MASK = HAO_COMPAT_L1_OFFSET_MASK;
static constexpr u32 PBE_L1_COUNT_SHIFT = HAO_COMPAT_L1_COUNT_SHIFT;
static constexpr u32 PBE_RULE_SLOTS_PER_ENTRY = HAO_COMPAT_RULE_SLOTS_PER_ENTRY;
static constexpr u32 PBE_BYTES_PER_RULE_SLOT = HAO_COMPAT_BYTES_PER_RULE_SLOT;
static constexpr u32 PBE_RULE_VECTOR_BYTES = HAO_COMPAT_RULE_VECTOR_BYTES;
static constexpr u32 PBE_TBL_CONTROL_BYTES = HAO_COMPAT_TBL_CONTROL_BYTES;
static constexpr u32 PBE_RULE_SLOT_MASK_WORDS = HAO_COMPAT_RULE_SLOT_MASK_WORDS;
static constexpr u32 PBE_MAX_SELECTORS = HAO_COMPAT_MAX_SELECTORS;
static constexpr u32 PBE_MAX_MASK_CLASSES = HAO_COMPAT_MAX_MASK_CLASSES;
static constexpr u32 PBE_MAX_HOT_MASK_CLASSES = HAO_COMPAT_MAX_HOT_MASK_CLASSES;
static constexpr u32 PBE_HOT_MASK_CLASS_COVERAGE_PCT =
    HAO_COMPAT_HOT_MASK_CLASS_COVERAGE_PCT;
static constexpr u32 PBE_EXTRACT_MODE_SCALAR = HAO_COMPAT_EXTRACT_MODE_SCALAR;
static constexpr u32 PBE_EXTRACT_MODE_BEXT = HAO_COMPAT_EXTRACT_MODE_BEXT;
static constexpr u32 PBE_MASK_CLASS_FLAG_HOT = HAO_COMPAT_MASK_CLASS_FLAG_HOT;
static constexpr u16 PBE_RULE_FLAG_NOCASE = HAO_COMPAT_RULE_FLAG_NOCASE;
static constexpr u16 PBE_RULE_FLAG_NORUNS = HAO_COMPAT_RULE_FLAG_NORUNS;
static constexpr u16 PBE_RULE_FLAG_HAS_MASK = HAO_COMPAT_RULE_FLAG_HAS_MASK;

static constexpr u32 HAO_RULE_PLAN_FLAG_KEY_EXPANDED = 1U << 0;
static constexpr u32 HAO_RULE_PLAN_FLAG_NEEDS_CONFIRM = 1U << 1;
static constexpr u32 HAO_RULE_PLAN_FLAG_NORMALIZED = 1U << 2;
static constexpr u32 HAO_RULE_PLAN_FLAG_HAS_SUPPLEMENTARY_MASK = 1U << 3;
static constexpr u32 HAO_RULE_PLAN_FLAG_OVER_AMBIG_LIMIT = 1U << 4;
static constexpr u32 HAO_RULE_PLAN_FLAG_OVER_EXPANSION_BUDGET = 1U << 5;
static constexpr u32 HAO_RULE_PLAN_FLAG_ANCHOR_FRAGMENT = 1U << 6;
static constexpr u32 HAO_RULE_PLAN_FLAG_DIRECT_REPORT_SAFE = 1U << 7;

struct Grey;
struct target_t;

struct PBEBitSelector {
    u8 byteOffset;
    u8 bitOffset;
};

struct PBEPrimaryHashTable {
    std::vector<u32> offsets;
};

struct PBEPrimaryHashBitmap {
    std::vector<u8> bits;
};

struct PBEMaskClassArtifacts {
    u32 classId = 0;
    u32 classMask = 0;
    u32 classKeyBits = 0;
    u32 ruleCount = 0;
    u32 flags = 0;
    u32 secondaryOffset = 0;
    u32 secondaryCount = 0;
    PBEPrimaryHashTable primaryHashTable;
    PBEPrimaryHashBitmap primaryHashBitmap;
};

struct PBESecondaryHashEntry {
    u8 ruleVector[HAO_COMPAT_RULE_VECTOR_BYTES];
    u8 tableControl[HAO_COMPAT_TBL_CONTROL_BYTES];
    u16 ruleIndex[HAO_COMPAT_RULE_SLOTS_PER_ENTRY];
    u32 keyValue[HAO_COMPAT_RULE_SLOTS_PER_ENTRY];
    u32 keyMask[HAO_COMPAT_RULE_SLOTS_PER_ENTRY];
    u32 headMask;
    u32 tailMask;
    u16 ruleCount;
    u16 reserved;
};

struct HAOSecondaryHashEntry {
    u8 ruleVector[HAO_COMPAT_RULE_VECTOR_BYTES];
    u8 tableControl[HAO_COMPAT_TBL_CONTROL_BYTES];
    u16 ruleIndex[HAO_COMPAT_RULE_SLOTS_PER_ENTRY];
    u32 headMask;
    u32 tailMask;
    u8 slotMask;
    u8 reserved[3];
};

struct PBERuleMeta {
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

/* Each HAO rule is classified into one explicit compile-time category. */
enum class HAORuleCategory : u8 {
    HAO_RULE_EXACT = 0,
    HAO_RULE_NOCASE = 1,
    HAO_RULE_SMALL_CLASS_EXPAND = 2,
    HAO_RULE_ANCHOR_CONFIRM = 3,
    HAO_RULE_UNSUPPORTED = 4
};

/* One concrete key variant produced after expanding a single rule. */
struct HAOExpandedKey {
    u32 keyValue = 0;
    u32 ambiguousSelectorMask = 0;
    u32 variantIndex = 0;
};

/* Selected-bit ambiguity plus the resulting controlled key expansion. */
struct HAOKeyExpansionInfo {
    u32 selectedAmbigBits = 0;
    u32 ambiguousSelectorMask = 0;
    u32 expandedKeyCount = 0;
    std::vector<HAOExpandedKey> expandedKeys;
};

/* Deterministic verifier fragment consumed by the L2 vector verifier. */
struct HAOVerifierFragment {
    std::array<u8, HAO_COMPAT_BYTES_PER_RULE_SLOT> bytes = {};
    u8 validByteMask = 0;
    u8 anchorOffset = 0;
    u8 anchorLength = 0;
    u8 flags = 0;
};

/* Compile-time rule plan used as the core HAO build input. */
struct HAOCompiledRulePlan {
    u32 ruleIndex = 0;
    HAORuleCategory category = HAORuleCategory::HAO_RULE_UNSUPPORTED;
    u32 flags = 0;
    bool needFullConfirm = false;
    HAOKeyExpansionInfo keyExpansion;
    HAOVerifierFragment verifier;
};

/* Summary counters used for feasibility decisions and tuning. */
struct HAOCompileSummary {
    u32 totalRules = 0;
    u32 fastPathRules = 0;
    u32 unsupportedRules = 0;
    u32 anchorConfirmRules = 0;
    u32 exactRules = 0;
    u32 nocaseRules = 0;
    u32 residualRules = 0;
    u32 residualUnsupportedRules = 0;
    u32 fastPathConfirmRules = 0;
    u32 directReportRules = 0;
    u32 keyExpandedRules = 0;
    u32 totalExpandedKeys = 0;
    u32 maxSelectedAmbigBits = 0;
};

/* Build-time statistics for the HAO v2 global single-table layout. */

struct HAOGlobalHashStats {
    u32 nonEmptyPrimary = 0;
    u32 totalRulesInBuckets = 0;
    u32 totalExpandedKeysInBuckets = 0;
    u32 totalSecondaryEntries = 0;
    u32 maxEntriesPerKey = 0;
};

/* Artifacts for the HAO v2 global single-table build. */
struct HAOGlobalHashArtifacts {
    bool valid = false;
    u32 flags = 0;
    u32 keyBits = 0;
    u32 fullKeyMask = 0;
    PBEPrimaryHashTable primaryHashTable;
    PBEPrimaryHashBitmap primaryHashBitmap;
    std::vector<HAOSecondaryHashEntry> secondaryHashTable;
    HAOGlobalHashStats stats;
};

/* Compile-time choice of which blob layout should be emitted. */
enum class HAOBlobLayoutMode : u8 {
    HAO_BLOB_LAYOUT_V1_COMPAT = 0,
    HAO_BLOB_LAYOUT_V2_GLOBAL = 1
};

struct PBECompileArtifacts {
    u32 keyBits = 0;
    u32 flags = 0;
    u32 extractMode = HAO_COMPAT_EXTRACT_MODE_SCALAR;
    u32 windowBytes = HAO_COMPAT_BYTES_PER_RULE_SLOT;
    u64a bextMask = 0;
    HAOBlobLayoutMode haoBlobLayoutMode =
        HAOBlobLayoutMode::HAO_BLOB_LAYOUT_V1_COMPAT;
    std::vector<PBEBitSelector> bitSelectors;
    /* HAO v2 additions: rule plans and summary counters. */
    std::vector<HAOCompiledRulePlan> haoRulePlans;
    HAOCompileSummary haoSummary;
    /* HAO v2 global single-table artifacts kept alongside compat data. */
    HAOGlobalHashArtifacts haoGlobalHash;
    std::vector<PBEMaskClassArtifacts> maskClasses;
    PBEPrimaryHashTable primaryHashTable;
    PBEPrimaryHashBitmap primaryHashBitmap;
    std::vector<PBESecondaryHashEntry> secondaryHashTable;
    std::vector<PBERuleMeta> ruleMeta;
    std::vector<u8> literalBlob;
};

struct HAOCompileArtifacts {
    u32 keyBits = 0;
    u32 flags = 0;
    u32 extractMode = HAO_COMPAT_EXTRACT_MODE_SCALAR;
    u32 windowBytes = HAO_COMPAT_BYTES_PER_RULE_SLOT;
    u64a bextMask = 0;
    std::vector<PBEBitSelector> bitSelectors;
    std::vector<HAOCompiledRulePlan> haoRulePlans;
    HAOCompileSummary haoSummary;
    HAOGlobalHashArtifacts haoGlobalHash;
    std::vector<PBERuleMeta> ruleMeta;
    std::vector<u8> literalBlob;
};

using HAOCompatBitSelector = PBEBitSelector;
using HAOCompatPrimaryHashTable = PBEPrimaryHashTable;
using HAOCompatPrimaryHashBitmap = PBEPrimaryHashBitmap;
using HAOCompatMaskClassArtifacts = PBEMaskClassArtifacts;
using HAOCompatSecondaryHashEntry = PBESecondaryHashEntry;
using HAOCompatRuleMeta = PBERuleMeta;
using HAOCompatCompileArtifacts = PBECompileArtifacts;


enum class PBEFeasibilityReason : u32 {
    OK = 0,
    GREY_DISABLED,
    ARCH_UNSUPPORTED,
    TOO_FEW_LITERALS,
    TOO_MANY_LITERALS,
    UNSUPPORTED_INCLUDED_LITERAL,
    NO_SELECTORS,
    PARTIAL_SECONDARY_CAPACITY,
    PARTIAL_ENTRY_OVERFLOW,
    PARTIAL_OTHER,
    ARTIFACT_BUILD_FAILED
};

enum class HAOFeasibilityReason : u32 {
    OK = 0,
    GREY_DISABLED,
    ARCH_UNSUPPORTED,
    TOO_FEW_LITERALS,
    TOO_MANY_LITERALS,
    UNSUPPORTED_INCLUDED_LITERAL,
    NO_SELECTORS,
    PARTIAL_SECONDARY_CAPACITY,
    PARTIAL_ENTRY_OVERFLOW,
    PARTIAL_OTHER,
    ARTIFACT_BUILD_FAILED
};

struct PBEFeasibilityResult {
    bool canBuild = false;
    u32 flags = 0;
    PBEFeasibilityReason reason = PBEFeasibilityReason::ARTIFACT_BUILD_FAILED;
};

struct HAOFeasibilityResult {
    bool canBuild = false;
    u32 flags = 0;
    HAOFeasibilityReason reason = HAOFeasibilityReason::ARTIFACT_BUILD_FAILED;
};

using HAOCompatFeasibilityReason = PBEFeasibilityReason;
using HAOCompatFeasibilityResult = PBEFeasibilityResult;


bool analyzePBEFeasibility(const target_t &target,
                           const std::vector<hwlmLiteral> &lits,
                           const Grey &grey, PBEFeasibilityResult *result,
                           PBECompileArtifacts *artifacts);
bool analyzeHAOCompatFeasibility(const target_t &target,
                                 const std::vector<hwlmLiteral> &lits,
                                 const Grey &grey,
                                 HAOCompatFeasibilityResult *result,
                                 HAOCompatCompileArtifacts *artifacts);

bool analyzeHAOFeasibility(const target_t &target,
                           const std::vector<hwlmLiteral> &lits,
                           const Grey &grey, HAOFeasibilityResult *result,
                           HAOCompileArtifacts *artifacts);

const char *pbeFeasibilityReasonName(PBEFeasibilityReason reason);
const char *haoCompatFeasibilityReasonName(HAOCompatFeasibilityReason reason);
const char *haoFeasibilityReasonName(HAOFeasibilityReason reason);

bool haoFamilyGreyEnabled(const Grey &grey);

bool haoHasSveBitPermPrereq(const target_t &target);
bool pbeHasSveBitPermPrereq(const target_t &target);

bool haoCanUseBextFastPath(const target_t &target);
bool pbeCanUseBextFastPath(const target_t &target);

bool canBuildPBE(const target_t &target, const std::vector<hwlmLiteral> &lits,
                 const Grey &grey);
bool canBuildHAOCompat(const target_t &target,
                       const std::vector<hwlmLiteral> &lits,
                       const Grey &grey);

bool canBuildHAO(const target_t &target, const std::vector<hwlmLiteral> &lits,
                 const Grey &grey);

bool buildPBEArtifacts(const std::vector<hwlmLiteral> &lits,
                       PBECompileArtifacts *artifacts,
                       bool enableDump = true);
bool buildHAOCompatArtifacts(const std::vector<hwlmLiteral> &lits,
                             HAOCompatCompileArtifacts *artifacts,
                             bool enableDump = true);

bool buildHAOArtifacts(const std::vector<hwlmLiteral> &lits,
                       HAOCompileArtifacts *artifacts,
                       bool enableDump = true);

bytecode_ptr<u8> buildPBEBlob(const PBECompileArtifacts &artifacts);
bytecode_ptr<u8> buildHAOCompatBlob(const HAOCompatCompileArtifacts &artifacts);
bytecode_ptr<u8> buildHAOBlob(const HAOCompileArtifacts &artifacts);
bytecode_ptr<u8> buildHAOGlobalBlob(const HAOCompatCompileArtifacts &artifacts);

} // namespace ue2

#endif // HAO_COMPILE_H
