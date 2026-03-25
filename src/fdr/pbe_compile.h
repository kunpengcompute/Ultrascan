#ifndef PBE_COMPILE_H
#define PBE_COMPILE_H

#include "ue2common.h"
#include "hwlm/hwlm_literal.h"
#include "util/bytecode_ptr.h"

#include <vector>

namespace ue2 {

static constexpr u32 PBE_ARTIFACT_FLAG_PARTIAL_COVERAGE = 1U << 0;

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

struct PBESecondaryHashEntry {
    u8 ruleVector[32];
    u8 tableControl[32];
    u16 ruleIndex[32];
    u32 headMask;
    u32 tailMask;
    u16 ruleBase;
    u16 ruleCount;
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
    std::vector<PBEBitSelector> bitSelectors;
    PBEPrimaryHashTable primaryHashTable;
    std::vector<PBESecondaryHashEntry> secondaryHashTable;
    std::vector<PBERuleMeta> ruleMeta;
    std::vector<u8> literalBlob;
};

bool canBuildPBE(const target_t &target, const std::vector<hwlmLiteral> &lits,
                 const Grey &grey);

bool buildPBEArtifacts(const std::vector<hwlmLiteral> &lits,
                       PBECompileArtifacts *artifacts);

bytecode_ptr<u8> buildPBEBlob(const PBECompileArtifacts &artifacts);

} // namespace ue2

#endif // PBE_COMPILE_H
