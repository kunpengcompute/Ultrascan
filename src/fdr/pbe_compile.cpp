#include "pbe_compile.h"

#include "grey.h"
#include "pbe_runtime.h"
#include "util/alloc.h"
#include "util/target_info.h"
#include "util/compare.h"
#include "util/verify_types.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <limits>
#include <map>
#include <sstream>
#include <string>
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

struct PBEBitCandidate {
    u32 bitIndex = 0;
    double score = 0.0;
    std::vector<u8> states; // 0, 1, or PBE_STATE_DONT_CARE
};

static
bool enumerateHashKeysForLiteral(const hwlmLiteral &lit,
                                 const std::vector<PBEBitSelector> &selectors,
                                 std::vector<u32> *keys,
                                 bool *truncated);

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
void dumpRuleBits(const std::vector<hwlmLiteral> &lits) {
    printf("[PBE][Rules-Bits] rule_count=%zu\n", lits.size());
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
void dumpSelectors(const std::vector<PBEBitSelector> &selectors) {
    printf("[PBE][Selectors] count=%zu\n", selectors.size());
    for (size_t i = 0; i < selectors.size(); i++) {
        const auto &s = selectors[i];
        const u32 bitIndex =
            static_cast<u32>(s.byteOffset) * 8U + static_cast<u32>(s.bitOffset);
        printf("  s%zu -> suffix_bit=%u (byteOffset=%u, bitOffset=%u)\n", i,
               bitIndex, (u32)s.byteOffset, (u32)s.bitOffset);
    }
}

static
void dumpRuleKeys(const std::vector<hwlmLiteral> &lits,
                  const std::vector<PBEBitSelector> &selectors,
                  u32 keyBits) {
    printf("[PBE][Rule->Keys] key_bits=%u\n", keyBits);
    for (size_t i = 0; i < lits.size(); i++) {
        std::vector<u32> keys;
        bool truncated = false;
        if (!enumerateHashKeysForLiteral(lits[i], selectors, &keys, &truncated)) {
            printf("  r%zu id=%u keys=<enumerate-failed>\n", i, lits[i].id);
            continue;
        }
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        printf("  r%zu id=%u keys(%zu)%s: ", i, lits[i].id, keys.size(),
               truncated ? " [TRUNCATED]" : "");
        for (size_t k = 0; k < keys.size(); k++) {
            const u32 key = keys[k];
            printf("{dec=%u hex=0x%x bin=%s}", key, key,
                   keyToBits(key, keyBits).c_str());
            if (k + 1 != keys.size()) {
                printf(", ");
            }
        }
        printf("\n");
    }
}

static
void dumpHashTables(const PBECompileArtifacts &artifacts,
                    const std::vector<hwlmLiteral> &lits) {
    printf("[PBE][L1] size=%zu\n", artifacts.primaryHashTable.offsets.size());
    size_t nonEmpty = 0;
    for (u32 key = 0; key < artifacts.primaryHashTable.offsets.size(); key++) {
        const u32 off = artifacts.primaryHashTable.offsets[key];
        if (!off) {
            continue;
        }
        nonEmpty++;
        printf("  key={dec=%u hex=0x%x bin=%s} -> value={dec=%u hex=0x%x}\n", key,
               key, keyToBits(key, artifacts.keyBits).c_str(), off, off);
    }
    printf("  non_empty=%zu\n", nonEmpty);

    printf("[PBE][L2] size=%zu (entry0 is null)\n",
           artifacts.secondaryHashTable.size());
    for (u32 i = 1; i < artifacts.secondaryHashTable.size(); i++) {
        const auto &e = artifacts.secondaryHashTable[i];
        printf("  L2[%u] ruleBase=%u ruleCount=%u headMask=0x%08x tailMask=0x%08x\n",
               i, (u32)e.ruleBase, (u32)e.ruleCount, e.headMask, e.tailMask);
        for (u32 j = 0; j < e.ruleCount && j < 32; j++) {
            const u16 ridx = e.ruleIndex[j];
            const u8 rv = e.ruleVector[j];
            const u8 len = e.tableControl[j];
            const char pc = std::isprint((unsigned char)rv) ? (char)rv : '.';
            printf("    slot%u: ruleIndex=%u len=%u ruleVector={'%c', dec=%u, hex=0x%02x, bin=%s}",
                   j, (u32)ridx, (u32)len, pc, (u32)rv, (u32)rv,
                   byteToBits(rv).c_str());
            if (ridx < lits.size()) {
                printf(" literal={id=%u s=\"%s\"}", lits[ridx].id,
                       lits[ridx].s.c_str());
            }
            printf("\n");
        }
    }
}

static
void dumpPBEArtifactsVerbose(const std::vector<hwlmLiteral> &lits,
                             const PBECompileArtifacts &artifacts) {
    printf("\n========== [PBE][Build-Artifacts] Begin ==========\n");
    dumpRuleBits(lits);
    dumpSelectors(artifacts.bitSelectors);
    dumpRuleKeys(lits, artifacts.bitSelectors, artifacts.keyBits);
    dumpHashTables(artifacts, lits);
    printf("[PBE][Flags] artifacts.flags=0x%x\n", artifacts.flags);
    printf("========== [PBE][Build-Artifacts] End ==========\n\n");
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

            entry.ruleIndex[i] = verify_u16(idxes[i]);
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
                   std::vector<PBERuleMeta> *ruleMeta,
                   std::vector<u8> *literalBlob) {
    ruleMeta->clear();
    ruleMeta->reserve(lits.size());
    literalBlob->clear();

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
        std::array<u8, 8> normMsk = {};
        std::array<u8, 8> normCmp = {};
        u8 normLen = 0;
        if (normalizeMaskCmp(lit, &normMsk, &normCmp, &normLen) && normLen) {
            m.flags |= PBE_RULE_FLAG_HAS_MASK;
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
    artifacts->literalBlob.clear();

    if (lits.empty()) {
        return false;
    }

    selectBitSelectors(lits, &artifacts->bitSelectors, &artifacts->keyBits);
    if (artifacts->bitSelectors.empty()) {
        return false;
    }

    buildHashTables(lits, artifacts->bitSelectors, &artifacts->primaryHashTable,
                    &artifacts->secondaryHashTable, &artifacts->flags);
    buildRuleMeta(lits, &artifacts->ruleMeta, &artifacts->literalBlob);

    // Compile-time dump for selector/key/hash construction inspection.
    dumpPBEArtifactsVerbose(lits, *artifacts);

    // PBE-only policy: if current artifacts cannot provide full coverage,
    // treat this literal set as not buildable by PBE.
    if (artifacts->flags & PBE_ARTIFACT_FLAG_PARTIAL_COVERAGE) {
        return false;
    }

    return true;
}

bytecode_ptr<u8> buildPBEBlob(const PBECompileArtifacts &artifacts) {
    const u32 selectorCount = verify_u32(artifacts.bitSelectors.size());
    const u32 primaryCount = verify_u32(artifacts.primaryHashTable.offsets.size());
    const u32 secondaryCount = verify_u32(artifacts.secondaryHashTable.size());
    const u32 ruleMetaCount = verify_u32(artifacts.ruleMeta.size());
    const u32 literalBlobSize = verify_u32(artifacts.literalBlob.size());

    const size_t selectorBytes =
        sizeof(PBERuntimeBitSelector) * artifacts.bitSelectors.size();
    const size_t primaryBytes = sizeof(u32) * artifacts.primaryHashTable.offsets.size();
    const size_t secondaryBytes = sizeof(PBERuntimeSecondaryHashEntry) *
                                  artifacts.secondaryHashTable.size();
    const size_t ruleMetaBytes =
        sizeof(PBERuntimeRuleMeta) * artifacts.ruleMeta.size();
    const size_t literalBlobBytes = artifacts.literalBlob.size();

    size_t totalSize = ROUNDUP_N(sizeof(PBERuntimeHeader), alignof(u32));
    const u32 selectorsOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(selectorBytes, alignof(u32));
    const u32 primaryOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(primaryBytes, alignof(u32));
    const u32 secondaryOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(secondaryBytes, alignof(u32));
    const u32 ruleMetaOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(ruleMetaBytes, alignof(u32));
    const u32 literalBlobOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(literalBlobBytes, alignof(u32));

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
    hdr->literalBlobSize = literalBlobSize;
    hdr->selectorsOffset = selectorsOffset;
    hdr->primaryOffset = primaryOffset;
    hdr->secondaryOffset = secondaryOffset;
    hdr->ruleMetaOffset = ruleMetaOffset;
    hdr->literalBlobOffset = literalBlobOffset;

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
        memcpy(out.ruleIndex, in.ruleIndex, sizeof(out.ruleIndex));
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
        ruleMetaOut[i].maskLen = artifacts.ruleMeta[i].maskLen;
        ruleMetaOut[i].litOffset = artifacts.ruleMeta[i].litOffset;
        memcpy(ruleMetaOut[i].lit, artifacts.ruleMeta[i].lit,
               sizeof(ruleMetaOut[i].lit));
        memcpy(ruleMetaOut[i].msk, artifacts.ruleMeta[i].msk,
               sizeof(ruleMetaOut[i].msk));
        memcpy(ruleMetaOut[i].cmp, artifacts.ruleMeta[i].cmp,
               sizeof(ruleMetaOut[i].cmp));
    }

    if (!artifacts.literalBlob.empty()) {
        memcpy(base + literalBlobOffset, artifacts.literalBlob.data(),
               artifacts.literalBlob.size());
    }

    return blob;
}

} // namespace ue2
