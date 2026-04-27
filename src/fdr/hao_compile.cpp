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
static constexpr u32 HAO_BUILD_SECONDARY_KEY_BITS = 18;
static constexpr u32 HAO_BUILD_MAX_SECONDARY_ENTRIES = 1U << HAO_BUILD_SECONDARY_KEY_BITS;
static constexpr u64a HAO_BUILD_MAX_TOTAL_PRIMARY_FOOTPRINT =
    128ULL * 1024ULL * 1024ULL;
static constexpr u8 HAO_BUILD_STATE_DONT_CARE = 2;

struct HAOBitCandidate {
    u32 bitIndex = 0;
    double score = 0.0;
    std::vector<u8> states; // 0, 1, or HAO_BUILD_STATE_DONT_CARE
};

static
bool getBitState(const hwlmLiteral &lit, u32 bitIndex, u8 *state);

static
void haoComputeKeyValueMaskForLiteral(const hwlmLiteral &lit,
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
u32 encodePrimaryValue(u32 secondaryOffset, u32 entryCount) {
    assert(secondaryOffset <= HAO_LAYOUT_L1_OFFSET_MASK);
    assert(entryCount < (1U << (32U - HAO_LAYOUT_L1_COUNT_SHIFT)));
    return (entryCount << HAO_LAYOUT_L1_COUNT_SHIFT) | secondaryOffset;
}

static
u8 normalizedLiteralByte(u8 c) {
    return ourisalpha(c) ? verify_u8(mytoupper(c)) : c;
}

static
u32 haoPrimaryBitmapBytes(u32 primaryCount) {
    return (primaryCount + 7U) / 8U;
}

static
void buildPrimaryBitmap(const HAOPrimaryHashTable &primaryHashTable,
                        HAOPrimaryHashBitmap *primaryHashBitmap);

static
void buildPrimaryCoarseBitmap(const HAOPrimaryHashTable &primaryHashTable,
                              HAOPrimaryHashBitmap *primaryCoarseBitmap);

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
 * In the current HAO v2 plan this maps to anchor-confirm. */
static
bool haoLiteralHasSupplementaryMask(const hwlmLiteral &lit) {
    return !lit.msk.empty() || !lit.cmp.empty();
}

/* Enumerate the selected-bit key variants for one rule. Callers exclude
 * rules from the fast path when ambiguity exceeds HAO_MAX_KEY_AMBIG_BITS. */
static
HAOKeyExpansionInfo haoEnumerateExpandedKeysForLiteral(
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

    // Enumerate all key variants induced by ambiguous selected bits. These
    // key values are based on normalized literal bytes; final confirm resolves
    // exact versus nocase semantics.
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
 * anchor-confirm, and unsupported. */
static
HAORuleCategory haoClassifyLiteral(const hwlmLiteral &lit,
                                   const HAOKeyExpansionInfo &expansion) {
    if (expansion.selectedAmbigBits > HAO_MAX_KEY_AMBIG_BITS) {
        return HAORuleCategory::HAO_RULE_UNSUPPORTED;
    }

    if (haoLiteralHasSupplementaryMask(lit)) {
        return HAORuleCategory::HAO_RULE_ANCHOR_CONFIRM;
    }

    if (lit.nocase) {
        return HAORuleCategory::HAO_RULE_NOCASE;
    }

    return HAORuleCategory::HAO_RULE_EXACT;
}

static
bool haoPlanRequiresResidualEval(const HAOCompiledRulePlan &plan) {
    if (plan.category == HAORuleCategory::HAO_RULE_UNSUPPORTED) {
        return true;
    }
    if (plan.category == HAORuleCategory::HAO_RULE_ANCHOR_CONFIRM) {
        return false;
    }
    if (plan.flags & HAO_RULE_PLAN_FLAG_NEEDS_CONFIRM) {
        return true;
    }
    return false;
}

static
bool haoSelectorsCoverExactCaseBits(
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
bool haoPlanCanDirectReport(const hwlmLiteral &lit,
                            const HAOCompiledRulePlan &plan,
                            const std::vector<HAOBitSelector> &selectors) {
    if (plan.category != HAORuleCategory::HAO_RULE_EXACT &&
        plan.category != HAORuleCategory::HAO_RULE_NOCASE) {
        return false;
    }
    if (haoLiteralHasSupplementaryMask(lit) || lit.noruns) {
        return false;
    }
    if (plan.category == HAORuleCategory::HAO_RULE_EXACT) {
        if (!haoSelectorsCoverExactCaseBits(lit, selectors)) {
            return false;
        }
    }
    return plan.verifier.anchorOffset == 0 &&
           plan.verifier.anchorLength == lit.s.size();
}

static
bool haoStatsDumpEnabled(void) {
    const char *env = getenv("HS_HAO_STATS");
    return env && *env && *env != '0';
}

/* Build the deterministic verifier fragment that the later L2 verifier
 * consumes. Both exact and nocase rules store normalized bytes here;
 * final confirm resolves the exact versus nocase distinction. */
static
HAOVerifierFragment haoBuildVerifierFragment(const hwlmLiteral &lit,
                                             HAORuleCategory category) {
    HAOVerifierFragment fragment = {};
    const u32 len = verify_u32(lit.s.size());
    const u32 suffixLen = std::min<u32>(len, HAO_LAYOUT_BYTES_PER_RULE_SLOT);
    const u32 laneStart = HAO_LAYOUT_BYTES_PER_RULE_SLOT - suffixLen;

    fragment.anchorOffset = verify_u8(len - suffixLen);
    fragment.anchorLength = verify_u8(suffixLen);
    for (u32 j = 0; j < suffixLen; j++) {
        const u8 c = verify_u8(lit.s[len - suffixLen + j]);
        const u32 idx = laneStart + j;
        fragment.bytes[idx] = normalizedLiteralByte(c);
        fragment.validByteMask |= verify_u8(1U << idx);
    }

    if (category == HAORuleCategory::HAO_RULE_NOCASE) {
        fragment.flags |= verify_u8(HAO_RULE_PLAN_FLAG_NORMALIZED);
    }
    if (category == HAORuleCategory::HAO_RULE_ANCHOR_CONFIRM) {
        fragment.flags |= verify_u8(HAO_RULE_PLAN_FLAG_ANCHOR_FRAGMENT);
    }
    return fragment;
}

/* Build the compile-time HAO rule-plan layer that later table construction
 * consumes directly. */
static
void buildHAORulePlans(const std::vector<hwlmLiteral> &lits,
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
        plan.ruleIndex = i;
        plan.keyExpansion = haoEnumerateExpandedKeysForLiteral(lits[i], selectors);
        plan.category = haoClassifyLiteral(lits[i], plan.keyExpansion);
        plan.verifier = haoBuildVerifierFragment(lits[i], plan.category);

        if (plan.keyExpansion.selectedAmbigBits) {
            plan.flags |= HAO_RULE_PLAN_FLAG_KEY_EXPANDED;
        }
        if (lits[i].nocase) {
            plan.flags |= HAO_RULE_PLAN_FLAG_NORMALIZED;
        }
        if (haoLiteralHasSupplementaryMask(lits[i])) {
            plan.flags |= HAO_RULE_PLAN_FLAG_HAS_SUPPLEMENTARY_MASK;
        }
        if (plan.keyExpansion.selectedAmbigBits > HAO_MAX_KEY_AMBIG_BITS) {
            plan.flags |= HAO_RULE_PLAN_FLAG_OVER_AMBIG_LIMIT;
        }

        if (plan.category == HAORuleCategory::HAO_RULE_ANCHOR_CONFIRM) {
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
        } else if ((u64a)summary->totalExpandedKeys +
                    plan.keyExpansion.expandedKeyCount >
            HAO_MAX_TOTAL_EXPANDED_KEYS) {
            plan.category = HAORuleCategory::HAO_RULE_UNSUPPORTED;
            plan.flags |= HAO_RULE_PLAN_FLAG_OVER_EXPANSION_BUDGET;
            plan.keyExpansion.expandedKeys.clear();
            plan.keyExpansion.expandedKeyCount = 0;
            summary->unsupportedRules++;
        }

        switch (plan.category) {
        case HAORuleCategory::HAO_RULE_EXACT:
            summary->exactRules++;
            break;
        case HAORuleCategory::HAO_RULE_NOCASE:
            summary->nocaseRules++;
            break;
        case HAORuleCategory::HAO_RULE_ANCHOR_CONFIRM:
            summary->anchorConfirmRules++;
            break;
        case HAORuleCategory::HAO_RULE_UNSUPPORTED:
            break;
        default:
            break;
        }

        const bool requiresResidual = haoPlanRequiresResidualEval(plan);
        const bool canDirectReport =
            haoPlanCanDirectReport(lits[i], plan, selectors);

        if (canDirectReport) {
            plan.flags |= HAO_RULE_PLAN_FLAG_DIRECT_REPORT_SAFE;
        }

        if (plan.flags & HAO_RULE_PLAN_FLAG_KEY_EXPANDED) {
            summary->keyExpandedRules++;
        }
        if (requiresResidual) {
            summary->residualRules++;
            if (plan.category == HAORuleCategory::HAO_RULE_UNSUPPORTED) {
                summary->residualUnsupportedRules++;
            }
        } else if (!canDirectReport) {
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

/* Write one HAO verifier fragment into one L2 slot. Runtime v2 consumes these
 * fragments directly from the global single-table layout. */
static
void haoFillSecondarySlotFromPlan(const HAOCompiledRulePlan &plan,
                                  u32 localSlot,
                                  HAOSecondaryHashEntry *entry) {
    assert(entry);
    const u32 laneBase = localSlot * HAO_LAYOUT_BYTES_PER_RULE_SLOT;
    const u8 validMask = plan.verifier.validByteMask;
    u32 lastValidBit = HAO_LAYOUT_BYTES_PER_RULE_SLOT;

    for (u32 i = 0; i < HAO_LAYOUT_BYTES_PER_RULE_SLOT; i++) {
        if (validMask & (1U << i)) {
            lastValidBit = i;
        }
    }

    entry->ruleIndex[localSlot] = verify_u16(plan.ruleIndex);
    entry->slotMask |= verify_u8(1U << localSlot);
    entry->slotCount = verify_u8(entry->slotCount + 1U);

    for (u32 i = 0; i < HAO_LAYOUT_BYTES_PER_RULE_SLOT; i++) {
        if (!(validMask & (1U << i))) {
            continue;
        }
        const u32 vecIndex = laneBase + i;
        const u8 srcCtl = verify_u8(vecIndex & 0x0fU);
        entry->ruleVector[vecIndex] = plan.verifier.bytes[i];
        entry->tableControl[vecIndex] = srcCtl;
        if (srcCtl != (vecIndex & 0x0fU)) {
            entry->flags = verify_u8(entry->flags &
                                     (u8)~HAO_SECONDARY_ENTRY_FLAG_IDENTITY_TBL);
        }
        entry->tailMask |= (1U << vecIndex);
        if (i != lastValidBit) {
            entry->headMask |= (1U << vecIndex);
        }
    }
}

/* Build the HAO global single-table hash. Expanded keys go straight into the
 * global key space instead of being grouped by mask class. */
static
void buildHAOGlobalHashTables(const std::vector<HAOCompiledRulePlan> &rulePlans,
                              u32 keyBits, HAOGlobalHashArtifacts *out) {
    if (!out) {
        return;
    }

    out->valid = false;
    out->flags = 0;
    out->keyBits = keyBits;
    out->fullKeyMask = haoFullKeyMask(keyBits);
    out->primaryHashTable.offsets.clear();
    out->primaryHashBitmap.bits.clear();
    out->primaryHashBitmapCoarse.bits.clear();
    out->primaryHashTableRaw.offsets.clear();
    out->primaryHashBitmapRaw.bits.clear();
    out->primaryHashBitmapRawCoarse.bits.clear();
    out->secondaryHashTable.clear();
    out->stats = {};

    if (!keyBits) {
        return;
    }

    const u32 primaryCount = haoPrimaryCountForKeyBits(keyBits);
    out->primaryHashTable.offsets.assign(primaryCount, 0);

    // Keep secondary[0] empty so runtime null-target handling stays unchanged.
    out->secondaryHashTable.push_back(HAOSecondaryHashEntry{});

    std::map<u32, std::vector<u32>> keyToRuleIndexes;
    for (const auto &plan : rulePlans) {
        if (haoPlanRequiresResidualEval(plan)) {
            continue;
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

        if (out->secondaryHashTable.size() + entryCount >
            HAO_BUILD_MAX_SECONDARY_ENTRIES) {
            out->flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
            out->flags |= HAO_ARTIFACT_FLAG_PARTIAL_SECONDARY_CAPACITY;
            return;
        }
        if (entryCount >= (1U << (32U - HAO_LAYOUT_L1_COUNT_SHIFT))) {
            out->flags |= HAO_ARTIFACT_FLAG_PARTIAL_COVERAGE;
            out->flags |= HAO_ARTIFACT_FLAG_PARTIAL_ENTRY_OVERFLOW;
            return;
        }

        const u32 secondaryOffset = verify_u32(out->secondaryHashTable.size());
        out->primaryHashTable.offsets[key] =
            encodePrimaryValue(secondaryOffset, entryCount);
        out->stats.nonEmptyPrimary++;
        out->stats.totalRulesInBuckets += ruleCount;
        out->stats.totalSecondaryEntries += entryCount;
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
            HAOSecondaryHashEntry entry = {};
            memset(entry.tableControl, 0x80, sizeof(entry.tableControl));
            entry.flags = HAO_SECONDARY_ENTRY_FLAG_IDENTITY_TBL;
            const size_t begin = chunk * HAO_LAYOUT_RULE_SLOTS_PER_ENTRY;
            const size_t end = std::min(bucketRules.size(),
                                        begin + HAO_LAYOUT_RULE_SLOTS_PER_ENTRY);

            for (size_t slot = begin; slot < end; slot++) {
                const u32 localSlot = verify_u32(slot - begin);
                const u32 ruleIndex = bucketRules[slot];
                assert(ruleIndex < rulePlans.size());
                haoFillSecondarySlotFromPlan(rulePlans[ruleIndex], localSlot,
                                             &entry);
            }

            out->secondaryHashTable.push_back(entry);
        }
    }

    buildPrimaryBitmap(out->primaryHashTable, &out->primaryHashBitmap);
    buildPrimaryCoarseBitmap(out->primaryHashTable,
                             &out->primaryHashBitmapCoarse);
    out->valid = true;
}

// Build the extraction descriptor for the selected bits. At present this
// chooses between scalar extraction and the BEXT-based path.
template <class ArtifactsT>
static
void buildExtractDescriptor(const std::vector<HAOBitSelector> &selectors,
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
u32 haoPrimaryCoarseBitmapBytes(u32 primaryCount) {
    const u32 coarseCount =
        (primaryCount + HAO_RUNTIME_PRIMARY_COARSE_KEY_GROUP - 1U) >>
        HAO_RUNTIME_PRIMARY_COARSE_KEY_SHIFT;
    return haoPrimaryBitmapBytes(coarseCount);
}

static
void buildPrimaryCoarseBitmap(const HAOPrimaryHashTable &primaryHashTable,
                              HAOPrimaryHashBitmap *primaryCoarseBitmap) {
    if (!primaryCoarseBitmap) {
        return;
    }

    primaryCoarseBitmap->bits.clear();
    const u32 primaryCount = verify_u32(primaryHashTable.offsets.size());
    primaryCoarseBitmap->bits.assign(haoPrimaryCoarseBitmapBytes(primaryCount),
                                     0);
    for (u32 i = 0; i < primaryCount; i++) {
        if (primaryHashTable.offsets[i]) {
            haoPrimaryBitmapSet(&primaryCoarseBitmap->bits,
                                i >> HAO_RUNTIME_PRIMARY_COARSE_KEY_SHIFT);
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
                              HAOPrimaryHashBitmap *rawBitmap,
                              HAOPrimaryHashBitmap *rawCoarseBitmap) {
    if (!rawTable || !rawBitmap || !rawCoarseBitmap) {
        return;
    }

    rawTable->offsets.clear();
    rawBitmap->bits.clear();
    rawCoarseBitmap->bits.clear();

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
    buildPrimaryCoarseBitmap(*rawTable, rawCoarseBitmap);
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
        haoComputeKeyValueMaskForLiteral(lits[i], selectors, &keyValue, &keyMask);
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
                                       ? (double)h.totalSecondaryEntries /
                                             (double)h.nonEmptyPrimary
                                       : 0.0;

    printf("[HAO][Summary/编译汇总]\n");
    HAO_SUMMARY_FMT("total(规则总数)",                           "%u", s.totalRules);
    HAO_SUMMARY_FMT("fastPath(快速路径规则数)",                  "%u", s.fastPathRules);
    HAO_SUMMARY_FMT("residual(兜底规则数)",                      "%u", s.residualRules);
    HAO_SUMMARY_FMT("residualUnsupported(兜底且不支持规则数)",   "%u", s.residualUnsupportedRules);
    HAO_SUMMARY_FMT("unsupported(不支持规则数)",                 "%u", s.unsupportedRules);
    HAO_SUMMARY_FMT("exact(精确规则数)",                         "%u", s.exactRules);
    HAO_SUMMARY_FMT("nocase(忽略大小写规则数)",                  "%u", s.nocaseRules);
    HAO_SUMMARY_FMT("anchorConfirm(anchor确认规则数)",           "%u", s.anchorConfirmRules);
    HAO_SUMMARY_FMT("directReport(可直接上报规则数)",            "%u", s.directReportRules);
    HAO_SUMMARY_FMT("fastPathConfirm(快速路径需确认规则数)",     "%u", s.fastPathConfirmRules);
    HAO_SUMMARY_FMT("keyExpanded(发生key展开规则数)",            "%u", s.keyExpandedRules);
    HAO_SUMMARY_FMT("expandedKeys(展开后的key总数)",             "%u", s.totalExpandedKeys);
    HAO_SUMMARY_FMT("maxSelectedAmbigBits(最大选位歧义bit数)",   "%u", s.maxSelectedAmbigBits);

    if (!artifacts.haoGlobalHash.valid || !h.nonEmptyPrimary) {
        return;
    }

    printf("[HAO][Hash/哈希分布]\n");
    HAO_SUMMARY_FMT("nonEmptyPrimary(非空一级桶数)",             "%u", h.nonEmptyPrimary);
    HAO_SUMMARY_FMT("collisionBuckets(冲突桶数)",                "%u", h.collisionBuckets);
    HAO_SUMMARY_FMT("collisionBucketPct(冲突桶占比)",            "%.5f",
                    haoCompilePct(h.collisionBuckets, h.nonEmptyPrimary));
    HAO_SUMMARY_FMT("avgRulesPerBucket(每桶平均规则数)",         "%.5f", avgRulesPerBucket);
    HAO_SUMMARY_FMT("minRulesPerBucket(每桶最少规则数)",         "%u", h.minRulesPerBucket);
    HAO_SUMMARY_FMT("maxRulesPerBucket(每桶最多规则数)",         "%u", h.maxRulesPerBucket);
    HAO_SUMMARY_FMT("ruleBucketsEq1(规则数=1的桶数)",            "%u", h.ruleBucketsEq1);
    HAO_SUMMARY_FMT("ruleBucketsEq1Pct(规则数=1桶占比)",         "%.5f",
                    haoCompilePct(h.ruleBucketsEq1, h.nonEmptyPrimary));
    HAO_SUMMARY_FMT("ruleBuckets2To4(规则数2~4的桶数)",          "%u", h.ruleBuckets2To4);
    HAO_SUMMARY_FMT("ruleBuckets2To4Pct(规则数2~4桶占比)",       "%.5f",
                    haoCompilePct(h.ruleBuckets2To4, h.nonEmptyPrimary));
    HAO_SUMMARY_FMT("ruleBucketsGt4(规则数>4的桶数)",            "%u", h.ruleBucketsGt4);
    HAO_SUMMARY_FMT("ruleBucketsGt4Pct(规则数>4桶占比)",         "%.5f",
                    haoCompilePct(h.ruleBucketsGt4, h.nonEmptyPrimary));
    HAO_SUMMARY_FMT("avgEntriesPerBucket(每桶平均entry数)",      "%.5f", avgEntriesPerBucket);
    HAO_SUMMARY_FMT("minEntriesPerBucket(每桶最少entry数)",      "%u", h.minEntriesPerBucket);
    HAO_SUMMARY_FMT("maxEntriesPerBucket(每桶最多entry数)",      "%u", h.maxEntriesPerKey);
    HAO_SUMMARY_FMT("entryBucketsEq1(entry数=1的桶数)",          "%u", h.entryBucketsEq1);
    HAO_SUMMARY_FMT("entryBucketsEq1Pct(entry数=1桶占比)",       "%.5f",
                    haoCompilePct(h.entryBucketsEq1, h.nonEmptyPrimary));
    HAO_SUMMARY_FMT("entryBuckets2To4(entry数2~4的桶数)",        "%u", h.entryBuckets2To4);
    HAO_SUMMARY_FMT("entryBuckets2To4Pct(entry数2~4桶占比)",     "%.5f",
                    haoCompilePct(h.entryBuckets2To4, h.nonEmptyPrimary));
    HAO_SUMMARY_FMT("entryBucketsGt4(entry数>4的桶数)",          "%u", h.entryBucketsGt4);
    HAO_SUMMARY_FMT("entryBucketsGt4Pct(entry数>4桶占比)",       "%.5f",
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
std::vector<HAOBitCandidate> buildBitCandidates(
    const std::vector<hwlmLiteral> &lits) {
    std::vector<HAOBitCandidate> out;
    out.reserve(HAO_BUILD_MAX_CANDIDATE_BITS);
    // Collect per-bit state distributions across the candidate 64-bit window.
    for (u32 bit = 0; bit < HAO_BUILD_MAX_CANDIDATE_BITS; bit++) {
        HAOBitCandidate c;
        c.bitIndex = bit;
        c.states.reserve(lits.size());

        u32 careCount = 0;
        u32 zeros = 0;
        u32 ones = 0;

        for (const auto &lit : lits) {
            u8 state = HAO_BUILD_STATE_DONT_CARE;
            getBitState(lit, bit, &state); // 0, 1, or don't care.
            c.states.push_back(state);
            // Skip don't-care bits when collecting statistics.
            if (state == HAO_BUILD_STATE_DONT_CARE) {
                continue;
            }
            careCount++;
            if (state) {
                ones++;
            } else {
                zeros++;
            }
        }
        // Skip bit positions that no literal cares about.
        if (!careCount) {
            continue;
        }
        // Heuristic scoring prefers widely cared-about bits with balanced
        // 0/1 distributions.
        const double careRatio =
            static_cast<double>(careCount) / std::max<size_t>(1, lits.size());
        const double entropy = entropyScore(zeros, ones);
        c.score = (careRatio * 0.7) + (entropy * 0.3);
        out.push_back(std::move(c));
    }

    return out;
}

static
void selectBitSelectors(const std::vector<hwlmLiteral> &lits,
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
                  return a.bitIndex < b.bitIndex;
              });

    const u32 targetBits = std::min<u32>(HAO_LAYOUT_KEY_BITS,
                                         verify_u32(candidates.size()));

    std::unordered_set<u64a> signatures;
    std::unordered_set<u32> chosenBits;
    for (const auto &cand : candidates) {
        if (selectors->size() >= targetBits) {
            break;
        }

        // Principle 3: keep only one from identical-feature columns. 
        const u64a sig = signatureOfStates(cand.states);
        if (!signatures.insert(sig).second) {
            continue;
        }

        HAOBitSelector s;
        s.byteOffset = verify_u8(cand.bitIndex / 8);
        s.bitOffset = verify_u8(cand.bitIndex % 8);
        selectors->push_back(s);
        chosenBits.insert(cand.bitIndex);
    }
    // If heuristic selection did not fill the target bit budget,
    // take additional candidates in bit-order until the budget is met.
    if (selectors->size() < targetBits) {
        for (const auto &cand : candidates) {
            if (selectors->size() >= targetBits) {
                break;
            }
            if (chosenBits.find(cand.bitIndex) != chosenBits.end()) {
                continue;
            }
            HAOBitSelector s;
            s.byteOffset = verify_u8(cand.bitIndex / 8);
            s.bitOffset = verify_u8(cand.bitIndex % 8);
            selectors->push_back(s);
            chosenBits.insert(cand.bitIndex);
        }
    }

    std::sort(selectors->begin(), selectors->end(),
              [](const HAOBitSelector &a, const HAOBitSelector &b) {
                  return haoSelectorBitIndex(a) < haoSelectorBitIndex(b);
              });

    if (keyBitsOut) {
        *keyBitsOut = verify_u32(selectors->size());
    }
}

static
void buildRuleMeta(const std::vector<hwlmLiteral> &lits,
                   std::vector<HAOCompileRuleMeta> *ruleMeta,
                   std::vector<u8> *literalBlob) {
    ruleMeta->clear();
    ruleMeta->reserve(lits.size());
    literalBlob->clear();

    for (const auto &lit : lits) {
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
            for (size_t j = 0; j < m.maskLen; j++) {
                m.msk[j] = normMsk[j];
                m.cmp[j] = normCmp[j];
            }
        }
        m.litOffset = verify_u32(literalBlob->size());
        literalBlob->reserve(literalBlob->size() + lit.s.size());
        for (size_t i = 0; i < lit.s.size(); i++) {
            u8 c = verify_u8(lit.s[i]);
            literalBlob->push_back(lit.nocase ? mytoupper(c) : c);
        }
        for (size_t i = 0; i < lit.s.size() && i < sizeof(m.lit); i++) {
            u8 c = verify_u8(lit.s[i]);
            m.lit[i] = lit.nocase ? mytoupper(c) : c;
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
    artifacts->literalBlob.clear();
}

template <class ArtifactsT>
static
bool buildSharedHAOArtifacts(const std::vector<hwlmLiteral> &lits,
                             ArtifactsT *artifacts) {
    if (!artifacts || lits.empty()) {
        return false;
    }

    selectBitSelectors(lits, &artifacts->bitSelectors, &artifacts->keyBits);
    if (artifacts->bitSelectors.empty()) {
        return false;
    }

    buildExtractDescriptor(artifacts->bitSelectors, artifacts);
    buildHAORulePlans(lits, artifacts->bitSelectors, &artifacts->haoRulePlans,
                      &artifacts->haoSummary);
    buildHAOGlobalHashTables(artifacts->haoRulePlans, artifacts->keyBits,
                             &artifacts->haoGlobalHash);
    buildHAORawPrimaryTables(artifacts->bitSelectors, artifacts->haoGlobalHash,
                             &artifacts->haoGlobalHash.primaryHashTableRaw,
                             &artifacts->haoGlobalHash.primaryHashBitmapRaw,
                             &(artifacts->haoGlobalHash
                                   .primaryHashBitmapRawCoarse));
    buildRuleMeta(lits, &artifacts->ruleMeta, &artifacts->literalBlob);
    artifacts->flags = artifacts->haoGlobalHash.flags;
    return true;
}

static
void dumpHAOArtifactsVerbose(const std::vector<hwlmLiteral> &lits,
                             const HAOCompileArtifacts &artifacts) {
    printf("\n========== [HAO][Build-Artifacts] Begin ==========\n");
    printf("[HAO][Params] key_bits(fixed=%u, selector_count=%zu) secondary_key_bits=%u secondary_capacity=%u entry_capacity=%u\n",
           artifacts.keyBits, artifacts.bitSelectors.size(),
           HAO_BUILD_SECONDARY_KEY_BITS, HAO_BUILD_MAX_SECONDARY_ENTRIES,
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
const char *buildFeasibilityReasonName(ReasonT reason) {
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
    case ReasonT::PARTIAL_SECONDARY_CAPACITY:
        return "PARTIAL_SECONDARY_CAPACITY";
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
    return buildFeasibilityReasonName(reason);
}

bool haoGreyEnabled(const Grey &grey) {
    return grey.allowHaoV2;
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
                       bool enableDump) {
    if (!artifacts) {
        return false;
    }

    resetHAOCompileArtifactsCommon(artifacts);
    if (!buildSharedHAOArtifacts(lits, artifacts)) {
        return false;
    }

    if (enableDump) {
        dumpHAOArtifactsVerbose(lits, *artifacts);
    } else if (haoStatsDumpEnabled()) {
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

    if (!haoGreyEnabled(grey)) {
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

    if (lits.size() < 4) {
        local.reason = HAOFeasibilityReason::TOO_FEW_LITERALS;
        if (result) {
            *result = local;
        }
        return false;
    }

    if (lits.size() > std::numeric_limits<u16>::max()) {
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

    if (!buildHAOArtifacts(lits, out, false)) {
        local.reason = HAOFeasibilityReason::ARTIFACT_BUILD_FAILED;
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
        if (out->haoGlobalHash.flags & HAO_ARTIFACT_FLAG_PARTIAL_SECONDARY_CAPACITY) {
            local.reason = HAOFeasibilityReason::PARTIAL_SECONDARY_CAPACITY;
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
bytecode_ptr<u8> buildHAOGlobalBlobImpl(const ArtifactsT &artifacts) {
    std::vector<u32> residualRuleIndexes;

    if (!artifacts.haoGlobalHash.valid) {
        return nullptr;
    }
    if (artifacts.haoRulePlans.size() != artifacts.ruleMeta.size()) {
        return nullptr;
    }

    residualRuleIndexes.reserve(artifacts.haoRulePlans.size());
    for (u32 i = 0; i < artifacts.haoRulePlans.size(); i++) {
        if (haoPlanRequiresResidualEval(artifacts.haoRulePlans[i])) {
            residualRuleIndexes.push_back(i);
        }
    }

    const u32 selectorCount = verify_u32(artifacts.bitSelectors.size());
    const u32 primaryCount =
        verify_u32(artifacts.haoGlobalHash.primaryHashTable.offsets.size());
    const u32 primaryBitmapSize =
        verify_u32(artifacts.haoGlobalHash.primaryHashBitmap.bits.size());
    const u32 primaryCoarseBitmapSize = verify_u32(
        artifacts.haoGlobalHash.primaryHashBitmapCoarse.bits.size());
    const u32 primaryRawCount = verify_u32(
        artifacts.haoGlobalHash.primaryHashTableRaw.offsets.size());
    const u32 primaryBitmapRawSize = verify_u32(
        artifacts.haoGlobalHash.primaryHashBitmapRaw.bits.size());
    const u32 primaryCoarseBitmapRawSize = verify_u32(
        artifacts.haoGlobalHash.primaryHashBitmapRawCoarse.bits.size());
    const u32 secondaryCount =
        verify_u32(artifacts.haoGlobalHash.secondaryHashTable.size());
    const u32 ruleMetaCount = verify_u32(artifacts.ruleMeta.size());
    const u32 literalBlobSize = verify_u32(artifacts.literalBlob.size());
    const u32 residualRuleCount = verify_u32(residualRuleIndexes.size());

    const size_t selectorBytes =
        sizeof(HAORuntimeBitSelector) * artifacts.bitSelectors.size();
    const size_t primaryBitmapBytes =
        artifacts.haoGlobalHash.primaryHashBitmap.bits.size();
    const size_t primaryCoarseBitmapBytes =
        artifacts.haoGlobalHash.primaryHashBitmapCoarse.bits.size();
    const size_t primaryBytes =
        sizeof(u32) * artifacts.haoGlobalHash.primaryHashTable.offsets.size();
    const size_t primaryBitmapRawBytes =
        artifacts.haoGlobalHash.primaryHashBitmapRaw.bits.size();
    const size_t primaryCoarseBitmapRawBytes =
        artifacts.haoGlobalHash.primaryHashBitmapRawCoarse.bits.size();
    const size_t primaryRawBytes =
        sizeof(u32) * artifacts.haoGlobalHash.primaryHashTableRaw.offsets.size();
    const size_t secondaryBytes =
        sizeof(HAORuntimeSecondaryHashEntry) *
        artifacts.haoGlobalHash.secondaryHashTable.size();
    const size_t ruleMetaBytes =
        sizeof(HAORuntimeRuleMeta) * artifacts.ruleMeta.size();
    const size_t literalBlobBytes = artifacts.literalBlob.size();
    const size_t residualRuleBytes = sizeof(u32) * residualRuleIndexes.size();

    if (primaryRawCount != primaryCount ||
        primaryBitmapRawSize != primaryBitmapSize ||
        primaryCoarseBitmapRawSize != primaryCoarseBitmapSize) {
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
    const u32 primaryBitmapRawOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(primaryBitmapRawBytes, alignof(u32));
    const u32 primaryBitmapRawCoarseOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(primaryCoarseBitmapRawBytes, alignof(u32));
    const u32 primaryRawOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(primaryRawBytes, alignof(u32));
    const u32 secondaryOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(secondaryBytes, alignof(u32));
    const u32 ruleMetaOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(ruleMetaBytes, alignof(u32));
    const u32 literalBlobOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(literalBlobBytes, alignof(u32));
    const u32 residualRuleIndexOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(residualRuleBytes, alignof(u32));

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
    hdr->secondaryCount = secondaryCount;
    hdr->ruleMetaCount = ruleMetaCount;
    hdr->literalBlobSize = literalBlobSize;
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
    hdr->secondaryOffset = secondaryOffset;
    hdr->ruleMetaOffset = ruleMetaOffset;
    hdr->literalBlobOffset = literalBlobOffset;
    hdr->residualRuleCount = residualRuleCount;
    hdr->residualRuleIndexOffset = residualRuleIndexOffset;

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
               artifacts.haoGlobalHash.primaryHashBitmap.bits.data(),
               primaryBitmapBytes);
    }
    if (primaryCoarseBitmapBytes) {
        memcpy(base + primaryBitmapCoarseOffset,
               artifacts.haoGlobalHash.primaryHashBitmapCoarse.bits.data(),
               primaryCoarseBitmapBytes);
    }
    if (primaryBytes) {
        memcpy(base + primaryOffset,
               artifacts.haoGlobalHash.primaryHashTable.offsets.data(),
               primaryBytes);
    }
    if (primaryBitmapRawBytes) {
        memcpy(base + primaryBitmapRawOffset,
               artifacts.haoGlobalHash.primaryHashBitmapRaw.bits.data(),
               primaryBitmapRawBytes);
    }
    if (primaryCoarseBitmapRawBytes) {
        memcpy(base + primaryBitmapRawCoarseOffset,
               artifacts.haoGlobalHash.primaryHashBitmapRawCoarse.bits.data(),
               primaryCoarseBitmapRawBytes);
    }
    if (primaryRawBytes) {
        memcpy(base + primaryRawOffset,
               artifacts.haoGlobalHash.primaryHashTableRaw.offsets.data(),
               primaryRawBytes);
    }
    if (secondaryBytes) {
        auto *secondaryOut =
            reinterpret_cast<HAORuntimeSecondaryHashEntry *>(base + secondaryOffset);
        for (u32 i = 0; i < secondaryCount; i++) {
            const auto &src = artifacts.haoGlobalHash.secondaryHashTable[i];
            auto &dst = secondaryOut[i];
            memcpy(dst.ruleVector, src.ruleVector, sizeof(dst.ruleVector));
            memcpy(dst.tableControl, src.tableControl, sizeof(dst.tableControl));
            memcpy(dst.ruleIndex, src.ruleIndex, sizeof(dst.ruleIndex));
            dst.headMask = src.headMask;
            dst.tailMask = src.tailMask;
            dst.slotMask = src.slotMask;
            dst.slotCount = src.slotCount;
            dst.flags = src.flags;
            dst.reserved = 0;
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
        dst.anchorOffset = plan.verifier.anchorOffset;
        dst.anchorLength = plan.verifier.anchorLength;
        dst.verifierFlags = plan.verifier.flags;
        dst.maskLen = srcMeta.maskLen;
        dst.reserved0 = 0;
        dst.planFlags = plan.flags;
        dst.litOffset = srcMeta.litOffset;
        memcpy(dst.lit, srcMeta.lit, sizeof(dst.lit));
        memcpy(dst.msk, srcMeta.msk, sizeof(dst.msk));
        memcpy(dst.cmp, srcMeta.cmp, sizeof(dst.cmp));
    }

    if (literalBlobBytes) {
        memcpy(base + literalBlobOffset, artifacts.literalBlob.data(),
               literalBlobBytes);
    }
    if (residualRuleBytes) {
        memcpy(base + residualRuleIndexOffset, residualRuleIndexes.data(),
               residualRuleBytes);
    }

    return blob;
}

bytecode_ptr<u8> buildHAOBlob(const HAOCompileArtifacts &artifacts) {
    return buildHAOGlobalBlobImpl(artifacts);
}

} // namespace ue2






