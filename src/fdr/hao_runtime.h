#ifndef HAO_RUNTIME_H
#define HAO_RUNTIME_H

#include "ue2common.h"
#include "hwlm/hwlm.h"

#ifndef HAO_KEY_BITS
#define HAO_KEY_BITS 22U
#endif

#define HAO_RUNTIME_MAGIC 0x48414f30U /* "HAO0" */
#define HAO_RUNTIME_VERSION 30U
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
#define HAO_RUNTIME_DOT_VECTOR_LANES 4U
#define HAO_RUNTIME_KEY_BITS_MASK 0xffU
#define HAO_RUNTIME_HASH_MODE_SHIFT 24U
#define HAO_BATCH_FALLBACK_WIDTH 4U
#define HAO_BATCH_MAX_WIDTH 32U

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
    u64a dotInputMask;
    u32 primaryBitmapOffset;
    u32 primaryOffset;
    u32 l2CheckOffset;
    u32 l2MetaOffset;
    u32 ruleMetaOffset;
};

struct HAORuntimeRuleMeta {
    u32 id;
    u16 flags;
    u16 reserved;
    hwlm_group_t groups;
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
