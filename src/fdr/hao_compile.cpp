#include "hao_compile.h"

#include "grey.h"
#include "hao_runtime.h"
#include "util/alloc.h"
#include "util/arch.h"
#include "util/bitutils.h"
#include "util/target_info.h"
#include "util/compare.h"
#include "util/verify_types.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

namespace ue2 {

static_assert(sizeof(HAORuntimeL2Check) == HAO_RUNTIME_L2_CHECK_ALIGN,
              "HAO L2 check must stay one aligned cache line");
static_assert(sizeof(HAORuntimeHeader) == 96U,
              "HAO runtime header must stay compact to keep table offsets stable");
static_assert(HAO_LAYOUT_HASH_BEXT == HAO_RUNTIME_HASH_BEXT,
              "compile/runtime HAO BEXT hash ids must match");
static_assert(HAO_LAYOUT_HASH_DOT == HAO_RUNTIME_HASH_DOT,
              "compile/runtime HAO DOT hash ids must match");
static_assert(HAO_LAYOUT_HASH_DOT_GROUP == HAO_RUNTIME_HASH_DOT_GROUP,
              "compile/runtime HAO DOT group hash ids must match");
static_assert(HAO_LAYOUT_DOT_VECTOR_LANES == HAO_RUNTIME_DOT_VECTOR_LANES,
              "compile/runtime HAO DOT vector width must match");

namespace {

static constexpr u32 HAO_BUILD_MAX_SUFFIX_BYTES = 8;
static constexpr u32 HAO_BUILD_MAX_CANDIDATE_BITS = HAO_BUILD_MAX_SUFFIX_BYTES * 8;
static constexpr u32 HAO_BUILD_L2_KEY_BITS = 22;
static constexpr u32 HAO_BUILD_MAX_L2_ENTRIES = 1U << HAO_BUILD_L2_KEY_BITS;
#ifndef HAO_HASH_OPT_MAX_LITS
#define HAO_HASH_OPT_MAX_LITS 10000U
#endif
#ifndef HAO_HASH_OPT_SPREAD_PENALTY
#define HAO_HASH_OPT_SPREAD_PENALTY 1.0
#endif
#ifndef HAO_HASH_OPT_ENABLE_SWAP
#define HAO_HASH_OPT_ENABLE_SWAP 1
#endif
#ifndef HAO_AUTO_INCLUDE_HASH_OPT
#define HAO_AUTO_INCLUDE_HASH_OPT 0
#endif
#ifndef HAO_FIXED_BEXT_MASK
#define HAO_FIXED_BEXT_MASK 0ULL
#endif
#ifndef HAO_DOT_MAX_KEY_EXPANSION
#define HAO_DOT_MAX_KEY_EXPANSION 4096U
#endif
#ifndef HAO_DOT_INPUT_MASK_MAX_BITS
#define HAO_DOT_INPUT_MASK_MAX_BITS 34U
#endif
#ifndef HAO_DOT_VECTOR_SEARCH_ROUNDS
#define HAO_DOT_VECTOR_SEARCH_ROUNDS 64U
#endif
#ifndef HAO_DOT_INPUT_MASK_GREEDY_CANDIDATES
#define HAO_DOT_INPUT_MASK_GREEDY_CANDIDATES 32U
#endif
static constexpr u64a HAO_BUILD_MAX_TOTAL_PRIMARY_FOOTPRINT =
    128ULL * 1024ULL * 1024ULL;
static constexpr u8 HAO_BUILD_STATE_DONT_CARE = 2;
static constexpr std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES>
    HAO_DOT_DEFAULT_VECTOR = {{0x010fU, 0x043fU, 0x1105U, 0x4417U}};
static constexpr u32 HAO_DOT_GROUP_DRYRUN_DEFAULT_MIN_KEY_BITS = 14U;

struct HAOBitCandidate {
    u32 bitIndex = 0;
    double score = 0.0;
    u32 fixedCount = 0;
    u32 ambiguousCount = 0;
    double careRatio = 0.0;
    double fixedRatio = 0.0;
    double ambiguousRatio = 0.0;
    double entropy = 0.0;
    std::vector<u8> states; // 0, 1, or HAO_BUILD_STATE_DONT_CARE
};

enum class HAOSelectorMode : u8 {
    DEFAULT,
    DYNAMIC_EXPANSION,
    HIGH_ALIGN,
    HASH_OPT
};

enum class HAOSelectorPolicy : u8 {
    AUTO,
    DEFAULT_ONLY,
    DYNAMIC_ONLY,
    HIGH_ALIGN_ONLY,
    HASH_OPT_ONLY
};

enum class HAOHashPolicy : u8 {
    BEXT,
    DOT,
    DOT_GROUP,
    DOT_GROUP_DRYRUN
};

struct HAOLiteralRef {
    const hwlmLiteral *lit = nullptr;
    u32 ruleIndex = 0;
};

static
bool getBitState(const hwlmLiteral &lit, u32 bitIndex, u8 *state);

static
std::vector<HAOBitCandidate> buildBitCandidates(
    const std::vector<hwlmLiteral> &lits);

static
void haoSelectBits(const std::vector<hwlmLiteral> &lits,
                   std::vector<HAOBitSelector> *selectors,
                   u32 *keyBitsOut, HAOSelectorMode mode,
                   u32 targetBits);

static
void haoLitKeyMask(const hwlmLiteral &lit,
                                   const std::vector<HAOBitSelector> &selectors,
                                   u32 *keyValue, u32 *keyMask) {
    u32 v = 0;
    u32 m = 0;
    for (u32 i = 0; i < selectors.size(); i++) {
        const auto &sel = selectors[i];
        const u32 bitIndex = static_cast<u32>(sel.byteOffset) * 8U +
                             static_cast<u32>(sel.bitOffset);
        u8 state = HAO_BUILD_STATE_DONT_CARE;
        getBitState(lit, bitIndex, &state);
        if (state == HAO_BUILD_STATE_DONT_CARE) {
            continue;
        }
        m |= (1U << i);
        if (state) {
            v |= (1U << i);
        }
    }
    *keyValue = v;
    *keyMask = m;
}

static
u32 haoSelectorBitIndex(const HAOBitSelector &selector) {
    return static_cast<u32>(selector.byteOffset) * 8U +
           static_cast<u32>(selector.bitOffset);
}

static
std::string byteToBits(u8 v) {
    std::string s(8, '0');
    for (u32 i = 0; i < 8; i++) {
        if (v & (1U << (7 - i))) {
            s[i] = '1';
        }
    }
    return s;
}

static
std::string keyToBits(u32 key, u32 width) {
    std::string s(width, '0');
    for (u32 i = 0; i < width; i++) {
        if (key & (1U << (width - i - 1))) {
            s[i] = '1';
        }
    }
    return s;
}

static
u32 encodePrimaryValue(u32 l2Offset, u32 entryCount) {
    assert(l2Offset <= HAO_LAYOUT_L1_OFFSET_MASK);
    assert(entryCount < (1U << (32U - HAO_LAYOUT_L1_COUNT_SHIFT)));
    return (entryCount << HAO_LAYOUT_L1_COUNT_SHIFT) | l2Offset;
}

static
u32 haoPrimaryBitmapBytes(u32 primaryCount) {
    return (primaryCount + 7U) / 8U;
}

static really_inline
u32 haoBitmapKeyForPrimaryKey(u32 key) {
#if HAO_COMPRESSED_BITMAP
    return key >> HAO_COMPRESSED_BITMAP_SHIFT;
#else
    return key;
#endif
}

static
u32 haoPrimaryBitmapCountForPrimaryCount(u32 primaryCount) {
#if HAO_COMPRESSED_BITMAP
    const u32 groupSize = 1U << HAO_COMPRESSED_BITMAP_SHIFT;
    return (primaryCount + groupSize - 1U) >> HAO_COMPRESSED_BITMAP_SHIFT;
#else
    return primaryCount;
#endif
}

static
void buildPrimaryBitmap(const HAOPrimaryHashTable &primaryHashTable,
                        HAOPrimaryHashBitmap *primaryHashBitmap);

static
u32 haoPrimaryCountForKeyBits(u32 keyBits) {
    if (!keyBits) {
        return 1U;
    }
    assert(keyBits <= HAO_LAYOUT_KEY_BITS);
    return 1U << keyBits;
}

/* Returns true when a literal carries supplementary mask/cmp semantics.
 * Mergeable masks are folded into L2; conflicting masks keep runtime confirm. */
static
bool haoHasMask(const hwlmLiteral &lit) {
    return !lit.msk.empty() || !lit.cmp.empty();
}

static
bool normalizeMaskCmp(const hwlmLiteral &lit, std::array<u8, 8> *mskOut,
                      std::array<u8, 8> *cmpOut, u8 *lenOut);

static
void packMaskCmpTail(const std::array<u8, 8> &msk,
                     const std::array<u8, 8> &cmp, u8 len,
                     u64a *maskWord, u64a *cmpWord);

/* Enumerate the selected-bit key variants for one rule. HAO is fast-path
 * only, so rules that exceed HAO_MAX_KEY_AMBIG_BITS reject the whole build. */
static
HAOKeyExpansionInfo haoExpandKeys(
    const hwlmLiteral &lit, const std::vector<HAOBitSelector> &selectors) {
    HAOKeyExpansionInfo info;
    u32 baseKeyValue = 0;
    std::vector<u32> ambiguousBits;
    ambiguousBits.reserve(selectors.size());

    for (u32 i = 0; i < selectors.size(); i++) {
        const auto &sel = selectors[i];
        const u32 bitIndex = haoSelectorBitIndex(sel);
        u8 state = HAO_BUILD_STATE_DONT_CARE;
        getBitState(lit, bitIndex, &state);
        if (state == HAO_BUILD_STATE_DONT_CARE) {
            ambiguousBits.push_back(i);
            info.ambiguousSelectorMask |= (1U << i);
            continue;
        }
        if (state) {
            baseKeyValue |= (1U << i);
        }
    }

    info.selectedAmbigBits = verify_u32(ambiguousBits.size());
    if (info.selectedAmbigBits > HAO_MAX_KEY_AMBIG_BITS) {
        return info;
    }

    // Enumerate all key variants induced by ambiguous selected bits. Nocase
    // alphabetic bit 5 is treated as don't-care; all other bits keep the raw
    // literal value so the runtime can compare raw input bytes end-to-end.
    const u32 variantCount = info.selectedAmbigBits ?
                             (1U << info.selectedAmbigBits) : 1U;
    info.expandedKeys.reserve(variantCount);
    for (u32 variant = 0; variant < variantCount; variant++) {
        u32 keyValue = baseKeyValue;
        for (u32 j = 0; j < ambiguousBits.size(); j++) {
            const u32 bit = ambiguousBits[j];
            if (variant & (1U << j)) {
                keyValue |= (1U << bit);
            } else {
                keyValue &= ~(1U << bit);
            }
        }

        HAOExpandedKey expanded = {};
        expanded.keyValue = keyValue;
        expanded.ambiguousSelectorMask = info.ambiguousSelectorMask;
        expanded.variantIndex = variant;
        info.expandedKeys.push_back(expanded);
    }
    info.expandedKeyCount = verify_u32(info.expandedKeys.size());
    return info;
}

static
u32 haoKeyMaskForBits(u32 keyBits) {
    if (!keyBits) {
        return 0;
    }
    if (keyBits >= 32U) {
        return 0xffffffffU;
    }
    return (1U << keyBits) - 1U;
}

static
u32 haoCeilLog2(u32 value) {
    u32 bits = 0;

    if (value <= 1U) {
        return 0;
    }
    value--;
    while (value) {
        bits++;
        value >>= 1U;
    }
    return bits;
}

static
bool haoDotByteValues(u8 ruleByte, u8 maskByte, u8 inputMaskByte,
                      std::vector<u8> *out) {
    if (!out) {
        return false;
    }

    out->clear();
    out->reserve(maskByte == 0xffU ? 1U : 2U);
    for (u32 value = 0; value <= 0xffU; value++) {
        if (((u8)value & maskByte) == ruleByte) {
            out->push_back(verify_u8(value & inputMaskByte));
        }
    }
    std::sort(out->begin(), out->end());
    out->erase(std::unique(out->begin(), out->end()), out->end());
    return !out->empty();
}

static
bool haoDotWordContribs(const std::vector<u8> &loValues,
                        const std::vector<u8> &hiValues, u16 coefficient,
                        u32 keyMask, std::vector<u32> *out) {
    if (!out || loValues.empty() || hiValues.empty()) {
        return false;
    }

    out->clear();
    if (!coefficient) {
        out->push_back(0);
        return true;
    }
    if (loValues.size() * hiValues.size() > HAO_DOT_MAX_KEY_EXPANSION) {
        return false;
    }

    out->reserve(loValues.size() * hiValues.size());
    for (const u8 lo : loValues) {
        for (const u8 hi : hiValues) {
            const u32 word = (u32)lo | ((u32)hi << 8U);
            out->push_back((word * (u32)coefficient) & keyMask);
        }
    }

    std::sort(out->begin(), out->end());
    out->erase(std::unique(out->begin(), out->end()), out->end());
    return !out->empty();
}

static
bool haoDotCombineKeys(std::vector<u32> *keys,
                       const std::vector<u32> &contribs, u32 keyMask) {
    if (!keys || keys->empty() || contribs.empty()) {
        return false;
    }
    if (keys->size() * contribs.size() > HAO_DOT_MAX_KEY_EXPANSION) {
        return false;
    }

    std::vector<u32> combined;
    combined.reserve(keys->size() * contribs.size());
    for (const u32 key : *keys) {
        for (const u32 contrib : contribs) {
            combined.push_back((key + contrib) & keyMask);
        }
    }
    std::sort(combined.begin(), combined.end());
    combined.erase(std::unique(combined.begin(), combined.end()),
                   combined.end());
    if (combined.size() > HAO_DOT_MAX_KEY_EXPANSION) {
        return false;
    }

    *keys = std::move(combined);
    return !keys->empty();
}

/* Enumerate all DOT keys compatible with the final L2 verifier constraint.
 * This keeps DOT correct for exact, nocase and merged mask/cmp rules: if a
 * verifier byte is don't-care, all values it can contribute to the dot hash
 * are represented here, bounded by HAO_DOT_MAX_KEY_EXPANSION. */
static
HAOKeyExpansionInfo haoExpandDotKeys(
    const HAOVerifierFragment &verifier,
    const std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> &dotVector,
    u64a dotInputMask, u32 keyBits) {
    HAOKeyExpansionInfo info;
    const u32 keyMask = haoKeyMaskForBits(keyBits);
    std::array<std::vector<u8>, HAO_LAYOUT_BYTES_PER_RULE_SLOT> byteValues;
    std::vector<u32> keys(1, 0);

    if (!keyBits) {
        return info;
    }

    for (u32 byte = 0; byte < HAO_LAYOUT_BYTES_PER_RULE_SLOT; byte++) {
        const u8 ruleByte =
            verify_u8((verifier.ruleWord >> (byte * 8U)) & 0xffU);
        const u8 maskByte =
            verify_u8((verifier.maskWord >> (byte * 8U)) & 0xffU);
        const u8 inputMaskByte =
            verify_u8((dotInputMask >> (byte * 8U)) & 0xffU);
        if (!haoDotByteValues(ruleByte, maskByte, inputMaskByte,
                              &byteValues[byte])) {
            return info;
        }
    }

    for (u32 lane = 0; lane < HAO_LAYOUT_DOT_VECTOR_LANES; lane++) {
        std::vector<u32> contribs;
        const u32 byteBase = lane * 2U;
        if (!haoDotWordContribs(byteValues[byteBase],
                                byteValues[byteBase + 1U],
                                dotVector[lane], keyMask, &contribs) ||
            !haoDotCombineKeys(&keys, contribs, keyMask)) {
            return HAOKeyExpansionInfo();
        }
    }

    info.selectedAmbigBits = haoCeilLog2(verify_u32(keys.size()));
    if (info.selectedAmbigBits > HAO_MAX_KEY_AMBIG_BITS) {
        return HAOKeyExpansionInfo();
    }

    info.expandedKeys.reserve(keys.size());
    for (u32 i = 0; i < keys.size(); i++) {
        HAOExpandedKey expanded = {};
        expanded.keyValue = keys[i];
        expanded.variantIndex = i;
        info.expandedKeys.push_back(expanded);
    }
    info.expandedKeyCount = verify_u32(info.expandedKeys.size());
    return info;
}

static
std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES>
haoBuildDotVectorForLits(const std::vector<hwlmLiteral> &lits) {
    std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> dotVector =
        HAO_DOT_DEFAULT_VECTOR;
    u32 minLen = HAO_LAYOUT_BYTES_PER_RULE_SLOT;

    for (const auto &lit : lits) {
        minLen = std::min(minLen, verify_u32(lit.s.size()));
    }

    const u32 firstGloballyKnownByte =
        minLen >= HAO_LAYOUT_BYTES_PER_RULE_SLOT
            ? 0U
            : HAO_LAYOUT_BYTES_PER_RULE_SLOT - minLen;
    for (u32 lane = 0; lane < HAO_LAYOUT_DOT_VECTOR_LANES; lane++) {
        const u32 byteBase = lane * 2U;
        if (byteBase < firstGloballyKnownByte ||
            byteBase + 1U < firstGloballyKnownByte) {
            dotVector[lane] = 0;
        }
    }
    return dotVector;
}

static
bool haoDotVectorHasNonZeroLane(
    const std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> &dotVector) {
    for (const u16 coefficient : dotVector) {
        if (coefficient) {
            return true;
        }
    }
    return false;
}

static
bool haoParseDotVectorEnv(
    std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> *dotVector) {
    const char *env = getenv("HS_HAO_DOT_VECTOR");
    unsigned values[HAO_LAYOUT_DOT_VECTOR_LANES] = {};

    if (!dotVector || !env || !*env) {
        return false;
    }
    if (sscanf(env, "%u,%u,%u,%u", &values[0], &values[1], &values[2],
               &values[3]) != (int)HAO_LAYOUT_DOT_VECTOR_LANES) {
        printf("[HAO][Dot] ignoring invalid HS_HAO_DOT_VECTOR=%s\n", env);
        return false;
    }
    for (u32 i = 0; i < HAO_LAYOUT_DOT_VECTOR_LANES; i++) {
        (*dotVector)[i] = verify_u16(values[i] & 0xffffU);
    }
    return true;
}

static
std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> haoApplyDotInputMaskToVector(
    const std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> &dotVector,
    u64a dotInputMask) {
    std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> masked = dotVector;

    for (u32 lane = 0; lane < HAO_LAYOUT_DOT_VECTOR_LANES; lane++) {
        const u64a laneMask = (dotInputMask >> (lane * 16U)) & 0xffffU;
        if (!laneMask) {
            masked[lane] = 0;
        }
    }
    return masked;
}

static
u64a haoPackDotVector(
    const std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> &dotVector) {
    u64a packed = 0;
    for (u32 i = 0; i < HAO_LAYOUT_DOT_VECTOR_LANES; i++) {
        packed |= (u64a)dotVector[i] << (i * 16U);
    }
    return packed;
}

/* Current first-pass rule categories describe the base literal verifier.
 * Supplementary mask/cmp payloads are merged or marked for confirm later. */
static
HAORuleCategory haoClassifyLiteral(const hwlmLiteral &lit,
                                   const HAOKeyExpansionInfo &expansion) {
    if (expansion.selectedAmbigBits > HAO_MAX_KEY_AMBIG_BITS) {
        return HAORuleCategory::HAO_RULE_UNSUPPORTED;
    }

    if (lit.nocase) {
        return HAORuleCategory::HAO_RULE_NOCASE;
    }

    return HAORuleCategory::HAO_RULE_EXACT;
}

static
bool haoStatsDumpEnabled(void) {
    const char *env = getenv("HS_HAO_STATS");
    return env && *env && *env != '0';
}

static
bool haoL2MapDumpEnabled(void) {
    const char *env = getenv("HS_HAO_L2MAP");
    return env && *env && *env != '0';
}

static
HAOSelectorPolicy haoSelectorPolicy(void) {
    static int policy = -1;
    if (policy >= 0) {
        return static_cast<HAOSelectorPolicy>(policy);
    }

    const char *env = getenv("HS_HAO_SELECTOR_MODE");
    if (!env || !env[0] || strcmp(env, "auto") == 0 ||
        strcmp(env, "AUTO") == 0) {
        policy = static_cast<int>(HAOSelectorPolicy::AUTO);
    } else if (strcmp(env, "default") == 0 || strcmp(env, "DEFAULT") == 0 ||
               strcmp(env, "0") == 0) {
        policy = static_cast<int>(HAOSelectorPolicy::DEFAULT_ONLY);
    } else if (strcmp(env, "dynamic") == 0 || strcmp(env, "DYNAMIC") == 0 ||
               strcmp(env, "1") == 0) {
        policy = static_cast<int>(HAOSelectorPolicy::DYNAMIC_ONLY);
    } else if (strcmp(env, "high") == 0 ||
               strcmp(env, "HIGH") == 0 ||
               strcmp(env, "high_align") == 0 ||
               strcmp(env, "HIGH_ALIGN") == 0 ||
               strcmp(env, "3") == 0) {
        policy = static_cast<int>(HAOSelectorPolicy::HIGH_ALIGN_ONLY);
    } else if (strcmp(env, "hash_opt") == 0 ||
               strcmp(env, "HASH_OPT") == 0 ||
               strcmp(env, "hashopt") == 0 ||
               strcmp(env, "HASHOPT") == 0 ||
               strcmp(env, "weighted") == 0 ||
               strcmp(env, "WEIGHTED") == 0 ||
               strcmp(env, "2") == 0) {
        policy = static_cast<int>(HAOSelectorPolicy::HASH_OPT_ONLY);
    } else {
        policy = static_cast<int>(HAOSelectorPolicy::AUTO);
    }

    return static_cast<HAOSelectorPolicy>(policy);
}

static
HAOHashPolicy haoHashPolicy(void) {
    static int policy = -1;
    if (policy >= 0) {
        return static_cast<HAOHashPolicy>(policy);
    }

    const char *env = getenv("HS_HAO_HASH_MODE");
    if (env && *env &&
        (strcmp(env, "dot") == 0 || strcmp(env, "DOT") == 0 ||
         strcmp(env, "1") == 0)) {
        policy = static_cast<int>(HAOHashPolicy::DOT);
    } else if (env && *env &&
               (strcmp(env, "dot_group") == 0 ||
                strcmp(env, "DOT_GROUP") == 0 ||
                strcmp(env, "dot-group") == 0 ||
                strcmp(env, "2") == 0)) {
        policy = static_cast<int>(HAOHashPolicy::DOT_GROUP);
    } else if (env && *env &&
               (strcmp(env, "dot_group_dryrun") == 0 ||
                strcmp(env, "DOT_GROUP_DRYRUN") == 0 ||
                strcmp(env, "dot-group-dryrun") == 0)) {
        policy = static_cast<int>(HAOHashPolicy::DOT_GROUP_DRYRUN);
    } else if (env && *env &&
               (strcmp(env, "bext") == 0 || strcmp(env, "BEXT") == 0 ||
                strcmp(env, "0") == 0)) {
        policy = static_cast<int>(HAOHashPolicy::BEXT);
    } else {
        policy = static_cast<int>(HAOHashPolicy::BEXT);
    }

    return static_cast<HAOHashPolicy>(policy);
}

static
bool haoHashPolicyIsExplicit(void) {
    const char *env = getenv("HS_HAO_HASH_MODE");

    return env && *env && strcmp(env, "auto") != 0 && strcmp(env, "AUTO") != 0;
}

static
const char *haoHashModeName(u32 hashMode) {
    switch (hashMode) {
    case HAO_LAYOUT_HASH_BEXT:
        return "bext";
    case HAO_LAYOUT_HASH_DOT:
        return "dot";
    case HAO_LAYOUT_HASH_DOT_GROUP:
        return "dot_group";
    default:
        return "unknown";
    }
}

static
HAOSelectorMode haoSelectorMode(HAOSelectorPolicy policy) {
    switch (policy) {
    case HAOSelectorPolicy::DEFAULT_ONLY:
        return HAOSelectorMode::DEFAULT;
    case HAOSelectorPolicy::HIGH_ALIGN_ONLY:
        return HAOSelectorMode::HIGH_ALIGN;
    case HAOSelectorPolicy::HASH_OPT_ONLY:
        return HAOSelectorMode::HASH_OPT;
    case HAOSelectorPolicy::AUTO:
    case HAOSelectorPolicy::DYNAMIC_ONLY:
    default:
        return HAOSelectorMode::DYNAMIC_EXPANSION;
    }
}

static
const char *haoSelectorName(HAOSelectorPolicy policy) {
    switch (policy) {
    case HAOSelectorPolicy::DEFAULT_ONLY:
        return "default-forced";
    case HAOSelectorPolicy::DYNAMIC_ONLY:
        return "dynamic-forced";
    case HAOSelectorPolicy::HIGH_ALIGN_ONLY:
        return "high_align-forced";
    case HAOSelectorPolicy::HASH_OPT_ONLY:
        return "hash_opt-forced";
    case HAOSelectorPolicy::AUTO:
    default:
        return "dynamic";
    }
}

static
u8 haoCareByteMaskFromWord(u64a maskWord) {
    u8 out = 0;
    for (u32 i = 0; i < HAO_LAYOUT_BYTES_PER_RULE_SLOT; i++) {
        if ((maskWord >> (i * 8U)) & 0xffU) {
            out |= verify_u8(1U << i);
        }
    }
    return out;
}

/* Build the deterministic verifier fragment that the later L2 verifier
 * consumes. hwlmLiteral stores nocase literals in canonical uppercase form;
 * nocase bytes are encoded through an L2 mask byte of 0xdf so runtime can
 * compare input bytes without normalizing them first. Supplementary mask/cmp
 * payloads must be merged into L2; a merge failure rejects the HAO plan. */
static
HAOVerifierFragment haoBuildCheck(const hwlmLiteral &lit,
                                             HAORuleCategory category) {
    HAOVerifierFragment fragment = {};
    const u32 len = verify_u32(lit.s.size());
    assert(len <= HAO_LAYOUT_BYTES_PER_RULE_SLOT);
    const u32 suffixLen = len;
    const u32 laneStart = HAO_LAYOUT_BYTES_PER_RULE_SLOT - suffixLen;

    for (u32 j = 0; j < suffixLen; j++) {
        const u8 c = verify_u8(lit.s[len - suffixLen + j]);
        const u32 idx = laneStart + j;
        u8 ruleByte = c;
        u8 maskByte = verify_u8(0xffU);

        fragment.bytes[idx] = c;
        fragment.validByteMask |= verify_u8(1U << idx);
        if (lit.nocase && ourisalpha(c)) {
            fragment.nocaseByteMask |= verify_u8(1U << idx);
            maskByte = verify_u8(0xdfU);
            ruleByte = verify_u8(ruleByte & maskByte);
        }

        fragment.ruleWord |= (u64a)ruleByte << (idx * 8U);
        fragment.maskWord |= (u64a)maskByte << (idx * 8U);
    }

    if (category == HAORuleCategory::HAO_RULE_NOCASE) {
        fragment.flags |= verify_u8(HAO_RULE_PLAN_FLAG_NORMALIZED);
    }

    if (haoHasMask(lit)) {
        std::array<u8, 8> normMsk = {};
        std::array<u8, 8> normCmp = {};
        u8 normLen = 0;
        if (normalizeMaskCmp(lit, &normMsk, &normCmp, &normLen) &&
            normLen) {
            u64a maskWord = 0;
            u64a cmpWord = 0;
            packMaskCmpTail(normMsk, normCmp, normLen,
                            &maskWord, &cmpWord);

            const u64a overlap = fragment.maskWord & maskWord;
            const u64a conflict =
                overlap & ((fragment.ruleWord ^ cmpWord));
            if (!conflict) {
                fragment.ruleWord =
                    (fragment.ruleWord & fragment.maskWord) |
                    (cmpWord & maskWord);
                fragment.maskWord |= maskWord;
                fragment.flags |= verify_u8(HAO_RULE_PLAN_FLAG_MASK_MERGED);
            } else {
                fragment.flags |=
                    verify_u8(HAO_RULE_PLAN_FLAG_MASK_MERGE_FAILED);
            }
        }
    }

    fragment.careByteMask = haoCareByteMaskFromWord(fragment.maskWord);
    return fragment;
}

/* Build the compile-time HAO rule-plan layer that later table construction
 * consumes directly. */
static
void haoBuildPlans(const std::vector<hwlmLiteral> &lits,
                       const std::vector<HAOBitSelector> &selectors,
                       std::vector<HAOCompiledRulePlan> *rulePlans,
                       HAOCompileSummary *summary) {
    if (!rulePlans || !summary) {
        return;
    }

    rulePlans->clear();
    rulePlans->reserve(lits.size());
    *summary = {};
    summary->totalRules = verify_u32(lits.size());

    for (u32 i = 0; i < lits.size(); i++) {
        HAOCompiledRulePlan plan = {};
        const u32 litLen = verify_u32(lits[i].s.size());
        plan.ruleIndex = i;
        plan.keyExpansion = haoExpandKeys(lits[i], selectors);
        plan.category = haoClassifyLiteral(lits[i], plan.keyExpansion);
        plan.verifier = haoBuildCheck(lits[i], plan.category);

        summary->totalLiteralBytes += litLen;
        if (!summary->minLiteralLen || litLen < summary->minLiteralLen) {
            summary->minLiteralLen = litLen;
        }
        summary->maxLiteralLen = std::max(summary->maxLiteralLen, litLen);
        if (litLen <= 4U) {
            summary->literalLenLe4++;
        } else {
            assert(litLen <= HAO_LAYOUT_BYTES_PER_RULE_SLOT);
            summary->literalLen5To8++;
        }

        if (plan.keyExpansion.selectedAmbigBits) {
            plan.flags |= HAO_RULE_PLAN_FLAG_KEY_EXPANDED;
        }
        if (lits[i].nocase) {
            plan.flags |= HAO_RULE_PLAN_FLAG_NORMALIZED;
        }
        if (haoHasMask(lits[i])) {
            plan.flags |= HAO_RULE_PLAN_FLAG_HAS_SUPPLEMENTARY_MASK;
            summary->maskRules++;
        }
        if (plan.keyExpansion.selectedAmbigBits > HAO_MAX_KEY_AMBIG_BITS) {
            plan.flags |= HAO_RULE_PLAN_FLAG_OVER_AMBIG_LIMIT;
        }

        if (plan.verifier.flags & HAO_RULE_PLAN_FLAG_MASK_MERGED) {
            plan.flags |= HAO_RULE_PLAN_FLAG_MASK_MERGED;
            summary->maskMergedRules++;
        }
        if (plan.verifier.flags & HAO_RULE_PLAN_FLAG_MASK_MERGE_FAILED) {
            plan.flags |= HAO_RULE_PLAN_FLAG_MASK_MERGE_FAILED;
            plan.category = HAORuleCategory::HAO_RULE_UNSUPPORTED;
            summary->maskConfirmRules++;
            summary->maskConflictRules++;
        }

        if (summary->maxSelectedAmbigBits < plan.keyExpansion.selectedAmbigBits) {
            summary->maxSelectedAmbigBits = plan.keyExpansion.selectedAmbigBits;
        }

        if (plan.category == HAORuleCategory::HAO_RULE_UNSUPPORTED) {
            summary->unsupportedRules++;
            plan.keyExpansion.expandedKeys.clear();
            plan.keyExpansion.expandedKeyCount = 0;
        }

        switch (plan.category) {
        case HAORuleCategory::HAO_RULE_EXACT:
            summary->exactRules++;
            break;
        case HAORuleCategory::HAO_RULE_NOCASE:
            summary->nocaseRules++;
            break;
        case HAORuleCategory::HAO_RULE_UNSUPPORTED:
            break;
        default:
            break;
        }

        if (plan.flags & HAO_RULE_PLAN_FLAG_KEY_EXPANDED) {
            summary->keyExpandedRules++;
        }
        if (plan.category != HAORuleCategory::HAO_RULE_UNSUPPORTED) {
            summary->fastPathRules++;
            summary->totalExpandedKeys += plan.keyExpansion.expandedKeyCount;
        }
        rulePlans->push_back(std::move(plan));
    }
}

static
void haoBuildDotPlansFromRefs(
    const std::vector<HAOLiteralRef> &lits,
    const std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> &dotVector,
    u64a dotInputMask, u32 keyBits,
    std::vector<HAOCompiledRulePlan> *rulePlans, HAOCompileSummary *summary) {
    if (!rulePlans || !summary) {
        return;
    }

    rulePlans->clear();
    rulePlans->reserve(lits.size());
    *summary = {};
    summary->totalRules = verify_u32(lits.size());

    for (u32 i = 0; i < lits.size(); i++) {
        assert(lits[i].lit);
        const hwlmLiteral &lit = *lits[i].lit;
        HAOCompiledRulePlan plan = {};
        const u32 litLen = verify_u32(lit.s.size());
        plan.ruleIndex = lits[i].ruleIndex;
        plan.category = lit.nocase ? HAORuleCategory::HAO_RULE_NOCASE
                                   : HAORuleCategory::HAO_RULE_EXACT;
        plan.verifier = haoBuildCheck(lit, plan.category);
        plan.keyExpansion = haoExpandDotKeys(plan.verifier, dotVector,
                                             dotInputMask, keyBits);
        plan.category = haoClassifyLiteral(lit, plan.keyExpansion);

        summary->totalLiteralBytes += litLen;
        if (!summary->minLiteralLen || litLen < summary->minLiteralLen) {
            summary->minLiteralLen = litLen;
        }
        summary->maxLiteralLen = std::max(summary->maxLiteralLen, litLen);
        if (litLen <= 4U) {
            summary->literalLenLe4++;
        } else {
            assert(litLen <= HAO_LAYOUT_BYTES_PER_RULE_SLOT);
            summary->literalLen5To8++;
        }

        if (plan.keyExpansion.selectedAmbigBits) {
            plan.flags |= HAO_RULE_PLAN_FLAG_KEY_EXPANDED;
        }
        if (lit.nocase) {
            plan.flags |= HAO_RULE_PLAN_FLAG_NORMALIZED;
        }
        if (haoHasMask(lit)) {
            plan.flags |= HAO_RULE_PLAN_FLAG_HAS_SUPPLEMENTARY_MASK;
            summary->maskRules++;
        }
        if (!plan.keyExpansion.expandedKeyCount ||
            plan.keyExpansion.selectedAmbigBits > HAO_MAX_KEY_AMBIG_BITS) {
            plan.flags |= HAO_RULE_PLAN_FLAG_OVER_AMBIG_LIMIT;
            plan.category = HAORuleCategory::HAO_RULE_UNSUPPORTED;
        }

        if (plan.verifier.flags & HAO_RULE_PLAN_FLAG_MASK_MERGED) {
            plan.flags |= HAO_RULE_PLAN_FLAG_MASK_MERGED;
            summary->maskMergedRules++;
        }
        if (plan.verifier.flags & HAO_RULE_PLAN_FLAG_MASK_MERGE_FAILED) {
            plan.flags |= HAO_RULE_PLAN_FLAG_MASK_MERGE_FAILED;
            plan.category = HAORuleCategory::HAO_RULE_UNSUPPORTED;
            summary->maskConfirmRules++;
            summary->maskConflictRules++;
        }

        summary->maxSelectedAmbigBits =
            std::max(summary->maxSelectedAmbigBits,
                     plan.keyExpansion.selectedAmbigBits);

        if (plan.category == HAORuleCategory::HAO_RULE_UNSUPPORTED) {
            summary->unsupportedRules++;
            plan.keyExpansion.expandedKeys.clear();
            plan.keyExpansion.expandedKeyCount = 0;
        }

        switch (plan.category) {
        case HAORuleCategory::HAO_RULE_EXACT:
            summary->exactRules++;
            break;
        case HAORuleCategory::HAO_RULE_NOCASE:
            summary->nocaseRules++;
            break;
        case HAORuleCategory::HAO_RULE_UNSUPPORTED:
            break;
        default:
            break;
        }

        if (plan.flags & HAO_RULE_PLAN_FLAG_KEY_EXPANDED) {
            summary->keyExpandedRules++;
        }
        if (plan.category != HAORuleCategory::HAO_RULE_UNSUPPORTED) {
            summary->fastPathRules++;
            summary->totalExpandedKeys += plan.keyExpansion.expandedKeyCount;
        }
        rulePlans->push_back(std::move(plan));
    }
}

static
void haoBuildDotPlans(
    const std::vector<hwlmLiteral> &lits,
    const std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> &dotVector,
    u64a dotInputMask, u32 keyBits,
    std::vector<HAOCompiledRulePlan> *rulePlans, HAOCompileSummary *summary) {
    std::vector<HAOLiteralRef> refs;
    refs.reserve(lits.size());
    for (u32 i = 0; i < lits.size(); i++) {
        HAOLiteralRef ref;
        ref.lit = &lits[i];
        ref.ruleIndex = i;
        refs.push_back(ref);
    }
    haoBuildDotPlansFromRefs(refs, dotVector, dotInputMask, keyBits,
                             rulePlans, summary);
}

static
u64a haoMaskFromSelectors(const std::vector<HAOBitSelector> &selectors) {
    u64a mask = 0;
    for (const auto &selector : selectors) {
        mask |= 1ULL << haoSelectorBitIndex(selector);
    }
    return mask;
}

static
bool haoDotMaskBuilds(const std::vector<hwlmLiteral> &lits,
                      const std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES>
                          &dotVector,
                      u64a dotInputMask) {
    std::vector<HAOCompiledRulePlan> plans;
    HAOCompileSummary summary;
    const auto activeVector =
        haoApplyDotInputMaskToVector(dotVector, dotInputMask);

    if (!haoDotVectorHasNonZeroLane(activeVector)) {
        return false;
    }

    haoBuildDotPlans(lits, activeVector, dotInputMask, HAO_LAYOUT_KEY_BITS,
                     &plans, &summary);
    return !summary.unsupportedRules &&
           summary.fastPathRules == summary.totalRules &&
           summary.maskRules == summary.maskMergedRules;
}

static
bool haoDryRunHashStats(const std::vector<HAOCompiledRulePlan> &rulePlans,
                        u32 keyBits, HAOHashStats *stats, u32 *flags);

static
double haoDotHashChoiceCost(const HAOCompileSummary &summary,
                            const HAOHashStats &stats, u32 keyBits,
                            u32 flags);

static
double haoDotInputMaskCost(
    const std::vector<hwlmLiteral> &lits,
    const std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> &dotVector,
    u64a dotInputMask) {
    std::vector<HAOCompiledRulePlan> plans;
    HAOCompileSummary summary;
    HAOHashStats stats;
    u32 flags = 0;
    const auto activeVector =
        haoApplyDotInputMaskToVector(dotVector, dotInputMask);

    if (!haoDotVectorHasNonZeroLane(activeVector)) {
        return std::numeric_limits<double>::infinity();
    }
    haoBuildDotPlans(lits, activeVector, dotInputMask, HAO_LAYOUT_KEY_BITS,
                     &plans, &summary);
    if (summary.unsupportedRules ||
        summary.fastPathRules != summary.totalRules ||
        summary.maskRules != summary.maskMergedRules) {
        return std::numeric_limits<double>::infinity();
    }
    if (!haoDryRunHashStats(plans, HAO_LAYOUT_KEY_BITS, &stats, &flags)) {
        return std::numeric_limits<double>::infinity();
    }

    return haoDotHashChoiceCost(summary, stats, HAO_LAYOUT_KEY_BITS, flags);
}

static
u32 haoMinLiteralLen(const std::vector<hwlmLiteral> &lits) {
    u32 minLen = HAO_LAYOUT_BYTES_PER_RULE_SLOT;

    if (lits.empty()) {
        return 0;
    }
    for (const auto &lit : lits) {
        minLen = std::min(minLen, verify_u32(lit.s.size()));
    }
    return minLen;
}

static
bool haoDotUseSuffixMaskSeed(const std::vector<hwlmLiteral> &lits) {
    const u32 minLen = haoMinLiteralLen(lits);

    return minLen == 1U;
}

static
bool haoDotBitInCommonSuffix(u32 bitIndex, u32 minLen) {
    if (!minLen || minLen > HAO_LAYOUT_BYTES_PER_RULE_SLOT) {
        return false;
    }
    const u32 byte = bitIndex / 8U;
    const u32 suffixSearchBytes = std::min<u32>(minLen + 1U, 2U);
    const u32 firstCommonSuffixByte =
        HAO_LAYOUT_BYTES_PER_RULE_SLOT - suffixSearchBytes;

    return byte >= firstCommonSuffixByte &&
           byte < HAO_LAYOUT_BYTES_PER_RULE_SLOT;
}

static
u64a haoChooseDotInputMask(
    const std::vector<hwlmLiteral> &lits,
    const std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> &dotVector,
    u32 maxBits = HAO_DOT_INPUT_MASK_MAX_BITS) {
    std::vector<HAOBitSelector> selectors;
    u32 keyBits = 0;
    const u32 minLen = haoMinLiteralLen(lits);
    const bool suffixSeed = haoDotUseSuffixMaskSeed(lits);
    const u32 suffixSearchBytes =
        suffixSeed ? std::min<u32>(minLen + 1U, 2U) : minLen;
    const u32 firstCommonSuffixByte =
        minLen && minLen <= HAO_LAYOUT_BYTES_PER_RULE_SLOT
            ? HAO_LAYOUT_BYTES_PER_RULE_SLOT - suffixSearchBytes
            : HAO_LAYOUT_BYTES_PER_RULE_SLOT;

    u64a mask = 0;
    if (!suffixSeed) {
        haoSelectBits(lits, &selectors, &keyBits, HAOSelectorMode::HASH_OPT,
                      HAO_LAYOUT_KEY_BITS);
        mask = haoMaskFromSelectors(selectors);
        if (!mask) {
            return ~0ULL;
        }
    }

    auto candidates = buildBitCandidates(lits);
    std::sort(candidates.begin(), candidates.end(),
              [suffixSeed, firstCommonSuffixByte]
              (const HAOBitCandidate &a, const HAOBitCandidate &b) {
                  if (suffixSeed) {
                      const u32 byteA = a.bitIndex / 8U;
                      const u32 byteB = b.bitIndex / 8U;
                      const bool suffixA = byteA >= firstCommonSuffixByte;
                      const bool suffixB = byteB >= firstCommonSuffixByte;
                      if (suffixA != suffixB) {
                          return suffixA;
                      }
                  }
                  if (a.score != b.score) {
                      return a.score > b.score;
                  }
                  if (a.fixedCount != b.fixedCount) {
                      return a.fixedCount > b.fixedCount;
                  }
                  if (a.ambiguousCount != b.ambiguousCount) {
                      return a.ambiguousCount < b.ambiguousCount;
                  }
                  return a.bitIndex > b.bitIndex;
              });
    if (suffixSeed) {
        candidates.erase(
            std::remove_if(candidates.begin(), candidates.end(),
                           [minLen](const HAOBitCandidate &cand) {
                               return !haoDotBitInCommonSuffix(cand.bitIndex,
                                                               minLen);
                           }),
            candidates.end());
    }
    if (candidates.size() > HAO_DOT_INPUT_MASK_GREEDY_CANDIDATES) {
        candidates.resize(HAO_DOT_INPUT_MASK_GREEDY_CANDIDATES);
    }

    double currentCost = haoDotInputMaskCost(lits, dotVector, mask);

    while (popcount64(mask) < maxBits) {
        u64a bestMask = mask;
        double bestCost = currentCost;
        bool found = false;

        if (!std::isfinite(currentCost) &&
            haoDotVectorHasNonZeroLane(
                haoApplyDotInputMaskToVector(dotVector, mask)) &&
            haoDotMaskBuilds(lits, dotVector, mask)) {
            currentCost = 0.0;
            bestCost = currentCost;
        }

        if (popcount64(mask) >= maxBits) {
            break;
        }
        for (const auto &cand : candidates) {
            if (mask & (1ULL << cand.bitIndex)) {
                continue;
            }
            if (cand.entropy <= 0.0 || cand.fixedRatio < 0.50) {
                continue;
            }

            const u64a trial = mask | (1ULL << cand.bitIndex);
            const double cost = haoDotInputMaskCost(lits, dotVector, trial);
            if (!std::isfinite(cost)) {
                continue;
            }
            if (!found || cost < bestCost ||
                (!std::isfinite(bestCost) && std::isfinite(cost))) {
                bestMask = trial;
                bestCost = cost;
                found = true;
            }
        }
        if (!found || (std::isfinite(currentCost) &&
                       bestCost >= currentCost)) {
            break;
        }
        mask = bestMask;
        currentCost = bestCost;
    }

    return mask;
}

static
void haoInitL2(HAOL2Check *check, HAOL2Meta *meta) {
    assert(check);
    assert(meta);

    for (u32 slot = 0; slot < HAO_LAYOUT_RULE_SLOTS_PER_ENTRY; slot++) {
        check->rule[slot] = 1U;
        check->mask[slot] = 0U;
        meta->ruleIndex[slot] = HAO_INVALID_RULE_INDEX;
    }
    meta->careBits = 0;
}

/* Write one HAO verifier fragment into one compact SVE L2 slot. */
static
void haoAddL2Slot(const HAOCompiledRulePlan &plan, u32 localSlot,
                           HAOL2Check *check, HAOL2Meta *meta) {
    const u8 careMask = plan.verifier.careByteMask;

    assert(check);
    assert(meta);
    assert(localSlot < HAO_LAYOUT_RULE_SLOTS_PER_ENTRY);
    assert(careMask);

    meta->ruleIndex[localSlot] = plan.ruleIndex;
    for (u32 i = 0; i < HAO_LAYOUT_BYTES_PER_RULE_SLOT; i++) {
        if (!(careMask & (1U << i))) {
            continue;
        }
        meta->careBits |=
            1U << (localSlot * HAO_LAYOUT_BYTES_PER_RULE_SLOT + i);
    }

    check->rule[localSlot] = plan.verifier.ruleWord;
    check->mask[localSlot] = plan.verifier.maskWord;
}

/* Build the HAO global single-table hash. Expanded keys go straight into the
 * global key space instead of being grouped by mask class. */
static
void haoBuildTables(const std::vector<HAOCompiledRulePlan> &rulePlans,
                              u32 keyBits, HAOHashBuild *out) {
    if (!out) {
        return;
    }

    out->valid = false;
    out->flags = 0;
    out->keyBits = keyBits;
    out->primary.offsets.clear();
    out->bitmap.bits.clear();
    out->l2Check.clear();
    out->l2Meta.clear();
    out->stats = {};

    if (!keyBits) {
        return;
    }

    const u32 primaryCount = haoPrimaryCountForKeyBits(keyBits);
    out->primary.offsets.assign(primaryCount, 0);

    // Keep L2[0] empty so runtime null-target handling stays unchanged.
    {
        HAOL2Check check = {};
        HAOL2Meta meta = {};

        haoInitL2(&check, &meta);
        out->l2Check.push_back(check);
        out->l2Meta.push_back(meta);
    }

    std::map<u32, std::vector<u32>> keyToRuleIndexes;
    std::map<u32, const HAOCompiledRulePlan *> ruleIndexToPlan;
    for (const auto &plan : rulePlans) {
        if (plan.category == HAORuleCategory::HAO_RULE_UNSUPPORTED) {
            out->flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
            return;
        }
        ruleIndexToPlan[plan.ruleIndex] = &plan;
        for (const auto &expanded : plan.keyExpansion.expandedKeys) {
            keyToRuleIndexes[expanded.keyValue].push_back(plan.ruleIndex);
            out->stats.totalExpandedKeysInBuckets++;
        }
    }

    for (const auto &it : keyToRuleIndexes) {
        const u32 key = it.first;
        const auto &bucketRules = it.second;
        const u32 ruleCount = verify_u32(bucketRules.size());
        const u32 entryCount = verify_u32(
            (bucketRules.size() + HAO_LAYOUT_RULE_SLOTS_PER_ENTRY - 1) /
            HAO_LAYOUT_RULE_SLOTS_PER_ENTRY);

        if (out->l2Check.size() + entryCount >
            HAO_BUILD_MAX_L2_ENTRIES) {
            out->flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
            out->flags |= HAO_ARTIFACT_FLAG_PARTIAL_L2_CAPACITY;
            return;
        }
        if (entryCount >= (1U << (32U - HAO_LAYOUT_L1_COUNT_SHIFT))) {
            out->flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
            out->flags |= HAO_ARTIFACT_FLAG_PARTIAL_ENTRY_OVERFLOW;
            return;
        }

        const u32 l2Offset = verify_u32(out->l2Check.size());
        out->primary.offsets[key] =
            encodePrimaryValue(l2Offset, entryCount);
        out->stats.nonEmptyPrimary++;
        out->stats.totalRulesInBuckets += ruleCount;
        out->stats.totalL2Entries += entryCount;
        if (!out->stats.minRulesPerBucket || ruleCount < out->stats.minRulesPerBucket) {
            out->stats.minRulesPerBucket = ruleCount;
        }
        out->stats.maxRulesPerBucket =
            std::max(out->stats.maxRulesPerBucket, ruleCount);
        if (!out->stats.minEntriesPerBucket || entryCount < out->stats.minEntriesPerBucket) {
            out->stats.minEntriesPerBucket = entryCount;
        }
        out->stats.maxEntriesPerKey =
            std::max(out->stats.maxEntriesPerKey, entryCount);
        if (ruleCount > 1) {
            out->stats.collisionBuckets++;
        }
        if (ruleCount == 1) {
            out->stats.ruleBucketsEq1++;
        } else if (ruleCount <= 4) {
            out->stats.ruleBuckets2To4++;
        } else {
            out->stats.ruleBucketsGt4++;
        }
        if (entryCount == 1) {
            out->stats.entryBucketsEq1++;
        } else if (entryCount <= 4) {
            out->stats.entryBuckets2To4++;
        } else {
            out->stats.entryBucketsGt4++;
        }

        for (u32 chunk = 0; chunk < entryCount; chunk++) {
            HAOL2Check l2Check = {};
            HAOL2Meta l2Meta = {};
            const size_t begin = chunk * HAO_LAYOUT_RULE_SLOTS_PER_ENTRY;
            const size_t end = std::min(bucketRules.size(),
                                        begin + HAO_LAYOUT_RULE_SLOTS_PER_ENTRY);

            haoInitL2(&l2Check, &l2Meta);
            for (size_t slot = begin; slot < end; slot++) {
                const u32 localSlot = verify_u32(slot - begin);
                const u32 ruleIndex = bucketRules[slot];
                const auto planIt = ruleIndexToPlan.find(ruleIndex);
                assert(planIt != ruleIndexToPlan.end());
                haoAddL2Slot(*planIt->second, localSlot, &l2Check, &l2Meta);
            }

            out->l2Check.push_back(l2Check);
            out->l2Meta.push_back(l2Meta);
        }
    }
    buildPrimaryBitmap(out->primary, &out->bitmap);
    out->valid = true;
}

static really_inline
u32 haoPrimaryOffset(u32 encoded) {
    return encoded & HAO_LAYOUT_L1_OFFSET_MASK;
}

static really_inline
u32 haoPrimaryCount(u32 encoded) {
    return encoded >> HAO_LAYOUT_L1_COUNT_SHIFT;
}

template <class Fn>
static
void haoForEachPrimaryL2Entry(const HAOHashBuild &hash, const Fn &fn) {
    for (const u32 encoded : hash.primary.offsets) {
        const u32 offset = haoPrimaryOffset(encoded);
        const u32 count = haoPrimaryCount(encoded);

        if (!offset || !count) {
            continue;
        }
        for (u32 n = 0; n < count; n++) {
            const u32 off = offset + n;
            if (off >= hash.l2Check.size() || off >= hash.l2Meta.size()) {
                break;
            }
            fn(off);
        }
    }
}

static
bool haoEntryBitStable(const HAOL2Check &check, const HAOL2Meta &meta,
                       u32 bit) {
    const u64a bitMask = 1ULL << bit;
    bool haveTag = false;
    u32 tag = 0;

    for (u32 slot = 0; slot < HAO_LAYOUT_RULE_SLOTS_PER_ENTRY; slot++) {
        if (meta.ruleIndex[slot] == HAO_INVALID_RULE_INDEX) {
            continue;
        }
        if ((check.mask[slot] & bitMask) != bitMask) {
            return false;
        }
        const u32 slotTag = check.rule[slot] & bitMask ? 1U : 0U;
        if (!haveTag) {
            tag = slotTag;
            haveTag = true;
        } else if (tag != slotTag) {
            return false;
        }
    }

    return haveTag;
}

static
bool haoEntryTagForMask(const HAOL2Check &check, const HAOL2Meta &meta,
                        u64a mask, u32 *tagOut) {
    bool haveTag = false;
    u32 tag = 0;

    assert(tagOut);
    if (!mask) {
        return false;
    }

    for (u32 slot = 0; slot < HAO_LAYOUT_RULE_SLOTS_PER_ENTRY; slot++) {
        if (meta.ruleIndex[slot] == HAO_INVALID_RULE_INDEX) {
            continue;
        }
        if ((check.mask[slot] & mask) != mask) {
            return false;
        }
        const u32 slotTag =
            (u32)pext64(check.rule[slot], mask) & HAO_L15_TAG_VALUE_MASK;
        if (!haveTag) {
            tag = slotTag;
            haveTag = true;
        } else if (tag != slotTag) {
            return false;
        }
    }

    if (!haveTag) {
        return false;
    }

    *tagOut = tag;
    return true;
}

static
bool haoBuildL15TagMaskTable(const HAOCompileArtifacts &artifacts,
                             std::vector<u64a> *masksOut,
                             u32 *maxOverlapOut) {
#if HAO_L15_TAG
    struct Candidate {
        u32 bit = 0;
        u32 score = 0;
        bool overlapsPrimary = false;
    };

    std::array<u32, HAO_LAYOUT_BYTES_PER_RULE_SLOT * 8U> scores = {};
    std::vector<Candidate> nonOverlap;
    std::vector<Candidate> overlap;

    assert(masksOut);
    assert(maxOverlapOut);
    masksOut->clear();
    *maxOverlapOut = 0;

    if (HAO_L15_TAG_BITS != 8U || artifacts.hashMode != HAO_LAYOUT_HASH_BEXT ||
        !artifacts.hash.valid || artifacts.hash.l2Check.size() !=
            artifacts.hash.l2Meta.size()) {
        return false;
    }

    haoForEachPrimaryL2Entry(artifacts.hash, [&](u32 offset) {
        const auto &check = artifacts.hash.l2Check[offset];
        const auto &meta = artifacts.hash.l2Meta[offset];
        for (u32 bit = 0; bit < scores.size(); bit++) {
            if (haoEntryBitStable(check, meta, bit)) {
                scores[bit]++;
            }
        }
    });

    for (u32 bit = 0; bit < scores.size(); bit++) {
        if (!scores[bit]) {
            continue;
        }
        Candidate c;
        c.bit = bit;
        c.score = scores[bit];
        c.overlapsPrimary = artifacts.bextMask & (1ULL << bit);
        (c.overlapsPrimary ? overlap : nonOverlap).push_back(c);
    }

    auto byScore = [](const Candidate &a, const Candidate &b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.bit < b.bit;
    };
    std::sort(nonOverlap.begin(), nonOverlap.end(), byScore);
    std::sort(overlap.begin(), overlap.end(), byScore);

    auto addMask = [&](u32 start) {
        u64a mask = 0;
        u32 selected = 0;
        u32 overlapBits = 0;

        for (u32 pass = 0; pass < 2 && selected < HAO_L15_TAG_BITS; pass++) {
            const u32 begin = pass ? 0 : start;
            const u32 end = pass ? start : verify_u32(nonOverlap.size());
            for (u32 i = begin; i < end && selected < HAO_L15_TAG_BITS; i++) {
                const u64a bit = 1ULL << nonOverlap[i].bit;
                if (mask & bit) {
                    continue;
                }
                mask |= bit;
                selected++;
            }
        }

        if (selected < HAO_L15_TAG_MIN_NEW_BITS) {
            return;
        }

        for (u32 i = 0; i < overlap.size() &&
                    selected < HAO_L15_TAG_BITS &&
                    overlapBits < HAO_L15_TAG_MAX_OVERLAP_BITS; i++) {
            const u64a bit = 1ULL << overlap[i].bit;
            if (mask & bit) {
                continue;
            }
            mask |= bit;
            selected++;
            overlapBits++;
        }

        if (selected != HAO_L15_TAG_BITS ||
            popcount64(mask & artifacts.bextMask) != overlapBits) {
            return;
        }
        if (std::find(masksOut->begin(), masksOut->end(), mask) !=
            masksOut->end()) {
            return;
        }
        masksOut->push_back(mask);
        *maxOverlapOut = std::max(*maxOverlapOut, overlapBits);
    };

    const u32 maxStarts = verify_u32(std::min<size_t>(
        nonOverlap.size(), HAO_L15_TAG_MAX_MASKS * 2U));
    for (u32 start = 0; start < maxStarts &&
                        masksOut->size() < HAO_L15_TAG_MAX_MASKS; start++) {
        addMask(start);
    }

    return !masksOut->empty();
#else
    (void)artifacts;
    (void)masksOut;
    (void)maxOverlapOut;
    return false;
#endif
}

static
void haoBuildL15Tags(HAOCompileArtifacts *artifacts) {
    u32 overlapBits = 0;
    u32 taggedEntries = 0;
    std::vector<u64a> masks;

    assert(artifacts);
    artifacts->l15TagMask = 0;
    artifacts->l15TagBits = 0;
    artifacts->l15TagOverlapBits = 0;
    artifacts->l15TaggedEntries = 0;
    artifacts->l15Tags.clear();
    artifacts->l15TagMasks.clear();

    if (!haoBuildL15TagMaskTable(*artifacts, &masks, &overlapBits)) {
        return;
    }

    std::vector<std::array<u32, HAO_L15_TAG_VALUE_MASK + 1U>> tagFreq(
        masks.size());
    for (auto &freq : tagFreq) {
        freq.fill(0);
    }
    haoForEachPrimaryL2Entry(artifacts->hash, [&](u32 offset) {
        for (u32 maskId = 0; maskId < masks.size(); maskId++) {
            u32 tag = 0;
            if (haoEntryTagForMask(artifacts->hash.l2Check[offset],
                                   artifacts->hash.l2Meta[offset],
                                   masks[maskId], &tag)) {
                tagFreq[maskId][tag & HAO_L15_TAG_VALUE_MASK]++;
            }
        }
    });

    artifacts->l15Tags.assign(artifacts->hash.l2Check.size(), 0);
    haoForEachPrimaryL2Entry(artifacts->hash, [&](u32 offset) {
        u32 bestMaskId = verify_u32(masks.size());
        u32 bestTag = 0;
        u32 bestFreq = ~0U;
        for (u32 maskId = 0; maskId < masks.size(); maskId++) {
            u32 tag = 0;
            if (!haoEntryTagForMask(artifacts->hash.l2Check[offset],
                                    artifacts->hash.l2Meta[offset],
                                    masks[maskId], &tag)) {
                continue;
            }

            const u32 freq = tagFreq[maskId][tag & HAO_L15_TAG_VALUE_MASK];
            if (freq < bestFreq) {
                bestFreq = freq;
                bestTag = tag;
                bestMaskId = maskId;
            }
        }

        if (bestMaskId >= masks.size()) {
            return;
        }

        artifacts->l15Tags[offset] =
            verify_u16(HAO_L15_TAG_VALID |
                       ((bestMaskId << HAO_L15_TAG_MASK_ID_SHIFT) &
                        HAO_L15_TAG_MASK_ID_MASK) |
                       (bestTag & HAO_L15_TAG_VALUE_MASK));
        taggedEntries++;
    });

    if (!taggedEntries) {
        artifacts->l15Tags.clear();
        return;
    }

    artifacts->l15TagMask = masks.front();
    artifacts->l15TagBits = HAO_L15_TAG_BITS;
    artifacts->l15TagOverlapBits = overlapBits;
    artifacts->l15TaggedEntries = taggedEntries;
    artifacts->l15TagMasks = std::move(masks);
}

// Build the BEXT mask used by the runtime extractor.
template <class ArtifactsT>
static
void haoBuildExtract(const std::vector<HAOBitSelector> &selectors,
                            ArtifactsT *artifacts) {
    if (!artifacts) {
        return;
    }

    artifacts->bextMask = 0;

    if (selectors.empty() || selectors.size() > HAO_LAYOUT_MAX_SELECTORS) {
        return;
    }
    
    for (const auto &selector : selectors) {
        artifacts->bextMask |= (1ULL << haoSelectorBitIndex(selector));
    }
}

static
void haoPrimaryBitmapSet(std::vector<u8> *bitmap, u32 idx) {
    if (!bitmap || idx / 8U >= bitmap->size()) {
        return;
    }
    (*bitmap)[idx / 8U] |= verify_u8(1U << (idx % 8U));
}

static
void buildPrimaryBitmap(const HAOPrimaryHashTable &primaryHashTable,
                        HAOPrimaryHashBitmap *primaryHashBitmap) {
    if (!primaryHashBitmap) {
        return;
    }
    primaryHashBitmap->bits.clear();
    const u32 primaryCount = verify_u32(primaryHashTable.offsets.size());
    const u32 bitmapCount =
        haoPrimaryBitmapCountForPrimaryCount(primaryCount);
    primaryHashBitmap->bits.assign(haoPrimaryBitmapBytes(bitmapCount), 0);
    for (u32 i = 0; i < primaryCount; i++) {
        if (primaryHashTable.offsets[i]) {
            haoPrimaryBitmapSet(&primaryHashBitmap->bits,
                                 haoBitmapKeyForPrimaryKey(i));
        }
    }
}

static
void dumpRuleBits(const std::vector<hwlmLiteral> &lits) {
    printf("[HAO][Rules-Bits] rule_count=%zu\n", lits.size());
    for (size_t i = 0; i < lits.size(); i++) {
        const auto &lit = lits[i];
        printf("  r%zu id=%u s=\"%s\" len=%zu nocase=%u noruns=%u groups=0x%llx\n",
               i, lit.id, lit.s.c_str(), lit.s.size(), lit.nocase ? 1 : 0,
               lit.noruns ? 1 : 0, (unsigned long long)lit.groups);
        printf("    bytes(literal order): ");
        for (size_t j = 0; j < lit.s.size(); j++) {
            const u8 c = verify_u8(lit.s[j]);
            const char pc = std::isprint((unsigned char)c) ? (char)c : '.';
            printf("[%zu:'%c' 0x%02x %s]", j, pc, c, byteToBits(c).c_str());
            if (j + 1 != lit.s.size()) {
                printf(" ");
            }
        }
        printf("\n");
    }
}

static
void dumpSelectors(const std::vector<HAOBitSelector> &selectors) {
    printf("[HAO][Selectors] count=%zu\n", selectors.size());
    for (size_t i = 0; i < selectors.size(); i++) {
        const auto &s = selectors[i];
        const u32 bitIndex = haoSelectorBitIndex(s);
        printf("  s%zu -> raw_bit=%u (byteOffset=%u, bitOffset=%u)\n", i,
               bitIndex, (u32)s.byteOffset, (u32)s.bitOffset);
    }
}

template <class ArtifactsT>
static
void dumpExtractDescriptor(const ArtifactsT &artifacts) {
    printf("[HAO][Extract] mode=%s windowBytes=%u keyBits=%u",
           haoHashModeName(artifacts.hashMode),
           HAO_LAYOUT_BYTES_PER_RULE_SLOT, artifacts.hash.keyBits);
    if (artifacts.hashMode == HAO_LAYOUT_HASH_DOT) {
        printf(" dotVector=[%u,%u,%u,%u]\n",
               artifacts.dotVector[0], artifacts.dotVector[1],
               artifacts.dotVector[2], artifacts.dotVector[3]);
        printf("[HAO][Extract] dotInputMask=0x%016llx bits=%u\n",
               (unsigned long long)artifacts.dotInputMask,
               popcount64(artifacts.dotInputMask));
    } else if (artifacts.hashMode == HAO_LAYOUT_HASH_DOT_GROUP) {
        printf(" dotGroupCount=%zu\n", artifacts.dotGroups.size());
    } else {
        printf(" bextMask=0x%llx\n",
               (unsigned long long)artifacts.bextMask);
        if (artifacts.l15TagBits) {
            printf("[HAO][L1.5Tag] mask=0x%016llx bits=%u overlapBits=%u "
                   "taggedEntries=%u tableEntries=%zu maskCount=%zu\n",
                   (unsigned long long)artifacts.l15TagMask,
                   artifacts.l15TagBits, artifacts.l15TagOverlapBits,
                   artifacts.l15TaggedEntries, artifacts.l15Tags.size(),
                   artifacts.l15TagMasks.size());
        }
    }
}

static
void dumpRuleKeys(const std::vector<hwlmLiteral> &lits,
                  const std::vector<HAOBitSelector> &selectors,
                  u32 keyBits) {
    printf("[HAO][Rule->KeyMask] key_bits=%u\n", keyBits);
    for (size_t i = 0; i < lits.size(); i++) {
        u32 keyValue = 0;
        u32 keyMask = 0;
        haoLitKeyMask(lits[i], selectors, &keyValue, &keyMask);
        printf("  r%zu id=%u keyValue={dec=%u hex=0x%x bin=%s} keyMask={dec=%u hex=0x%x bin=%s}\n",
               i, lits[i].id, keyValue, keyValue,
               keyToBits(keyValue, keyBits).c_str(), keyMask, keyMask,
               keyToBits(keyMask, keyBits).c_str());
    }
}

static
std::string haoEscapeLiteral(const hwlmLiteral &lit) {
    std::string out;

    for (size_t i = 0; i < lit.s.size(); i++) {
        const unsigned char c = (unsigned char)lit.s[i];
        char buf[5];

        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (std::isprint(c)) {
                out += (char)c;
            } else {
                snprintf(buf, sizeof(buf), "\\x%02x", c);
                out += buf;
            }
            break;
        }
    }
    return out;
}

static
void dumpL2Map(const std::vector<hwlmLiteral> &lits,
               const HAOCompileArtifacts &artifacts) {
    if (artifacts.hashMode == HAO_LAYOUT_HASH_DOT_GROUP) {
        printf("[HAO][L2Map] hashMode=%s groups=%zu\n",
               haoHashModeName(artifacts.hashMode),
               artifacts.dotGroups.size());
        for (const auto &group : artifacts.dotGroups) {
            printf("[HAO][L2Map] group=%s entries=%zu keyBits=%u "
                   "dotVector=[%u,%u,%u,%u]\n",
                   group.name, group.hash.l2Meta.size(), group.hash.keyBits,
                   group.dotVector[0], group.dotVector[1],
                   group.dotVector[2], group.dotVector[3]);
            for (size_t entry = 1; entry < group.hash.l2Meta.size(); entry++) {
                const auto &meta = group.hash.l2Meta[entry];
                for (u32 slot = 0; slot < HAO_LAYOUT_RULE_SLOTS_PER_ENTRY;
                     slot++) {
                    const u32 ruleIndex = meta.ruleIndex[slot];
                    if (ruleIndex == HAO_INVALID_RULE_INDEX ||
                        ruleIndex >= lits.size()) {
                        continue;
                    }
                    const auto &lit = lits[ruleIndex];
                    const auto *ruleMeta = ruleIndex < artifacts.meta.size()
                                               ? &artifacts.meta[ruleIndex]
                                               : nullptr;
                    const u32 runtimeId = ruleMeta ? ruleMeta->id : lit.id;
                    const u32 flags = ruleMeta ? ruleMeta->flags : 0U;
                    printf("  group=%s entry=%zu slot=%u ruleIndex=%u "
                           "id=%u flags=0x%x len=%zu nocase=%u lit=\"%s\"\n",
                           group.name, entry, slot, ruleIndex, runtimeId,
                           flags, lit.s.size(), lit.nocase ? 1U : 0U,
                           haoEscapeLiteral(lit).c_str());
                }
            }
        }
        return;
    }

    printf("[HAO][L2Map] entries=%zu keyBits=%u hashMode=%s selectorCount=%zu bextMask=0x%016llx dotVector=[%u,%u,%u,%u]\n",
           artifacts.hash.l2Meta.size(), artifacts.hash.keyBits,
           haoHashModeName(artifacts.hashMode),
           artifacts.selectors.size(), (unsigned long long)artifacts.bextMask,
           artifacts.dotVector[0], artifacts.dotVector[1],
           artifacts.dotVector[2], artifacts.dotVector[3]);
    for (size_t entry = 1; entry < artifacts.hash.l2Meta.size(); entry++) {
        const auto &meta = artifacts.hash.l2Meta[entry];

        for (u32 slot = 0; slot < HAO_LAYOUT_RULE_SLOTS_PER_ENTRY; slot++) {
            const u32 ruleIndex = meta.ruleIndex[slot];

            if (ruleIndex == HAO_RUNTIME_INVALID_RULE_INDEX ||
                ruleIndex >= lits.size()) {
                continue;
            }

            const auto &lit = lits[ruleIndex];
            const auto *ruleMeta = ruleIndex < artifacts.meta.size()
                                       ? &artifacts.meta[ruleIndex]
                                       : nullptr;
            const u32 runtimeId = ruleMeta ? ruleMeta->id : lit.id;
            const u32 flags = ruleMeta ? ruleMeta->flags : 0U;
            printf("  entry=%zu slot=%u ruleIndex=%u id=%u runtimeId=%u litId=%u flags=0x%x len=%zu nocase=%u lit=\"%s\"\n",
                   entry, slot, ruleIndex, runtimeId, runtimeId, lit.id,
                   flags, lit.s.size(),
                   lit.nocase ? 1U : 0U,
                   haoEscapeLiteral(lit).c_str());
        }
    }
}

#define HAO_SUMMARY_FMT(label, fmt, val)                                  \
    do {                                                                   \
        int _blen = (int)strlen(label);                                    \
        int _cjk  = 0;                                                     \
        for (const char *_p = (label); *_p; ) {                           \
            unsigned char _c = (unsigned char)*_p;                         \
            if (_c >= 0xE0) { _cjk++; _p += 3; }                         \
            else if (_c >= 0xC0) { _p += 2; }                             \
            else { _p += 1; }                                              \
        }                                                                  \
        int _pad = 42 - (_blen - _cjk);                                   \
        if (_pad < 1) _pad = 1;                                            \
        printf("  %s%*s: " fmt "\n", label, _pad, "", val);               \
    } while(0)



static
double haoCompilePct(u64a num, u64a den) {
    if (!den) {
        return 0.0;
    }
    return (100.0 * (double)num) / (double)den;
}

static
int haoHexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static
bool haoParseDebugLiteral(const char *env, std::string *litOut) {
    if (!env || !litOut) {
        return false;
    }

    litOut->clear();
    for (const char *p = env; *p; p++) {
        if (*p != '\\') {
            litOut->push_back(*p);
            continue;
        }

        p++;
        switch (*p) {
        case '\0':
            litOut->push_back('\\');
            return true;
        case '\\':
            litOut->push_back('\\');
            break;
        case '"':
            litOut->push_back('"');
            break;
        case 'n':
            litOut->push_back('\n');
            break;
        case 'r':
            litOut->push_back('\r');
            break;
        case 't':
            litOut->push_back('\t');
            break;
        case 'x': {
            const int hi = haoHexValue(p[1]);
            const int lo = haoHexValue(p[2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            litOut->push_back((char)((hi << 4) | lo));
            p += 2;
            break;
        }
        default:
            litOut->push_back(*p);
            break;
        }
    }

    return true;
}

static
bool haoDebugPrimaryKey(u32 *keyOut) {
    const char *env = getenv("HS_HAO_DEBUG_KEY");
    char *end = nullptr;
    unsigned long long key;

    if (!env || !*env || !keyOut) {
        return false;
    }

    key = strtoull(env, &end, 0);
    if (end == env || *end || key > std::numeric_limits<u32>::max()) {
        printf("[HAO][PrimaryKey] invalid HS_HAO_DEBUG_KEY=%s\n", env);
        return false;
    }

    *keyOut = verify_u32(key);
    return true;
}

template <class ArtifactsT>
static
void haoDumpPrimaryKeyState(const ArtifactsT &artifacts, u32 key) {
    const bool inRange = key < artifacts.hash.primary.offsets.size();
    const u32 encoded = inRange ? artifacts.hash.primary.offsets[key] : 0U;
    const u32 offset = encoded & HAO_LAYOUT_L1_OFFSET_MASK;
    const u32 count = encoded >> HAO_LAYOUT_L1_COUNT_SHIFT;

    HAO_SUMMARY_FMT("key",          "%u", key);
    HAO_SUMMARY_FMT("inRange",      "%u", inRange ? 1U : 0U);
    HAO_SUMMARY_FMT("encoded",      "0x%08x", encoded);
    HAO_SUMMARY_FMT("isEmpty",      "%u", encoded ? 0U : 1U);
    HAO_SUMMARY_FMT("l2Offset",     "%u", offset);
    HAO_SUMMARY_FMT("l2Count",      "%u", count);
}

template <class ArtifactsT>
static
void haoDumpDebugLiteral(const ArtifactsT &artifacts) {
    const char *env = getenv("HS_HAO_DEBUG_LIT");
    if (!env || !*env) {
        return;
    }

    std::string litString;
    if (!haoParseDebugLiteral(env, &litString)) {
        printf("[HAO][LiteralKey] invalid HS_HAO_DEBUG_LIT=%s\n", env);
        return;
    }
    if (litString.size() > HAO_LAYOUT_BYTES_PER_RULE_SLOT) {
        printf("[HAO][LiteralKey] len=%zu exceeds HAO window bytes=%u\n",
               litString.size(), HAO_LAYOUT_BYTES_PER_RULE_SLOT);
        return;
    }

    const char *nocaseEnv = getenv("HS_HAO_DEBUG_LIT_NOCASE");
    const bool nocase = nocaseEnv && *nocaseEnv && strcmp(nocaseEnv, "0");
    const hwlmLiteral lit(litString, nocase, false, 0, HWLM_ALL_GROUPS,
                          {}, {});
    u32 keyValue = 0;
    u32 keyMask = 0;
    HAOKeyExpansionInfo expansion;
    if (artifacts.hashMode == HAO_LAYOUT_HASH_DOT_GROUP) {
        printf("[HAO][LiteralKey] HS_HAO_DEBUG_LIT is not supported for "
               "dot_group artifacts yet\n");
        return;
    }
    if (artifacts.hashMode == HAO_LAYOUT_HASH_DOT) {
        const HAORuleCategory category =
            nocase ? HAORuleCategory::HAO_RULE_NOCASE
                   : HAORuleCategory::HAO_RULE_EXACT;
        const HAOVerifierFragment verifier = haoBuildCheck(lit, category);
        expansion = haoExpandDotKeys(verifier, artifacts.dotVector,
                                     artifacts.dotInputMask,
                                     artifacts.hash.keyBits);
        if (!expansion.expandedKeys.empty()) {
            keyValue = expansion.expandedKeys.front().keyValue;
        }
    } else {
        haoLitKeyMask(lit, artifacts.selectors, &keyValue, &keyMask);
        expansion = haoExpandKeys(lit, artifacts.selectors);
    }

    u32 nonEmptyKeys = 0;
    for (const auto &expanded : expansion.expandedKeys) {
        const u32 key = expanded.keyValue;
        if (key < artifacts.hash.primary.offsets.size() &&
            artifacts.hash.primary.offsets[key]) {
            nonEmptyKeys++;
        }
    }

    printf("[HAO][LiteralKey]\n");
    HAO_SUMMARY_FMT("lit",                   "\"%s\"",
                    haoEscapeLiteral(lit).c_str());
    HAO_SUMMARY_FMT("len",                   "%u",
                    verify_u32(litString.size()));
    HAO_SUMMARY_FMT("nocase",                "%u", nocase ? 1U : 0U);
    HAO_SUMMARY_FMT("keyBits",               "%u",
                    artifacts.hash.keyBits);
    HAO_SUMMARY_FMT("hashMode",              "%s",
                    haoHashModeName(artifacts.hashMode));
    if (artifacts.hashMode == HAO_LAYOUT_HASH_DOT) {
        char vectorBuf[64];
        snprintf(vectorBuf, sizeof(vectorBuf), "[%u,%u,%u,%u]",
                 artifacts.dotVector[0], artifacts.dotVector[1],
                 artifacts.dotVector[2], artifacts.dotVector[3]);
        HAO_SUMMARY_FMT("dotVector",          "%s", vectorBuf);
        HAO_SUMMARY_FMT("dotInputMask",       "0x%016llx",
                        (unsigned long long)artifacts.dotInputMask);
        HAO_SUMMARY_FMT("dotInputMaskBits",   "%u",
                        popcount64(artifacts.dotInputMask));
        HAO_SUMMARY_FMT("firstKeyValue",      "%u", keyValue);
        HAO_SUMMARY_FMT("firstKeyValueHex",   "0x%x", keyValue);
    } else {
        HAO_SUMMARY_FMT("keyValue",           "%u", keyValue);
        HAO_SUMMARY_FMT("keyValueHex",        "0x%x", keyValue);
        HAO_SUMMARY_FMT("keyMask",            "0x%x", keyMask);
    }
    HAO_SUMMARY_FMT("selectedAmbigBits",     "%u",
                    expansion.selectedAmbigBits);
    HAO_SUMMARY_FMT("expandedKeyCount",       "%u",
                    expansion.expandedKeyCount);
    HAO_SUMMARY_FMT("nonEmptyExpandedKeys",   "%u", nonEmptyKeys);

    const u32 printLimit = 64U;
    u32 printed = 0;
    for (const auto &expanded : expansion.expandedKeys) {
        if (printed >= printLimit) {
            printf("[HAO][LiteralKeyPrimary] omitted remaining %u keys\n",
                   expansion.expandedKeyCount - printed);
            break;
        }
        printf("[HAO][LiteralKeyPrimary] variant=%u ambiguousMask=0x%x\n",
               expanded.variantIndex, expanded.ambiguousSelectorMask);
        haoDumpPrimaryKeyState(artifacts, expanded.keyValue);
        printed++;
    }
}

template <class ArtifactsT>
static
void dumpHAOSummary(const ArtifactsT &artifacts) {
    const auto &s = artifacts.summary;
    const auto &h = artifacts.hash.stats;
    const double avgRulesPerBucket = h.nonEmptyPrimary
                                     ? (double)h.totalRulesInBuckets /
                                           (double)h.nonEmptyPrimary
                                     : 0.0;
    const double avgEntriesPerBucket = h.nonEmptyPrimary
                                       ? (double)h.totalL2Entries /
                                             (double)h.nonEmptyPrimary
                                       : 0.0;
    const double avgLiteralLen = s.totalRules
                                     ? (double)s.totalLiteralBytes /
                                           (double)s.totalRules
                                     : 0.0;

    printf("[HAO][Summary]\n");
    HAO_SUMMARY_FMT("total",                  "%u", s.totalRules);
    HAO_SUMMARY_FMT("fastPath",               "%u", s.fastPathRules);
    HAO_SUMMARY_FMT("unsupported",            "%u", s.unsupportedRules);
    HAO_SUMMARY_FMT("exact",                  "%u", s.exactRules);
    HAO_SUMMARY_FMT("nocase",                 "%u", s.nocaseRules);
    HAO_SUMMARY_FMT("maskRules",              "%u", s.maskRules);
    HAO_SUMMARY_FMT("maskMerged",             "%u", s.maskMergedRules);
    HAO_SUMMARY_FMT("maskConflict",           "%u", s.maskConflictRules);
    HAO_SUMMARY_FMT("maskConfirm",            "%u", s.maskConfirmRules);
    HAO_SUMMARY_FMT("keyExpanded",            "%u", s.keyExpandedRules);
    HAO_SUMMARY_FMT("expandedKeys",           "%u", s.totalExpandedKeys);
    HAO_SUMMARY_FMT("maxSelectedAmbigBits",   "%u", s.maxSelectedAmbigBits);

    printf("[HAO][Extract]\n");
    HAO_SUMMARY_FMT("extractMode",            "%s",
                    haoHashModeName(artifacts.hashMode));
    HAO_SUMMARY_FMT("windowBytes",            "%u",
                    HAO_LAYOUT_BYTES_PER_RULE_SLOT);
    if (artifacts.hashMode == HAO_LAYOUT_HASH_DOT) {
        char vectorBuf[64];
        snprintf(vectorBuf, sizeof(vectorBuf), "[%u,%u,%u,%u]",
                 artifacts.dotVector[0], artifacts.dotVector[1],
                 artifacts.dotVector[2], artifacts.dotVector[3]);
        HAO_SUMMARY_FMT("dotVector",           "%s", vectorBuf);
        HAO_SUMMARY_FMT("dotInputMask",        "0x%016llx",
                        (unsigned long long)artifacts.dotInputMask);
        HAO_SUMMARY_FMT("dotInputMaskBits",    "%u",
                        popcount64(artifacts.dotInputMask));
    } else if (artifacts.hashMode == HAO_LAYOUT_HASH_DOT_GROUP) {
        HAO_SUMMARY_FMT("dotGroupCount",        "%zu",
                        artifacts.dotGroups.size());
    } else {
        HAO_SUMMARY_FMT("bextMask(runtime)",   "0x%016llx",
                        (unsigned long long)artifacts.bextMask);
#if HAO_COMPRESSED_BITMAP
        HAO_SUMMARY_FMT("primaryBitmapMode",   "%s",
                        "compressed-direct");
        HAO_SUMMARY_FMT("primaryBitmapShift",  "%u",
                        HAO_COMPRESSED_BITMAP_SHIFT);
#else
        HAO_SUMMARY_FMT("primaryBitmapMode",   "%s",
                        "full");
#endif
        HAO_SUMMARY_FMT("primaryBitmapBytes",  "%zu",
                        artifacts.hash.bitmap.bits.size());
        HAO_SUMMARY_FMT("l15TagMask",          "0x%016llx",
                        (unsigned long long)artifacts.l15TagMask);
        HAO_SUMMARY_FMT("l15TagBits",          "%u",
                        artifacts.l15TagBits);
        HAO_SUMMARY_FMT("l15TagOverlapBits",   "%u",
                        artifacts.l15TagOverlapBits);
        HAO_SUMMARY_FMT("l15TagEntries",       "%u",
                        artifacts.l15TaggedEntries);
        HAO_SUMMARY_FMT("l15MaskCount",        "%zu",
                        artifacts.l15TagMasks.size());
    }
    HAO_SUMMARY_FMT("keyBits", "%u", artifacts.hash.keyBits);
    HAO_SUMMARY_FMT("selectorCount", "%zu", artifacts.selectors.size());

    printf("[HAO][Length]\n");
    HAO_SUMMARY_FMT("minLiteralLen",          "%u", s.minLiteralLen);
    HAO_SUMMARY_FMT("maxLiteralLen",          "%u", s.maxLiteralLen);
    HAO_SUMMARY_FMT("avgLiteralLen",          "%.5f", avgLiteralLen);
    HAO_SUMMARY_FMT("literalLenLe4",          "%u", s.literalLenLe4);
    HAO_SUMMARY_FMT("literalLen5To8",         "%u", s.literalLen5To8);

    {
        u32 debugKey = 0;
        if (haoDebugPrimaryKey(&debugKey)) {
            printf("[HAO][PrimaryKey]\n");
            haoDumpPrimaryKeyState(artifacts, debugKey);
        }
    }
    haoDumpDebugLiteral(artifacts);

    if (!artifacts.hash.valid || !h.nonEmptyPrimary) {
        return;
    }

    if (artifacts.hashMode == HAO_LAYOUT_HASH_DOT_GROUP) {
        printf("[HAO][DotGroup]\n");
        printf("  group knownBytes keyBits rules nonEmptyPrimary "
               "collisionPct avgRules maxRules avgEntries maxEntries "
               "entryGt4Pct dotVector\n");
        for (const auto &group : artifacts.dotGroups) {
            const auto &gh = group.hash.stats;
            const double groupAvgRules = gh.nonEmptyPrimary
                                             ? (double)gh.totalRulesInBuckets /
                                                   (double)gh.nonEmptyPrimary
                                             : 0.0;
            const double groupAvgEntries = gh.nonEmptyPrimary
                                               ? (double)gh.totalL2Entries /
                                                     (double)gh.nonEmptyPrimary
                                               : 0.0;
            printf("  %-5s %10u %7u %5u %15u %12.5f %8.5f %8u "
                   "%10.5f %10u %11.5f [%u,%u,%u,%u]\n",
                   group.name, group.knownBytes, group.hash.keyBits,
                   group.summary.totalRules, gh.nonEmptyPrimary,
                   haoCompilePct(gh.collisionBuckets, gh.nonEmptyPrimary),
                   groupAvgRules, gh.maxRulesPerBucket, groupAvgEntries,
                   gh.maxEntriesPerKey,
                   haoCompilePct(gh.entryBucketsGt4, gh.nonEmptyPrimary),
                   group.dotVector[0], group.dotVector[1],
                   group.dotVector[2], group.dotVector[3]);
        }
    }

    printf("[HAO][Hash]\n");
    HAO_SUMMARY_FMT("nonEmptyPrimary",        "%u", h.nonEmptyPrimary);
    HAO_SUMMARY_FMT("collisionBuckets",       "%u", h.collisionBuckets);
    HAO_SUMMARY_FMT("collisionBucketPct",     "%.5f",
                    haoCompilePct(h.collisionBuckets, h.nonEmptyPrimary));
    HAO_SUMMARY_FMT("avgRulesPerBucket",      "%.5f", avgRulesPerBucket);
    HAO_SUMMARY_FMT("minRulesPerBucket",      "%u", h.minRulesPerBucket);
    HAO_SUMMARY_FMT("maxRulesPerBucket",      "%u", h.maxRulesPerBucket);
    HAO_SUMMARY_FMT("ruleBucketsEq1",         "%u", h.ruleBucketsEq1);
    HAO_SUMMARY_FMT("ruleBucketsEq1Pct",      "%.5f",
                    haoCompilePct(h.ruleBucketsEq1, h.nonEmptyPrimary));
    HAO_SUMMARY_FMT("ruleBuckets2To4",        "%u", h.ruleBuckets2To4);
    HAO_SUMMARY_FMT("ruleBuckets2To4Pct",     "%.5f",
                    haoCompilePct(h.ruleBuckets2To4, h.nonEmptyPrimary));
    HAO_SUMMARY_FMT("ruleBucketsGt4",         "%u", h.ruleBucketsGt4);
    HAO_SUMMARY_FMT("ruleBucketsGt4Pct",      "%.5f",
                    haoCompilePct(h.ruleBucketsGt4, h.nonEmptyPrimary));
    HAO_SUMMARY_FMT("avgEntriesPerBucket",    "%.5f", avgEntriesPerBucket);
    HAO_SUMMARY_FMT("minEntriesPerBucket",    "%u", h.minEntriesPerBucket);
    HAO_SUMMARY_FMT("maxEntriesPerBucket",    "%u", h.maxEntriesPerKey);
    HAO_SUMMARY_FMT("entryBucketsEq1",        "%u", h.entryBucketsEq1);
    HAO_SUMMARY_FMT("entryBucketsEq1Pct",     "%.5f",
                    haoCompilePct(h.entryBucketsEq1, h.nonEmptyPrimary));
    HAO_SUMMARY_FMT("entryBuckets2To4",       "%u", h.entryBuckets2To4);
    HAO_SUMMARY_FMT("entryBuckets2To4Pct",    "%.5f",
                    haoCompilePct(h.entryBuckets2To4, h.nonEmptyPrimary));
    HAO_SUMMARY_FMT("entryBucketsGt4",        "%u", h.entryBucketsGt4);
    HAO_SUMMARY_FMT("entryBucketsGt4Pct",     "%.5f",
                    haoCompilePct(h.entryBucketsGt4, h.nonEmptyPrimary));
}

static
bool normalizeMaskCmp(const hwlmLiteral &lit, std::array<u8, 8> *mskOut,
                      std::array<u8, 8> *cmpOut, u8 *lenOut) {
    if (!mskOut || !cmpOut || !lenOut) {
        return false;
    }
    mskOut->fill(0);
    cmpOut->fill(0);
    *lenOut = 0;

    const size_t mlen = lit.msk.size();
    const size_t clen = lit.cmp.size();

    if (!mlen && !clen) {
        return true;
    }

    // Normalize to a common tail window:
    // 1) msk-only => cmp defaults to 0.
    // 2) cmp-only => msk defaults to 0xff.
    // 3) unequal lengths => missing msk defaults to 0xff, missing cmp to 0.
    // This preserves a deterministic `(byte & msk) == cmp` semantics.
    const size_t useLen = std::min<size_t>(std::max(mlen, clen), mskOut->size());
    *lenOut = verify_u8(useLen);
    for (size_t i = 0; i < useLen; i++) {
        (*mskOut)[i] = (i < mlen) ? lit.msk[i] : 0xff;
        (*cmpOut)[i] = (i < clen) ? lit.cmp[i] : 0;
    }
    return true;
}

static
void packMaskCmpTail(const std::array<u8, 8> &msk,
                     const std::array<u8, 8> &cmp, u8 len,
                     u64a *maskWord, u64a *cmpWord) {
    assert(maskWord);
    assert(cmpWord);
    assert(len <= HAO_LAYOUT_BYTES_PER_RULE_SLOT);

    *maskWord = 0;
    *cmpWord = 0;
    const u32 laneStart = HAO_LAYOUT_BYTES_PER_RULE_SLOT - len;
    for (u32 i = 0; i < len; i++) {
        const u32 laneByte = laneStart + i;
        *maskWord |= (u64a)msk[i] << (laneByte * 8U);
        *cmpWord |= (u64a)cmp[i] << (laneByte * 8U);
    }
}

static
bool getBitState(const hwlmLiteral &lit, u32 bitIndex, u8 *state) {
    if (!state) {
        return false;
    }

    const u32 rawByte = bitIndex / 8;
    const u32 bitInByte = bitIndex % 8;
    const u32 len = verify_u32(lit.s.size());

    bool care = false;
    bool value = false;

    if (rawByte < HAO_LAYOUT_BYTES_PER_RULE_SLOT &&
        rawByte + len >= HAO_LAYOUT_BYTES_PER_RULE_SLOT) {
        const u32 litIdx = rawByte + len - HAO_LAYOUT_BYTES_PER_RULE_SLOT;
        const u8 c = verify_u8(lit.s[litIdx]);
        care = true;
        value = !!(c & (1U << bitInByte));
        if (lit.nocase && ourisalpha(c) && bitInByte == 5) {
            care = false;
        }
    }
    std::array<u8, 8> normMsk = {};
    std::array<u8, 8> normCmp = {};
    u8 normLen = 0;
    if (normalizeMaskCmp(lit, &normMsk, &normCmp, &normLen) && normLen) {
        const u32 mlen = normLen;
        if (rawByte + mlen >= HAO_LAYOUT_BYTES_PER_RULE_SLOT) {
            const u32 maskIdx =
                rawByte + mlen - HAO_LAYOUT_BYTES_PER_RULE_SLOT;
            const u8 m = normMsk[maskIdx];
            if (m & (1U << bitInByte)) {
                const u8 v = normCmp[maskIdx];
                care = true;
                value = !!(v & (1U << bitInByte));
            }
        }
    }
    *state = care ? (value ? 1 : 0) : HAO_BUILD_STATE_DONT_CARE;
    return true;
}

// Computes an entropy score for one bit position from its 0/1 distribution
// across all literals.
static
double entropyScore(u32 zeros, u32 ones) {
    const u32 total = zeros + ones;
    if (!total || !zeros || !ones) {
        return 0.0;
    }

    const double p0 = static_cast<double>(zeros) / total;
    const double p1 = static_cast<double>(ones) / total;
    return -(p0 * std::log2(p0) + p1 * std::log2(p1));
}

// Hashes one bit-state distribution so selector choice can avoid picking
// multiple columns with identical features.
static
u64a signatureOfStates(const std::vector<u8> &states) {
    // FNV-1a 64-bit
    u64a h = 1469598103934665603ULL;
    for (u8 v : states) {
        h ^= static_cast<u64a>(v + 1U);
        h *= 1099511628211ULL;
    }
    return h;
}

static
bool haoCandFits(const HAOBitCandidate &cand,
                                 const std::vector<u8> &selectedAmbigs) {
    assert(cand.states.size() == selectedAmbigs.size());
    for (u32 i = 0; i < cand.states.size(); i++) {
        if (cand.states[i] != HAO_BUILD_STATE_DONT_CARE) {
            continue;
        }
        if (selectedAmbigs[i] >= HAO_MAX_KEY_AMBIG_BITS) {
            return false;
        }
    }
    return true;
}

static
void haoCandApply(const HAOBitCandidate &cand,
                                  std::vector<u8> *selectedAmbigs) {
    assert(selectedAmbigs);
    assert(cand.states.size() == selectedAmbigs->size());
    for (u32 i = 0; i < cand.states.size(); i++) {
        if (cand.states[i] == HAO_BUILD_STATE_DONT_CARE) {
            (*selectedAmbigs)[i]++;
        }
    }
}

static
double haoCandExpansionCost(const HAOBitCandidate &cand,
                            const std::vector<u8> &selectedAmbigs) {
    assert(cand.states.size() == selectedAmbigs.size());

    double cost = 0.0;
    for (u32 i = 0; i < cand.states.size(); i++) {
        if (cand.states[i] != HAO_BUILD_STATE_DONT_CARE) {
            continue;
        }
        cost += std::ldexp(1.0, selectedAmbigs[i]);
    }

    return cost / std::max<size_t>(1, cand.states.size());
}

static
double haoCandDynamicScore(const HAOBitCandidate &cand,
                           const std::vector<u8> &selectedAmbigs) {
    const double expansionCost =
        std::log2(1.0 + haoCandExpansionCost(cand, selectedAmbigs));

    // Start from the original fixed/entropy preference, but penalize bits
    // that would amplify the current key expansion frontier.
    return cand.fixedRatio * 0.70 +
           cand.entropy * 0.25 -
           expansionCost * 0.18 -
           cand.ambiguousRatio * 0.10;
}

static
std::vector<HAOBitCandidate> buildBitCandidates(
    const std::vector<hwlmLiteral> &lits) {
    std::vector<HAOBitCandidate> out;
    out.reserve(HAO_BUILD_MAX_CANDIDATE_BITS);
    // Collect per-bit state distributions across the candidate 64-bit window.
    for (u32 bit = 0; bit < HAO_BUILD_MAX_CANDIDATE_BITS; bit++) {
        HAOBitCandidate c;
        c.bitIndex = bit;
        c.states.reserve(lits.size());

        u32 fixedCount = 0;
        u32 ambiguousCount = 0;
        u32 zeros = 0;
        u32 ones = 0;

        for (const auto &lit : lits) {
            u8 state = HAO_BUILD_STATE_DONT_CARE;
            getBitState(lit, bit, &state); // 0, 1, or don't care.
            c.states.push_back(state);
            if (state == HAO_BUILD_STATE_DONT_CARE) {
                ambiguousCount++;
                continue;
            }
            fixedCount++;
            if (state) {
                ones++;
            } else {
                zeros++;
            }
        }
        // Bits with no fixed contribution cannot improve the primary key.
        if (!fixedCount) {
            continue;
        }
        c.fixedCount = fixedCount;
        c.ambiguousCount = ambiguousCount;
        c.careRatio =
            static_cast<double>(fixedCount) / std::max<size_t>(1, lits.size());
        c.fixedRatio =
            static_cast<double>(fixedCount) / std::max<size_t>(1, lits.size());
        c.ambiguousRatio =
            static_cast<double>(ambiguousCount) /
            std::max<size_t>(1, lits.size());
        c.entropy = entropyScore(zeros, ones);

        // Selector quality: broad fixed coverage plus distribution balance.
        c.score = (c.fixedRatio * 0.8) + (c.entropy * 0.2);

        out.push_back(std::move(c));
    }

    return out;
}

static
void haoAddSelector(const HAOBitCandidate &cand,
                                    std::vector<HAOBitSelector> *selectors) {
    HAOBitSelector s;
    s.byteOffset = verify_u8(cand.bitIndex / 8);
    s.bitOffset = verify_u8(cand.bitIndex % 8);
    selectors->push_back(s);
}

static
void finalizeBitSelectors(std::vector<HAOBitSelector> *selectors,
                          u32 *keyBitsOut) {
    std::sort(selectors->begin(), selectors->end(),
              [](const HAOBitSelector &a, const HAOBitSelector &b) {
                  return haoSelectorBitIndex(a) < haoSelectorBitIndex(b);
              });

    if (keyBitsOut) {
        *keyBitsOut = verify_u32(selectors->size());
    }
}

static
bool haoHighCandBetter(const HAOBitCandidate &cand, double score,
                       const HAOBitCandidate *best, double bestScore) {
    if (!best) {
        return true;
    }
    if (score != bestScore) {
        return score > bestScore;
    }
    if (cand.fixedCount != best->fixedCount) {
        return cand.fixedCount > best->fixedCount;
    }
    if (cand.ambiguousCount != best->ambiguousCount) {
        return cand.ambiguousCount < best->ambiguousCount;
    }
    return cand.bitIndex > best->bitIndex;
}

static
void haoSelHighRange(const std::vector<HAOBitCandidate> &candidates,
                     u32 minByte, u32 maxByte, u32 quota,
                     u32 targetBits,
                     std::unordered_set<u32> *chosenBits,
                     std::vector<u8> *selectedAmbigs,
                     std::vector<HAOBitSelector> *selectors) {
    assert(chosenBits);
    assert(selectedAmbigs);
    assert(selectors);

    u32 selected = 0;
    while (selected < quota && selectors->size() < targetBits) {
        const HAOBitCandidate *best = nullptr;
        double bestScore = -std::numeric_limits<double>::infinity();

        for (const auto &cand : candidates) {
            const u32 byte = cand.bitIndex / 8U;
            if (byte < minByte || byte > maxByte) {
                continue;
            }
            if (chosenBits->find(cand.bitIndex) != chosenBits->end()) {
                continue;
            }
            if (!haoCandFits(cand, *selectedAmbigs)) {
                continue;
            }

            const double score = haoCandDynamicScore(cand, *selectedAmbigs);
            if (haoHighCandBetter(cand, score, best, bestScore)) {
                best = &cand;
                bestScore = score;
            }
        }

        if (!best) {
            break;
        }

        haoAddSelector(*best, selectors);
        chosenBits->insert(best->bitIndex);
        haoCandApply(*best, selectedAmbigs);
        selected++;
    }
}

static
void haoSelDefault(const std::vector<hwlmLiteral> &lits,
                   std::vector<HAOBitSelector> *selectors,
                   u32 targetBits, u32 *keyBitsOut) {
    selectors->clear();
    if (keyBitsOut) {
        *keyBitsOut = 0;
    }
    if (lits.empty()) {
        return;
    }

    auto candidates = buildBitCandidates(lits);
    if (candidates.empty()) {
        return;
    }
    // Sort candidate bits by score first, then by bit index so ties stay
    // closer to the end of the literal window.
    std::sort(candidates.begin(), candidates.end(),
              [](const HAOBitCandidate &a, const HAOBitCandidate &b) {
                  if (a.score != b.score) {
                      return a.score > b.score;
                  }
                  if (a.fixedCount != b.fixedCount) {
                      return a.fixedCount > b.fixedCount;
                  }
                  if (a.ambiguousCount != b.ambiguousCount) {
                      return a.ambiguousCount < b.ambiguousCount;
                  }
                  return a.bitIndex > b.bitIndex;
              });

    targetBits = std::min<u32>(targetBits, HAO_LAYOUT_KEY_BITS);
    targetBits = std::min<u32>(targetBits, verify_u32(candidates.size()));

    std::unordered_set<u64a> signatures;
    std::unordered_set<u32> chosenBits;
    std::vector<u8> selectedAmbigs(lits.size(), 0);

    for (const auto &cand : candidates) {
        if (selectors->size() >= targetBits) {
            break;
        }
        if (!haoCandFits(cand, selectedAmbigs)) {
            continue;
        }
        const u64a sig = signatureOfStates(cand.states);
        if (signatures.find(sig) != signatures.end()) {
            continue;
        }
        signatures.insert(sig);
        haoAddSelector(cand, selectors);
        chosenBits.insert(cand.bitIndex);
        haoCandApply(cand, &selectedAmbigs);
    }

    if (selectors->size() < targetBits) {
        for (const auto &cand : candidates) {
            if (selectors->size() >= targetBits) {
                break;
            }
            if (chosenBits.find(cand.bitIndex) != chosenBits.end()) {
                continue;
            }
            if (!haoCandFits(cand, selectedAmbigs)) {
                continue;
            }
            haoAddSelector(cand, selectors);
            chosenBits.insert(cand.bitIndex);
            haoCandApply(cand, &selectedAmbigs);
        }
    }

    finalizeBitSelectors(selectors, keyBitsOut);
}

static
void haoSelDynamic(const std::vector<hwlmLiteral> &lits,
                   std::vector<HAOBitSelector> *selectors,
                   u32 targetBits, u32 *keyBitsOut) {
    selectors->clear();
    if (keyBitsOut) {
        *keyBitsOut = 0;
    }
    if (lits.empty()) {
        return;
    }

    auto candidates = buildBitCandidates(lits);
    if (candidates.empty()) {
        return;
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const HAOBitCandidate &a, const HAOBitCandidate &b) {
                  if (a.score != b.score) {
                      return a.score > b.score;
                  }
                  if (a.fixedCount != b.fixedCount) {
                      return a.fixedCount > b.fixedCount;
                  }
                  if (a.ambiguousCount != b.ambiguousCount) {
                      return a.ambiguousCount < b.ambiguousCount;
                  }
                  return a.bitIndex > b.bitIndex;
              });

    targetBits = std::min<u32>(targetBits, HAO_LAYOUT_KEY_BITS);
    targetBits = std::min<u32>(targetBits, verify_u32(candidates.size()));

    std::unordered_set<u64a> signatures;
    std::unordered_set<u32> chosenBits;
    std::vector<u8> selectedAmbigs(lits.size(), 0);

    while (selectors->size() < targetBits) {
        const HAOBitCandidate *best = nullptr;
        double bestScore = -std::numeric_limits<double>::infinity();

        for (u32 pass = 0; pass < 2 && !best; pass++) {
            const bool allowDuplicateSignature = pass != 0;

            for (const auto &cand : candidates) {
                if (chosenBits.find(cand.bitIndex) != chosenBits.end()) {
                    continue;
                }
                if (!haoCandFits(cand, selectedAmbigs)) {
                    continue;
                }

                const u64a sig = signatureOfStates(cand.states);
                if (!allowDuplicateSignature &&
                    signatures.find(sig) != signatures.end()) {
                    continue;
                }

                const double score =
                    haoCandDynamicScore(cand, selectedAmbigs);
                if (!best || score > bestScore ||
                    (score == bestScore &&
                     cand.fixedCount > best->fixedCount) ||
                    (score == bestScore &&
                     cand.fixedCount == best->fixedCount &&
                     cand.ambiguousCount < best->ambiguousCount) ||
                    (score == bestScore &&
                     cand.fixedCount == best->fixedCount &&
                     cand.ambiguousCount == best->ambiguousCount &&
                     cand.bitIndex > best->bitIndex)) {
                    best = &cand;
                    bestScore = score;
                }
            }
        }

        if (!best) {
            break;
        }

        const u64a sig = signatureOfStates(best->states);
        signatures.insert(sig);
        haoAddSelector(*best, selectors);
        chosenBits.insert(best->bitIndex);
        haoCandApply(*best, &selectedAmbigs);
    }

    if (selectors->size() < targetBits) {
        for (const auto &cand : candidates) {
            if (selectors->size() >= targetBits) {
                break;
            }
            if (chosenBits.find(cand.bitIndex) != chosenBits.end()) {
                continue;
            }
            if (!haoCandFits(cand, selectedAmbigs)) {
                continue;
            }
            haoAddSelector(cand, selectors);
            chosenBits.insert(cand.bitIndex);
            haoCandApply(cand, &selectedAmbigs);
        }
    }

    finalizeBitSelectors(selectors, keyBitsOut);
}

static
void haoSelHighAlign(const std::vector<hwlmLiteral> &lits,
                     std::vector<HAOBitSelector> *selectors,
                     u32 targetBits, u32 *keyBitsOut) {
    selectors->clear();
    if (keyBitsOut) {
        *keyBitsOut = 0;
    }
    if (lits.empty()) {
        return;
    }

    auto candidates = buildBitCandidates(lits);
    if (candidates.empty()) {
        return;
    }

    targetBits = std::min<u32>(targetBits, HAO_LAYOUT_KEY_BITS);
    targetBits = std::min<u32>(targetBits, verify_u32(candidates.size()));
    std::unordered_set<u32> chosenBits;
    std::vector<u8> selectedAmbigs(lits.size(), 0);

    // Rules are high-aligned in the 8-byte window. Prefer the suffix bytes
    // that usually contain literal bytes for short and long literals, while
    // still choosing the best bit inside each byte range.
    // byte7   = 8
    // byte6   = 6
    // byte4-5 = 5
    // byte0-3 = 3
    haoSelHighRange(candidates, 7U, 7U, 8U, targetBits, &chosenBits,
                    &selectedAmbigs, selectors);
    haoSelHighRange(candidates, 6U, 6U, 6U, targetBits, &chosenBits,
                    &selectedAmbigs, selectors);
    haoSelHighRange(candidates, 4U, 5U, 5U, targetBits, &chosenBits,
                    &selectedAmbigs, selectors);
    haoSelHighRange(candidates, 0U, 3U, 3U, targetBits, &chosenBits,
                    &selectedAmbigs, selectors);

    if (selectors->size() < targetBits) {
        haoSelHighRange(candidates, 0U, 7U,
                        targetBits - verify_u32(selectors->size()),
                        targetBits, &chosenBits, &selectedAmbigs, selectors);
    }

    finalizeBitSelectors(selectors, keyBitsOut);
}

struct HAOHashOptRule {
    u64a careMask = 0;
    u64a value = 0;
};

static
std::vector<HAOHashOptRule> haoMakeOptRules(
    const std::vector<hwlmLiteral> &lits) {
    std::vector<HAOHashOptRule> rules;
    rules.reserve(lits.size());

    for (const auto &lit : lits) {
        HAOHashOptRule rule;
        for (u32 bit = 0; bit < HAO_BUILD_MAX_CANDIDATE_BITS; bit++) {
            u8 state = HAO_BUILD_STATE_DONT_CARE;
            getBitState(lit, bit, &state);
            if (state == HAO_BUILD_STATE_DONT_CARE) {
                continue;
            }
            rule.careMask |= 1ULL << bit;
            if (state) {
                rule.value |= 1ULL << bit;
            }
        }
        rules.push_back(rule);
    }

    return rules;
}

static really_inline
bool haoHashOptOverlap(const HAOHashOptRule &a, const HAOHashOptRule &b) {
    const u64a commonCare = a.careMask & b.careMask;
    return !((a.value ^ b.value) & commonCare);
}

static
bool haoHashOptCanAddRawBit(
    u32 bit, const std::vector<std::vector<u32>> &rulesWithDcBit,
    const std::vector<u8> &kR) {
    assert(bit < HAO_BUILD_MAX_CANDIDATE_BITS);
    for (u32 r : rulesWithDcBit[bit]) {
        if ((u32)kR[r] >= HAO_MAX_KEY_AMBIG_BITS) {
            return false;
        }
    }
    return true;
}

static
bool haoHashOptCanSwapRawBit(
    u32 oldBit, u32 newBit,
    const std::vector<std::vector<u32>> &rulesWithDcBit,
    const std::vector<u8> &kR, const std::vector<u64a> &ruleDcMask) {
    assert(oldBit < HAO_BUILD_MAX_CANDIDATE_BITS);
    assert(newBit < HAO_BUILD_MAX_CANDIDATE_BITS);
    const u64a oldMask = 1ULL << oldBit;

    for (u32 r : rulesWithDcBit[newBit]) {
        const u32 afterRemove = (ruleDcMask[r] & oldMask) ? 1U : 0U;
        if ((u32)kR[r] - afterRemove >= HAO_MAX_KEY_AMBIG_BITS) {
            return false;
        }
    }
    return true;
}

static
void haoSelHashOpt(const std::vector<hwlmLiteral> &lits,
                   std::vector<HAOBitSelector> *selectors,
                   u32 targetBits, u32 *keyBitsOut) {
    selectors->clear();
    if (keyBitsOut) {
        *keyBitsOut = 0;
    }
    if (lits.empty() || lits.size() > HAO_HASH_OPT_MAX_LITS) {
        haoSelDefault(lits, selectors, targetBits, keyBitsOut);
        return;
    }

    targetBits = std::min<u32>(targetBits, HAO_LAYOUT_KEY_BITS);

    const auto rules = haoMakeOptRules(lits);
    const u32 ruleCount = verify_u32(rules.size());
    if (!ruleCount) {
        return;
    }

    std::vector<u64a> ruleDcMask(ruleCount, 0);
    std::vector<std::vector<u32>> rulesWithDcBit(HAO_BUILD_MAX_CANDIDATE_BITS);
    for (u32 r = 0; r < ruleCount; r++) {
        ruleDcMask[r] = ~rules[r].careMask;
        u64a dc = ruleDcMask[r];
        while (dc) {
            const u32 bit = ctz64(dc);
            rulesWithDcBit[bit].push_back(r);
            dc &= dc - 1;
        }
    }

    std::vector<u64a> pairBitsMask;
    pairBitsMask.reserve((size_t)ruleCount * (ruleCount - 1U) / 4U);
    for (u32 r = 0; r < ruleCount; r++) {
        for (u32 s = r + 1U; s < ruleCount; s++) {
            if (haoHashOptOverlap(rules[r], rules[s])) {
                continue;
            }
            const u64a dist = (rules[r].careMask & rules[s].careMask) &
                              (rules[r].value ^ rules[s].value);
            if (dist) {
                pairBitsMask.push_back(dist);
            }
        }
    }

    std::vector<std::vector<u32>> bitPairs(HAO_BUILD_MAX_CANDIDATE_BITS);
    const u32 totalPairs = verify_u32(pairBitsMask.size());
    for (u32 bit = 0; bit < HAO_BUILD_MAX_CANDIDATE_BITS; bit++) {
        bitPairs[bit].reserve(totalPairs / 4U);
    }
    for (u32 pi = 0; pi < totalPairs; pi++) {
        u64a bits = pairBitsMask[pi];
        while (bits) {
            const u32 bit = ctz64(bits);
            bitPairs[bit].push_back(pi);
            bits &= bits - 1;
        }
    }

    std::vector<u64a> coverageMask(totalPairs, 0);
    std::array<u32, HAO_BUILD_MAX_CANDIDATE_BITS> newPairs = {};
    std::array<u32, HAO_BUILD_MAX_CANDIDATE_BITS> singletonPairs = {};
    std::array<u64a, HAO_BUILD_MAX_CANDIDATE_BITS> spreadCost = {};
    std::vector<u8> kR(ruleCount, 0);
    u64a selectedMask = 0;
    u64a totalPlacements = ruleCount;
    u32 coveredPairs = 0;
    std::vector<u32> selectedBits;
    selectedBits.reserve(targetBits);

    for (u32 bit = 0; bit < HAO_BUILD_MAX_CANDIDATE_BITS; bit++) {
        newPairs[bit] = verify_u32(bitPairs[bit].size());
        spreadCost[bit] = verify_u32(rulesWithDcBit[bit].size());
    }

    auto addBit = [&](u32 bit) {
        const u64a bitMask = 1ULL << bit;
        for (u32 pi : bitPairs[bit]) {
            const u64a before = coverageMask[pi];
            const u32 beforePop = popcount64(before);
            coverageMask[pi] |= bitMask;

            if (!beforePop) {
                singletonPairs[bit]++;
                u64a bits = pairBitsMask[pi];
                while (bits) {
                    const u32 b = ctz64(bits);
                    assert(newPairs[b]);
                    newPairs[b]--;
                    bits &= bits - 1;
                }
            } else if (beforePop == 1U) {
                const u32 prev = ctz64(before);
                assert(singletonPairs[prev]);
                singletonPairs[prev]--;
            }
        }

        for (u32 r : rulesWithDcBit[bit]) {
            const u64a delta = 1ULL << kR[r];
            u64a dc = ruleDcMask[r];
            while (dc) {
                const u32 b = ctz64(dc);
                spreadCost[b] += delta;
                dc &= dc - 1;
            }
            totalPlacements += delta;
            kR[r]++;
        }
    };

    auto removeBit = [&](u32 bit) {
        const u64a bitMask = 1ULL << bit;
        for (u32 r : rulesWithDcBit[bit]) {
            assert(kR[r]);
            kR[r]--;
            const u64a delta = 1ULL << kR[r];
            u64a dc = ruleDcMask[r];
            while (dc) {
                const u32 b = ctz64(dc);
                assert(spreadCost[b] >= delta);
                spreadCost[b] -= delta;
                dc &= dc - 1;
            }
            assert(totalPlacements >= delta);
            totalPlacements -= delta;
        }

        for (u32 pi : bitPairs[bit]) {
            const u64a before = coverageMask[pi];
            const u32 beforePop = popcount64(before);
            coverageMask[pi] &= ~bitMask;
            const u64a after = coverageMask[pi];
            const u32 afterPop = popcount64(after);

            if (beforePop == 1U && !afterPop) {
                assert(singletonPairs[bit]);
                singletonPairs[bit]--;
                u64a bits = pairBitsMask[pi];
                while (bits) {
                    const u32 b = ctz64(bits);
                    newPairs[b]++;
                    bits &= bits - 1;
                }
            } else if (beforePop == 2U && afterPop == 1U) {
                const u32 other = ctz64(after);
                singletonPairs[other]++;
            }
        }
    };

    while (selectedBits.size() < targetBits &&
           coveredPairs < totalPairs) {
        u32 bestBit = HAO_BUILD_MAX_CANDIDATE_BITS;
        double bestScore = -1.0;

        for (u32 bit = 0; bit < HAO_BUILD_MAX_CANDIDATE_BITS; bit++) {
            if (selectedMask & (1ULL << bit)) {
                continue;
            }
            if (!newPairs[bit]) {
                continue;
            }
            if (!haoHashOptCanAddRawBit(bit, rulesWithDcBit, kR)) {
                continue;
            }

            const double score =
                (double)newPairs[bit] /
                (1.0 + HAO_HASH_OPT_SPREAD_PENALTY *
                           (double)spreadCost[bit]);
            if (score > bestScore) {
                bestBit = bit;
                bestScore = score;
            }
        }

        if (bestBit >= HAO_BUILD_MAX_CANDIDATE_BITS) {
            break;
        }

        selectedBits.push_back(bestBit);
        selectedMask |= 1ULL << bestBit;
        coveredPairs += newPairs[bestBit];
        addBit(bestBit);
    }

#if HAO_HASH_OPT_ENABLE_SWAP
    bool improved = false;
    u32 rounds = 0;
    do {
        improved = false;
        rounds++;

        std::vector<std::array<u32, HAO_BUILD_MAX_CANDIDATE_BITS>>
            singletonOverlap(selectedBits.size());
        for (auto &row : singletonOverlap) {
            row.fill(0);
        }

        for (u32 idx = 0; idx < selectedBits.size(); idx++) {
            const u32 sBit = selectedBits[idx];
            const u64a sMask = 1ULL << sBit;
            for (u32 pi : bitPairs[sBit]) {
                if (coverageMask[pi] != sMask) {
                    continue;
                }
                u64a bits = pairBitsMask[pi];
                while (bits) {
                    const u32 b = ctz64(bits);
                    singletonOverlap[idx][b]++;
                    bits &= bits - 1;
                }
            }
        }

        for (u32 idx = 0; idx < selectedBits.size(); idx++) {
            const u32 oldBit = selectedBits[idx];
            const u64a oldMask = 1ULL << oldBit;

            for (u32 newBit = 0; newBit < HAO_BUILD_MAX_CANDIDATE_BITS;
                 newBit++) {
                const u64a newMask = 1ULL << newBit;
                if (selectedMask & newMask) {
                    continue;
                }
                if (!haoHashOptCanSwapRawBit(oldBit, newBit, rulesWithDcBit,
                                             kR, ruleDcMask)) {
                    continue;
                }

                int64_t placeDelta = 0;
                for (u32 r : rulesWithDcBit[newBit]) {
                    if (!(ruleDcMask[r] & oldMask)) {
                        placeDelta += (int64_t)(1ULL << kR[r]);
                    }
                }
                for (u32 r : rulesWithDcBit[oldBit]) {
                    if (!(ruleDcMask[r] & newMask)) {
                        assert(kR[r]);
                        placeDelta -= (int64_t)(1ULL << (kR[r] - 1U));
                    }
                }
                if (placeDelta >= 0) {
                    continue;
                }

                const u32 covLoss = singletonPairs[oldBit];
                const u32 covGain =
                    newPairs[newBit] + singletonOverlap[idx][newBit];
                if (covGain < covLoss) {
                    continue;
                }

                removeBit(oldBit);
                addBit(newBit);
                coveredPairs += covGain - covLoss;
                selectedMask &= ~oldMask;
                selectedMask |= newMask;
                selectedBits[idx] = newBit;
                improved = true;
                break;
            }
            if (improved) {
                break;
            }
        }
    } while (improved && rounds < 50U);
#else
    (void)removeBit;
    (void)totalPlacements;
#endif

    while (selectedBits.size() < targetBits) {
        u32 bestBit = HAO_BUILD_MAX_CANDIDATE_BITS;
        u64a bestCost = std::numeric_limits<u64a>::max();

        for (u32 bit = 0; bit < HAO_BUILD_MAX_CANDIDATE_BITS; bit++) {
            if (selectedMask & (1ULL << bit)) {
                continue;
            }
            if (!haoHashOptCanAddRawBit(bit, rulesWithDcBit, kR)) {
                continue;
            }
            if (spreadCost[bit] < bestCost) {
                bestBit = bit;
                bestCost = spreadCost[bit];
            }
        }

        if (bestBit >= HAO_BUILD_MAX_CANDIDATE_BITS) {
            break;
        }
        selectedBits.push_back(bestBit);
        selectedMask |= 1ULL << bestBit;
        coveredPairs += newPairs[bestBit];
        addBit(bestBit);
    }

    for (u32 bit : selectedBits) {
        HAOBitCandidate cand;
        cand.bitIndex = bit;
        haoAddSelector(cand, selectors);
    }
    finalizeBitSelectors(selectors, keyBitsOut);
}

static
bool haoSelFixed(std::vector<HAOBitSelector> *selectors,
                 u32 *keyBitsOut, u32 targetBits) {
    if (!selectors) {
        return false;
    }

    auto buildSelectorsFromMask = [selectors, keyBitsOut](u64a mask) {
        u64a m = mask;

        selectors->clear();
        if (keyBitsOut) {
            *keyBitsOut = 0;
        }

        while (m) {
            const u32 bit = ctz64(m);
            HAOBitSelector selector;
            selector.byteOffset = verify_u8(bit / 8U);
            selector.bitOffset = verify_u8(bit % 8U);
            selectors->push_back(selector);
            m &= m - 1;
        }

        finalizeBitSelectors(selectors, keyBitsOut);
    };

    const char *env = getenv("HS_HAO_BEXT_MASK");
    if (env && *env) {
        char *end = nullptr;
        const u64a mask = (u64a)strtoull(env, &end, 0);
        const u32 bitCount = popcount64(mask);

        selectors->clear();
        if (keyBitsOut) {
            *keyBitsOut = 0;
        }

        if (end == env || *end || !mask ||
            bitCount > HAO_LAYOUT_MAX_SELECTORS) {
            printf("[HAO][BEXT] invalid HS_HAO_BEXT_MASK=%s "
                   "(popcount=%u targetBits=%u)\n",
                   env, bitCount, targetBits);
            return true;
        }
        if (bitCount != targetBits) {
            return false;
        }

        buildSelectorsFromMask(mask);
        return true;
    }

#if HAO_FIXED_BEXT_MASK
    const u64a mask = (u64a)HAO_FIXED_BEXT_MASK;

    selectors->clear();
    if (keyBitsOut) {
        *keyBitsOut = 0;
    }

    if (popcount64(mask) > HAO_LAYOUT_MAX_SELECTORS) {
        return true;
    }

    buildSelectorsFromMask(mask);
    return true;
#else
    (void)keyBitsOut;
    (void)targetBits;
    return false;
#endif
}

static
void haoSelectBits(const std::vector<hwlmLiteral> &lits,
                   std::vector<HAOBitSelector> *selectors,
                   u32 *keyBitsOut, HAOSelectorMode mode,
                   u32 targetBits) {
    if (haoSelFixed(selectors, keyBitsOut, targetBits)) {
        return;
    }

    if (mode == HAOSelectorMode::HASH_OPT) {
        haoSelHashOpt(lits, selectors, targetBits, keyBitsOut);
    } else if (mode == HAOSelectorMode::HIGH_ALIGN) {
        haoSelHighAlign(lits, selectors, targetBits, keyBitsOut);
    } else if (mode == HAOSelectorMode::DYNAMIC_EXPANSION) {
        haoSelDynamic(lits, selectors, targetBits, keyBitsOut);
    } else {
        haoSelDefault(lits, selectors, targetBits, keyBitsOut);
    }
}

static
void haoBuildMeta(const std::vector<hwlmLiteral> &lits,
                  std::vector<HAOCompileRuleMeta> *meta) {
    meta->clear();
    meta->reserve(lits.size());

    for (const auto &lit : lits) {
        assert(lit.s.size() <= HAO_LAYOUT_BYTES_PER_RULE_SLOT);
        HAOCompileRuleMeta m = {};
        m.id = lit.id;
        m.groups = lit.groups;
        if (lit.nocase) {
            m.flags |= HAO_RULE_META_FLAG_NOCASE;
        }
        if (lit.noruns) {
            m.flags |= HAO_RULE_META_FLAG_NORUNS;
        }
        meta->push_back(m);
    }
}

static
std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> haoChooseDynamicDotVector(
    const std::vector<hwlmLiteral> &lits, u64a dotInputMask,
    const std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> &fallback);

struct HAODotHashChoice {
    std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> dotVector = {};
    u64a dotInputMask = 0;
    HAOCompileSummary summary;
    HAOHashStats hashStats;
    u32 flags = 0;
    double cost = std::numeric_limits<double>::infinity();
    bool valid = false;
};

static
HAODotHashChoice haoChooseDotHash(
    const std::vector<hwlmLiteral> &lits,
    const std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> &baseVector);

static
HAODotHashChoice haoEvaluateDotHashChoice(
    const std::vector<hwlmLiteral> &lits,
    const std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> &baseVector,
    u32 maxInputMaskBits, bool allowDynamicVector = true);

template <class ArtifactsT>
static
bool haoCompileCore(const std::vector<hwlmLiteral> &lits,
                    ArtifactsT *artifacts, HAOSelectorMode selectorMode,
                    u32 targetBits) {
    if (!artifacts || lits.empty()) {
        return false;
    }

    artifacts->hashMode = HAO_LAYOUT_HASH_BEXT;
    u32 keyBits = 0;
    haoSelectBits(lits, &artifacts->selectors, &keyBits, selectorMode,
                  targetBits);
    if (artifacts->selectors.empty()) {
        return false;
    }

    haoBuildExtract(artifacts->selectors, artifacts);
    haoBuildPlans(lits, artifacts->selectors, &artifacts->plans,
                      &artifacts->summary);
    if (artifacts->summary.unsupportedRules ||
        artifacts->summary.fastPathRules != artifacts->summary.totalRules ||
        artifacts->summary.maskRules != artifacts->summary.maskMergedRules) {
        artifacts->hash.flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
        return false;
    }
    haoBuildTables(artifacts->plans, keyBits, &artifacts->hash);
    haoBuildL15Tags(artifacts);
    haoBuildMeta(lits, &artifacts->meta);
    return true;
}

static
bool haoCompileDotCore(const std::vector<hwlmLiteral> &lits,
                       HAOCompileArtifacts *artifacts) {
    if (!artifacts || lits.empty()) {
        return false;
    }

    artifacts->hashMode = HAO_LAYOUT_HASH_DOT;
    artifacts->selectorName = "dot-forced";
    artifacts->dotVector = HAO_DOT_DEFAULT_VECTOR;
    const bool forcedDotVector = haoParseDotVectorEnv(&artifacts->dotVector);
    if (!haoDotVectorHasNonZeroLane(artifacts->dotVector)) {
        artifacts->hash.flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
        return false;
    }
    HAODotHashChoice dotChoice;
    if (forcedDotVector) {
        dotChoice = haoEvaluateDotHashChoice(lits, artifacts->dotVector,
                                             HAO_DOT_INPUT_MASK_MAX_BITS,
                                             false);
    } else {
        dotChoice = haoChooseDotHash(lits, artifacts->dotVector);
    }
    if (dotChoice.valid) {
        artifacts->dotInputMask = dotChoice.dotInputMask;
        artifacts->dotVector = dotChoice.dotVector;
    } else {
        artifacts->dotInputMask = haoChooseDotInputMask(lits,
                                                       artifacts->dotVector);
        if (!forcedDotVector) {
            artifacts->dotVector = haoChooseDynamicDotVector(
                lits, artifacts->dotInputMask, artifacts->dotVector);
        }
    }
    if ((!haoDotVectorHasNonZeroLane(artifacts->dotVector) ||
        !haoDotMaskBuilds(lits, artifacts->dotVector,
                          artifacts->dotInputMask)) && !forcedDotVector) {
        artifacts->dotVector = haoBuildDotVectorForLits(lits);
        artifacts->dotInputMask = haoChooseDotInputMask(lits,
                                                       artifacts->dotVector);
    }
    if (!haoDotVectorHasNonZeroLane(artifacts->dotVector) ||
        !haoDotMaskBuilds(lits, artifacts->dotVector,
                          artifacts->dotInputMask)) {
        artifacts->hash.flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
        return false;
    }
    artifacts->selectors.clear();
    artifacts->bextMask = 0;

    haoBuildDotPlans(lits, artifacts->dotVector, artifacts->dotInputMask,
                     HAO_LAYOUT_KEY_BITS, &artifacts->plans,
                     &artifacts->summary);
    if (artifacts->summary.unsupportedRules ||
        artifacts->summary.fastPathRules != artifacts->summary.totalRules ||
        artifacts->summary.maskRules != artifacts->summary.maskMergedRules) {
        artifacts->hash.flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
        return false;
    }

    haoBuildTables(artifacts->plans, HAO_LAYOUT_KEY_BITS, &artifacts->hash);
    haoBuildMeta(lits, &artifacts->meta);
    return true;
}

static
double haoPct(u32 value, u32 total) {
    if (!total) {
        return 0.0;
    }
    return (static_cast<double>(value) * 100.0) / static_cast<double>(total);
}

struct HAOCandidateStats {
    const char *name = nullptr;
    HAOSelectorMode mode = HAOSelectorMode::DEFAULT;
    u32 keyBits = 0;
    u32 flags = 0;
    HAOCompileSummary summary;
    HAOHashStats hashStats;
    double cost = std::numeric_limits<double>::infinity();
};

static
bool haoCandidateStatsOk(const HAOCompileSummary &summary,
                         const HAOHashStats &stats, u32 flags) {
    return !(flags & HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE) &&
           summary.fastPathRules == summary.totalRules &&
           !summary.unsupportedRules &&
           summary.maskRules == summary.maskMergedRules &&
           stats.nonEmptyPrimary;
}

static
double haoAvgRulesPerBucket(const HAOHashStats &stats) {
    if (!stats.nonEmptyPrimary) {
        return 0.0;
    }
    return static_cast<double>(stats.totalRulesInBuckets) /
           static_cast<double>(stats.nonEmptyPrimary);
}

static
double haoAvgEntriesPerBucket(const HAOHashStats &stats) {
    if (!stats.nonEmptyPrimary) {
        return 0.0;
    }
    return static_cast<double>(stats.totalL2Entries) /
           static_cast<double>(stats.nonEmptyPrimary);
}

static
double haoPrimaryOccupancyPct(u32 nonEmptyPrimary, u32 keyBits) {
    if (!keyBits || keyBits >= 31U) {
        return 0.0;
    }
    const double primaryCount = static_cast<double>(1U << keyBits);
    return static_cast<double>(nonEmptyPrimary) * 100.0 / primaryCount;
}

static
double haoEstimateFootprintMiB(const HAOHashStats &stats, u32 ruleCount,
                               u32 keyBits) {
    const u32 primaryCount = haoPrimaryCountForKeyBits(keyBits);
    const u32 bitmapCount =
        haoPrimaryBitmapCountForPrimaryCount(primaryCount);
    const u32 l2EntryCount = stats.totalL2Entries + 1U; // L2[0] is empty.
    const u64a bytes =
        (u64a)primaryCount * sizeof(u32) +
        (u64a)haoPrimaryBitmapBytes(bitmapCount) * sizeof(u8) +
        (u64a)l2EntryCount * sizeof(HAOL2Check) +
        (u64a)l2EntryCount * sizeof(HAOL2Meta) +
        (u64a)ruleCount * sizeof(HAOCompileRuleMeta);
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

static
double haoAutoStatsCost(const HAOCompileSummary &summary,
                        const HAOHashStats &stats, u32 keyBits, u32 flags) {
    if (!haoCandidateStatsOk(summary, stats, flags)) {
        return std::numeric_limits<double>::infinity();
    }

    const double expansionAvg = summary.totalRules
                                    ? static_cast<double>(
                                          summary.totalExpandedKeys) /
                                          static_cast<double>(
                                              summary.totalRules)
                                    : 0.0;
    const double collisionPct =
        haoPct(stats.collisionBuckets, stats.nonEmptyPrimary);
    const double ruleGt4Pct =
        haoPct(stats.ruleBucketsGt4, stats.nonEmptyPrimary);
    const double entryGt4Pct =
        haoPct(stats.entryBucketsGt4, stats.nonEmptyPrimary);
    const double avgRules = haoAvgRulesPerBucket(stats);
    const double avgEntries = haoAvgEntriesPerBucket(stats);

    // Compile-time proxy for runtime cost. Primary occupancy is useful but not
    // dominant: measured corpora showed that collision depth and large rule
    // buckets predict L2 pressure better than table density alone.
    double cost =
        haoPrimaryOccupancyPct(stats.nonEmptyPrimary, keyBits) * 25.0 +
        collisionPct * 4.0 +
        ruleGt4Pct * 18.0 +
        entryGt4Pct * 12.0 +
        avgRules * 8.0 +
        avgEntries * 4.0 +
        expansionAvg * 0.30 +
        haoEstimateFootprintMiB(stats, summary.totalRules, keyBits) * 0.15 +
        static_cast<double>(stats.maxRulesPerBucket) * 0.05;

    // Soft guardrails catch compact-but-noisy tables without forbidding
    // smaller key widths that still have clean bucket distributions.
    cost += std::max(0.0, collisionPct - 10.0) * 15.0;
    cost += std::max(0.0, ruleGt4Pct - 2.0) * 45.0;
    cost += std::max(0.0, avgRules - 1.25) * 80.0;
    cost += std::max(0.0,
                     static_cast<double>(stats.maxRulesPerBucket) - 64.0) *
            1.5;
    return cost;
}

static
bool haoDryRunHashStats(const std::vector<HAOCompiledRulePlan> &rulePlans,
                        u32 keyBits, HAOHashStats *stats, u32 *flags) {
    if (!stats || !flags || !keyBits) {
        return false;
    }

    *stats = {};
    *flags = 0;

    size_t placementCount = 0;
    for (const auto &plan : rulePlans) {
        if (plan.category == HAORuleCategory::HAO_RULE_UNSUPPORTED) {
            *flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
            return false;
        }
        placementCount += plan.keyExpansion.expandedKeys.size();
    }

    std::vector<std::pair<u32, u32>> placements;
    placements.reserve(placementCount);
    for (const auto &plan : rulePlans) {
        for (const auto &expanded : plan.keyExpansion.expandedKeys) {
            placements.emplace_back(expanded.keyValue, plan.ruleIndex);
            stats->totalExpandedKeysInBuckets++;
        }
    }
    if (placements.empty()) {
        return false;
    }

    std::sort(placements.begin(), placements.end(),
              [](const std::pair<u32, u32> &a,
                 const std::pair<u32, u32> &b) {
                  if (a.first != b.first) {
                      return a.first < b.first;
                  }
                  return a.second < b.second;
              });

    u32 l2EntryCount = 1U; // Keep the runtime null L2 entry reserved.
    for (size_t begin = 0; begin < placements.size();) {
        size_t end = begin + 1U;
        while (end < placements.size() &&
               placements[end].first == placements[begin].first) {
            end++;
        }

        const u32 ruleCount = verify_u32(end - begin);
        const u32 entryCount = verify_u32(
            (ruleCount + HAO_LAYOUT_RULE_SLOTS_PER_ENTRY - 1U) /
            HAO_LAYOUT_RULE_SLOTS_PER_ENTRY);

        if (l2EntryCount + entryCount > HAO_BUILD_MAX_L2_ENTRIES) {
            *flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
            *flags |= HAO_ARTIFACT_FLAG_PARTIAL_L2_CAPACITY;
            return false;
        }
        if (entryCount >= (1U << (32U - HAO_LAYOUT_L1_COUNT_SHIFT))) {
            *flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
            *flags |= HAO_ARTIFACT_FLAG_PARTIAL_ENTRY_OVERFLOW;
            return false;
        }

        l2EntryCount += entryCount;
        stats->nonEmptyPrimary++;
        stats->totalRulesInBuckets += ruleCount;
        stats->totalL2Entries += entryCount;
        if (!stats->minRulesPerBucket ||
            ruleCount < stats->minRulesPerBucket) {
            stats->minRulesPerBucket = ruleCount;
        }
        stats->maxRulesPerBucket =
            std::max(stats->maxRulesPerBucket, ruleCount);
        if (!stats->minEntriesPerBucket ||
            entryCount < stats->minEntriesPerBucket) {
            stats->minEntriesPerBucket = entryCount;
        }
        stats->maxEntriesPerKey =
            std::max(stats->maxEntriesPerKey, entryCount);
        if (ruleCount > 1U) {
            stats->collisionBuckets++;
        }
        if (ruleCount == 1U) {
            stats->ruleBucketsEq1++;
        } else if (ruleCount <= 4U) {
            stats->ruleBuckets2To4++;
        } else {
            stats->ruleBucketsGt4++;
        }
        if (entryCount == 1U) {
            stats->entryBucketsEq1++;
        } else if (entryCount <= 4U) {
            stats->entryBuckets2To4++;
        } else {
            stats->entryBucketsGt4++;
        }

        begin = end;
    }

    return true;
}

struct HAODotVectorCandidate {
    std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> dotVector = {};
    HAOCompileSummary summary;
    HAOHashStats hashStats;
    u32 flags = 0;
    double cost = std::numeric_limits<double>::infinity();
    bool valid = false;
};

static
u64a haoMix64(u64a x) {
    x ^= x >> 33U;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33U;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33U;
    return x;
}

static
u64a haoDotRuleSetSeed(const std::vector<hwlmLiteral> &lits,
                       u64a dotInputMask) {
    u64a seed = 0x9e3779b97f4a7c15ULL ^ dotInputMask;

    for (const auto &lit : lits) {
        u64a word = 0;
        const u32 len = std::min<u32>(verify_u32(lit.s.size()),
                                      HAO_LAYOUT_BYTES_PER_RULE_SLOT);
        for (u32 i = 0; i < len; i++) {
            word |= (u64a)verify_u8(lit.s[i]) << (i * 8U);
        }
        seed ^= haoMix64(word ^ ((u64a)len << 56U) ^
                         (lit.nocase ? 0xa5a5a5a5a5a5a5a5ULL : 0ULL) ^
                         (lit.noruns ? 0x5a5a5a5a5a5a5a5aULL : 0ULL));
        seed = haoMix64(seed + 0x9e3779b97f4a7c15ULL);
    }
    return seed;
}

static
u64a haoXorshift64(u64a *state) {
    u64a x = *state ? *state : 0x2545f4914f6cdd1dULL;

    x ^= x << 13U;
    x ^= x >> 7U;
    x ^= x << 17U;
    *state = x;
    return x;
}

static
u16 haoDotOddCoeff(u64a value) {
    u16 coeff = verify_u16(value & 0xffffU);

    coeff |= 1U;
    if (coeff == 1U) {
        coeff = 257U;
    }
    return coeff;
}

static
bool haoAddDotVectorCandidate(
    std::vector<std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES>> *candidates,
    std::unordered_set<u64a> *seen,
    const std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> &dotVector,
    u64a dotInputMask) {
    assert(candidates);
    assert(seen);

    const auto masked = haoApplyDotInputMaskToVector(dotVector, dotInputMask);
    if (!haoDotVectorHasNonZeroLane(masked)) {
        return false;
    }
    const u64a key = haoPackDotVector(masked);
    if (!seen->insert(key).second) {
        return false;
    }
    candidates->push_back(masked);
    return true;
}

static
std::vector<std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES>>
haoGenerateDotVectorCandidates(const std::vector<hwlmLiteral> &lits,
                               u64a dotInputMask) {
    std::vector<std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES>> candidates;
    std::unordered_set<u64a> seen;
    u64a rng = haoDotRuleSetSeed(lits, dotInputMask);

    candidates.reserve(HAO_DOT_VECTOR_SEARCH_ROUNDS + 8U);
    haoAddDotVectorCandidate(&candidates, &seen, HAO_DOT_DEFAULT_VECTOR,
                             dotInputMask);
    haoAddDotVectorCandidate(&candidates, &seen,
                             {{0x0031U, 0x0041U, 0x0059U, 0x0026U}},
                             dotInputMask);
    haoAddDotVectorCandidate(&candidates, &seen,
                             {{17U, 131U, 521U, 2053U}}, dotInputMask);
    haoAddDotVectorCandidate(&candidates, &seen,
                             {{257U, 1031U, 4099U, 16411U}}, dotInputMask);
    haoAddDotVectorCandidate(&candidates, &seen,
                             {{257U, 1543U, 6151U, 24593U}}, dotInputMask);

    for (u32 round = 0; round < HAO_DOT_VECTOR_SEARCH_ROUNDS; round++) {
        std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> dotVector = {};
        for (u32 lane = 0; lane < HAO_LAYOUT_DOT_VECTOR_LANES; lane++) {
            const u64a mixed = haoMix64(haoXorshift64(&rng) +
                                       ((u64a)lane << 32U) + round);
            dotVector[lane] = haoDotOddCoeff(mixed);
        }
        haoAddDotVectorCandidate(&candidates, &seen, dotVector,
                                 dotInputMask);
    }

    return candidates;
}

static
HAODotVectorCandidate haoEvaluateDotVectorCandidate(
    const std::vector<hwlmLiteral> &lits,
    const std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> &dotVector,
    u64a dotInputMask) {
    HAODotVectorCandidate result;
    std::vector<HAOCompiledRulePlan> plans;

    result.dotVector = dotVector;
    if (!haoDotVectorHasNonZeroLane(dotVector)) {
        return result;
    }
    haoBuildDotPlans(lits, dotVector, dotInputMask, HAO_LAYOUT_KEY_BITS,
                     &plans, &result.summary);
    if (result.summary.unsupportedRules ||
        result.summary.fastPathRules != result.summary.totalRules ||
        result.summary.maskRules != result.summary.maskMergedRules) {
        result.flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
        return result;
    }
    if (!haoDryRunHashStats(plans, HAO_LAYOUT_KEY_BITS, &result.hashStats,
                            &result.flags)) {
        result.flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
        return result;
    }

    result.cost = haoAutoStatsCost(result.summary, result.hashStats,
                                   HAO_LAYOUT_KEY_BITS, result.flags);
    result.valid = std::isfinite(result.cost);
    return result;
}

static
double haoDotHashChoiceCost(const HAOCompileSummary &summary,
                            const HAOHashStats &stats, u32 keyBits,
                            u32 flags) {
    if (!haoCandidateStatsOk(summary, stats, flags)) {
        return std::numeric_limits<double>::infinity();
    }

    const double expansionAvg = summary.totalRules
                                    ? static_cast<double>(
                                          summary.totalExpandedKeys) /
                                          static_cast<double>(
                                              summary.totalRules)
                                    : 0.0;
    const double collisionPct =
        haoPct(stats.collisionBuckets, stats.nonEmptyPrimary);
    const double ruleGt4Pct =
        haoPct(stats.ruleBucketsGt4, stats.nonEmptyPrimary);
    const double avgRules = haoAvgRulesPerBucket(stats);

    return
        haoPrimaryOccupancyPct(stats.nonEmptyPrimary, keyBits) * 15.0 +
        collisionPct * 10.0 +
        ruleGt4Pct * 40.0 +
        avgRules * 20.0 +
        expansionAvg * 0.50 +
        haoEstimateFootprintMiB(stats, summary.totalRules, keyBits) * 0.30 +
        static_cast<double>(stats.maxRulesPerBucket) * 0.10;
}

static
std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> haoChooseDynamicDotVector(
    const std::vector<hwlmLiteral> &lits, u64a dotInputMask,
    const std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> &fallback) {
    auto candidates = haoGenerateDotVectorCandidates(lits, dotInputMask);
    HAODotVectorCandidate best;

    for (const auto &candidate : candidates) {
        HAODotVectorCandidate current =
            haoEvaluateDotVectorCandidate(lits, candidate, dotInputMask);
        if (!current.valid) {
            continue;
        }
        if (!best.valid || current.cost < best.cost ||
            (current.cost == best.cost &&
             current.hashStats.collisionBuckets <
                 best.hashStats.collisionBuckets)) {
            best = std::move(current);
        }
    }

    if (!best.valid) {
        return haoApplyDotInputMaskToVector(fallback, dotInputMask);
    }
    return best.dotVector;
}

static
HAODotHashChoice haoEvaluateDotHashChoice(
    const std::vector<hwlmLiteral> &lits,
    const std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> &baseVector,
    u32 maxInputMaskBits, bool allowDynamicVector) {
    HAODotHashChoice result;
    std::vector<HAOCompiledRulePlan> plans;

    result.dotInputMask = haoChooseDotInputMask(lits, baseVector,
                                                maxInputMaskBits);
    result.dotVector = allowDynamicVector
                           ? haoChooseDynamicDotVector(lits,
                                                       result.dotInputMask,
                                                       baseVector)
                           : haoApplyDotInputMaskToVector(baseVector,
                                                          result.dotInputMask);
    if (!haoDotVectorHasNonZeroLane(result.dotVector)) {
        return result;
    }
    haoBuildDotPlans(lits, result.dotVector, result.dotInputMask,
                     HAO_LAYOUT_KEY_BITS, &plans, &result.summary);
    if (result.summary.unsupportedRules ||
        result.summary.fastPathRules != result.summary.totalRules ||
        result.summary.maskRules != result.summary.maskMergedRules) {
        result.flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
        return result;
    }
    if (!haoDryRunHashStats(plans, HAO_LAYOUT_KEY_BITS, &result.hashStats,
                            &result.flags)) {
        result.flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
        return result;
    }

    result.cost = haoDotHashChoiceCost(result.summary, result.hashStats,
                                       HAO_LAYOUT_KEY_BITS, result.flags);
    result.valid = std::isfinite(result.cost);
    return result;
}

static
HAODotHashChoice haoChooseDotHash(
    const std::vector<hwlmLiteral> &lits,
    const std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> &baseVector) {
    static constexpr std::array<u32, 3> CANDIDATE_MAX_BITS = {{
        24U, 28U, HAO_DOT_INPUT_MASK_MAX_BITS
    }};
    HAODotHashChoice best;
    std::unordered_set<u64a> seenMasks;

    for (const u32 maxBits : CANDIDATE_MAX_BITS) {
        HAODotHashChoice current =
            haoEvaluateDotHashChoice(lits, baseVector, maxBits);
        if (!current.valid) {
            continue;
        }
        if (!seenMasks.insert(current.dotInputMask).second) {
            continue;
        }
        if (!best.valid || current.cost < best.cost ||
            (current.cost == best.cost &&
             current.hashStats.collisionBuckets <
                 best.hashStats.collisionBuckets)) {
            best = std::move(current);
        }
    }

    return best;
}

static
std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES>
haoDotVectorForKnownSuffixBytes(u32 knownBytes) {
    std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> dotVector =
        HAO_DOT_DEFAULT_VECTOR;

    knownBytes = std::min<u32>(knownBytes, HAO_LAYOUT_BYTES_PER_RULE_SLOT);
    const u32 firstKnownByte = HAO_LAYOUT_BYTES_PER_RULE_SLOT - knownBytes;
    for (u32 lane = 0; lane < HAO_LAYOUT_DOT_VECTOR_LANES; lane++) {
        const u32 byteBase = lane * 2U;
        if (byteBase < firstKnownByte ||
            byteBase + 1U < firstKnownByte) {
            dotVector[lane] = 0;
        }
    }
    return dotVector;
}

static
u32 haoDotKnownSuffixBytesForLen(u32 len) {
    len = std::min<u32>(len, HAO_LAYOUT_BYTES_PER_RULE_SLOT);
    return (len / 2U) * 2U;
}

static
int haoDotGroupIndexForKnownBytes(u32 knownBytes) {
    switch (knownBytes) {
    case 2U:
        return 0;
    case 4U:
        return 1;
    case 6U:
        return 2;
    case 8U:
        return 3;
    default:
        return -1;
    }
}

struct HAODotGroupDryRun {
    const char *name = nullptr;
    u32 knownBytes = 0;
    std::array<u16, HAO_LAYOUT_DOT_VECTOR_LANES> dotVector = {};
    std::vector<HAOLiteralRef> refs;
};

static
u32 haoEnvU32Clamped(const char *name, u32 defaultValue, u32 minValue,
                     u32 maxValue) {
    const char *env = getenv(name);
    char *end = nullptr;

    if (!env || !*env || minValue > maxValue) {
        return defaultValue;
    }

    const unsigned long parsed = strtoul(env, &end, 10);
    if (end == env) {
        return defaultValue;
    }

    const u32 value = verify_u32(std::min<unsigned long>(
        parsed, static_cast<unsigned long>(std::numeric_limits<u32>::max())));
    return std::max(minValue, std::min(value, maxValue));
}

struct HAODotGroupDryRunResult {
    HAOCompileSummary summary;
    HAOHashStats stats;
    u32 flags = 0;
    u32 keyBits = 0;
    bool ok = false;
    double cost = std::numeric_limits<double>::infinity();
};

static
HAODotGroupDryRunResult haoRunDotGroupDryRunKeyBits(
    const HAODotGroupDryRun &group, u32 keyBits) {
    HAODotGroupDryRunResult result;
    std::vector<HAOCompiledRulePlan> plans;

    result.keyBits = keyBits;
    if (group.refs.empty()) {
        result.ok = true;
        result.cost = 0.0;
        return result;
    }

    haoBuildDotPlansFromRefs(group.refs, group.dotVector, ~0ULL, keyBits,
                             &plans, &result.summary);
    if (result.summary.unsupportedRules ||
        result.summary.fastPathRules != result.summary.totalRules ||
        result.summary.maskRules != result.summary.maskMergedRules) {
        result.flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
    } else {
        result.ok = haoDryRunHashStats(plans, keyBits, &result.stats,
                                       &result.flags);
    }

    if (!result.ok) {
        result.flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
    }
    result.cost = haoAutoStatsCost(result.summary, result.stats, keyBits,
                                   result.flags);
    return result;
}

static
void haoDumpDotGroupDryRun(const std::vector<hwlmLiteral> &lits) {
    std::array<HAODotGroupDryRun, HAO_LAYOUT_DOT_VECTOR_LANES> groups;
    const char *names[HAO_LAYOUT_DOT_VECTOR_LANES] = {
        "G2", "G4", "G6", "G8"
    };
    const u32 defaultMinKeyBits = std::min<u32>(
        HAO_DOT_GROUP_DRYRUN_DEFAULT_MIN_KEY_BITS, HAO_LAYOUT_KEY_BITS);
    const u32 minKeyBits = haoEnvU32Clamped(
        "HS_HAO_DOT_GROUP_MIN_BITS", defaultMinKeyBits, 1U,
        HAO_LAYOUT_KEY_BITS);
    const u32 maxKeyBits = haoEnvU32Clamped(
        "HS_HAO_DOT_GROUP_MAX_BITS", HAO_LAYOUT_KEY_BITS, minKeyBits,
        HAO_LAYOUT_KEY_BITS);
    u32 skippedRules = 0;

    for (u32 i = 0; i < HAO_LAYOUT_DOT_VECTOR_LANES; i++) {
        groups[i].name = names[i];
        groups[i].knownBytes = (i + 1U) * 2U;
        groups[i].dotVector =
            haoDotVectorForKnownSuffixBytes(groups[i].knownBytes);
    }

    for (u32 i = 0; i < lits.size(); i++) {
        const u32 knownBytes =
            haoDotKnownSuffixBytesForLen(verify_u32(lits[i].s.size()));
        const int groupIndex = haoDotGroupIndexForKnownBytes(knownBytes);
        if (groupIndex < 0) {
            skippedRules++;
            continue;
        }

        HAOLiteralRef ref;
        ref.lit = &lits[i];
        ref.ruleIndex = i;
        groups[static_cast<u32>(groupIndex)].refs.push_back(ref);
    }

    printf("[HAO][DotGroup-DryRun] keyBitsRange=%u..%u "
           "defaultVector=[%u,%u,%u,%u]\n",
           minKeyBits, maxKeyBits, HAO_DOT_DEFAULT_VECTOR[0],
           HAO_DOT_DEFAULT_VECTOR[1], HAO_DOT_DEFAULT_VECTOR[2],
           HAO_DOT_DEFAULT_VECTOR[3]);
    printf("  group keyBits best knownBytes rules fast unsupported maskRules "
           "maskMerged expandedKeys keyExpanded maxAmbig nonEmptyPrimary "
           "occupancyPct collisionPct avgRules maxRules avgEntries maxEntries "
           "entryGt4Pct cost flags dotVector\n");

    for (const auto &group : groups) {
        std::vector<HAODotGroupDryRunResult> results;
        size_t bestIndex = std::numeric_limits<size_t>::max();
        double bestCost = std::numeric_limits<double>::infinity();

        results.reserve(maxKeyBits - minKeyBits + 1U);
        for (u32 keyBits = minKeyBits; keyBits <= maxKeyBits; keyBits++) {
            HAODotGroupDryRunResult result =
                haoRunDotGroupDryRunKeyBits(group, keyBits);
            if (std::isfinite(result.cost) &&
                (bestIndex == std::numeric_limits<size_t>::max() ||
                 result.cost < bestCost ||
                 (result.cost == bestCost &&
                  result.keyBits < results[bestIndex].keyBits))) {
                bestIndex = results.size();
                bestCost = result.cost;
            }
            results.push_back(std::move(result));
        }

        for (size_t i = 0; i < results.size(); i++) {
            const auto &result = results[i];
            const auto &summary = result.summary;
            const auto &stats = result.stats;
            const double occupancyPct =
                haoPrimaryOccupancyPct(stats.nonEmptyPrimary,
                                       result.keyBits);
            const double collisionPct =
                haoPct(stats.collisionBuckets, stats.nonEmptyPrimary);
            const double avgRules = haoAvgRulesPerBucket(stats);
            const double avgEntries = haoAvgEntriesPerBucket(stats);
            const double entryGt4Pct =
                haoPct(stats.entryBucketsGt4, stats.nonEmptyPrimary);

            printf("  %-5s %7u %4s %10u %5u %4u %11u %9u %10u "
                   "%12u %11u %8u %15u %12.5f %12.5f %8.5f "
                   "%8u %10.5f %10u %11.5f %10.3f 0x%08x "
                   "[%u,%u,%u,%u]\n",
                   group.name, result.keyBits,
                   i == bestIndex ? "yes" : "no", group.knownBytes,
                   summary.totalRules, summary.fastPathRules,
                   summary.unsupportedRules, summary.maskRules,
                   summary.maskMergedRules, summary.totalExpandedKeys,
                   summary.keyExpandedRules, summary.maxSelectedAmbigBits,
                   stats.nonEmptyPrimary, occupancyPct, collisionPct,
                   avgRules, stats.maxRulesPerBucket, avgEntries,
                   stats.maxEntriesPerKey, entryGt4Pct, result.cost,
                   result.flags, group.dotVector[0], group.dotVector[1],
                   group.dotVector[2], group.dotVector[3]);
        }
    }

    if (skippedRules) {
        printf("  skippedRules(<2 known suffix bytes): %u\n", skippedRules);
    }
}

static
void haoDotGroupKeyBitRange(u32 *minKeyBits, u32 *maxKeyBits) {
    assert(minKeyBits);
    assert(maxKeyBits);

    const u32 defaultMinKeyBits = std::min<u32>(
        HAO_DOT_GROUP_DRYRUN_DEFAULT_MIN_KEY_BITS, HAO_LAYOUT_KEY_BITS);
    *minKeyBits = haoEnvU32Clamped(
        "HS_HAO_DOT_GROUP_MIN_BITS", defaultMinKeyBits, 1U,
        HAO_LAYOUT_KEY_BITS);
    *maxKeyBits = haoEnvU32Clamped(
        "HS_HAO_DOT_GROUP_MAX_BITS", HAO_LAYOUT_KEY_BITS, *minKeyBits,
        HAO_LAYOUT_KEY_BITS);
}

static
void haoInitDotGroupRefs(
    const std::vector<hwlmLiteral> &lits,
    std::array<HAODotGroupDryRun, HAO_LAYOUT_DOT_VECTOR_LANES> *groups,
    u32 *skippedRules) {
    static const char *names[HAO_LAYOUT_DOT_VECTOR_LANES] = {
        "G2", "G4", "G6", "G8"
    };

    assert(groups);
    if (skippedRules) {
        *skippedRules = 0;
    }

    for (u32 i = 0; i < HAO_LAYOUT_DOT_VECTOR_LANES; i++) {
        (*groups)[i] = HAODotGroupDryRun();
        (*groups)[i].name = names[i];
        (*groups)[i].knownBytes = (i + 1U) * 2U;
        (*groups)[i].dotVector =
            haoDotVectorForKnownSuffixBytes((*groups)[i].knownBytes);
    }

    for (u32 i = 0; i < lits.size(); i++) {
        const u32 knownBytes =
            haoDotKnownSuffixBytesForLen(verify_u32(lits[i].s.size()));
        const int groupIndex = haoDotGroupIndexForKnownBytes(knownBytes);
        if (groupIndex < 0) {
            if (skippedRules) {
                (*skippedRules)++;
            }
            continue;
        }

        HAOLiteralRef ref;
        ref.lit = &lits[i];
        ref.ruleIndex = i;
        (*groups)[static_cast<u32>(groupIndex)].refs.push_back(ref);
    }
}

static
void haoAccumulateSummary(HAOCompileSummary *dst,
                          const HAOCompileSummary &src) {
    assert(dst);

    dst->totalRules += src.totalRules;
    dst->fastPathRules += src.fastPathRules;
    dst->unsupportedRules += src.unsupportedRules;
    dst->maskRules += src.maskRules;
    dst->maskMergedRules += src.maskMergedRules;
    dst->maskConflictRules += src.maskConflictRules;
    dst->maskConfirmRules += src.maskConfirmRules;
    dst->exactRules += src.exactRules;
    dst->nocaseRules += src.nocaseRules;
    dst->keyExpandedRules += src.keyExpandedRules;
    dst->totalExpandedKeys += src.totalExpandedKeys;
    dst->maxSelectedAmbigBits =
        std::max(dst->maxSelectedAmbigBits, src.maxSelectedAmbigBits);
    dst->totalLiteralBytes += src.totalLiteralBytes;
    if (src.minLiteralLen &&
        (!dst->minLiteralLen || src.minLiteralLen < dst->minLiteralLen)) {
        dst->minLiteralLen = src.minLiteralLen;
    }
    dst->maxLiteralLen = std::max(dst->maxLiteralLen, src.maxLiteralLen);
    dst->literalLenLe4 += src.literalLenLe4;
    dst->literalLen5To8 += src.literalLen5To8;
}

static
void haoAccumulateHashStats(HAOHashStats *dst, const HAOHashStats &src) {
    assert(dst);

    dst->nonEmptyPrimary += src.nonEmptyPrimary;
    dst->collisionBuckets += src.collisionBuckets;
    dst->totalRulesInBuckets += src.totalRulesInBuckets;
    dst->totalExpandedKeysInBuckets += src.totalExpandedKeysInBuckets;
    dst->totalL2Entries += src.totalL2Entries;
    if (src.minRulesPerBucket &&
        (!dst->minRulesPerBucket ||
         src.minRulesPerBucket < dst->minRulesPerBucket)) {
        dst->minRulesPerBucket = src.minRulesPerBucket;
    }
    dst->maxRulesPerBucket =
        std::max(dst->maxRulesPerBucket, src.maxRulesPerBucket);
    if (src.minEntriesPerBucket &&
        (!dst->minEntriesPerBucket ||
         src.minEntriesPerBucket < dst->minEntriesPerBucket)) {
        dst->minEntriesPerBucket = src.minEntriesPerBucket;
    }
    dst->maxEntriesPerKey =
        std::max(dst->maxEntriesPerKey, src.maxEntriesPerKey);
    dst->ruleBucketsEq1 += src.ruleBucketsEq1;
    dst->ruleBuckets2To4 += src.ruleBuckets2To4;
    dst->ruleBucketsGt4 += src.ruleBucketsGt4;
    dst->entryBucketsEq1 += src.entryBucketsEq1;
    dst->entryBuckets2To4 += src.entryBuckets2To4;
    dst->entryBucketsGt4 += src.entryBucketsGt4;
}

static
bool haoSelectDotGroupKeyBits(const HAODotGroupDryRun &group,
                              u32 *bestKeyBits) {
    u32 minKeyBits = 0;
    u32 maxKeyBits = 0;
    bool haveBest = false;
    double bestCost = std::numeric_limits<double>::infinity();

    assert(bestKeyBits);
    haoDotGroupKeyBitRange(&minKeyBits, &maxKeyBits);
    for (u32 keyBits = minKeyBits; keyBits <= maxKeyBits; keyBits++) {
        const HAODotGroupDryRunResult result =
            haoRunDotGroupDryRunKeyBits(group, keyBits);
        if (!std::isfinite(result.cost)) {
            continue;
        }
        if (!haveBest || result.cost < bestCost ||
            (result.cost == bestCost && keyBits < *bestKeyBits)) {
            haveBest = true;
            bestCost = result.cost;
            *bestKeyBits = keyBits;
        }
    }

    return haveBest;
}

static
bool haoCompileDotGroupCore(const std::vector<hwlmLiteral> &lits,
                            HAOCompileArtifacts *artifacts) {
    std::array<HAODotGroupDryRun, HAO_LAYOUT_DOT_VECTOR_LANES> groups;
    u32 skippedRules = 0;

    if (!artifacts || lits.empty()) {
        return false;
    }

    *artifacts = HAOCompileArtifacts();
    artifacts->hashMode = HAO_LAYOUT_HASH_DOT_GROUP;
    artifacts->selectorName = "dot_group-forced";
    artifacts->bextMask = 0;
    artifacts->selectors.clear();
    artifacts->hash.valid = true;
    artifacts->hash.keyBits = 0;

    haoInitDotGroupRefs(lits, &groups, &skippedRules);
    if (skippedRules) {
        artifacts->hash.flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
        return false;
    }

    for (const auto &groupRef : groups) {
        u32 bestKeyBits = 0;
        HAODotGroupBuild group;

        if (groupRef.refs.empty()) {
            continue;
        }
        if (!haoSelectDotGroupKeyBits(groupRef, &bestKeyBits)) {
            artifacts->hash.flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
            return false;
        }

        group.name = groupRef.name;
        group.knownBytes = groupRef.knownBytes;
        group.dotVector = groupRef.dotVector;
        haoBuildDotPlansFromRefs(groupRef.refs, group.dotVector, ~0ULL,
                                 bestKeyBits, &group.plans,
                                 &group.summary);
        if (group.summary.unsupportedRules ||
            group.summary.fastPathRules != group.summary.totalRules ||
            group.summary.maskRules != group.summary.maskMergedRules) {
            artifacts->hash.flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
            return false;
        }

        haoBuildTables(group.plans, bestKeyBits, &group.hash);
        if (!group.hash.valid ||
            group.hash.flags & HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE) {
            artifacts->hash.flags |= group.hash.flags |
                                     HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
            return false;
        }

        artifacts->hash.keyBits =
            std::max(artifacts->hash.keyBits, group.hash.keyBits);
        artifacts->hash.flags |= group.hash.flags;
        haoAccumulateSummary(&artifacts->summary, group.summary);
        haoAccumulateHashStats(&artifacts->hash.stats, group.hash.stats);
        artifacts->plans.insert(artifacts->plans.end(), group.plans.begin(),
                                group.plans.end());
        artifacts->dotGroups.push_back(std::move(group));
    }

    if (artifacts->dotGroups.empty() ||
        artifacts->summary.totalRules != lits.size()) {
        artifacts->hash.flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
        return false;
    }

    haoBuildMeta(lits, &artifacts->meta);
    return true;
}

static
bool haoBuildCandidateStats(const std::vector<hwlmLiteral> &lits,
                            HAOSelectorMode mode, const char *name,
                            u32 targetBits, HAOCandidateStats *out) {
    if (!out || lits.empty()) {
        return false;
    }

    *out = HAOCandidateStats();
    out->name = name;
    out->mode = mode;

    std::vector<HAOBitSelector> selectors;
    std::vector<HAOCompiledRulePlan> plans;
    u32 keyBits = 0;

    haoSelectBits(lits, &selectors, &keyBits, mode, targetBits);
    if (selectors.empty()) {
        return false;
    }

    haoBuildPlans(lits, selectors, &plans, &out->summary);
    out->keyBits = keyBits;

    if (out->summary.unsupportedRules ||
        out->summary.fastPathRules != out->summary.totalRules ||
        out->summary.maskRules != out->summary.maskMergedRules) {
        out->flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
        return false;
    }
    if (!haoDryRunHashStats(plans, keyBits, &out->hashStats,
                            &out->flags)) {
        return false;
    }

    out->cost = haoAutoStatsCost(out->summary, out->hashStats,
                                 out->keyBits, out->flags);
    return std::isfinite(out->cost);
}

static
bool haoBuildWithMode(const std::vector<hwlmLiteral> &lits,
                      HAOSelectorMode mode, const char *name,
                      u32 targetBits, HAOCompileArtifacts *artifacts) {
    assert(artifacts);

    *artifacts = HAOCompileArtifacts();
    artifacts->selectorName = name;
    return haoCompileCore(lits, artifacts, mode, targetBits);
}

static
bool haoBuildAuto(const std::vector<hwlmLiteral> &lits,
                  HAOCompileArtifacts *artifacts) {
    assert(artifacts);

    struct Candidate {
        HAOSelectorMode mode;
        const char *name;
    };
    static constexpr Candidate candidates[] = {
        {HAOSelectorMode::DEFAULT, "default-auto"},
        {HAOSelectorMode::DYNAMIC_EXPANSION, "dynamic-auto"},
        {HAOSelectorMode::HIGH_ALIGN, "high_align-auto"},
#if HAO_AUTO_INCLUDE_HASH_OPT
        {HAOSelectorMode::HASH_OPT, "hash_opt-auto"},
#endif
    };

    bool haveBest = false;
    double bestCost = std::numeric_limits<double>::infinity();
    HAOSelectorMode bestMode = HAOSelectorMode::DEFAULT;
    const char *bestName = nullptr;
    u32 bestKeyBits = 0;
    const u32 minKeyBits = std::min<u32>(HAO_AUTO_MIN_KEY_BITS,
                                         HAO_LAYOUT_KEY_BITS);

    for (u32 keyBits = minKeyBits; keyBits <= HAO_LAYOUT_KEY_BITS; keyBits++) {
        for (const auto &candidate : candidates) {
            HAOCandidateStats current;
            if (!haoBuildCandidateStats(lits, candidate.mode, candidate.name,
                                        keyBits, &current)) {
                continue;
            }

            if (!std::isfinite(current.cost)) {
                continue;
            }
            if (!haveBest || current.cost < bestCost ||
                (current.cost == bestCost && current.keyBits > bestKeyBits)) {
                haveBest = true;
                bestCost = current.cost;
                bestMode = current.mode;
                bestName = current.name;
                bestKeyBits = current.keyBits;
            }
        }
    }

    if (!haveBest) {
        return false;
    }

    assert(bestName);
    return haoBuildWithMode(lits, bestMode, bestName, bestKeyBits, artifacts);
}

void haoDumpArtifacts(const std::vector<hwlmLiteral> &lits,
                             const HAOCompileArtifacts &artifacts) {
    printf("\n========== [HAO][Build-Artifacts] Begin ==========\n");
    printf("[HAO][Params] key_bits(fixed=%u, selector_count=%zu) l2_key_bits=%u l2_capacity=%u l2_entry_capacity=%u\n",
           artifacts.hash.keyBits, artifacts.selectors.size(),
           HAO_BUILD_L2_KEY_BITS, HAO_BUILD_MAX_L2_ENTRIES,
           HAO_LAYOUT_RULE_SLOTS_PER_ENTRY);
    dumpRuleBits(lits);
    if (artifacts.hashMode == HAO_LAYOUT_HASH_BEXT) {
        dumpSelectors(artifacts.selectors);
    }
    dumpExtractDescriptor(artifacts);
    if (artifacts.hashMode == HAO_LAYOUT_HASH_BEXT) {
        dumpRuleKeys(lits, artifacts.selectors, artifacts.hash.keyBits);
    }
    dumpHAOSummary(artifacts);
    printf("[HAO][Flags] hash.flags=0x%x\n", artifacts.hash.flags);
    printf("========== [HAO][Build-Artifacts] End ==========\n\n");
}

} // namespace

template <class ReasonT>
static
const char *haoReasonName(ReasonT reason) {
    switch (reason) {
    case ReasonT::OK:
        return "OK";
    case ReasonT::GREY_DISABLED:
        return "GREY_DISABLED";
    case ReasonT::ARCH_UNSUPPORTED:
        return "ARCH_UNSUPPORTED";
    case ReasonT::TOO_FEW_LITERALS:
        return "TOO_FEW_LITERALS";
    case ReasonT::TOO_MANY_LITERALS:
        return "TOO_MANY_LITERALS";
    case ReasonT::UNSUPPORTED_LITERAL:
        return "UNSUPPORTED_LITERAL";
    case ReasonT::NO_SELECTORS:
        return "NO_SELECTORS";
    case ReasonT::PARTIAL_L2_CAPACITY:
        return "PARTIAL_L2_CAPACITY";
    case ReasonT::PARTIAL_ENTRY_OVERFLOW:
        return "PARTIAL_ENTRY_OVERFLOW";
    case ReasonT::PARTIAL_OTHER:
        return "PARTIAL_OTHER";
    case ReasonT::ARTIFACT_BUILD_FAILED:
        return "ARTIFACT_BUILD_FAILED";
    default:
        return "UNKNOWN";
    }
}

const char *haoFeasibilityReasonName(HAOFeasibilityReason reason) {
    return haoReasonName(reason);
}

bool haoCanUseBextFastPath(const target_t &target) {
#if defined(__ARM_FEATURE_SVE2_BITPERM)
    return target.has_sve_bitperm();
#else
    (void)target;
    return false;
#endif
}

bool haoHasSveBitPermPrereq(const target_t &target) {
#if defined(__ARM_FEATURE_SVE2_BITPERM)
    return target.has_sve_bitperm();
#else
    (void)target;
    return false;
#endif
}

static
bool buildHAOArtifactsWithPolicy(const std::vector<hwlmLiteral> &lits,
                                 HAOCompileArtifacts *artifacts,
                                 HAODumpMode dumpMode,
                                 HAOHashPolicy hashPolicy,
                                 const char *selectorNameOverride = nullptr) {
    if (!artifacts) {
        return false;
    }

    if (hashPolicy == HAOHashPolicy::DOT_GROUP_DRYRUN) {
        haoDumpDotGroupDryRun(lits);
    }

    if (hashPolicy == HAOHashPolicy::DOT) {
        *artifacts = HAOCompileArtifacts();
        if (!haoCompileDotCore(lits, artifacts)) {
            return false;
        }
    } else if (hashPolicy == HAOHashPolicy::DOT_GROUP) {
        if (!haoCompileDotGroupCore(lits, artifacts)) {
            return false;
        }
    } else {
        const HAOSelectorPolicy policy = haoSelectorPolicy();
        if (policy == HAOSelectorPolicy::AUTO) {
            if (!haoBuildAuto(lits, artifacts)) {
                return false;
            }
        } else {
            const HAOSelectorMode mode = haoSelectorMode(policy);
            const char *selectorMode = haoSelectorName(policy);
            if (!haoBuildWithMode(lits, mode, selectorMode,
                                  HAO_LAYOUT_KEY_BITS, artifacts)) {
                return false;
            }
        }
    }

    if (selectorNameOverride) {
        artifacts->selectorName = selectorNameOverride;
    }

    if (dumpMode == HAODumpMode::Full) {
        printf("[HAO][Selector] mode=%s\n", artifacts->selectorName);
        haoDumpArtifacts(lits, *artifacts);
    } else if (dumpMode == HAODumpMode::SummaryIfEnabled &&
               haoStatsDumpEnabled()) {
        printf("[HAO][Selector] mode=%s\n", artifacts->selectorName);
        dumpHAOSummary(*artifacts);
    }
    return true;
}

bool buildHAOArtifacts(const std::vector<hwlmLiteral> &lits,
                       HAOCompileArtifacts *artifacts,
                       HAODumpMode dumpMode) {
    HAOHashPolicy hashPolicy = haoHashPolicy();
    bool defaultMappedToDot = false;
#if !defined(__ARM_FEATURE_SVE2_BITPERM)
    if (hashPolicy == HAOHashPolicy::BEXT) {
        if (haoHashPolicyIsExplicit()) {
            return false;
        }
        hashPolicy = HAOHashPolicy::DOT;
        defaultMappedToDot = true;
    }
#endif
    return buildHAOArtifactsWithPolicy(lits, artifacts, dumpMode, hashPolicy,
                                       defaultMappedToDot ? "default-auto" :
                                                            nullptr);
}

void dumpHAOCompileStats(const HAOCompileArtifacts &artifacts) {
    printf("[HAO][Selector] mode=%s\n", artifacts.selectorName);
    dumpHAOSummary(artifacts);
}

void dumpHAOL2MapIfEnabled(const std::vector<hwlmLiteral> &lits,
                           const HAOCompileArtifacts &artifacts) {
    if (haoL2MapDumpEnabled()) {
        dumpL2Map(lits, artifacts);
    }
}

bool haoArtifactsOk(const HAOCompileArtifacts &artifacts) {
    const HAOCompileSummary &summary = artifacts.summary;

    if (!artifacts.hash.valid) {
        return false;
    }
    if (artifacts.hash.flags & HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE) {
        return false;
    }
    if (summary.fastPathRules != summary.totalRules ||
        summary.unsupportedRules ||
        summary.maskRules != summary.maskMergedRules) {
        return false;
    }
    return true;
}

bool refreshHAOReports(HAOCompileArtifacts *artifacts,
                       const std::vector<hwlmLiteral> &lits) {
    if (!artifacts || artifacts->meta.size() != lits.size()) {
        return false;
    }

    for (u32 i = 0; i < lits.size(); i++) {
        artifacts->meta[i].id = lits[i].id;
        artifacts->meta[i].groups = lits[i].groups;
    }
    return true;
}

bool analyzeHAOFeasibility(const target_t &target,
                           const std::vector<hwlmLiteral> &lits,
                           const Grey &grey, HAOFeasibilityResult *result,
                           HAOCompileArtifacts *artifacts) {
    HAOFeasibilityResult local;
    local.canBuild = false;
    local.reason = HAOFeasibilityReason::ARTIFACT_BUILD_FAILED;
    local.flags = 0;

    auto finish = [&](HAOFeasibilityReason reason, bool canBuild = false) {
        local.canBuild = canBuild;
        local.reason = reason;
        if (result) {
            *result = local;
        }
        return canBuild;
    };

    if (!grey.allowHao) {
        return finish(HAOFeasibilityReason::GREY_DISABLED);
    }

#if !defined(__aarch64__)
    (void)target;
    return finish(HAOFeasibilityReason::ARCH_UNSUPPORTED);
#else
    if (!target.has_sve()) {
        return finish(HAOFeasibilityReason::ARCH_UNSUPPORTED);
    }
#endif

    if (lits.empty()) {
        return finish(HAOFeasibilityReason::TOO_FEW_LITERALS);
    }

    if (lits.size() > HAO_MAX_LITERALS) {
        return finish(HAOFeasibilityReason::TOO_MANY_LITERALS);
    }

    std::unique_ptr<HAOCompileArtifacts> tempStorage;
    HAOCompileArtifacts *out = artifacts;
    if (!out) {
        tempStorage.reset(new HAOCompileArtifacts());
        out = tempStorage.get();
    }

    const HAOHashPolicy requestedHashPolicy = haoHashPolicy();
    const bool explicitHashPolicy = haoHashPolicyIsExplicit();
    const bool canUseBext = haoCanUseBextFastPath(target);
    HAOHashPolicy effectiveHashPolicy = requestedHashPolicy;
    bool defaultMappedToDot = false;

    if (!canUseBext && lits.size() < HAO_RUNTIME_RULE_SLOTS_PER_ENTRY) {
        return finish(HAOFeasibilityReason::TOO_FEW_LITERALS);
    }

    if (!canUseBext && explicitHashPolicy &&
        requestedHashPolicy == HAOHashPolicy::BEXT) {
        return finish(HAOFeasibilityReason::ARCH_UNSUPPORTED);
    }
    if (!canUseBext && !explicitHashPolicy &&
        requestedHashPolicy == HAOHashPolicy::BEXT) {
        effectiveHashPolicy = HAOHashPolicy::DOT;
        defaultMappedToDot = true;
    }

    bool built = buildHAOArtifactsWithPolicy(lits, out, HAODumpMode::None,
                                             effectiveHashPolicy);
    if (built && defaultMappedToDot) {
        out->selectorName = "default-auto";
    }

    if (!built) {
        local.flags = out->hash.flags;
        if (out->hashMode == HAO_LAYOUT_HASH_BEXT && out->selectors.empty()) {
            return finish(HAOFeasibilityReason::NO_SELECTORS);
        }
        if (out->summary.unsupportedRules ||
            out->summary.fastPathRules != out->summary.totalRules ||
            out->summary.maskRules != out->summary.maskMergedRules) {
            return finish(HAOFeasibilityReason::UNSUPPORTED_LITERAL);
        }
        return finish(HAOFeasibilityReason::ARTIFACT_BUILD_FAILED);
    }

    local.flags = out->hash.flags;
    if (out->summary.unsupportedRules ||
        out->summary.fastPathRules != out->summary.totalRules ||
        out->summary.maskRules != out->summary.maskMergedRules) {
        return finish(HAOFeasibilityReason::UNSUPPORTED_LITERAL);
    }

    if (!haoArtifactsOk(*out)) {
        if (out->hash.flags & HAO_ARTIFACT_FLAG_PARTIAL_L2_CAPACITY) {
            local.reason = HAOFeasibilityReason::PARTIAL_L2_CAPACITY;
        } else if (out->hash.flags & HAO_ARTIFACT_FLAG_PARTIAL_ENTRY_OVERFLOW) {
            local.reason = HAOFeasibilityReason::PARTIAL_ENTRY_OVERFLOW;
        } else {
            local.reason = HAOFeasibilityReason::PARTIAL_OTHER;
        }
        return finish(local.reason);
    }

    return finish(HAOFeasibilityReason::OK, true);
}

bool canBuildHAO(const target_t &target, const std::vector<hwlmLiteral> &lits,
                 const Grey &grey) {
    HAOFeasibilityResult result;
    return analyzeHAOFeasibility(target, lits, grey, &result, nullptr);
}

template <class ArtifactsT>
static
bytecode_ptr<u8> haoBuildBlobImpl(const ArtifactsT &artifacts) {
    if (!artifacts.hash.valid) {
        return nullptr;
    }
    if (artifacts.plans.size() != artifacts.meta.size()) {
        return nullptr;
    }

    const u32 keyBits = verify_u32(artifacts.hash.keyBits);
    const u32 primaryCount =
        verify_u32(artifacts.hash.primary.offsets.size());
    const u32 primaryBitmapSize =
        verify_u32(artifacts.hash.bitmap.bits.size());
    const u32 l2EntryCount =
        verify_u32(artifacts.hash.l2Check.size());
    if (artifacts.hash.l2Meta.size() != l2EntryCount) {
        return nullptr;
    }
    const u32 ruleMetaCount = verify_u32(artifacts.meta.size());
    const size_t primaryBitmapBytes =
        artifacts.hash.bitmap.bits.size();
    const size_t primaryBytes =
        sizeof(u32) * artifacts.hash.primary.offsets.size();
    const size_t l2CheckBytes =
        sizeof(HAORuntimeL2Check) *
        artifacts.hash.l2Check.size();
    const size_t l2MetaBytes =
        sizeof(HAORuntimeL2Meta) *
        artifacts.hash.l2Meta.size();
    const size_t ruleMetaBytes =
        sizeof(HAORuntimeRuleMeta) * artifacts.meta.size();
    const bool haveL15Tags =
        artifacts.hashMode == HAO_LAYOUT_HASH_BEXT && artifacts.l15TagMask &&
        artifacts.l15TagBits == HAO_L15_TAG_BITS &&
        artifacts.l15Tags.size() == l2EntryCount &&
        !artifacts.l15TagMasks.empty() &&
        artifacts.l15TagMasks.size() <= HAO_L15_TAG_MAX_MASKS;
    const size_t l15TagBytes =
        haveL15Tags ? sizeof(u16) * artifacts.l15Tags.size() : 0;
    const size_t l15MaskTableBytes =
        haveL15Tags ? sizeof(u64a) * artifacts.l15TagMasks.size() : 0;

    size_t totalSize = ROUNDUP_N(sizeof(HAORuntimeHeader), alignof(u32));
    const u32 primaryBitmapOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(primaryBitmapBytes, alignof(u32));
    const u32 primaryOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(primaryBytes, alignof(u32));
    totalSize = ROUNDUP_N(totalSize, HAO_RUNTIME_L2_CHECK_ALIGN);
    const u32 l2CheckOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(l2CheckBytes, HAO_RUNTIME_L2_CHECK_ALIGN);
    const u32 l2MetaOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(l2MetaBytes, alignof(u32));
    const u32 ruleMetaOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(ruleMetaBytes, alignof(u32));
    const u32 l15TagOffset = l15TagBytes ? verify_u32(totalSize) : 0;
    totalSize += ROUNDUP_N(l15TagBytes, alignof(u16));
    totalSize = ROUNDUP_N(totalSize, alignof(u64a));
    const u32 l15MaskTableOffset =
        l15MaskTableBytes ? verify_u32(totalSize) : 0;
    totalSize += ROUNDUP_N(l15MaskTableBytes, alignof(u64a));

    auto blob = make_zeroed_bytecode_ptr<u8>(totalSize, 64);
    if (!blob) {
        return nullptr;
    }

    auto *hdr = reinterpret_cast<HAORuntimeHeader *>(blob.get());
    hdr->magic = HAO_RUNTIME_MAGIC;
    hdr->version = HAO_RUNTIME_VERSION;
    hdr->keyBits = (keyBits & HAO_RUNTIME_KEY_BITS_MASK) |
                   (artifacts.hashMode << HAO_RUNTIME_HASH_MODE_SHIFT);
    hdr->primaryCount = primaryCount;
    hdr->primaryBitmapSize = primaryBitmapSize;
    hdr->l2EntryCount = l2EntryCount;
    hdr->ruleMetaCount = ruleMetaCount;
    hdr->bextMask = artifacts.hashMode == HAO_LAYOUT_HASH_DOT
                        ? haoPackDotVector(artifacts.dotVector)
                        : artifacts.bextMask;
    hdr->dotInputMask = artifacts.hashMode == HAO_LAYOUT_HASH_DOT
                            ? artifacts.dotInputMask
                            : ~0ULL;
    hdr->primaryBitmapOffset = primaryBitmapOffset;
    hdr->primaryOffset = primaryOffset;
    hdr->l2CheckOffset = l2CheckOffset;
    hdr->l2MetaOffset = l2MetaOffset;
    hdr->ruleMetaOffset = ruleMetaOffset;
    hdr->l15TagOffset = haveL15Tags ? l15TagOffset : 0;
    hdr->l15TagCount = haveL15Tags ? l2EntryCount : 0;
    hdr->l15TagBits = haveL15Tags ? artifacts.l15TagBits : 0;
    hdr->l15TagOverlapBits = haveL15Tags
                                 ? artifacts.l15TagOverlapBits
                                 : 0;
    hdr->l15MaskTableOffset = haveL15Tags ? l15MaskTableOffset : 0;
    hdr->l15MaskCount =
        haveL15Tags ? verify_u32(artifacts.l15TagMasks.size()) : 0;

    u8 *base = blob.get();
    if (primaryBitmapBytes) {
        memcpy(base + primaryBitmapOffset,
               artifacts.hash.bitmap.bits.data(),
               primaryBitmapBytes);
    }
    if (primaryBytes) {
        memcpy(base + primaryOffset,
               artifacts.hash.primary.offsets.data(),
               primaryBytes);
    }
    if (l2CheckBytes) {
        auto *checkOut =
            reinterpret_cast<HAORuntimeL2Check *>(base + l2CheckOffset);
        for (u32 i = 0; i < l2EntryCount; i++) {
            const auto &src = artifacts.hash.l2Check[i];
            auto &dst = checkOut[i];
            memcpy(dst.rule, src.rule, sizeof(dst.rule));
            memcpy(dst.mask, src.mask, sizeof(dst.mask));
        }
    }
    if (l2MetaBytes) {
        auto *metaOut =
            reinterpret_cast<HAORuntimeL2Meta *>(base + l2MetaOffset);
        for (u32 i = 0; i < l2EntryCount; i++) {
            const auto &src = artifacts.hash.l2Meta[i];
            auto &dst = metaOut[i];
            memcpy(dst.ruleIndex, src.ruleIndex, sizeof(dst.ruleIndex));
            dst.careBits = src.careBits;
        }
    }

    auto *ruleMetaOut =
        reinterpret_cast<HAORuntimeRuleMeta *>(base + ruleMetaOffset);
    for (u32 i = 0; i < ruleMetaCount; i++) {
        const auto &srcMeta = artifacts.meta[i];
        auto &dst = ruleMetaOut[i];
        dst.id = srcMeta.id;
        dst.flags = srcMeta.flags;
        dst.reserved = 0;
        dst.groups = srcMeta.groups;
    }
    if (l15TagBytes) {
        memcpy(base + l15TagOffset, artifacts.l15Tags.data(), l15TagBytes);
    }
    if (l15MaskTableBytes) {
        memcpy(base + l15MaskTableOffset, artifacts.l15TagMasks.data(),
               l15MaskTableBytes);
    }
    return blob;
}

static
bytecode_ptr<u8> haoBuildDotGroupBlob(
    const HAOCompileArtifacts &artifacts) {
    const u32 groupCount = verify_u32(artifacts.dotGroups.size());
    const u32 ruleMetaCount = verify_u32(artifacts.meta.size());
    const size_t descBytes =
        sizeof(HAORuntimeDotGroupDesc) * artifacts.dotGroups.size();
    const size_t ruleMetaBytes =
        sizeof(HAORuntimeRuleMeta) * artifacts.meta.size();

    if (!artifacts.hash.valid || !groupCount ||
        groupCount > HAO_LAYOUT_DOT_GROUP_COUNT ||
        artifacts.plans.size() != artifacts.meta.size()) {
        return nullptr;
    }

    size_t totalSize = ROUNDUP_N(sizeof(HAORuntimeHeader),
                                 alignof(HAORuntimeDotGroupDesc));
    const u32 groupDescOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(descBytes, HAO_RUNTIME_L2_CHECK_ALIGN);

    struct GroupOffsets {
        u32 primaryBitmapOffset = 0;
        u32 primaryOffset = 0;
        u32 l2CheckOffset = 0;
        u32 l2MetaOffset = 0;
    };
    std::vector<GroupOffsets> offsets(groupCount);

    for (u32 i = 0; i < groupCount; i++) {
        const auto &group = artifacts.dotGroups[i];
        if (!group.hash.valid || !group.hash.keyBits ||
            group.hash.l2Meta.size() != group.hash.l2Check.size()) {
            return nullptr;
        }

        const size_t primaryBitmapBytes = group.hash.bitmap.bits.size();
        const size_t primaryBytes =
            sizeof(u32) * group.hash.primary.offsets.size();
        const size_t l2CheckBytes =
            sizeof(HAORuntimeL2Check) * group.hash.l2Check.size();
        const size_t l2MetaBytes =
            sizeof(HAORuntimeL2Meta) * group.hash.l2Meta.size();

        totalSize = ROUNDUP_N(totalSize, HAO_RUNTIME_L2_CHECK_ALIGN);
        offsets[i].primaryBitmapOffset = verify_u32(totalSize);
        totalSize += ROUNDUP_N(primaryBitmapBytes,
                               HAO_RUNTIME_L2_CHECK_ALIGN);

        totalSize = ROUNDUP_N(totalSize, HAO_RUNTIME_L2_CHECK_ALIGN);
        offsets[i].primaryOffset = verify_u32(totalSize);
        totalSize += ROUNDUP_N(primaryBytes, HAO_RUNTIME_L2_CHECK_ALIGN);

        totalSize = ROUNDUP_N(totalSize, HAO_RUNTIME_L2_CHECK_ALIGN);
        offsets[i].l2CheckOffset = verify_u32(totalSize);
        totalSize += ROUNDUP_N(l2CheckBytes, HAO_RUNTIME_L2_CHECK_ALIGN);

        totalSize = ROUNDUP_N(totalSize, HAO_RUNTIME_L2_CHECK_ALIGN);
        offsets[i].l2MetaOffset = verify_u32(totalSize);
        totalSize += ROUNDUP_N(l2MetaBytes, HAO_RUNTIME_L2_CHECK_ALIGN);
    }

    totalSize = ROUNDUP_N(totalSize, HAO_RUNTIME_L2_CHECK_ALIGN);
    const u32 ruleMetaOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(ruleMetaBytes, HAO_RUNTIME_L2_CHECK_ALIGN);

    auto blob = make_zeroed_bytecode_ptr<u8>(totalSize, 64);
    if (!blob) {
        return nullptr;
    }

    u8 *base = blob.get();
    auto *hdr = reinterpret_cast<HAORuntimeHeader *>(base);
    hdr->magic = HAO_RUNTIME_MAGIC;
    hdr->version = HAO_RUNTIME_VERSION;
    hdr->keyBits = (artifacts.hash.keyBits & HAO_RUNTIME_KEY_BITS_MASK) |
                   (artifacts.hashMode << HAO_RUNTIME_HASH_MODE_SHIFT);
    hdr->primaryCount = groupCount;
    hdr->primaryBitmapSize = verify_u32(descBytes);
    hdr->l2EntryCount = verify_u32(std::max<u32>(
        artifacts.hash.stats.totalL2Entries, 1U));
    hdr->ruleMetaCount = ruleMetaCount;
    hdr->bextMask = 0;
    hdr->dotInputMask = ~0ULL;
    hdr->primaryBitmapOffset = groupDescOffset;
    hdr->primaryOffset = 0;
    hdr->l2CheckOffset = 0;
    hdr->l2MetaOffset = 0;
    hdr->ruleMetaOffset = ruleMetaOffset;
    hdr->l15TagOffset = 0;
    hdr->l15TagCount = 0;
    hdr->l15TagBits = 0;
    hdr->l15TagOverlapBits = 0;
    hdr->l15MaskTableOffset = 0;
    hdr->l15MaskCount = 0;

    auto *descOut =
        reinterpret_cast<HAORuntimeDotGroupDesc *>(base + groupDescOffset);
    for (u32 i = 0; i < groupCount; i++) {
        const auto &group = artifacts.dotGroups[i];
        auto &desc = descOut[i];

        desc.keyBits = group.hash.keyBits;
        desc.primaryCount = verify_u32(group.hash.primary.offsets.size());
        desc.primaryBitmapSize = verify_u32(group.hash.bitmap.bits.size());
        desc.l2EntryCount = verify_u32(group.hash.l2Check.size());
        desc.knownBytes = group.knownBytes;
        desc.reserved = 0;
        desc.dotVector = haoPackDotVector(group.dotVector);
        desc.primaryBitmapOffset = offsets[i].primaryBitmapOffset;
        desc.primaryOffset = offsets[i].primaryOffset;
        desc.l2CheckOffset = offsets[i].l2CheckOffset;
        desc.l2MetaOffset = offsets[i].l2MetaOffset;

        if (!group.hash.bitmap.bits.empty()) {
            memcpy(base + desc.primaryBitmapOffset,
                   group.hash.bitmap.bits.data(),
                   group.hash.bitmap.bits.size());
        }
        if (!group.hash.primary.offsets.empty()) {
            memcpy(base + desc.primaryOffset,
                   group.hash.primary.offsets.data(),
                   sizeof(u32) * group.hash.primary.offsets.size());
        }
        if (!group.hash.l2Check.empty()) {
            auto *checkOut = reinterpret_cast<HAORuntimeL2Check *>(
                base + desc.l2CheckOffset);
            for (u32 n = 0; n < desc.l2EntryCount; n++) {
                const auto &src = group.hash.l2Check[n];
                auto &dst = checkOut[n];
                memcpy(dst.rule, src.rule, sizeof(dst.rule));
                memcpy(dst.mask, src.mask, sizeof(dst.mask));
            }
        }
        if (!group.hash.l2Meta.empty()) {
            auto *metaOut = reinterpret_cast<HAORuntimeL2Meta *>(
                base + desc.l2MetaOffset);
            for (u32 n = 0; n < desc.l2EntryCount; n++) {
                const auto &src = group.hash.l2Meta[n];
                auto &dst = metaOut[n];
                memcpy(dst.ruleIndex, src.ruleIndex, sizeof(dst.ruleIndex));
                dst.careBits = src.careBits;
            }
        }
    }

    auto *ruleMetaOut =
        reinterpret_cast<HAORuntimeRuleMeta *>(base + ruleMetaOffset);
    for (u32 i = 0; i < ruleMetaCount; i++) {
        const auto &srcMeta = artifacts.meta[i];
        auto &dst = ruleMetaOut[i];
        dst.id = srcMeta.id;
        dst.flags = srcMeta.flags;
        dst.reserved = 0;
        dst.groups = srcMeta.groups;
    }

    return blob;
}

bytecode_ptr<u8> buildHAOBlob(const HAOCompileArtifacts &artifacts) {
    if (artifacts.hashMode == HAO_LAYOUT_HASH_DOT_GROUP) {
        return haoBuildDotGroupBlob(artifacts);
    }
    return haoBuildBlobImpl(artifacts);
}

} // namespace ue2
