#include "pbe_compile.h"

#include "grey.h"
#include "pbe_runtime.h"
#include "util/alloc.h"
#include "util/target_info.h"
#include "util/compare.h"
#include "util/verify_types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_set>
#include <utility>

namespace ue2 {

namespace {

static constexpr u32 PBE_MAX_SUFFIX_BYTES = 8;
static constexpr u32 PBE_MAX_CANDIDATE_BITS = PBE_MAX_SUFFIX_BYTES * 8;
static constexpr u32 PBE_DEFAULT_KEY_BITS = 16;
static constexpr u32 PBE_MAX_RULES_PER_ENTRY = 32;
static constexpr u32 PBE_MAX_EXPANDED_KEYS_PER_RULE = 64;
static constexpr u8 PBE_STATE_DONT_CARE = 2;
static constexpr u16 PBE_RULE_FLAG_NOCASE = 1U << 0;
static constexpr u16 PBE_RULE_FLAG_NORUNS = 1U << 1;

struct PBEBitCandidate {
    u32 bitIndex = 0;
    double score = 0.0;
    std::vector<u8> states; // 0, 1, or PBE_STATE_DONT_CARE
};

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

    if (!lit.msk.empty() && !lit.cmp.empty()) {
        const u32 mlen = verify_u32(lit.msk.size());
        if (byteFromEnd < mlen) {
            const u8 m = lit.msk[mlen - byteFromEnd - 1];
            if (m & (1U << bitInByte)) {
                const u8 v = lit.cmp[mlen - byteFromEnd - 1];
                care = true;
                value = !!(v & (1U << bitInByte));
            }
        }
    }

    *state = care ? (value ? 1 : 0) : PBE_STATE_DONT_CARE;
    return true;
}

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
std::vector<PBEBitCandidate> buildBitCandidates(
    const std::vector<hwlmLiteral> &lits) {
    std::vector<PBEBitCandidate> out;
    out.reserve(PBE_MAX_CANDIDATE_BITS);

    for (u32 bit = 0; bit < PBE_MAX_CANDIDATE_BITS; bit++) {
        PBEBitCandidate c;
        c.bitIndex = bit;
        c.states.reserve(lits.size());

        u32 careCount = 0;
        u32 zeros = 0;
        u32 ones = 0;

        for (const auto &lit : lits) {
            u8 state = PBE_STATE_DONT_CARE;
            getBitState(lit, bit, &state);
            c.states.push_back(state);
            if (state == PBE_STATE_DONT_CARE) {
                continue;
            }
            careCount++;
            if (state) {
                ones++;
            } else {
                zeros++;
            }
        }

        if (!careCount) {
            continue;
        }

        // Principle 1 + 2:
        // prioritize low don't-care ratio and high discrimination.
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
                        std::vector<PBEBitSelector> *selectors,
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
              [](const PBEBitCandidate &a, const PBEBitCandidate &b) {
                  if (a.score != b.score) {
                      return a.score > b.score;
                  }
                  return a.bitIndex < b.bitIndex;
              });

    const u32 targetBits =
        std::min<u32>(PBE_DEFAULT_KEY_BITS, verify_u32(candidates.size()));

    std::unordered_set<u64a> signatures;
    for (const auto &cand : candidates) {
        if (selectors->size() >= targetBits) {
            break;
        }

        // Principle 3: keep only one from identical-feature columns.
        const u64a sig = signatureOfStates(cand.states);
        if (!signatures.insert(sig).second) {
            continue;
        }

        PBEBitSelector s;
        s.byteOffset = verify_u8(cand.bitIndex / 8);
        s.bitOffset = verify_u8(cand.bitIndex % 8);
        selectors->push_back(s);
    }

    if (keyBitsOut) {
        *keyBitsOut = verify_u32(selectors->size());
    }
}

static
bool enumerateHashKeysForLiteral(const hwlmLiteral &lit,
                                 const std::vector<PBEBitSelector> &selectors,
                                 std::vector<u32> *keys,
                                 bool *truncated) {
    if (!keys) {
        return false;
    }

    keys->clear();
    keys->push_back(0);
    bool cut = false;

    for (u32 i = 0; i < selectors.size(); i++) {
        const auto &sel = selectors[i];
        const u32 bitIndex = static_cast<u32>(sel.byteOffset) * 8U +
                             static_cast<u32>(sel.bitOffset);

        u8 state = PBE_STATE_DONT_CARE;
        getBitState(lit, bitIndex, &state);

        if (state == PBE_STATE_DONT_CARE) {
            const size_t oldSize = keys->size();
            if (oldSize >= PBE_MAX_EXPANDED_KEYS_PER_RULE) {
                // Keep subset and mark truncated; runtime fallback still covers
                // semantic corner cases for this rule.
                cut = true;
                continue;
            }

            const size_t newSize = std::min<size_t>(
                oldSize * 2, PBE_MAX_EXPANDED_KEYS_PER_RULE);
            keys->resize(newSize);
            for (size_t k = 0; k < newSize - oldSize; k++) {
                (*keys)[oldSize + k] = (*keys)[k] | (1U << i);
            }

            if (newSize < oldSize * 2) {
                cut = true;
            }
            continue;
        }

        if (state) {
            for (auto &k : *keys) {
                k |= (1U << i);
            }
        }
    }

    if (truncated) {
        *truncated = cut;
    }
    return true;
}

static
void buildHashTables(const std::vector<hwlmLiteral> &lits,
                     const std::vector<PBEBitSelector> &selectors,
                     PBEPrimaryHashTable *primaryHashTable,
                     std::vector<PBESecondaryHashEntry> *secondaryHashTable,
                     u32 *flags) {
    primaryHashTable->offsets.clear();
    secondaryHashTable->clear();

    if (selectors.empty()) {
        return;
    }

    const u32 tableSize = 1U << verify_u32(selectors.size());
    primaryHashTable->offsets.assign(tableSize, 0);

    // secondaryHashTable[0] stays empty as a null target.
    secondaryHashTable->push_back(PBESecondaryHashEntry{});

    std::map<u32, std::vector<u32>> keyToLiteralIndexes;
    for (u32 i = 0; i < lits.size(); i++) {
        std::vector<u32> keys;
        bool truncated = false;
        if (!enumerateHashKeysForLiteral(lits[i], selectors, &keys, &truncated)) {
            continue;
        }
        if (truncated && flags) {
            *flags |= PBE_ARTIFACT_FLAG_PARTIAL_COVERAGE;
        }

        for (const auto key : keys) {
            keyToLiteralIndexes[key].push_back(i);
        }
    }

    for (const auto &it : keyToLiteralIndexes) {
        const u32 key = it.first;
        const auto &idxes = it.second;
        if (idxes.empty()) {
            continue;
        }

        PBESecondaryHashEntry entry = {};
        entry.ruleBase = verify_u16(idxes.front());
        entry.ruleCount =
            verify_u16(std::min<size_t>(idxes.size(), PBE_MAX_RULES_PER_ENTRY));
        if (idxes.size() > PBE_MAX_RULES_PER_ENTRY && flags) {
            *flags |= PBE_ARTIFACT_FLAG_PARTIAL_COVERAGE;
        }

        for (u32 i = 0; i < entry.ruleCount; i++) {
            const auto &lit = lits[idxes[i]];
            const u32 len = verify_u32(lit.s.size());
            const u8 last = verify_u8(lit.s.back());

            entry.ruleVector[i] = lit.nocase ? mytoupper(last) : last;
            entry.tableControl[i] = verify_u8(std::min<u32>(len, 255));
            if (len) {
                entry.tailMask |= (1U << i);
            }
            if (len > 1) {
                entry.headMask |= (1U << i);
            }
        }

        const u32 secondaryOffset = verify_u32(secondaryHashTable->size());
        secondaryHashTable->push_back(entry);
        primaryHashTable->offsets[key] = secondaryOffset;
    }
}

static
void buildRuleMeta(const std::vector<hwlmLiteral> &lits,
                   std::vector<PBERuleMeta> *ruleMeta) {
    ruleMeta->clear();
    ruleMeta->reserve(lits.size());

    for (const auto &lit : lits) {
        PBERuleMeta m = {};
        m.id = lit.id;
        m.groups = lit.groups;
        m.len = verify_u16(std::min<size_t>(lit.s.size(),
                                            std::numeric_limits<u16>::max()));
        if (lit.nocase) {
            m.flags |= PBE_RULE_FLAG_NOCASE;
        }
        if (lit.noruns) {
            m.flags |= PBE_RULE_FLAG_NORUNS;
        }
        ruleMeta->push_back(m);
    }
}

} // namespace

bool canBuildPBE(const target_t &target, const std::vector<hwlmLiteral> &lits,
                 const Grey &grey) {
    if (!grey.allowPbe) {
        return false;
    }

    // PBE is currently only enabled on Arm64 builds.
#if !defined(__aarch64__)
    (void)target;
    return false;
#else
    (void)target;
#endif

    if (lits.size() < 4) {
        return false;
    }

    if (lits.size() > std::numeric_limits<u16>::max()) {
        return false;
    }

    return true;
}

bool buildPBEArtifacts(const std::vector<hwlmLiteral> &lits,
                       PBECompileArtifacts *artifacts) {
    if (!artifacts) {
        return false;
    }

    artifacts->keyBits = 0;
    artifacts->flags = 0;
    artifacts->bitSelectors.clear();
    artifacts->primaryHashTable.offsets.clear();
    artifacts->secondaryHashTable.clear();
    artifacts->ruleMeta.clear();

    if (lits.empty()) {
        return false;
    }

    selectBitSelectors(lits, &artifacts->bitSelectors, &artifacts->keyBits);
    if (artifacts->bitSelectors.empty()) {
        return false;
    }

    buildHashTables(lits, artifacts->bitSelectors, &artifacts->primaryHashTable,
                    &artifacts->secondaryHashTable, &artifacts->flags);
    buildRuleMeta(lits, &artifacts->ruleMeta);

    return true;
}

bytecode_ptr<u8> buildPBEBlob(const PBECompileArtifacts &artifacts) {
    const u32 selectorCount = verify_u32(artifacts.bitSelectors.size());
    const u32 primaryCount = verify_u32(artifacts.primaryHashTable.offsets.size());
    const u32 secondaryCount = verify_u32(artifacts.secondaryHashTable.size());
    const u32 ruleMetaCount = verify_u32(artifacts.ruleMeta.size());

    const size_t selectorBytes =
        sizeof(PBERuntimeBitSelector) * artifacts.bitSelectors.size();
    const size_t primaryBytes = sizeof(u32) * artifacts.primaryHashTable.offsets.size();
    const size_t secondaryBytes = sizeof(PBERuntimeSecondaryHashEntry) *
                                  artifacts.secondaryHashTable.size();
    const size_t ruleMetaBytes =
        sizeof(PBERuntimeRuleMeta) * artifacts.ruleMeta.size();

    size_t totalSize = ROUNDUP_N(sizeof(PBERuntimeHeader), alignof(u32));
    const u32 selectorsOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(selectorBytes, alignof(u32));
    const u32 primaryOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(primaryBytes, alignof(u32));
    const u32 secondaryOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(secondaryBytes, alignof(u32));
    const u32 ruleMetaOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(ruleMetaBytes, alignof(u32));

    auto blob = make_zeroed_bytecode_ptr<u8>(totalSize);
    if (!blob) {
        return nullptr;
    }

    auto *hdr = reinterpret_cast<PBERuntimeHeader *>(blob.get());
    hdr->magic = PBE_RUNTIME_MAGIC;
    hdr->version = PBE_RUNTIME_VERSION;
    hdr->flags = artifacts.flags;
    hdr->keyBits = artifacts.keyBits;
    hdr->selectorCount = selectorCount;
    hdr->primaryCount = primaryCount;
    hdr->secondaryCount = secondaryCount;
    hdr->ruleMetaCount = ruleMetaCount;
    hdr->selectorsOffset = selectorsOffset;
    hdr->primaryOffset = primaryOffset;
    hdr->secondaryOffset = secondaryOffset;
    hdr->ruleMetaOffset = ruleMetaOffset;

    u8 *base = blob.get();
    auto *selectorsOut =
        reinterpret_cast<PBERuntimeBitSelector *>(base + selectorsOffset);
    for (u32 i = 0; i < selectorCount; i++) {
        selectorsOut[i].byteOffset = artifacts.bitSelectors[i].byteOffset;
        selectorsOut[i].bitOffset = artifacts.bitSelectors[i].bitOffset;
        selectorsOut[i].reserved = 0;
    }

    auto *primaryOut = reinterpret_cast<u32 *>(base + primaryOffset);
    for (u32 i = 0; i < primaryCount; i++) {
        primaryOut[i] = artifacts.primaryHashTable.offsets[i];
    }

    auto *secondaryOut =
        reinterpret_cast<PBERuntimeSecondaryHashEntry *>(base + secondaryOffset);
    for (u32 i = 0; i < secondaryCount; i++) {
        const auto &in = artifacts.secondaryHashTable[i];
        auto &out = secondaryOut[i];
        memcpy(out.ruleVector, in.ruleVector, sizeof(out.ruleVector));
        memcpy(out.tableControl, in.tableControl, sizeof(out.tableControl));
        out.headMask = in.headMask;
        out.tailMask = in.tailMask;
        out.ruleBase = in.ruleBase;
        out.ruleCount = in.ruleCount;
    }

    auto *ruleMetaOut =
        reinterpret_cast<PBERuntimeRuleMeta *>(base + ruleMetaOffset);
    for (u32 i = 0; i < ruleMetaCount; i++) {
        ruleMetaOut[i].id = artifacts.ruleMeta[i].id;
        ruleMetaOut[i].groups = artifacts.ruleMeta[i].groups;
        ruleMetaOut[i].len = artifacts.ruleMeta[i].len;
        ruleMetaOut[i].flags = artifacts.ruleMeta[i].flags;
    }

    return blob;
}

} // namespace ue2
