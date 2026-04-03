#ifndef PBE_COMPILE_H
#define PBE_COMPILE_H

#include "ue2common.h"
#include "hwlm/hwlm_literal.h"
#include "util/bytecode_ptr.h"

#include <array>
#include <vector>

/* HAO v2 顶层调参宏：后续编译期展开预算与准入判断都依赖这些参数。 */
#ifndef HAO_KEY_BITS
#define HAO_KEY_BITS 22U
#endif

#ifndef HAO_MAX_KEY_AMBIG_BITS
#define HAO_MAX_KEY_AMBIG_BITS 2U
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

static constexpr u32 PBE_ARTIFACT_FLAG_PARTIAL_COVERAGE = 1U << 0;
static constexpr u32 PBE_ARTIFACT_FLAG_PARTIAL_SECONDARY_CAPACITY = 1U << 1;
static constexpr u32 PBE_ARTIFACT_FLAG_PARTIAL_ENTRY_OVERFLOW = 1U << 2;
static constexpr u32 PBE_KEY_BITS = 22U;
static constexpr u32 PBE_L1_OFFSET_BITS = 18U;
static constexpr u32 PBE_L1_OFFSET_MASK = (1U << PBE_L1_OFFSET_BITS) - 1U;
static constexpr u32 PBE_L1_COUNT_SHIFT = PBE_L1_OFFSET_BITS;
static constexpr u32 PBE_RULE_SLOTS_PER_ENTRY = 4U;
static constexpr u32 PBE_BYTES_PER_RULE_SLOT = 8U;
static constexpr u32 PBE_RULE_VECTOR_BYTES =
    PBE_RULE_SLOTS_PER_ENTRY * PBE_BYTES_PER_RULE_SLOT;
static constexpr u32 PBE_TBL_CONTROL_BYTES =
    PBE_RULE_SLOTS_PER_ENTRY * PBE_BYTES_PER_RULE_SLOT;
static constexpr u32 PBE_RULE_SLOT_MASK_WORDS = 1U;
static constexpr u32 PBE_MAX_SELECTORS = 32U;
static constexpr u32 PBE_MAX_MASK_CLASSES = 32U;
static constexpr u32 PBE_MAX_HOT_MASK_CLASSES = 4U;
static constexpr u32 PBE_HOT_MASK_CLASS_COVERAGE_PCT = 80U;
static constexpr u32 PBE_EXTRACT_MODE_SCALAR = 0U;
static constexpr u32 PBE_EXTRACT_MODE_BEXT = 1U;

static constexpr u32 PBE_MASK_CLASS_FLAG_HOT = 1U << 0;

static constexpr u16 PBE_RULE_FLAG_NOCASE = 1U << 0;
static constexpr u16 PBE_RULE_FLAG_NORUNS = 1U << 1;
static constexpr u16 PBE_RULE_FLAG_HAS_MASK = 1U << 2;

static constexpr u32 HAO_RULE_PLAN_FLAG_KEY_EXPANDED = 1U << 0;
static constexpr u32 HAO_RULE_PLAN_FLAG_NEEDS_CONFIRM = 1U << 1;
static constexpr u32 HAO_RULE_PLAN_FLAG_NORMALIZED = 1U << 2;
static constexpr u32 HAO_RULE_PLAN_FLAG_HAS_SUPPLEMENTARY_MASK = 1U << 3;
static constexpr u32 HAO_RULE_PLAN_FLAG_OVER_AMBIG_LIMIT = 1U << 4;
static constexpr u32 HAO_RULE_PLAN_FLAG_OVER_EXPANSION_BUDGET = 1U << 5;
static constexpr u32 HAO_RULE_PLAN_FLAG_ANCHOR_FRAGMENT = 1U << 6;

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
    u8 ruleVector[PBE_RULE_VECTOR_BYTES];
    u8 tableControl[PBE_TBL_CONTROL_BYTES];
    u16 ruleIndex[PBE_RULE_SLOTS_PER_ENTRY];
    u32 keyValue[PBE_RULE_SLOTS_PER_ENTRY];
    u32 keyMask[PBE_RULE_SLOTS_PER_ENTRY];
    u32 headMask;
    u32 tailMask;
    u16 ruleCount;
    u16 reserved;
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

/* HAO v2 中每条规则在编译期都会被归入一个明确类别。 */
enum class HAORuleCategory : u8 {
    HAO_RULE_EXACT = 0,
    HAO_RULE_NOCASE = 1,
    HAO_RULE_SMALL_CLASS_EXPAND = 2,
    HAO_RULE_ANCHOR_CONFIRM = 3,
    HAO_RULE_UNSUPPORTED = 4
};

/* 单条规则在一级 key 空间展开后得到的一个确定 key 变体。 */
struct HAOExpandedKey {
    u32 keyValue = 0;
    u32 ambiguousSelectorMask = 0;
    u32 variantIndex = 0;
};

/* 记录 selected bits 上的模糊信息以及受控展开结果。 */
struct HAOKeyExpansionInfo {
    u32 selectedAmbigBits = 0;
    u32 ambiguousSelectorMask = 0;
    u32 expandedKeyCount = 0;
    std::vector<HAOExpandedKey> expandedKeys;
};

/* verifier fragment 是后续 L2 向量校验真正消费的确定性片段。 */
struct HAOVerifierFragment {
    std::array<u8, PBE_BYTES_PER_RULE_SLOT> bytes = {};
    u8 validByteMask = 0;
    u8 anchorOffset = 0;
    u8 anchorLength = 0;
    u8 flags = 0;
};

/* HAO 编译期规则计划：后续构表应以它为核心，而不是 Mask-Class。 */
struct HAOCompiledRulePlan {
    u32 ruleIndex = 0;
    HAORuleCategory category = HAORuleCategory::HAO_RULE_UNSUPPORTED;
    u32 flags = 0;
    bool needFullConfirm = false;
    HAOKeyExpansionInfo keyExpansion;
    HAOVerifierFragment verifier;
};

/* HAO 编译期汇总信息，用于 feasibility 判断和后续调优。 */
struct HAOCompileSummary {
    u32 totalRules = 0;
    u32 fastPathRules = 0;
    u32 unsupportedRules = 0;
    u32 anchorConfirmRules = 0;
    u32 totalExpandedKeys = 0;
    u32 maxSelectedAmbigBits = 0;
};

/* HAO v2 全局单表构建的统计信息，用于跟踪是否已经收束到单一 22-bit
 * 键空间。 */
struct HAOGlobalHashStats {
    u32 nonEmptyPrimary = 0;
    u32 totalRulesInBuckets = 0;
    u32 totalExpandedKeysInBuckets = 0;
    u32 totalSecondaryEntries = 0;
    u32 maxEntriesPerKey = 0;
};

/* HAO v2 第一轮并行接入的全局单表结果。 */
struct HAOGlobalHashArtifacts {
    bool valid = false;
    u32 flags = 0;
    u32 keyBits = 0;
    u32 fullKeyMask = 0;
    PBEPrimaryHashTable primaryHashTable;
    PBEPrimaryHashBitmap primaryHashBitmap;
    std::vector<PBESecondaryHashEntry> secondaryHashTable;
    HAOGlobalHashStats stats;
};

struct PBECompileArtifacts {
    u32 keyBits = 0;
    u32 flags = 0;
    u32 extractMode = PBE_EXTRACT_MODE_SCALAR;
    u32 windowBytes = PBE_BYTES_PER_RULE_SLOT;
    u64a bextMask = 0;
    std::vector<PBEBitSelector> bitSelectors;
    /* HAO v2 新增：规则计划层和汇总信息。 */
    std::vector<HAOCompiledRulePlan> haoRulePlans;
    HAOCompileSummary haoSummary;
    /* HAO v2 全局单表结果：当前先和 HAO v1 旧结果并存。 */
    HAOGlobalHashArtifacts haoGlobalHash;
    std::vector<PBEMaskClassArtifacts> maskClasses;
    PBEPrimaryHashTable primaryHashTable;
    PBEPrimaryHashBitmap primaryHashBitmap;
    std::vector<PBESecondaryHashEntry> secondaryHashTable;
    std::vector<PBERuleMeta> ruleMeta;
    std::vector<u8> literalBlob;
};

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

struct PBEFeasibilityResult {
    bool canBuild = false;
    u32 flags = 0;
    PBEFeasibilityReason reason = PBEFeasibilityReason::ARTIFACT_BUILD_FAILED;
};

bool analyzePBEFeasibility(const target_t &target,
                           const std::vector<hwlmLiteral> &lits,
                           const Grey &grey, PBEFeasibilityResult *result,
                           PBECompileArtifacts *artifacts);

const char *pbeFeasibilityReasonName(PBEFeasibilityReason reason);

bool pbeHasSveBitPermPrereq(const target_t &target);

bool pbeCanUseBextFastPath(const target_t &target);

bool canBuildPBE(const target_t &target, const std::vector<hwlmLiteral> &lits,
                 const Grey &grey);

bool buildPBEArtifacts(const std::vector<hwlmLiteral> &lits,
                       PBECompileArtifacts *artifacts,
                       bool enableDump = true);

bytecode_ptr<u8> buildPBEBlob(const PBECompileArtifacts &artifacts);

} // namespace ue2

#endif // PBE_COMPILE_H
