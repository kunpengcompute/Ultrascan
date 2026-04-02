#include "pbe_compile.h"

#include "grey.h"
#include "pbe_runtime.h"
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
#include <cstring>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>

namespace ue2 {

namespace {

static constexpr u32 PBE_MAX_SUFFIX_BYTES = 8;
static constexpr u32 PBE_MAX_CANDIDATE_BITS = PBE_MAX_SUFFIX_BYTES * 8;
static constexpr u32 PBE_SECONDARY_KEY_BITS = 18;
static constexpr u32 PBE_MAX_SECONDARY_ENTRIES = 1U << PBE_SECONDARY_KEY_BITS;
static constexpr u64a PBE_MAX_TOTAL_PRIMARY_FOOTPRINT =
    128ULL * 1024ULL * 1024ULL;
static constexpr u8 PBE_STATE_DONT_CARE = 2;

struct PBEBitCandidate {
    u32 bitIndex = 0;
    double score = 0.0;
    std::vector<u8> states; // 0, 1, or PBE_STATE_DONT_CARE
};

static
bool getBitState(const hwlmLiteral &lit, u32 bitIndex, u8 *state);

static
void computeKeyValueMaskForLiteral(const hwlmLiteral &lit,
                                   const std::vector<PBEBitSelector> &selectors,
                                   u32 *keyValue, u32 *keyMask) {
    u32 v = 0;
    u32 m = 0;
    for (u32 i = 0; i < selectors.size(); i++) {
        const auto &sel = selectors[i];
        const u32 bitIndex = static_cast<u32>(sel.byteOffset) * 8U +
                             static_cast<u32>(sel.bitOffset);
        u8 state = PBE_STATE_DONT_CARE;
        getBitState(lit, bitIndex, &state);
        if (state == PBE_STATE_DONT_CARE) {
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
u32 selectorBitIndex(const PBEBitSelector &selector) {
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
std::string maskToBits(u32 mask) {
    return keyToBits(mask, PBE_RULE_VECTOR_BYTES);
}

static
const char *extractModeName(u32 mode) {
    switch (mode) {
    case PBE_EXTRACT_MODE_SCALAR:
        return "scalar";
    case PBE_EXTRACT_MODE_BEXT:
        return "bext";
    default:
        return "unknown";
    }
}

static
u32 encodePrimaryValue(u32 secondaryOffset, u32 entryCount) {
    assert(secondaryOffset <= PBE_L1_OFFSET_MASK);
    assert(entryCount < (1U << (32U - PBE_L1_COUNT_SHIFT)));
    return (entryCount << PBE_L1_COUNT_SHIFT) | secondaryOffset;
}

static
u8 normalizedLiteralByte(u8 c) {
    return ourisalpha(c) ? verify_u8(mytoupper(c)) : c;
}

static
u32 pbePrimaryBitmapBytes(u32 primaryCount) {
    return (primaryCount + 7U) / 8U;
}

static
u32 pbePackedKeyBits(u32 classMask) {
    return classMask ? popcount32(classMask) : 0;
}

static
u32 pbePrimaryCountForKeyBits(u32 keyBits) {
    if (!keyBits) {
        return 1U;
    }
    assert(keyBits <= PBE_KEY_BITS);
    return 1U << keyBits;
}

static
u32 pbeFullKeyMask(u32 keyBits) {
    if (!keyBits) {
        return 0;
    }
    if (keyBits >= 32U) {
        return 0xffffffffU;
    }
    return (1U << keyBits) - 1U;
}

static
void buildExtractDescriptor(const std::vector<PBEBitSelector> &selectors,
                            PBECompileArtifacts *artifacts) {
    if (!artifacts) {
        return;
    }

    artifacts->extractMode = PBE_EXTRACT_MODE_SCALAR;
    artifacts->windowBytes = PBE_BYTES_PER_RULE_SLOT;
    artifacts->bextMask = 0;

    if (selectors.empty() || selectors.size() > PBE_MAX_SELECTORS) {
        return;
    }

    for (const auto &selector : selectors) {
        artifacts->bextMask |= (1ULL << selectorBitIndex(selector));
    }
    artifacts->extractMode = PBE_EXTRACT_MODE_BEXT;
}

static
void pbePrimaryBitmapSet(std::vector<u8> *bitmap, u32 idx) {
    if (!bitmap || idx / 8U >= bitmap->size()) {
        return;
    }
    (*bitmap)[idx / 8U] |= verify_u8(1U << (idx % 8U));
}

static
void buildPrimaryBitmap(const PBEPrimaryHashTable &primaryHashTable,
                        PBEPrimaryHashBitmap *primaryHashBitmap) {
    if (!primaryHashBitmap) {
        return;
    }
    primaryHashBitmap->bits.clear();
    const u32 primaryCount = verify_u32(primaryHashTable.offsets.size());
    primaryHashBitmap->bits.assign(pbePrimaryBitmapBytes(primaryCount), 0);
    for (u32 i = 0; i < primaryCount; i++) {
        if (primaryHashTable.offsets[i]) {
            pbePrimaryBitmapSet(&primaryHashBitmap->bits, i);
        }
    }
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
        const u32 bitIndex = selectorBitIndex(s);
        printf("  s%zu -> suffix_bit=%u (byteOffset=%u, bitOffset=%u)\n", i,
               bitIndex, (u32)s.byteOffset, (u32)s.bitOffset);
    }
}

static
void dumpExtractDescriptor(const PBECompileArtifacts &artifacts) {
    printf("[PBE][Extract] mode=%s windowBytes=%u bextMask=0x%llx\n",
           extractModeName(artifacts.extractMode), artifacts.windowBytes,
           (unsigned long long)artifacts.bextMask);
}

static
void dumpRuleKeys(const std::vector<hwlmLiteral> &lits,
                  const std::vector<PBEBitSelector> &selectors,
                  u32 keyBits) {
    printf("[PBE][Rule->KeyMask] key_bits=%u\n", keyBits);
    for (size_t i = 0; i < lits.size(); i++) {
        u32 keyValue = 0;
        u32 keyMask = 0;
        computeKeyValueMaskForLiteral(lits[i], selectors, &keyValue, &keyMask);
        printf("  r%zu id=%u keyValue={dec=%u hex=0x%x bin=%s} keyMask={dec=%u hex=0x%x bin=%s}\n",
               i, lits[i].id, keyValue, keyValue,
               keyToBits(keyValue, keyBits).c_str(), keyMask, keyMask,
               keyToBits(keyMask, keyBits).c_str());
    }
}

static
void dumpHashTables(const PBECompileArtifacts &artifacts,
                    const std::vector<hwlmLiteral> &lits) {
    printf("[PBE][Mask-Classes] count=%zu\n", artifacts.maskClasses.size());
    for (const auto &klass : artifacts.maskClasses) {
        printf("  classId=%u classMask={dec=%u hex=0x%x bin=%s} classKeyBits=%u secondaryOffset=%u secondaryCount=%u\n",
               klass.classId, klass.classMask, klass.classMask,
               keyToBits(klass.classMask, artifacts.keyBits).c_str(),
               klass.classKeyBits, klass.secondaryOffset,
               klass.secondaryCount);
        printf("    [L1] size=%zu\n", klass.primaryHashTable.offsets.size());
        printf("    [L1-Bitmap] bytes=%zu\n", klass.primaryHashBitmap.bits.size());
        size_t nonEmpty = 0;
        for (u32 key = 0; key < klass.primaryHashTable.offsets.size(); key++) {
            const u32 off = klass.primaryHashTable.offsets[key];
            if (!off) {
                continue;
            }
            nonEmpty++;
            const u32 secondaryOffset = off & PBE_L1_OFFSET_MASK;
            const u32 entryCount = off >> PBE_L1_COUNT_SHIFT;
            printf("      key={dec=%u hex=0x%x bin=%s} -> value={dec=%u hex=0x%x offset=%u count=%u}\n",
                   key, key, keyToBits(key, klass.classKeyBits).c_str(), off, off,
                   secondaryOffset, entryCount);
        }
        printf("      non_empty=%zu\n", nonEmpty);
    }

    printf("[PBE][L2] size=%zu (entry0 is null)\n",
           artifacts.secondaryHashTable.size());
    for (u32 i = 1; i < artifacts.secondaryHashTable.size(); i++) {
        const auto &e = artifacts.secondaryHashTable[i];
        printf("  L2[%u] ruleCount=%u entryCapacity=%u headMask=0x%08x tailMask=0x%08x\n",
               i, (u32)e.ruleCount, PBE_RULE_SLOTS_PER_ENTRY, e.headMask,
               e.tailMask);
        printf("    headMaskBits=%s\n", maskToBits(e.headMask).c_str());
        printf("    tailMaskBits=%s\n", maskToBits(e.tailMask).c_str());
        for (u32 j = 0; j < e.ruleCount && j < PBE_RULE_SLOTS_PER_ENTRY; j++) {
            const u16 ridx = e.ruleIndex[j];
            const u8 *rv = &e.ruleVector[j * PBE_BYTES_PER_RULE_SLOT];
            const u8 *tc = &e.tableControl[j * PBE_BYTES_PER_RULE_SLOT];
            printf("    slot%u: ruleIndex=%u keyValue=0x%x keyMask=0x%x suffix=[",
                   j, (u32)ridx, e.keyValue[j], e.keyMask[j]);
            for (u32 k = 0; k < PBE_BYTES_PER_RULE_SLOT; k++) {
                const char pc = std::isprint((unsigned char)rv[k]) ?
                                (char)rv[k] : '.';
                printf("{'%c',0x%02x,%s}%s", pc, (u32)rv[k],
                       byteToBits(rv[k]).c_str(),
                       k + 1 == PBE_BYTES_PER_RULE_SLOT ? "" : ", ");
            }
            printf("] tbl=[");
            for (u32 k = 0; k < PBE_BYTES_PER_RULE_SLOT; k++) {
                printf("%u%s", (u32)tc[k],
                       k + 1 == PBE_BYTES_PER_RULE_SLOT ? "" : ", ");
            }
            printf("]");
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
    printf("[PBE][Params] key_bits(fixed=%u, selector_count=%zu, mask_classes=%zu) secondary_key_bits=%u secondary_capacity=%u entry_capacity=%u\n",
           artifacts.keyBits, artifacts.bitSelectors.size(),
           artifacts.maskClasses.size(), PBE_SECONDARY_KEY_BITS,
           PBE_MAX_SECONDARY_ENTRIES, PBE_RULE_SLOTS_PER_ENTRY);
    dumpRuleBits(lits);
    dumpSelectors(artifacts.bitSelectors);
    dumpExtractDescriptor(artifacts);
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

    const u32 targetBits = std::min<u32>(PBE_KEY_BITS,
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

        PBEBitSelector s;
        s.byteOffset = verify_u8(cand.bitIndex / 8);
        s.bitOffset = verify_u8(cand.bitIndex % 8);
        selectors->push_back(s);
        chosenBits.insert(cand.bitIndex);
    }

    if (selectors->size() < targetBits) {
        for (const auto &cand : candidates) {
            if (selectors->size() >= targetBits) {
                break;
            }
            if (chosenBits.find(cand.bitIndex) != chosenBits.end()) {
                continue;
            }
            PBEBitSelector s;
            s.byteOffset = verify_u8(cand.bitIndex / 8);
            s.bitOffset = verify_u8(cand.bitIndex % 8);
            selectors->push_back(s);
            chosenBits.insert(cand.bitIndex);
        }
    }

    std::sort(selectors->begin(), selectors->end(),
              [](const PBEBitSelector &a, const PBEBitSelector &b) {
                  return selectorBitIndex(a) < selectorBitIndex(b);
              });

    if (keyBitsOut) {
        *keyBitsOut = verify_u32(selectors->size());
    }
}

static
void buildHashTables(const std::vector<hwlmLiteral> &lits,
                     const std::vector<PBEBitSelector> &selectors,
                     std::vector<PBEMaskClassArtifacts> *maskClasses,
                     PBEPrimaryHashTable *legacyPrimaryHashTable,
                     PBEPrimaryHashBitmap *legacyPrimaryHashBitmap,
                     std::vector<PBESecondaryHashEntry> *secondaryHashTable,
                     u32 *flags) {
    if (maskClasses) {
        maskClasses->clear();
    }
    legacyPrimaryHashTable->offsets.clear();
    legacyPrimaryHashBitmap->bits.clear();
    secondaryHashTable->clear();

    if (selectors.empty()) {
        return;
    }

    // secondaryHashTable[0] stays empty as a null target.
    secondaryHashTable->push_back(PBESecondaryHashEntry{});

    std::vector<u32> litKeyValue(lits.size(), 0);
    std::map<u32, std::vector<u32>> maskToLiteralIndexes;
    const u32 fullMask = pbeFullKeyMask(verify_u32(selectors.size()));
    for (u32 i = 0; i < lits.size(); i++) {
        u32 keyValue = 0;
        u32 keyMask = 0;
        computeKeyValueMaskForLiteral(lits[i], selectors, &keyValue, &keyMask);
        litKeyValue[i] = keyValue;
        maskToLiteralIndexes[keyMask].push_back(i);
    }

    std::vector<u32> orderedMasks;
    orderedMasks.reserve(maskToLiteralIndexes.size());
    for (const auto &it : maskToLiteralIndexes) {
        orderedMasks.push_back(it.first);
    }
    std::sort(orderedMasks.begin(), orderedMasks.end(),
              [fullMask](u32 a, u32 b) {
                  const bool aFull = a == fullMask;
                  const bool bFull = b == fullMask;
                  if (aFull != bFull) {
                      return aFull;
                  }
                  const u32 aBits = pbePackedKeyBits(a);
                  const u32 bBits = pbePackedKeyBits(b);
                  if (aBits != bBits) {
                      return aBits > bBits;
                  }
                  return a < b;
              });

    if (orderedMasks.size() > PBE_MAX_MASK_CLASSES) {
        if (flags) {
            *flags |= PBE_ARTIFACT_FLAG_PARTIAL_COVERAGE;
        }
        return;
    }

    u64a totalPrimaryFootprint = 0;
    u32 classId = 0;
    bool stop = false;
    for (u32 classMask : orderedMasks) {
        if (stop) {
            break;
        }
        const auto &idxes = maskToLiteralIndexes[classMask];
        if (idxes.empty() || !maskClasses) {
            continue;
        }

        PBEMaskClassArtifacts klass = {};
        klass.classId = classId++;
        klass.classMask = classMask;
        klass.classKeyBits = pbePackedKeyBits(classMask);
        klass.secondaryOffset = verify_u32(secondaryHashTable->size());

        const u32 primaryCount = pbePrimaryCountForKeyBits(klass.classKeyBits);
        totalPrimaryFootprint +=
            (u64a)primaryCount * sizeof(u32) + pbePrimaryBitmapBytes(primaryCount);
        if (totalPrimaryFootprint > PBE_MAX_TOTAL_PRIMARY_FOOTPRINT) {
            if (flags) {
                *flags |= PBE_ARTIFACT_FLAG_PARTIAL_COVERAGE;
            }
            break;
        }
        klass.primaryHashTable.offsets.assign(primaryCount, 0);

        std::map<u32, std::vector<u32>> keyToLiteralIndexes;
        for (u32 ridx : idxes) {
            const u32 bucketKey = compress32(litKeyValue[ridx], classMask);
            keyToLiteralIndexes[bucketKey].push_back(ridx);
        }

        const u32 packedMask = pbeFullKeyMask(klass.classKeyBits);
        for (const auto &it : keyToLiteralIndexes) {
            const u32 key = it.first;
            const auto &bucketIdxes = it.second;
            const u32 entryCount = verify_u32(
                (bucketIdxes.size() + PBE_RULE_SLOTS_PER_ENTRY - 1) /
                PBE_RULE_SLOTS_PER_ENTRY);

            if (secondaryHashTable->size() + entryCount > PBE_MAX_SECONDARY_ENTRIES) {
                if (flags) {
                    *flags |= PBE_ARTIFACT_FLAG_PARTIAL_COVERAGE;
                    *flags |= PBE_ARTIFACT_FLAG_PARTIAL_SECONDARY_CAPACITY;
                }
                stop = true;
                break;
            }

            if (entryCount >= (1U << (32U - PBE_L1_COUNT_SHIFT))) {
                if (flags) {
                    *flags |= PBE_ARTIFACT_FLAG_PARTIAL_COVERAGE;
                    *flags |= PBE_ARTIFACT_FLAG_PARTIAL_ENTRY_OVERFLOW;
                }
                stop = true;
                break;
            }

            const u32 secondaryOffset = verify_u32(secondaryHashTable->size());
            klass.primaryHashTable.offsets[key] =
                encodePrimaryValue(secondaryOffset, entryCount);

            for (u32 chunk = 0; chunk < entryCount; chunk++) {
                PBESecondaryHashEntry entry = {};
                const size_t begin = chunk * PBE_RULE_SLOTS_PER_ENTRY;
                const size_t end = std::min(bucketIdxes.size(),
                                            begin + PBE_RULE_SLOTS_PER_ENTRY);
                entry.ruleCount = verify_u16(end - begin);

                for (size_t slot = begin; slot < end; slot++) {
                    const u32 localSlot = verify_u32(slot - begin);
                    const auto &lit = lits[bucketIdxes[slot]];
                    const u32 len = verify_u32(lit.s.size());
                    const u32 suffixLen =
                        std::min<u32>(len, PBE_BYTES_PER_RULE_SLOT);
                    const u32 laneBase = localSlot * PBE_BYTES_PER_RULE_SLOT;

                    entry.ruleIndex[localSlot] = verify_u16(bucketIdxes[slot]);
                    entry.keyValue[localSlot] = key;
                    entry.keyMask[localSlot] = packedMask;

                    for (u32 j = 0; j < suffixLen; j++) {
                        const u32 vecIndex = laneBase +
                            (PBE_BYTES_PER_RULE_SLOT - suffixLen + j);
                        const u8 c = verify_u8(lit.s[len - suffixLen + j]);
                        entry.ruleVector[vecIndex] = normalizedLiteralByte(c);
                        entry.tableControl[vecIndex] = 1;
                        entry.tailMask |= (1U << vecIndex);
                        if (j + 1 != suffixLen) {
                            entry.headMask |= (1U << vecIndex);
                        }
                    }
                }

                secondaryHashTable->push_back(entry);
                klass.secondaryCount++;
            }
        }

        buildPrimaryBitmap(klass.primaryHashTable, &klass.primaryHashBitmap);
        maskClasses->push_back(std::move(klass));
    }

    if (maskClasses && !maskClasses->empty()) {
        *legacyPrimaryHashTable = (*maskClasses)[0].primaryHashTable;
        *legacyPrimaryHashBitmap = (*maskClasses)[0].primaryHashBitmap;
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

const char *pbeFeasibilityReasonName(PBEFeasibilityReason reason) {
    switch (reason) {
    case PBEFeasibilityReason::OK:
        return "OK";
    case PBEFeasibilityReason::GREY_DISABLED:
        return "GREY_DISABLED";
    case PBEFeasibilityReason::ARCH_UNSUPPORTED:
        return "ARCH_UNSUPPORTED";
    case PBEFeasibilityReason::TOO_FEW_LITERALS:
        return "TOO_FEW_LITERALS";
    case PBEFeasibilityReason::TOO_MANY_LITERALS:
        return "TOO_MANY_LITERALS";
    case PBEFeasibilityReason::UNSUPPORTED_INCLUDED_LITERAL:
        return "UNSUPPORTED_INCLUDED_LITERAL";
    case PBEFeasibilityReason::NO_SELECTORS:
        return "NO_SELECTORS";
    case PBEFeasibilityReason::PARTIAL_SECONDARY_CAPACITY:
        return "PARTIAL_SECONDARY_CAPACITY";
    case PBEFeasibilityReason::PARTIAL_ENTRY_OVERFLOW:
        return "PARTIAL_ENTRY_OVERFLOW";
    case PBEFeasibilityReason::PARTIAL_OTHER:
        return "PARTIAL_OTHER";
    case PBEFeasibilityReason::ARTIFACT_BUILD_FAILED:
        return "ARTIFACT_BUILD_FAILED";
    default:
        return "UNKNOWN";
    }
}

bool pbeCanUseBextFastPath(const target_t &target) {
#if defined(HS_BUILD_HAVE_SVEBITPERM)
    return target.has_sve_bitperm();
#else
    (void)target;
    return false;
#endif
}

bool pbeHasSveBitPermPrereq(const target_t &target) {
#if defined(HS_BUILD_HAVE_SVEBITPERM)
    return target.has_sve_bitperm();
#else
    (void)target;
    return false;
#endif
}

bool buildPBEArtifacts(const std::vector<hwlmLiteral> &lits,
                       PBECompileArtifacts *artifacts,
                       bool enableDump) {
    if (!artifacts) {
        return false;
    }

    artifacts->keyBits = 0;
    artifacts->flags = 0;
    artifacts->extractMode = PBE_EXTRACT_MODE_SCALAR;
    artifacts->windowBytes = PBE_BYTES_PER_RULE_SLOT;
    artifacts->bextMask = 0;
    artifacts->bitSelectors.clear();
    artifacts->maskClasses.clear();
    artifacts->primaryHashTable.offsets.clear();
    artifacts->primaryHashBitmap.bits.clear();
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
    buildExtractDescriptor(artifacts->bitSelectors, artifacts);

    buildHashTables(lits, artifacts->bitSelectors, &artifacts->maskClasses,
                    &artifacts->primaryHashTable, &artifacts->primaryHashBitmap,
                    &artifacts->secondaryHashTable, &artifacts->flags);
    buildRuleMeta(lits, &artifacts->ruleMeta, &artifacts->literalBlob);

    // Compile-time dump for selector/key/hash construction inspection.
    if (enableDump) {
        dumpPBEArtifactsVerbose(lits, *artifacts);
    }

    return true;
}

bool analyzePBEFeasibility(const target_t &target,
                           const std::vector<hwlmLiteral> &lits,
                           const Grey &grey, PBEFeasibilityResult *result,
                           PBECompileArtifacts *artifacts) {
    PBEFeasibilityResult local;
    local.canBuild = false;
    local.reason = PBEFeasibilityReason::ARTIFACT_BUILD_FAILED;
    local.flags = 0;

    if (!grey.allowPbe) {
        local.reason = PBEFeasibilityReason::GREY_DISABLED;
        if (result) {
            *result = local;
        }
        return false;
    }

    // PBE is currently only enabled on Arm64 builds.
#if !defined(__aarch64__)
    (void)target;
    local.reason = PBEFeasibilityReason::ARCH_UNSUPPORTED;
    if (result) {
        *result = local;
    }
    return false;
#else
    (void)target;
#endif

    if (lits.size() < 4) {
        local.reason = PBEFeasibilityReason::TOO_FEW_LITERALS;
        if (result) {
            *result = local;
        }
        return false;
    }

    if (lits.size() > std::numeric_limits<u16>::max()) {
        local.reason = PBEFeasibilityReason::TOO_MANY_LITERALS;
        if (result) {
            *result = local;
        }
        return false;
    }

    PBECompileArtifacts temp;
    PBECompileArtifacts *out = artifacts ? artifacts : &temp;
    if (!buildPBEArtifacts(lits, out, false)) {
        local.reason = PBEFeasibilityReason::ARTIFACT_BUILD_FAILED;
        if (result) {
            *result = local;
        }
        return false;
    }

    local.flags = out->flags;
    if (out->bitSelectors.empty()) {
        local.reason = PBEFeasibilityReason::NO_SELECTORS;
        if (result) {
            *result = local;
        }
        return false;
    }

    if (out->flags & PBE_ARTIFACT_FLAG_PARTIAL_COVERAGE) {
        if (out->flags & PBE_ARTIFACT_FLAG_PARTIAL_SECONDARY_CAPACITY) {
            local.reason = PBEFeasibilityReason::PARTIAL_SECONDARY_CAPACITY;
        } else if (out->flags & PBE_ARTIFACT_FLAG_PARTIAL_ENTRY_OVERFLOW) {
            local.reason = PBEFeasibilityReason::PARTIAL_ENTRY_OVERFLOW;
        } else {
            local.reason = PBEFeasibilityReason::PARTIAL_OTHER;
        }
        if (result) {
            *result = local;
        }
        return false;
    }

    local.canBuild = true;
    local.reason = PBEFeasibilityReason::OK;
    if (result) {
        *result = local;
    }
    return true;
}

bool canBuildPBE(const target_t &target, const std::vector<hwlmLiteral> &lits,
                 const Grey &grey) {
    PBEFeasibilityResult result;
    return analyzePBEFeasibility(target, lits, grey, &result, nullptr);
}

bytecode_ptr<u8> buildPBEBlob(const PBECompileArtifacts &artifacts) {
    const u32 selectorCount = verify_u32(artifacts.bitSelectors.size());
    const u32 classCount = verify_u32(artifacts.maskClasses.size());
    const u32 primaryCount = verify_u32(artifacts.primaryHashTable.offsets.size());
    const u32 primaryBitmapSize = verify_u32(artifacts.primaryHashBitmap.bits.size());
    const u32 secondaryCount = verify_u32(artifacts.secondaryHashTable.size());
    const u32 ruleMetaCount = verify_u32(artifacts.ruleMeta.size());
    const u32 literalBlobSize = verify_u32(artifacts.literalBlob.size());

    const size_t selectorBytes =
        sizeof(PBERuntimeBitSelector) * artifacts.bitSelectors.size();
    const size_t classTableBytes =
        sizeof(PBERuntimeMaskClass) * artifacts.maskClasses.size();
    const size_t secondaryBytes = sizeof(PBERuntimeSecondaryHashEntry) *
                                  artifacts.secondaryHashTable.size();
    const size_t ruleMetaBytes =
        sizeof(PBERuntimeRuleMeta) * artifacts.ruleMeta.size();
    const size_t literalBlobBytes = artifacts.literalBlob.size();

    struct PBEMaskClassLayout {
        u32 classMask;
        u32 classKeyBits;
        u32 primaryCount;
        u32 primaryBitmapSize;
        u32 primaryBitmapOffset;
        u32 primaryOffset;
        u32 secondaryOffset;
        u32 secondaryCount;
    };
    std::vector<PBEMaskClassLayout> classLayouts;
    classLayouts.reserve(artifacts.maskClasses.size());

    size_t totalSize = ROUNDUP_N(sizeof(PBERuntimeHeader), alignof(u32));
    const u32 selectorsOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(selectorBytes, alignof(u32));
    const u32 classTableOffset = verify_u32(totalSize);
    totalSize += ROUNDUP_N(classTableBytes, alignof(u32));
    for (const auto &klass : artifacts.maskClasses) {
        PBEMaskClassLayout layout = {};
        layout.classMask = klass.classMask;
        layout.classKeyBits = klass.classKeyBits;
        layout.primaryCount = verify_u32(klass.primaryHashTable.offsets.size());
        layout.primaryBitmapSize =
            verify_u32(klass.primaryHashBitmap.bits.size());
        layout.primaryBitmapOffset = verify_u32(totalSize);
        totalSize += ROUNDUP_N(klass.primaryHashBitmap.bits.size(), alignof(u32));
        layout.primaryOffset = verify_u32(totalSize);
        totalSize += ROUNDUP_N(sizeof(u32) * klass.primaryHashTable.offsets.size(),
                               alignof(u32));
        layout.secondaryOffset = klass.secondaryOffset;
        layout.secondaryCount = klass.secondaryCount;
        classLayouts.push_back(layout);
    }
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
    hdr->classCount = classCount;
    hdr->primaryCount = primaryCount;
    hdr->primaryBitmapSize = primaryBitmapSize;
    hdr->secondaryCount = secondaryCount;
    hdr->ruleMetaCount = ruleMetaCount;
    hdr->literalBlobSize = literalBlobSize;
    hdr->extractMode = artifacts.extractMode;
    hdr->windowBytes = artifacts.windowBytes;
    hdr->bextMask = artifacts.bextMask;
    hdr->selectorsOffset = selectorsOffset;
    hdr->classTableOffset = classTableOffset;
    hdr->primaryBitmapOffset = classLayouts.empty() ? 0 : classLayouts[0].primaryBitmapOffset;
    hdr->primaryOffset = classLayouts.empty() ? 0 : classLayouts[0].primaryOffset;
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

    auto *classOut =
        reinterpret_cast<PBERuntimeMaskClass *>(base + classTableOffset);
    for (u32 i = 0; i < classCount; i++) {
        classOut[i].classMask = classLayouts[i].classMask;
        classOut[i].classKeyBits = classLayouts[i].classKeyBits;
        classOut[i].primaryCount = classLayouts[i].primaryCount;
        classOut[i].primaryBitmapSize = classLayouts[i].primaryBitmapSize;
        classOut[i].primaryBitmapOffset = classLayouts[i].primaryBitmapOffset;
        classOut[i].primaryOffset = classLayouts[i].primaryOffset;
        classOut[i].secondaryOffset = classLayouts[i].secondaryOffset;
        classOut[i].secondaryCount = classLayouts[i].secondaryCount;
    }

    for (u32 i = 0; i < classCount; i++) {
        const auto &klass = artifacts.maskClasses[i];
        if (!klass.primaryHashBitmap.bits.empty()) {
            memcpy(base + classLayouts[i].primaryBitmapOffset,
                   klass.primaryHashBitmap.bits.data(),
                   klass.primaryHashBitmap.bits.size());
        }

        auto *primaryOut =
            reinterpret_cast<u32 *>(base + classLayouts[i].primaryOffset);
        for (u32 j = 0; j < classLayouts[i].primaryCount; j++) {
            primaryOut[j] = klass.primaryHashTable.offsets[j];
        }
    }

    auto *secondaryOut =
        reinterpret_cast<PBERuntimeSecondaryHashEntry *>(base + secondaryOffset);
    for (u32 i = 0; i < secondaryCount; i++) {
        const auto &in = artifacts.secondaryHashTable[i];
        auto &out = secondaryOut[i];
        memcpy(out.ruleVector, in.ruleVector, sizeof(out.ruleVector));
        memcpy(out.tableControl, in.tableControl, sizeof(out.tableControl));
        memcpy(out.ruleIndex, in.ruleIndex, sizeof(out.ruleIndex));
        memcpy(out.keyValue, in.keyValue, sizeof(out.keyValue));
        memcpy(out.keyMask, in.keyMask, sizeof(out.keyMask));
        out.headMask = in.headMask;
        out.tailMask = in.tailMask;
        out.ruleCount = in.ruleCount;
        out.reserved = in.reserved;
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
