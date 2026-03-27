#ifndef PBE_RUNTIME_H
#define PBE_RUNTIME_H

#include "ue2common.h"
#include "hwlm/hwlm.h"

#define PBE_RUNTIME_MAGIC 0x50424530U /* "PBE0" */
#define PBE_RUNTIME_VERSION 3U
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

#define PBE_RULE_FLAG_NOCASE (1U << 0)
#define PBE_RULE_FLAG_NORUNS (1U << 1)
#define PBE_RULE_FLAG_HAS_MASK (1U << 2)

struct PBERuntimeHeader {
    u32 magic;
    u32 version;
    u32 flags;
    u32 keyBits;
    u32 selectorCount;
    u32 primaryCount;
    u32 secondaryCount;
    u32 ruleMetaCount;
    u32 literalBlobSize;
    u32 selectorsOffset;
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

#endif // PBE_RUNTIME_H
