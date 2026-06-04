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

#ifndef HAO_AUTO_MIN_KEY_BITS
#define HAO_AUTO_MIN_KEY_BITS 18U
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
#define HAO_MAX_LITERALS (1U << HAO_KEY_BITS)
#endif

namespace ue2 {

static constexpr u32 HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE = 1U << 0;
static constexpr u32 HAO_ARTIFACT_FLAG_PARTIAL_L2_CAPACITY =
    1U << 1;
static constexpr u32 HAO_ARTIFACT_FLAG_PARTIAL_ENTRY_OVERFLOW =
    1U << 2;
static constexpr u32 HAO_LAYOUT_KEY_BITS = HAO_KEY_BITS;
static constexpr u32 HAO_LAYOUT_L1_OFFSET_BITS = 22U;
static constexpr u32 HAO_LAYOUT_L1_OFFSET_MASK =
    (1U << HAO_LAYOUT_L1_OFFSET_BITS) - 1U;
static constexpr u32 HAO_LAYOUT_L1_COUNT_SHIFT = HAO_LAYOUT_L1_OFFSET_BITS;
static constexpr u32 HAO_LAYOUT_RULE_SLOTS_PER_ENTRY = 4U;
static constexpr u32 HAO_LAYOUT_BYTES_PER_RULE_SLOT = 8U;
static constexpr u32 HAO_INVALID_RULE_INDEX = ~0U;
static constexpr u32 HAO_LAYOUT_MAX_SELECTORS = 32U;
static constexpr u32 HAO_ENGINE_ID = 2U;

static_assert(HAO_LAYOUT_KEY_BITS <= HAO_LAYOUT_L1_OFFSET_BITS,
              "HAO key bits must fit the primary key space");
static_assert(HAO_LAYOUT_KEY_BITS <= HAO_LAYOUT_MAX_SELECTORS,
              "HAO key bits exceed selector capacity");

static constexpr u16 HAO_RULE_META_FLAG_NOCASE = 1U << 0;
static constexpr u16 HAO_RULE_META_FLAG_NORUNS = 1U << 1;

static constexpr u32 HAO_RULE_PLAN_FLAG_KEY_EXPANDED = 1U << 0;
static constexpr u32 HAO_RULE_PLAN_FLAG_NORMALIZED = 1U << 2;
static constexpr u32 HAO_RULE_PLAN_FLAG_HAS_SUPPLEMENTARY_MASK = 1U << 3;
static constexpr u32 HAO_RULE_PLAN_FLAG_OVER_AMBIG_LIMIT = 1U << 4;
static constexpr u32 HAO_RULE_PLAN_FLAG_MASK_MERGED = 1U << 5;
static constexpr u32 HAO_RULE_PLAN_FLAG_MASK_MERGE_FAILED = 1U << 6;

struct Grey;
struct target_t;

struct HAOBitSelector {
    u8 byteOffset;
    u8 bitOffset;
};

struct HAOPrimaryHashTable {
    std::vector<u32> offsets;
};

struct HAOPrimaryHashBitmap {
    std::vector<u8> bits;
};

struct HAOL2Check {
    u64a rule[HAO_LAYOUT_RULE_SLOTS_PER_ENTRY];
    u64a mask[HAO_LAYOUT_RULE_SLOTS_PER_ENTRY];
};

struct HAOL2Meta {
    u32 ruleIndex[HAO_LAYOUT_RULE_SLOTS_PER_ENTRY];
    u32 careBits;
};

struct HAOCompileRuleMeta {
    u32 id;
    u16 flags;
    u16 reserved;
    hwlm_group_t groups;
};

/* Each HAO rule is classified into one explicit compile-time category. */
enum class HAORuleCategory : u8 {
    HAO_RULE_EXACT = 0,
    HAO_RULE_NOCASE = 1,
    HAO_RULE_SMALL_CLASS_EXPAND = 2,
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
    std::array<u8, HAO_LAYOUT_BYTES_PER_RULE_SLOT> bytes = {};
    u8 validByteMask = 0;
    u8 nocaseByteMask = 0;
    u8 careByteMask = 0;
    u8 reserved0 = 0;
    u8 flags = 0;
    u64a ruleWord = 0;
    u64a maskWord = 0;
};

/* Compile-time rule plan used as the core HAO build input. */
struct HAOCompiledRulePlan {
    u32 ruleIndex = 0;
    HAORuleCategory category = HAORuleCategory::HAO_RULE_UNSUPPORTED;
    u32 flags = 0;
    HAOKeyExpansionInfo keyExpansion;
    HAOVerifierFragment verifier;
};

/* Summary counters used for feasibility decisions and tuning. */
struct HAOCompileSummary {
    u32 totalRules = 0;
    u32 fastPathRules = 0;
    u32 unsupportedRules = 0;
    u32 maskRules = 0;
    u32 maskMergedRules = 0;
    u32 maskConflictRules = 0;
    u32 maskConfirmRules = 0;
    u32 exactRules = 0;
    u32 nocaseRules = 0;
    u32 keyExpandedRules = 0;
    u32 totalExpandedKeys = 0;
    u32 maxSelectedAmbigBits = 0;
    u64a totalLiteralBytes = 0;
    u32 minLiteralLen = 0;
    u32 maxLiteralLen = 0;
    u32 literalLenLe4 = 0;
    u32 literalLen5To8 = 0;
};

/* Build-time statistics for the HAO global single-table layout. */

struct HAOHashStats {
    u32 nonEmptyPrimary = 0;
    u32 collisionBuckets = 0;
    u32 totalRulesInBuckets = 0;
    u32 totalExpandedKeysInBuckets = 0;
    u32 totalL2Entries = 0;
    u32 minRulesPerBucket = 0;
    u32 maxRulesPerBucket = 0;
    u32 minEntriesPerBucket = 0;
    u32 maxEntriesPerKey = 0;
    u32 ruleBucketsEq1 = 0;
    u32 ruleBuckets2To4 = 0;
    u32 ruleBucketsGt4 = 0;
    u32 entryBucketsEq1 = 0;
    u32 entryBuckets2To4 = 0;
    u32 entryBucketsGt4 = 0;
};

/* Artifacts for the HAO global single-table build. */
struct HAOHashBuild {
    bool valid = false;
    u32 flags = 0;
    u32 keyBits = 0;
    HAOPrimaryHashTable primary;
    HAOPrimaryHashBitmap bitmap;
    std::vector<HAOL2Check> l2Check;
    std::vector<HAOL2Meta> l2Meta;
    HAOHashStats stats;
};

struct HAOCompileArtifacts {
    const char *selectorName = "unknown";
    u64a bextMask = 0;
    std::vector<HAOBitSelector> selectors;
    std::vector<HAOCompiledRulePlan> plans;
    HAOCompileSummary summary;
    HAOHashBuild hash;
    std::vector<HAOCompileRuleMeta> meta;
};

enum class HAODumpMode : u8 {
    None = 0,
    SummaryIfEnabled,
    Full
};

enum class HAOFeasibilityReason : u32 {
    OK = 0,
    GREY_DISABLED,
    ARCH_UNSUPPORTED,
    TOO_FEW_LITERALS,
    TOO_MANY_LITERALS,
    UNSUPPORTED_LITERAL,
    NO_SELECTORS,
    PARTIAL_L2_CAPACITY,
    PARTIAL_ENTRY_OVERFLOW,
    PARTIAL_OTHER,
    ARTIFACT_BUILD_FAILED
};

struct HAOFeasibilityResult {
    bool canBuild = false;
    u32 flags = 0;
    HAOFeasibilityReason reason = HAOFeasibilityReason::ARTIFACT_BUILD_FAILED;
};

bool analyzeHAOFeasibility(const target_t &target,
                           const std::vector<hwlmLiteral> &lits,
                           const Grey &grey, HAOFeasibilityResult *result,
                           HAOCompileArtifacts *artifacts);

const char *haoFeasibilityReasonName(HAOFeasibilityReason reason);

bool haoHasSveBitPermPrereq(const target_t &target);

bool haoCanUseBextFastPath(const target_t &target);

bool canBuildHAO(const target_t &target, const std::vector<hwlmLiteral> &lits,
                 const Grey &grey);

bool buildHAOArtifacts(const std::vector<hwlmLiteral> &lits,
                       HAOCompileArtifacts *artifacts,
                       HAODumpMode dumpMode = HAODumpMode::Full);

void dumpHAOCompileStats(const HAOCompileArtifacts &artifacts);

void dumpHAOL2MapIfEnabled(const std::vector<hwlmLiteral> &lits,
                           const HAOCompileArtifacts &artifacts);

bool haoArtifactsOk(const HAOCompileArtifacts &artifacts);

bool refreshHAOReports(HAOCompileArtifacts *artifacts,
                       const std::vector<hwlmLiteral> &lits);

bytecode_ptr<u8> buildHAOBlob(const HAOCompileArtifacts &artifacts);

} // namespace ue2

#endif // HAO_COMPILE_H
