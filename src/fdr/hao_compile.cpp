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

namespace {

static constexpr u32 HAO_BUILD_MAX_SUFFIX_BYTES = 8;
static constexpr u32 HAO_BUILD_MAX_CANDIDATE_BITS = HAO_BUILD_MAX_SUFFIX_BYTES * 8;
static constexpr u32 HAO_BUILD_L2_KEY_BITS = 22;
static constexpr u32 HAO_BUILD_MAX_L2_ENTRIES = 1U << HAO_BUILD_L2_KEY_BITS;
static constexpr u64a HAO_BUILD_MAX_TOTAL_PRIMARY_FOOTPRINT =
    128ULL * 1024ULL * 1024ULL;
static constexpr u8 HAO_BUILD_STATE_DONT_CARE = 2;

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
    DYNAMIC_EXPANSION
};

enum class HAOSelectorPolicy : u8 {
    AUTO,
    DEFAULT_ONLY,
    DYNAMIC_ONLY
};

static
bool getBitState(const hwlmLiteral &lit, u32 bitIndex, u8 *state);

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
const char *extractModeName(u32 mode) {
    switch (mode) {
    case HAO_EXTRACT_MODE_SCALAR:
        return "scalar";
    case HAO_EXTRACT_MODE_BEXT:
        return "bext";
    default:
        return "unknown";
    }
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

static
u32 haoFullKeyMask(u32 keyBits) {
    if (!keyBits) {
        return 0;
    }
    if (keyBits >= 32U) {
        return 0xffffffffU;
    }
    return (1U << keyBits) - 1U;
}

/* Returns true when a literal carries supplementary mask/cmp semantics.
 * In the current HAO plan this maps to mask-confirm. */
static
bool haoHasMask(const hwlmLiteral &lit) {
    return !lit.msk.empty() || !lit.cmp.empty();
}

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

/* Current first-pass rule categories are conservative: exact, nocase,
 * mask-confirm, and unsupported. */
static
HAORuleCategory haoClassifyLiteral(const hwlmLiteral &lit,
                                   const HAOKeyExpansionInfo &expansion) {
    if (expansion.selectedAmbigBits > HAO_MAX_KEY_AMBIG_BITS) {
        return HAORuleCategory::HAO_RULE_UNSUPPORTED;
    }

    if (haoHasMask(lit)) {
        return HAORuleCategory::HAO_RULE_MASK_CONFIRM;
    }

    if (lit.nocase) {
        return HAORuleCategory::HAO_RULE_NOCASE;
    }

    return HAORuleCategory::HAO_RULE_EXACT;
}

static
bool haoCoverCaseBits(
    const hwlmLiteral &lit, const std::vector<HAOBitSelector> &selectors) {
    for (u32 byteFromEnd = 0; byteFromEnd < lit.s.size(); byteFromEnd++) {
        const u8 c = verify_u8(lit.s[lit.s.size() - byteFromEnd - 1]);
        bool found = false;

        if (!ourisalpha(c)) {
            continue;
        }
        for (const auto &sel : selectors) {
            if (sel.byteOffset == byteFromEnd && sel.bitOffset == 5U) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

static
bool haoCanDirectReport(const hwlmLiteral &lit,
                            const HAOCompiledRulePlan &plan,
                            const std::vector<HAOBitSelector> &selectors) {
    if (plan.category != HAORuleCategory::HAO_RULE_EXACT &&
        plan.category != HAORuleCategory::HAO_RULE_NOCASE) {
        return false;
    }
    if (haoHasMask(lit) || lit.noruns) {
        return false;
    }
    if (plan.category == HAORuleCategory::HAO_RULE_EXACT) {
        if (!haoCoverCaseBits(lit, selectors)) {
            return false;
        }
    }
    return true;
}

static
bool haoStatsDumpEnabled(void) {
    const char *env = getenv("HS_HAO_STATS");
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
    } else {
        policy = static_cast<int>(HAOSelectorPolicy::AUTO);
    }

    return static_cast<HAOSelectorPolicy>(policy);
}

/* Build the deterministic verifier fragment that the later L2 verifier
 * consumes. hwlmLiteral stores nocase literals in canonical uppercase form;
 * nocase bytes are encoded through an L2 mask byte of 0xdf so runtime can
 * compare input bytes without normalizing them first. */
static
HAOVerifierFragment haoBuildFrag(const hwlmLiteral &lit,
                                             HAORuleCategory category) {
    HAOVerifierFragment fragment = {};
    const u32 len = verify_u32(lit.s.size());
    assert(len <= HAO_LAYOUT_BYTES_PER_RULE_SLOT);
    const u32 suffixLen = len;
    const u32 laneStart = HAO_LAYOUT_BYTES_PER_RULE_SLOT - suffixLen;

    for (u32 j = 0; j < suffixLen; j++) {
        const u8 c = verify_u8(lit.s[len - suffixLen + j]);
        const u32 idx = laneStart + j;
        fragment.bytes[idx] = c;
        fragment.validByteMask |= verify_u8(1U << idx);
        if (lit.nocase && ourisalpha(c)) {
            fragment.nocaseByteMask |= verify_u8(1U << idx);
        }
    }

    if (category == HAORuleCategory::HAO_RULE_NOCASE) {
        fragment.flags |= verify_u8(HAO_RULE_PLAN_FLAG_NORMALIZED);
    }
    if (category == HAORuleCategory::HAO_RULE_MASK_CONFIRM) {
        fragment.flags |= verify_u8(HAO_RULE_PLAN_FLAG_MASK_CONFIRM);
    }
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
        plan.verifier = haoBuildFrag(lits[i], plan.category);

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
        }
        if (plan.keyExpansion.selectedAmbigBits > HAO_MAX_KEY_AMBIG_BITS) {
            plan.flags |= HAO_RULE_PLAN_FLAG_OVER_AMBIG_LIMIT;
        }

        if (plan.category == HAORuleCategory::HAO_RULE_MASK_CONFIRM) {
            plan.needFullConfirm = true;
            plan.flags |= HAO_RULE_PLAN_FLAG_NEEDS_CONFIRM;
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
        case HAORuleCategory::HAO_RULE_MASK_CONFIRM:
            summary->maskConfirmRules++;
            break;
        case HAORuleCategory::HAO_RULE_UNSUPPORTED:
            break;
        default:
            break;
        }

        const bool canDirectReport =
            haoCanDirectReport(lits[i], plan, selectors);

        if (canDirectReport) {
            plan.flags |= HAO_RULE_PLAN_FLAG_DIRECT_REPORT_SAFE;
        }

        if (plan.flags & HAO_RULE_PLAN_FLAG_KEY_EXPANDED) {
            summary->keyExpandedRules++;
        }
        if (!canDirectReport &&
            plan.category != HAORuleCategory::HAO_RULE_UNSUPPORTED) {
            summary->fastPathConfirmRules++;
        }
        if (canDirectReport) {
            summary->directReportRules++;
        }
        if (plan.category != HAORuleCategory::HAO_RULE_UNSUPPORTED) {
            summary->fastPathRules++;
            summary->totalExpandedKeys += plan.keyExpansion.expandedKeyCount;
        }
        rulePlans->push_back(std::move(plan));
    }
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
void haoFillL2(const HAOCompiledRulePlan &plan, u32 localSlot,
                           HAOL2Check *check, HAOL2Meta *meta) {
    const u8 validMask = plan.verifier.validByteMask;
    u64a ruleWord = 0;
    u64a maskWord = 0;

    assert(check);
    assert(meta);
    assert(localSlot < HAO_LAYOUT_RULE_SLOTS_PER_ENTRY);
    assert(validMask);

    meta->ruleIndex[localSlot] = plan.ruleIndex;

    for (u32 i = 0; i < HAO_LAYOUT_BYTES_PER_RULE_SLOT; i++) {
        u8 ruleByte;
        u8 maskByte;

        if (!(validMask & (1U << i))) {
            continue;
        }

        ruleByte = plan.verifier.bytes[i];
        maskByte = verify_u8(0xffU);
        if (plan.verifier.nocaseByteMask & (1U << i)) {
            maskByte = verify_u8(0xdfU);
            ruleByte = verify_u8(ruleByte & maskByte);
        }

        ruleWord |= (u64a)ruleByte << (i * 8U);
        maskWord |= (u64a)maskByte << (i * 8U);
        meta->careBits |=
            1U << (localSlot * HAO_LAYOUT_BYTES_PER_RULE_SLOT + i);
    }

    check->rule[localSlot] = ruleWord;
    check->mask[localSlot] = maskWord;
}

/* Build the HAO global single-table hash. Expanded keys go straight into the
 * global key space instead of being grouped by mask class. */
static
void haoBuildHash(const std::vector<HAOCompiledRulePlan> &rulePlans,
                              u32 keyBits, HAOGlobalHashArtifacts *out) {
    if (!out) {
        return;
    }

    out->valid = false;
    out->flags = 0;
    out->keyBits = keyBits;
    out->fullKeyMask = haoFullKeyMask(keyBits);
    out->primaryHashTable.offsets.clear();
    out->primaryHashTableRaw.offsets.clear();
    out->primaryHashBitmapRaw.bits.clear();
    out->l2CheckTable.clear();
    out->l2MetaTable.clear();
    out->stats = {};

    if (!keyBits) {
        return;
    }

    const u32 primaryCount = haoPrimaryCountForKeyBits(keyBits);
    out->primaryHashTable.offsets.assign(primaryCount, 0);

    // Keep L2[0] empty so runtime null-target handling stays unchanged.
    {
        HAOL2Check check = {};
        HAOL2Meta meta = {};

        haoInitL2(&check, &meta);
        out->l2CheckTable.push_back(check);
        out->l2MetaTable.push_back(meta);
    }

    std::map<u32, std::vector<u32>> keyToRuleIndexes;
    for (const auto &plan : rulePlans) {
        if (plan.category == HAORuleCategory::HAO_RULE_UNSUPPORTED) {
            out->flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
            return;
        }
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

        if (out->l2CheckTable.size() + entryCount >
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

        const u32 l2Offset = verify_u32(out->l2CheckTable.size());
        out->primaryHashTable.offsets[key] =
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
                assert(ruleIndex < rulePlans.size());
                haoFillL2(rulePlans[ruleIndex], localSlot,
                                      &l2Check, &l2Meta);
            }

            out->l2CheckTable.push_back(l2Check);
            out->l2MetaTable.push_back(l2Meta);
        }
    }
    out->valid = true;
}

// Build the extraction descriptor for the selected bits. At present this
// chooses between scalar extraction and the BEXT-based path.
template <class ArtifactsT>
static
void haoBuildExtract(const std::vector<HAOBitSelector> &selectors,
                            ArtifactsT *artifacts) {
    if (!artifacts) {
        return;
    }

    artifacts->extractMode = HAO_EXTRACT_MODE_SCALAR;
    artifacts->windowBytes = HAO_LAYOUT_BYTES_PER_RULE_SLOT;
    artifacts->bextMask = 0;
    artifacts->bextMaskRaw = 0;

    if (selectors.empty() || selectors.size() > HAO_LAYOUT_MAX_SELECTORS) {
        return;
    }
    
    // For now, feed every selected bit directly into the BEXT mask.
    for (const auto &selector : selectors) {
        const u32 normBit = haoSelectorBitIndex(selector);
        const u32 rawBit =
            (HAO_LAYOUT_BYTES_PER_RULE_SLOT - 1U - (u32)selector.byteOffset) *
                8U +
            (u32)selector.bitOffset;
        artifacts->bextMask |= (1ULL << normBit);
        artifacts->bextMaskRaw |= (1ULL << rawBit);
    }
    artifacts->extractMode = HAO_EXTRACT_MODE_BEXT;
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
    primaryHashBitmap->bits.assign(haoPrimaryBitmapBytes(primaryCount), 0);
    for (u32 i = 0; i < primaryCount; i++) {
        if (primaryHashTable.offsets[i]) {
            haoPrimaryBitmapSet(&primaryHashBitmap->bits, i);
        }
    }
}

static
u32 haoRawSelectorBitIndex(const HAOBitSelector &selector) {
    return (HAO_LAYOUT_BYTES_PER_RULE_SLOT - 1U - (u32)selector.byteOffset) *
               8U +
           (u32)selector.bitOffset;
}

static
std::vector<u8> buildLogicalToRawPackedMap(
    const std::vector<HAOBitSelector> &selectors) {
    std::vector<u8> order;
    std::vector<u8> logicalToRaw;

    order.reserve(selectors.size());
    logicalToRaw.assign(selectors.size(), 0);

    for (u32 i = 0; i < selectors.size(); i++) {
        order.push_back(verify_u8(i));
    }

    std::stable_sort(order.begin(), order.end(),
                     [&selectors](u8 a, u8 b) {
                         return haoRawSelectorBitIndex(selectors[a]) <
                                haoRawSelectorBitIndex(selectors[b]);
                     });

    for (u32 packedBit = 0; packedBit < order.size(); packedBit++) {
        logicalToRaw[order[packedBit]] = verify_u8(packedBit);
    }

    return logicalToRaw;
}

static
u32 haoRemapLogicalKeyToRaw(u32 keyValue,
                            const std::vector<u8> &logicalToRaw) {
    u32 rawKey = 0;

    for (u32 logicalBit = 0; logicalBit < logicalToRaw.size(); logicalBit++) {
        if (!(keyValue & (1U << logicalBit))) {
            continue;
        }
        rawKey |= 1U << logicalToRaw[logicalBit];
    }

    return rawKey;
}

static
void buildHAORawPrimaryTables(const std::vector<HAOBitSelector> &selectors,
                              const HAOGlobalHashArtifacts &logical,
                              HAOPrimaryHashTable *rawTable,
                              HAOPrimaryHashBitmap *rawBitmap) {
    if (!rawTable || !rawBitmap) {
        return;
    }

    rawTable->offsets.clear();
    rawBitmap->bits.clear();

    if (selectors.empty() || logical.primaryHashTable.offsets.empty()) {
        return;
    }

    const auto logicalToRaw = buildLogicalToRawPackedMap(selectors);
    rawTable->offsets.assign(logical.primaryHashTable.offsets.size(), 0);

    for (u32 logicalKey = 0; logicalKey < logical.primaryHashTable.offsets.size();
         logicalKey++) {
        const u32 encoded = logical.primaryHashTable.offsets[logicalKey];
        if (!encoded) {
            continue;
        }

        const u32 rawKey = haoRemapLogicalKeyToRaw(logicalKey, logicalToRaw);
        assert(rawKey < rawTable->offsets.size());
        rawTable->offsets[rawKey] = encoded;
    }

    buildPrimaryBitmap(*rawTable, rawBitmap);
}

static
void dumpRuleBits(const std::vector<hwlmLiteral> &lits) {
    printf("[HAO][Rules-Bits] rule_count=%zu\n", lits.size());
    for (size_t i = 0; i < lits.size(); i++) {
        const auto &lit = lits[i];
        printf("  r%zu id=%u s=\"%s\" len=%zu nocase=%u noruns=%u groups=0x%llx\n",
               i, lit.id, lit.s.c_str(), lit.s.size(), lit.nocase ? 1 : 0,
               lit.noruns ? 1 : 0, (unsigned long long)lit.groups);
        printf("    bytes(msb->lsb): ");
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
        printf("  s%zu -> suffix_bit=%u (byteOffset=%u, bitOffset=%u)\n", i,
               bitIndex, (u32)s.byteOffset, (u32)s.bitOffset);
    }
}

template <class ArtifactsT>
static
void dumpExtractDescriptor(const ArtifactsT &artifacts) {
    printf("[HAO][Extract] mode=%s windowBytes=%u bextMask=0x%llx raw=0x%llx\n",
           extractModeName(artifacts.extractMode), artifacts.windowBytes,
           (unsigned long long)artifacts.bextMask,
           (unsigned long long)artifacts.bextMaskRaw);
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

template <class ArtifactsT>
static
void dumpHAOSummary(const ArtifactsT &artifacts) {
    const auto &s = artifacts.haoSummary;
    const auto &h = artifacts.haoGlobalHash.stats;
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
    HAO_SUMMARY_FMT("maskConfirm",            "%u", s.maskConfirmRules);
    HAO_SUMMARY_FMT("directReport",           "%u", s.directReportRules);
    HAO_SUMMARY_FMT("fastPathConfirm",        "%u", s.fastPathConfirmRules);
    HAO_SUMMARY_FMT("keyExpanded",            "%u", s.keyExpandedRules);
    HAO_SUMMARY_FMT("expandedKeys",           "%u", s.totalExpandedKeys);
    HAO_SUMMARY_FMT("maxSelectedAmbigBits",   "%u", s.maxSelectedAmbigBits);

    printf("[HAO][Length]\n");
    HAO_SUMMARY_FMT("minLiteralLen",          "%u", s.minLiteralLen);
    HAO_SUMMARY_FMT("maxLiteralLen",          "%u", s.maxLiteralLen);
    HAO_SUMMARY_FMT("avgLiteralLen",          "%.5f", avgLiteralLen);
    HAO_SUMMARY_FMT("literalLenLe4",          "%u", s.literalLenLe4);
    HAO_SUMMARY_FMT("literalLen5To8",         "%u", s.literalLen5To8);

    if (!artifacts.haoGlobalHash.valid || !h.nonEmptyPrimary) {
        return;
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

    const u32 byteFromEnd = bitIndex / 8;
    const u32 bitInByte = bitIndex % 8;
    const u32 len = verify_u32(lit.s.size());

    bool care = false;
    bool value = false;

    if (byteFromEnd < len) {
        const u8 c = verify_u8(lit.s[len - byteFromEnd - 1]);
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
        if (byteFromEnd < mlen) {
            const u8 m = normMsk[mlen - byteFromEnd - 1];
            if (m & (1U << bitInByte)) {
                const u8 v = normCmp[mlen - byteFromEnd - 1];
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
void selectBitSelectorsDefault(const std::vector<hwlmLiteral> &lits,
                               std::vector<HAOBitSelector> *selectors,
                               u32 *keyBitsOut) {
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
                  return a.bitIndex < b.bitIndex;
              });

    const u32 targetBits = std::min<u32>(HAO_LAYOUT_KEY_BITS,
                                         verify_u32(candidates.size()));

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
void selectBitSelectorsDynamic(const std::vector<hwlmLiteral> &lits,
                               std::vector<HAOBitSelector> *selectors,
                               u32 *keyBitsOut) {
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
                  return a.bitIndex < b.bitIndex;
              });

    const u32 targetBits = std::min<u32>(HAO_LAYOUT_KEY_BITS,
                                         verify_u32(candidates.size()));

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
                     cand.bitIndex < best->bitIndex)) {
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
void selectBitSelectors(const std::vector<hwlmLiteral> &lits,
                        std::vector<HAOBitSelector> *selectors,
                        u32 *keyBitsOut, HAOSelectorMode mode) {
    if (mode == HAOSelectorMode::DYNAMIC_EXPANSION) {
        selectBitSelectorsDynamic(lits, selectors, keyBitsOut);
    } else {
        selectBitSelectorsDefault(lits, selectors, keyBitsOut);
    }
}

static
void haoBuildMeta(const std::vector<hwlmLiteral> &lits,
                  std::vector<HAOCompileRuleMeta> *ruleMeta) {
    ruleMeta->clear();
    ruleMeta->reserve(lits.size());

    for (const auto &lit : lits) {
        assert(lit.s.size() <= HAO_LAYOUT_BYTES_PER_RULE_SLOT);
        HAOCompileRuleMeta m = {};
        m.id = lit.id;
        m.groups = lit.groups;
        m.len = verify_u16(std::min<size_t>(lit.s.size(),
                                            std::numeric_limits<u16>::max()));
        if (lit.nocase) {
            m.flags |= HAO_RULE_META_FLAG_NOCASE;
        }
        if (lit.noruns) {
            m.flags |= HAO_RULE_META_FLAG_NORUNS;
        }
        std::array<u8, 8> normMsk = {};
        std::array<u8, 8> normCmp = {};
        u8 normLen = 0;
        if (normalizeMaskCmp(lit, &normMsk, &normCmp, &normLen) && normLen) {
            m.flags |= HAO_RULE_META_FLAG_HAS_MASK;
            m.maskLen = normLen;
            packMaskCmpTail(normMsk, normCmp, normLen,
                            &m.maskWord, &m.cmpWord);
            for (size_t j = 0; j < m.maskLen; j++) {
                m.msk[j] = normMsk[j];
                m.cmp[j] = normCmp[j];
            }
        }
        ruleMeta->push_back(m);
    }
}

template <class ArtifactsT>
static
void resetHAOCompileArtifactsCommon(ArtifactsT *artifacts) {
    if (!artifacts) {
        return;
    }
    artifacts->keyBits = 0;
    artifacts->flags = 0;
    artifacts->extractMode = HAO_EXTRACT_MODE_SCALAR;
    artifacts->windowBytes = HAO_LAYOUT_BYTES_PER_RULE_SLOT;
    artifacts->bextMask = 0;
    artifacts->bextMaskRaw = 0;
    artifacts->bitSelectors.clear();
    artifacts->haoRulePlans.clear();
    artifacts->haoSummary = {};
    artifacts->haoGlobalHash = {};
    artifacts->ruleMeta.clear();
}

template <class ArtifactsT>
static
bool haoBuildShared(const std::vector<hwlmLiteral> &lits,
                    ArtifactsT *artifacts, HAOSelectorMode selectorMode) {
    if (!artifacts || lits.empty()) {
        return false;
    }

    selectBitSelectors(lits, &artifacts->bitSelectors, &artifacts->keyBits,
                       selectorMode);
    if (artifacts->bitSelectors.empty()) {
        return false;
    }

    haoBuildExtract(artifacts->bitSelectors, artifacts);
    haoBuildPlans(lits, artifacts->bitSelectors, &artifacts->haoRulePlans,
                      &artifacts->haoSummary);
    if (artifacts->haoSummary.unsupportedRules ||
        artifacts->haoSummary.fastPathRules != artifacts->haoSummary.totalRules) {
        artifacts->flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
        return false;
    }
    haoBuildHash(artifacts->haoRulePlans, artifacts->keyBits,
                             &artifacts->haoGlobalHash);
    buildHAORawPrimaryTables(artifacts->bitSelectors, artifacts->haoGlobalHash,
                             &artifacts->haoGlobalHash.primaryHashTableRaw,
                             &artifacts->haoGlobalHash.primaryHashBitmapRaw);
    haoBuildMeta(lits, &artifacts->ruleMeta);
    artifacts->flags = artifacts->haoGlobalHash.flags;
    return true;
}

static
double haoPct(u32 value, u32 total) {
    if (!total) {
        return 0.0;
    }
    return (static_cast<double>(value) * 100.0) / static_cast<double>(total);
}

static
double haoAvgRulesPerBucket(const HAOGlobalHashStats &stats) {
    if (!stats.nonEmptyPrimary) {
        return 0.0;
    }
    return static_cast<double>(stats.totalRulesInBuckets) /
           static_cast<double>(stats.nonEmptyPrimary);
}

static
bool haoAcceptDynamicSelector(const HAOCompileArtifacts &base,
                              const HAOCompileArtifacts &candidate) {
    const auto &baseStats = base.haoGlobalHash.stats;
    const auto &candStats = candidate.haoGlobalHash.stats;
    if (!candidate.haoGlobalHash.valid ||
        (candidate.flags & HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE)) {
        return false;
    }

    if (candidate.haoSummary.totalExpandedKeys >=
        base.haoSummary.totalExpandedKeys) {
        return false;
    }

    const double baseAvgRules = haoAvgRulesPerBucket(baseStats);
    const double candAvgRules = haoAvgRulesPerBucket(candStats);
    if (baseAvgRules > 0.0 && candAvgRules > baseAvgRules * 1.25) {
        return false;
    }

    const double collisionPct =
        haoPct(candStats.collisionBuckets, candStats.nonEmptyPrimary);
    if (collisionPct > 10.0) {
        return false;
    }

    const double ruleGt4Pct =
        haoPct(candStats.ruleBucketsGt4, candStats.nonEmptyPrimary);
    if (ruleGt4Pct > 2.0) {
        return false;
    }

    return true;
}

static
void haoDumpArtifacts(const std::vector<hwlmLiteral> &lits,
                             const HAOCompileArtifacts &artifacts) {
    printf("\n========== [HAO][Build-Artifacts] Begin ==========\n");
    printf("[HAO][Params] key_bits(fixed=%u, selector_count=%zu) l2_key_bits=%u l2_capacity=%u l2_entry_capacity=%u\n",
           artifacts.keyBits, artifacts.bitSelectors.size(),
           HAO_BUILD_L2_KEY_BITS, HAO_BUILD_MAX_L2_ENTRIES,
           HAO_LAYOUT_RULE_SLOTS_PER_ENTRY);
    dumpRuleBits(lits);
    dumpSelectors(artifacts.bitSelectors);
    dumpExtractDescriptor(artifacts);
    dumpRuleKeys(lits, artifacts.bitSelectors, artifacts.keyBits);
    dumpHAOSummary(artifacts);
    printf("[HAO][Flags] artifacts.flags=0x%x\n", artifacts.flags);
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
    case ReasonT::UNSUPPORTED_INCLUDED_LITERAL:
        return "UNSUPPORTED_INCLUDED_LITERAL";
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
#if defined(HS_BUILD_HAVE_SVEBITPERM)
    return target.has_sve_bitperm();
#else
    (void)target;
    return false;
#endif
}

bool haoHasSveBitPermPrereq(const target_t &target) {
#if defined(HS_BUILD_HAVE_SVEBITPERM)
    return target.has_sve_bitperm();
#else
    (void)target;
    return false;
#endif
}

bool buildHAOArtifacts(const std::vector<hwlmLiteral> &lits,
                       HAOCompileArtifacts *artifacts,
                       bool enableDump, bool allowStatsDump) {
    if (!artifacts) {
        return false;
    }

    HAOCompileArtifacts base;
    resetHAOCompileArtifactsCommon(&base);
    if (!haoBuildShared(lits, &base, HAOSelectorMode::DEFAULT)) {
        return false;
    }

    const HAOSelectorPolicy policy = haoSelectorPolicy();
    const char *selectorMode = "default";
    if (policy == HAOSelectorPolicy::DEFAULT_ONLY) {
        *artifacts = std::move(base);
    } else {
        HAOCompileArtifacts dynamic;
        resetHAOCompileArtifactsCommon(&dynamic);
        const bool haveDynamic =
            haoBuildShared(lits, &dynamic, HAOSelectorMode::DYNAMIC_EXPANSION);
        const bool useDynamic =
            haveDynamic &&
            (policy == HAOSelectorPolicy::DYNAMIC_ONLY ||
             haoAcceptDynamicSelector(base, dynamic));

        if (useDynamic) {
            *artifacts = std::move(dynamic);
            selectorMode = policy == HAOSelectorPolicy::DYNAMIC_ONLY
                               ? "dynamic-forced"
                               : "dynamic";
        } else {
            *artifacts = std::move(base);
        }
    }

    if (enableDump) {
        printf("[HAO][Selector] mode=%s\n", selectorMode);
        haoDumpArtifacts(lits, *artifacts);
    } else if (allowStatsDump && haoStatsDumpEnabled()) {
        printf("[HAO][Selector] mode=%s\n", selectorMode);
        dumpHAOSummary(*artifacts);
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

    if (!grey.allowHao) {
        local.reason = HAOFeasibilityReason::GREY_DISABLED;
        if (result) {
            *result = local;
        }
        return false;
    }

#if !defined(__aarch64__)
    (void)target;
    local.reason = HAOFeasibilityReason::ARCH_UNSUPPORTED;
    if (result) {
        *result = local;
    }
    return false;
#else
    (void)target;
#endif

    if (lits.size() < 1) {
        local.reason = HAOFeasibilityReason::TOO_FEW_LITERALS;
        if (result) {
            *result = local;
        }
        return false;
    }

    if (lits.size() > HAO_MAX_LITERALS) {
        local.reason = HAOFeasibilityReason::TOO_MANY_LITERALS;
        if (result) {
            *result = local;
        }
        return false;
    }

    std::unique_ptr<HAOCompileArtifacts> tempStorage;
    HAOCompileArtifacts *out = artifacts;
    if (!out) {
        tempStorage.reset(new HAOCompileArtifacts());
        out = tempStorage.get();
    }

    if (!buildHAOArtifacts(lits, out, false, false)) {
        local.reason = out->haoSummary.unsupportedRules ?
                       HAOFeasibilityReason::UNSUPPORTED_INCLUDED_LITERAL :
                       HAOFeasibilityReason::ARTIFACT_BUILD_FAILED;
        if (result) {
            *result = local;
        }
        return false;
    }

    local.flags = out->flags;
    if (out->bitSelectors.empty()) {
        local.reason = HAOFeasibilityReason::NO_SELECTORS;
        if (result) {
            *result = local;
        }
        return false;
    }

    if (!out->haoGlobalHash.valid ||
        (out->haoGlobalHash.flags & HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE)) {
        if (out->haoGlobalHash.flags & HAO_ARTIFACT_FLAG_PARTIAL_L2_CAPACITY) {
            local.reason = HAOFeasibilityReason::PARTIAL_L2_CAPACITY;
        } else if (out->haoGlobalHash.flags & HAO_ARTIFACT_FLAG_PARTIAL_ENTRY_OVERFLOW) {
            local.reason = HAOFeasibilityReason::PARTIAL_ENTRY_OVERFLOW;
        } else {
            local.reason = HAOFeasibilityReason::PARTIAL_OTHER;
        }
        if (result) {
            *result = local;
        }
        return false;
    }

    local.canBuild = true;
    local.reason = HAOFeasibilityReason::OK;
    if (result) {
        *result = local;
    }
    return true;
}

bool canBuildHAO(const target_t &target, const std::vector<hwlmLiteral> &lits,
                 const Grey &grey) {
    HAOFeasibilityResult result;
    return analyzeHAOFeasibility(target, lits, grey, &result, nullptr);
}

template <class ArtifactsT>
static
bytecode_ptr<u8> haoBuildBlobImpl(const ArtifactsT &artifacts) {
    if (!artifacts.haoGlobalHash.valid) {
        return nullptr;
    }
    if (artifacts.haoRulePlans.size() != artifacts.ruleMeta.size()) {
        return nullptr;
    }

    const u32 selectorCount = verify_u32(artifacts.bitSelectors.size());
    const u32 primaryCount =
        verify_u32(artifacts.haoGlobalHash.primaryHashTableRaw.offsets.size());
    const u32 primaryBitmapSize =
        verify_u32(artifacts.haoGlobalHash.primaryHashBitmapRaw.bits.size());
    const u32 primaryCoarseBitmapSize = 0;
    const u32 primaryRawCount = verify_u32(
        artifacts.haoGlobalHash.primaryHashTableRaw.offsets.size());
    const u32 primaryBitmapRawSize = verify_u32(
        artifacts.haoGlobalHash.primaryHashBitmapRaw.bits.size());
    const u32 l2EntryCount =
        verify_u32(artifacts.haoGlobalHash.l2CheckTable.size());
    if (artifacts.haoGlobalHash.l2MetaTable.size() != l2EntryCount) {
        return nullptr;
    }
    const u32 ruleMetaCount = verify_u32(artifacts.ruleMeta.size());
    const size_t selectorBytes =
        sizeof(HAORuntimeBitSelector) * artifacts.bitSelectors.size();
    const size_t primaryBitmapBytes =
        artifacts.haoGlobalHash.primaryHashBitmapRaw.bits.size();
    const size_t primaryCoarseBitmapBytes = 0;
    const size_t primaryBytes =
        sizeof(u32) * artifacts.haoGlobalHash.primaryHashTableRaw.offsets.size();
    const size_t l2CheckBytes =
        sizeof(HAORuntimeL2Check) *
        artifacts.haoGlobalHash.l2CheckTable.size();
    const size_t l2MetaBytes =
        sizeof(HAORuntimeL2Meta) *
        artifacts.haoGlobalHash.l2MetaTable.size();
    const size_t ruleMetaBytes =
        sizeof(HAORuntimeRuleMeta) * artifacts.ruleMeta.size();

    if (primaryRawCount != primaryCount ||
        primaryBitmapRawSize != primaryBitmapSize) {
        return nullptr;
    }

    size_t totalSize = ROUNDUP_N(sizeof(HAORuntimeHeader), alignof(u32));
    const u32 selectorsOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(selectorBytes, alignof(u32));
    const u32 primaryBitmapOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(primaryBitmapBytes, alignof(u32));
    const u32 primaryBitmapCoarseOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(primaryCoarseBitmapBytes, alignof(u32));
    const u32 primaryOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(primaryBytes, alignof(u32));
    const u32 primaryBitmapRawOffset = primaryBitmapOffset;
    const u32 primaryBitmapRawCoarseOffset = primaryBitmapCoarseOffset;
    const u32 primaryRawOffset = primaryOffset;
    totalSize = ROUNDUP_N(totalSize, 64);
    const u32 l2CheckOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(l2CheckBytes, 64);
    const u32 l2MetaOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(l2MetaBytes, alignof(u32));
    const u32 ruleMetaOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(ruleMetaBytes, alignof(u32));

    auto blob = make_zeroed_bytecode_ptr<u8>(totalSize, 64);
    if (!blob) {
        return nullptr;
    }

    auto *hdr = reinterpret_cast<HAORuntimeHeader *>(blob.get());
    hdr->magic = HAO_RUNTIME_MAGIC;
    hdr->version = HAO_RUNTIME_VERSION;
    hdr->flags = artifacts.haoGlobalHash.flags;
    hdr->keyBits = artifacts.haoGlobalHash.keyBits;
    hdr->selectorCount = selectorCount;
    hdr->primaryCount = primaryCount;
    hdr->primaryBitmapSize = primaryBitmapSize;
    hdr->primaryCoarseBitmapSize = primaryCoarseBitmapSize;
    hdr->l2EntryCount = l2EntryCount;
    hdr->ruleMetaCount = ruleMetaCount;
    hdr->reservedLiteralBlobSize = 0;
    hdr->extractMode = artifacts.extractMode;
    hdr->windowBytes = artifacts.windowBytes;
    hdr->bextMask = artifacts.bextMask;
    hdr->bextMaskRaw = artifacts.bextMaskRaw;
    hdr->selectorsOffset = selectorsOffset;
    hdr->primaryBitmapOffset = primaryBitmapOffset;
    hdr->primaryBitmapCoarseOffset = primaryBitmapCoarseOffset;
    hdr->primaryOffset = primaryOffset;
    hdr->primaryBitmapRawOffset = primaryBitmapRawOffset;
    hdr->primaryBitmapRawCoarseOffset = primaryBitmapRawCoarseOffset;
    hdr->primaryRawOffset = primaryRawOffset;
    hdr->l2CheckOffset = l2CheckOffset;
    hdr->l2MetaOffset = l2MetaOffset;
    hdr->ruleMetaOffset = ruleMetaOffset;
    hdr->reservedLiteralBlobOffset = 0;
    hdr->reserved1 = 0;
    hdr->reserved2 = 0;

    u8 *base = blob.get();
    auto *selectorsOut =
        reinterpret_cast<HAORuntimeBitSelector *>(base + selectorsOffset);
    for (u32 i = 0; i < selectorCount; i++) {
        selectorsOut[i].byteOffset = artifacts.bitSelectors[i].byteOffset;
        selectorsOut[i].bitOffset = artifacts.bitSelectors[i].bitOffset;
        selectorsOut[i].reserved = 0;
    }

    if (primaryBitmapBytes) {
        memcpy(base + primaryBitmapOffset,
               artifacts.haoGlobalHash.primaryHashBitmapRaw.bits.data(),
               primaryBitmapBytes);
    }
    if (primaryBytes) {
        memcpy(base + primaryOffset,
               artifacts.haoGlobalHash.primaryHashTableRaw.offsets.data(),
               primaryBytes);
    }
    if (l2CheckBytes) {
        auto *checkOut =
            reinterpret_cast<HAORuntimeL2Check *>(base + l2CheckOffset);
        for (u32 i = 0; i < l2EntryCount; i++) {
            const auto &src = artifacts.haoGlobalHash.l2CheckTable[i];
            auto &dst = checkOut[i];
            memcpy(dst.rule, src.rule, sizeof(dst.rule));
            memcpy(dst.mask, src.mask, sizeof(dst.mask));
        }
    }
    if (l2MetaBytes) {
        auto *metaOut =
            reinterpret_cast<HAORuntimeL2Meta *>(base + l2MetaOffset);
        for (u32 i = 0; i < l2EntryCount; i++) {
            const auto &src = artifacts.haoGlobalHash.l2MetaTable[i];
            auto &dst = metaOut[i];
            memcpy(dst.ruleIndex, src.ruleIndex, sizeof(dst.ruleIndex));
            dst.careBits = src.careBits;
        }
    }

    auto *ruleMetaOut =
        reinterpret_cast<HAORuntimeRuleMeta *>(base + ruleMetaOffset);
    for (u32 i = 0; i < ruleMetaCount; i++) {
        const auto &srcMeta = artifacts.ruleMeta[i];
        const auto &plan = artifacts.haoRulePlans[i];
        auto &dst = ruleMetaOut[i];
        dst.id = srcMeta.id;
        dst.groups = srcMeta.groups;
        dst.len = srcMeta.len;
        dst.flags = srcMeta.flags;
        dst.category = static_cast<u8>(plan.category);
        dst.verifierValidByteMask = plan.verifier.validByteMask;
        dst.reserved1 = 0;
        dst.reserved2 = 0;
        dst.verifierFlags = plan.verifier.flags;
        dst.maskLen = srcMeta.maskLen;
        dst.reserved0 = 0;
        dst.planFlags = plan.flags;
        dst.reserved3 = 0;
        dst.maskWord = srcMeta.maskWord;
        dst.cmpWord = srcMeta.cmpWord;
        memcpy(dst.msk, srcMeta.msk, sizeof(dst.msk));
        memcpy(dst.cmp, srcMeta.cmp, sizeof(dst.cmp));
    }
    return blob;
}

bytecode_ptr<u8> buildHAOBlob(const HAOCompileArtifacts &artifacts) {
    return haoBuildBlobImpl(artifacts);
}

} // namespace ue2
