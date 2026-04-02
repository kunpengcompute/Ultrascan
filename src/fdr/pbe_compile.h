#ifndef PBE_COMPILE_H
#define PBE_COMPILE_H

#include "ue2common.h"
#include "hwlm/hwlm_literal.h"
#include "util/bytecode_ptr.h"

#include <array>
#include <vector>

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
static constexpr u32 PBE_EXTRACT_MODE_SCALAR = 0U;
static constexpr u32 PBE_EXTRACT_MODE_BEXT = 1U;

static constexpr u16 PBE_RULE_FLAG_NOCASE = 1U << 0;
static constexpr u16 PBE_RULE_FLAG_NORUNS = 1U << 1;
static constexpr u16 PBE_RULE_FLAG_HAS_MASK = 1U << 2;

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

struct PBECompileArtifacts {
    u32 keyBits = 0;
    u32 flags = 0;
    u32 extractMode = PBE_EXTRACT_MODE_SCALAR;
    u32 windowBytes = PBE_BYTES_PER_RULE_SLOT;
    u64a bextMask = 0;
    std::vector<PBEBitSelector> bitSelectors;
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
