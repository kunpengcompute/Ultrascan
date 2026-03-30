/*
 * Copyright (c) 2026, Huawei Technologies Co., Ltd.
 */

#include "config.h"

#include "hs_compile.h"
#include "ue2common.h"
#include "grey.h"
#include "fdr/fdr.h"
#include "fdr/fdr_compile.h"
#include "fdr/fdr_compile_internal.h"
#include "fdr/fdr_internal.h"
#include "fdr/fdr_enhanced.h"
#include "fdr/pbe_compile.h"
#include "fdr/pbe_runtime.h"
#include "hwlm/hwlm_internal.h"
#include "scratch.h"
#include "util/arch.h"
#include "util/compare.h"
#include "util/target_info.h"
#include "util/verify_types.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <bitset>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

using namespace ue2;

namespace {

static constexpr u32 ENGINE_ID_NEO = 1;
static constexpr u32 ENGINE_ID_PBE = 2;

struct Match {
    size_t end;
    u32 id;

    bool operator==(const Match &b) const {
        return std::tie(id, end) == std::tie(b.id, b.end);
    }

    bool operator<(const Match &b) const {
        return std::tie(id, end) < std::tie(b.id, b.end);
    }
};

struct PBEInspectStats {
    u32 nonEmptyL1 = 0;
    u32 bitmapBytes = 0;
    u32 exactBucketCount = 0;
    u32 multiEntryBucketCount = 0;
    u32 maxL2EntriesPerKey = 0;
    u32 wildcardL2Entries = 0;
    u32 wildcardRules = 0;
    u32 totalL2Entries = 0;
    u32 totalRulesInL2 = 0;
};

static std::vector<Match> g_matches;

extern "C" {

static
hwlmcb_rv_t collectCallback(size_t end, u32 id,
                            UNUSED struct hs_scratch *scratch) {
    g_matches.push_back({end, id});
    return HWLM_CONTINUE_MATCHING;
}

}

static
bytecode_ptr<FDR> buildFdrWithHint(std::vector<hwlmLiteral> lits, u32 hint) {
    Grey grey;
    grey.fdrAllowTeddy = false;
    grey.allowNeoFdr = true;
    grey.allowPbe = true;

    auto proto = fdrBuildProtoHinted(HWLM_ENGINE_FDR, std::move(lits), false,
                                     hint, get_current_target(), grey);
    if (!proto) {
        return nullptr;
    }

    return fdrBuildTable(*proto, grey);
}

static
bool buildNeoAndPbe(std::vector<hwlmLiteral> lits,
                    bytecode_ptr<FDR> *neoOut, bytecode_ptr<FDR> *pbeOut) {
    if (!neoOut || !pbeOut) {
        return false;
    }

    auto neo = buildFdrWithHint(lits, ENGINE_ID_NEO);
    auto pbe = buildFdrWithHint(std::move(lits), ENGINE_ID_PBE);

    if (!neo || !pbe) {
        return false;
    }

    if (neo->engineID != ENGINE_ID_NEO || pbe->engineID != ENGINE_ID_PBE) {
        return false;
    }

    if (!pbe->pbeOffset || !pbe->pbeSize) {
        return false;
    }

    *neoOut = std::move(neo);
    *pbeOut = std::move(pbe);
    return true;
}

static
std::vector<Match> runBlock(const FDR *fdr, const std::vector<u8> &data,
                            hwlm_group_t groups) {
    g_matches.clear();

    hs_scratch scratch = {};
    scratch.fdr_conf = nullptr;

    hwlm_error_t rv = fdrExec(fdr, data.data(), data.size(), 0, collectCallback,
                              &scratch, groups);
    EXPECT_EQ(HWLM_SUCCESS, rv);

    std::sort(g_matches.begin(), g_matches.end());
    return g_matches;
}

static
std::vector<Match> runBlockInOrder(const FDR *fdr, const std::vector<u8> &data,
                                   hwlm_group_t groups) {
    g_matches.clear();

    hs_scratch scratch = {};
    scratch.fdr_conf = nullptr;

    hwlm_error_t rv = fdrExec(fdr, data.data(), data.size(), 0, collectCallback,
                              &scratch, groups);
    EXPECT_EQ(HWLM_SUCCESS, rv);

    return g_matches;
}

static
std::vector<Match> runStreaming(const FDR *fdr, const std::vector<u8> &history,
                                const std::vector<u8> &data,
                                hwlm_group_t groups) {
    g_matches.clear();

    hs_scratch scratch = {};
    scratch.fdr_conf = nullptr;

    hwlm_error_t rv = fdrExecStreaming(fdr, history.data(), history.size(),
                                       data.data(), data.size(), 0,
                                       collectCallback, &scratch, groups);
    EXPECT_EQ(HWLM_SUCCESS, rv);

    std::sort(g_matches.begin(), g_matches.end());
    return g_matches;
}

static
hwlm_error_t runPbeDirect(const FDR *fdr, const std::vector<u8> &data,
                          hwlm_group_t groups) {
    g_matches.clear();

    hs_scratch scratch = {};
    scratch.fdr_conf = nullptr;

    const FDR_Runtime_Args args = {
        data.data(),
        data.size(),
        nullptr,
        0,
        0,
        collectCallback,
        &scratch,
        nullptr,
        0
    };

    return PbeEngineExec(fdr, &args, groups);
}

static
std::vector<Match> runPbeDirectInOrder(const FDR *fdr,
                                       const std::vector<u8> &data,
                                       hwlm_group_t groups, bool useNaive) {
    g_matches.clear();

    hs_scratch scratch = {};
    scratch.fdr_conf = nullptr;

    const FDR_Runtime_Args args = {
        data.data(),
        data.size(),
        nullptr,
        0,
        0,
        collectCallback,
        &scratch,
        nullptr,
        0
    };

    const hwlm_error_t rv = useNaive ? PbeEngineExecNaiveForTest(fdr, &args,
                                                                 groups)
                                     : PbeEngineExec(fdr, &args, groups);
    EXPECT_EQ(HWLM_SUCCESS, rv);
    return g_matches;
}

static
void skipIfNoPbeSupport() {
    // PBE compile path is currently gated on Arm64 in canBuildPBE().
    SUCCEED() << "Skip PBE vs Neo regression on this target (PBE unavailable).";
}

static
std::string u32ToBin(u32 v, u32 width) {
    const std::string full = std::bitset<32>(v).to_string();
    if (width >= 32) {
        return full;
    }
    return full.substr(32 - width);
}

static
std::string u8ToBin(u8 v) {
    return std::bitset<8>(v).to_string();
}

static
std::string maskToBin(u32 mask) {
    return u32ToBin(mask, PBE_RULE_VECTOR_BYTES);
}

static
char printableOrDot(u8 c) {
    return std::isprint(static_cast<unsigned char>(c)) ? static_cast<char>(c)
                                                        : '.';
}

static
u8 ruleBitStateNoMask(const hwlmLiteral &lit, const PBEBitSelector &sel) {
    // 0 -> bit 0, 1 -> bit 1, 2 -> don't-care(X)
    const u32 bitIndex = static_cast<u32>(sel.byteOffset) * 8U +
                         static_cast<u32>(sel.bitOffset);
    const u32 byteFromEnd = bitIndex / 8;
    const u32 bitInByte = bitIndex % 8;
    const u32 len = static_cast<u32>(lit.s.size());
    if (byteFromEnd >= len) {
        return 2;
    }
    const u8 c = verify_u8(lit.s[len - byteFromEnd - 1]);
    if (lit.nocase && ourisalpha(c) && bitInByte == 5) {
        return 2;
    }
    return (c & (1U << bitInByte)) ? 1 : 0;
}

static
std::vector<hwlmLiteral> makeDuplicateLiterals(const std::string &s, bool nocase,
                                               bool noruns, u32 baseId,
                                               u32 count,
                                               hwlm_group_t groups) {
    std::vector<hwlmLiteral> lits;
    lits.reserve(count);
    for (u32 i = 0; i < count; i++) {
        lits.emplace_back(s, nocase, noruns, baseId + i, groups,
                          std::vector<u8>{}, std::vector<u8>{});
    }
    return lits;
}

static
Grey makePbeGrey(bool allowPbe = true) {
    Grey grey;
    grey.fdrAllowTeddy = false;
    grey.allowNeoFdr = true;
    grey.allowPbe = allowPbe;
    return grey;
}

static
PBEInspectStats computeInspectStats(const PBECompileArtifacts &artifacts) {
    PBEInspectStats stats;
    stats.bitmapBytes = verify_u32(artifacts.primaryHashBitmap.bits.size());
    stats.totalL2Entries = artifacts.secondaryHashTable.empty()
                               ? 0
                               : verify_u32(artifacts.secondaryHashTable.size() - 1);

    for (const auto &entry : artifacts.secondaryHashTable) {
        stats.totalRulesInL2 += entry.ruleCount;
    }

    for (u32 key = 0; key < artifacts.primaryHashTable.offsets.size(); key++) {
        const u32 value = artifacts.primaryHashTable.offsets[key];
        if (!value) {
            continue;
        }

        const u32 entryCount = value >> PBE_L1_COUNT_SHIFT;
        const u32 offset = value & PBE_L1_OFFSET_MASK;
        stats.nonEmptyL1++;
        stats.maxL2EntriesPerKey = std::max(stats.maxL2EntriesPerKey, entryCount);
        if (entryCount > 1) {
            stats.multiEntryBucketCount++;
        }

        if (key == 0) {
            stats.wildcardL2Entries += entryCount;
            for (u32 i = 0; i < entryCount &&
                            offset + i < artifacts.secondaryHashTable.size(); i++) {
                stats.wildcardRules +=
                    artifacts.secondaryHashTable[offset + i].ruleCount;
            }
        } else {
            stats.exactBucketCount++;
        }
    }

    return stats;
}

static
u64a loadWindow64NormalizedForTest(const std::vector<u8> &history,
                                   const std::vector<u8> &data,
                                   size_t historyLen, size_t endPos,
                                   u32 windowBytes) {
    const size_t totalLen = historyLen + data.size();
    if (!windowBytes || windowBytes > PBE_BYTES_PER_RULE_SLOT) {
        windowBytes = PBE_BYTES_PER_RULE_SLOT;
    }

    u64a window = 0;
    for (u32 i = 0; i < windowBytes; i++) {
        const size_t pos = endPos >= i ? endPos - i : totalLen;
        u8 c = 0;
        if (pos < historyLen) {
            c = history[pos];
        } else if (pos < totalLen) {
            c = data[pos - historyLen];
        }
        window |= ((u64a)c) << (i * 8U);
    }
    return window;
}

static
u32 extractScalarKeyFromWindowForTest(const PBECompileArtifacts &artifacts,
                                      u64a window) {
    u32 key = 0;
    for (u32 i = 0; i < artifacts.bitSelectors.size() && i < PBE_MAX_SELECTORS;
         i++) {
        const auto &sel = artifacts.bitSelectors[i];
        const u32 bitIndex = static_cast<u32>(sel.byteOffset) * 8U +
                             static_cast<u32>(sel.bitOffset);
        if (window & ((u64a)1 << bitIndex)) {
            key |= (1U << i);
        }
    }
    return key;
}

static
u64a extractPackedBitsFallbackForTest(u64a window, u64a mask) {
    u64a packed = 0;
    u32 outBit = 0;

    while (mask && outBit < PBE_MAX_SELECTORS) {
        const u64a lowest = mask & (0 - mask);
        if (window & lowest) {
            packed |= ((u64a)1 << outBit);
        }
        mask &= mask - 1;
        outBit++;
    }

    return packed;
}

static
u32 remapPackedBitsForTest(const PBECompileArtifacts &artifacts, u64a packed) {
    u32 key = 0;
    for (u32 i = 0; i < artifacts.bitSelectors.size() && i < PBE_MAX_SELECTORS;
         i++) {
        const u8 keyBit = artifacts.bextToKeyBit[i];
        if (keyBit >= PBE_MAX_SELECTORS) {
            continue;
        }
        if (packed & ((u64a)1 << i)) {
            key |= (1U << keyBit);
        }
    }
    return key;
}

TEST(PBEvsNeo, BlockGroupsConsistency) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 10, 0x1, {}, {}),
        hwlmLiteral("beta", false, false, 11, 0x2, {}, {}),
        hwlmLiteral("gamma", false, false, 12, 0x4, {}, {}),
        hwlmLiteral("delta", true, false, 13, 0x2, {}, {})
    };

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndPbe(lits, &neo, &pbe)) {
        skipIfNoPbeSupport();
        return;
    }

    const std::vector<u8> data = {'a','l','p','h','a',' ',
                                  'B','E','T','A',' ',
                                  'b','e','t','a',' ',
                                  'g','a','m','m','a',' ',
                                  'D','E','L','T','A',' ',
                                  'd','e','l','t','a'};

    const auto neoMatches = runBlock(neo.get(), data, 0x2);
    const auto pbeMatches = runBlock(pbe.get(), data, 0x2);
    EXPECT_EQ(neoMatches, pbeMatches);
}

TEST(PBEvsNeo, BlockNorunsConsistency) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("z", false, true, 20, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("zz", false, false, 21, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ab", false, false, 22, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("cd", false, false, 23, HWLM_ALL_GROUPS, {}, {})
    };

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndPbe(lits, &neo, &pbe)) {
        skipIfNoPbeSupport();
        return;
    }

    const std::vector<u8> data = {'z','z','z','z','z','z','a','b','z','z'};

    const auto neoMatches = runBlock(neo.get(), data, HWLM_ALL_GROUPS);
    const auto pbeMatches = runBlock(pbe.get(), data, HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, pbeMatches);
}

TEST(PBEvsNeo, StreamingMaskConsistency) {
    const std::vector<u8> msk = {0xff, 0xf0};
    const std::vector<u8> cmp = {'c', 0x70};

    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("abcz", false, false, 30, HWLM_ALL_GROUPS, msk, cmp),
        hwlmLiteral("yy", true, false, 31, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("kappa", false, false, 32, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 33, HWLM_ALL_GROUPS, {}, {})
    };

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndPbe(lits, &neo, &pbe)) {
        skipIfNoPbeSupport();
        return;
    }

    const std::vector<u8> history = {'x', 'x', 'a', 'b'};
    const std::vector<u8> data = {'c', 'z', 'Y', 'Y', 'a', 'B', 'c', 'z'};

    const auto neoMatches = runStreaming(neo.get(), history, data,
                                         HWLM_ALL_GROUPS);
    const auto pbeMatches = runStreaming(pbe.get(), history, data,
                                         HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, pbeMatches);
}

TEST(PBEvsNeo, BlockMaskAndNoCaseConsistency) {
    const std::vector<u8> msk = {0xff, 0xf0};
    const std::vector<u8> cmp = {'s', 0x70};

    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("test", false, false, 40, HWLM_ALL_GROUPS, msk, cmp),
        hwlmLiteral("mix", true, false, 41, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("pbe", false, false, 42, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("neo", false, false, 43, HWLM_ALL_GROUPS, {}, {})
    };

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndPbe(lits, &neo, &pbe)) {
        skipIfNoPbeSupport();
        return;
    }

    const std::vector<u8> data = {'t','e','s','t',' ',
                                  'T','E','S','T',' ',
                                  'm','I','x',' ',
                                  'M','i','X',' ',
                                  'p','b','e',' ',
                                  'n','e','o'};

    const auto neoMatches = runBlock(neo.get(), data, HWLM_ALL_GROUPS);
    const auto pbeMatches = runBlock(pbe.get(), data, HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, pbeMatches);
}

TEST(PBEvsNeo, MultiEntryCollisionAcceptedByPbeBuild) {
    std::vector<hwlmLiteral> lits;
    lits.reserve(PBE_RULE_SLOTS_PER_ENTRY + 3);
    for (u32 i = 0; i < PBE_RULE_SLOTS_PER_ENTRY + 3; i++) {
        lits.emplace_back("abcd", false, false, 100 + i, HWLM_ALL_GROUPS,
                          std::vector<u8>{}, std::vector<u8>{});
    }

    auto neo = buildFdrWithHint(lits, ENGINE_ID_NEO);
    if (!neo) {
        skipIfNoPbeSupport();
        return;
    }
    ASSERT_EQ(ENGINE_ID_NEO, neo->engineID);

    auto pbe = buildFdrWithHint(std::move(lits), ENGINE_ID_PBE);
    ASSERT_NE(nullptr, pbe.get());
    EXPECT_EQ(ENGINE_ID_PBE, pbe->engineID);
}

TEST(PBEvsNeo, PrimaryValueEncodesL2CountAndOffset) {
    std::vector<hwlmLiteral> lits;
    lits.reserve(7);
    for (u32 i = 0; i < 7; i++) {
        lits.emplace_back("abcd", false, false, 200 + i, HWLM_ALL_GROUPS,
                          std::vector<u8>{}, std::vector<u8>{});
    }

    PBECompileArtifacts artifacts;
    const bool ok = buildPBEArtifacts(lits, &artifacts, false);
    ASSERT_TRUE(ok);

    u32 foundKey = 0;
    u32 foundValue = 0;
    for (u32 key = 0; key < artifacts.primaryHashTable.offsets.size(); key++) {
        const u32 value = artifacts.primaryHashTable.offsets[key];
        if (!value) {
            continue;
        }
        foundKey = key;
        foundValue = value;
        break;
    }

    ASSERT_NE(0U, foundValue);
    EXPECT_EQ(2U, foundValue >> PBE_L1_COUNT_SHIFT);
    EXPECT_EQ(1U, foundValue & PBE_L1_OFFSET_MASK);
    EXPECT_EQ(3U, artifacts.secondaryHashTable.size());
    (void)foundKey;
}

TEST(PBEvsNeo, BlockMultiEntryExactBucketConsistency) {
    auto lits = makeDuplicateLiterals("abcd", false, false, 300, 7,
                                      HWLM_ALL_GROUPS);

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndPbe(lits, &neo, &pbe)) {
        skipIfNoPbeSupport();
        return;
    }

    const std::vector<u8> data = {'x','x','a','b','c','d','y','y',
                                  'a','b','c','d'};

    const auto neoMatches = runBlock(neo.get(), data, HWLM_ALL_GROUPS);
    const auto pbeMatches = runBlock(pbe.get(), data, HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, pbeMatches);
}

TEST(PBEvsNeo, StreamingMultiEntryExactBucketConsistency) {
    auto lits = makeDuplicateLiterals("abcd", false, false, 320, 7,
                                      HWLM_ALL_GROUPS);

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndPbe(lits, &neo, &pbe)) {
        skipIfNoPbeSupport();
        return;
    }

    const std::vector<u8> history = {'x', 'a', 'b'};
    const std::vector<u8> data = {'c', 'd', 'y', 'a', 'b', 'c', 'd'};

    const auto neoMatches = runStreaming(neo.get(), history, data,
                                         HWLM_ALL_GROUPS);
    const auto pbeMatches = runStreaming(pbe.get(), history, data,
                                         HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, pbeMatches);
}

TEST(PBEvsNeo, BlockMultiEntryWildcardBucketConsistency) {
    auto lits = makeDuplicateLiterals("alpha", true, false, 340, 7,
                                      HWLM_ALL_GROUPS);

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndPbe(lits, &neo, &pbe)) {
        skipIfNoPbeSupport();
        return;
    }

    const std::vector<u8> data = {'a','l','p','h','a',' ',
                                  'A','L','P','H','A',' ',
                                  'a','L','p','H','a'};

    const auto neoMatches = runBlock(neo.get(), data, HWLM_ALL_GROUPS);
    const auto pbeMatches = runBlock(pbe.get(), data, HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, pbeMatches);
}

TEST(PBEvsNeo, WildcardBucketPrimaryValueEncodesTwoEntries) {
    auto lits = makeDuplicateLiterals("ab", true, false, 360, 7,
                                      HWLM_ALL_GROUPS);
    lits.emplace_back("AB", false, false, 500, HWLM_ALL_GROUPS,
                      std::vector<u8>{}, std::vector<u8>{});

    PBECompileArtifacts artifacts;
    const bool ok = buildPBEArtifacts(lits, &artifacts, false);
    ASSERT_TRUE(ok);

    ASSERT_LT(0U, artifacts.primaryHashTable.offsets.size());
    const u32 wildcardValue = artifacts.primaryHashTable.offsets[0];
    ASSERT_NE(0U, wildcardValue);
    EXPECT_EQ(2U, wildcardValue >> PBE_L1_COUNT_SHIFT);
    EXPECT_EQ(1U, wildcardValue & PBE_L1_OFFSET_MASK);
    EXPECT_EQ(4U, artifacts.secondaryHashTable.size());
}

TEST(PBEvsNeo, BlockMultiEntryGroupsConsistency) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("ab", true, false, 380, 0x1, {}, {}),
        hwlmLiteral("cd", true, false, 381, 0x2, {}, {}),
        hwlmLiteral("ef", true, false, 382, 0x1, {}, {}),
        hwlmLiteral("gh", true, false, 383, 0x2, {}, {}),
        hwlmLiteral("ij", true, false, 384, 0x1, {}, {}),
        hwlmLiteral("kl", true, false, 385, 0x2, {}, {}),
        hwlmLiteral("mn", true, false, 386, 0x1, {}, {})
    };

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndPbe(lits, &neo, &pbe)) {
        skipIfNoPbeSupport();
        return;
    }

    const std::vector<u8> data = {'A','B',' ','C','D',' ','E','F',' ',
                                  'G','H',' ','I','J',' ','K','L',' ',
                                  'M','N'};
    const auto neoMatches = runBlock(neo.get(), data, 0x2);
    const auto pbeMatches = runBlock(pbe.get(), data, 0x2);
    EXPECT_EQ(neoMatches, pbeMatches);
}

TEST(PBEvsNeo, BlockMultiEntryNorunsConsistency) {
    auto lits = makeDuplicateLiterals("z", false, false, 400, 7,
                                      HWLM_ALL_GROUPS);
    for (size_t i = 0; i < lits.size(); i += 2) {
        lits[i].noruns = true;
    }

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndPbe(lits, &neo, &pbe)) {
        skipIfNoPbeSupport();
        return;
    }

    const std::vector<u8> data = {'z','z','z','z','z','z'};
    const auto neoMatches = runBlock(neo.get(), data, HWLM_ALL_GROUPS);
    const auto pbeMatches = runBlock(pbe.get(), data, HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, pbeMatches);
}

TEST(PBEvsNeo, StreamingMultiEntryMaskConsistency) {
    const std::vector<u8> msk = {0xff, 0xf0};
    const std::vector<u8> cmp = {'c', 0x70};

    std::vector<hwlmLiteral> lits;
    for (u32 i = 0; i < 7; i++) {
        lits.emplace_back("abcz", false, false, 420 + i, HWLM_ALL_GROUPS, msk,
                          cmp);
    }

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndPbe(lits, &neo, &pbe)) {
        skipIfNoPbeSupport();
        return;
    }

    const std::vector<u8> history = {'x', 'x', 'a', 'b'};
    const std::vector<u8> data = {'c', 'z', 'y', 'a', 'b', 'c', 'z'};
    const auto neoMatches = runStreaming(neo.get(), history, data,
                                         HWLM_ALL_GROUPS);
    const auto pbeMatches = runStreaming(pbe.get(), history, data,
                                         HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, pbeMatches);
}

TEST(PBEvsNeo, BlockMultiEntryExactAndWildcardConsistency) {
    auto wildcardLits = makeDuplicateLiterals("ab", true, false, 440, 7,
                                              HWLM_ALL_GROUPS);
    auto exactLits = makeDuplicateLiterals("wxyz", false, false, 500, 7,
                                           HWLM_ALL_GROUPS);
    wildcardLits.insert(wildcardLits.end(), exactLits.begin(), exactLits.end());

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndPbe(wildcardLits, &neo, &pbe)) {
        skipIfNoPbeSupport();
        return;
    }

    const std::vector<u8> data = {'A','b',' ','w','x','y','z',' ',
                                  'a','B',' ','w','x','y','z'};
    const auto neoMatches = runBlock(neo.get(), data, HWLM_ALL_GROUPS);
    const auto pbeMatches = runBlock(pbe.get(), data, HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, pbeMatches);
}

TEST(PBEvsNeo, BlockCallbackOrderMonotonicAcrossExactAndWildcard) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("ab", true, false, 520, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("wxyz", false, false, 521, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("mnop", false, false, 522, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("qrst", false, false, 523, HWLM_ALL_GROUPS, {}, {})
    };

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndPbe(lits, &neo, &pbe)) {
        skipIfNoPbeSupport();
        return;
    }

    const std::vector<u8> data = {'A','b','x','w','x','y','z'};

    const auto neoMatches = runBlockInOrder(neo.get(), data, HWLM_ALL_GROUPS);
    const auto pbeMatches = runBlockInOrder(pbe.get(), data, HWLM_ALL_GROUPS);

    ASSERT_EQ(2U, neoMatches.size());
    ASSERT_EQ(2U, pbeMatches.size());
    EXPECT_EQ(neoMatches, pbeMatches);
    EXPECT_LE(pbeMatches[0].end, pbeMatches[1].end);
}

TEST(PBECompile, FeasibilityReasonNameMapping) {
    EXPECT_STREQ("OK", pbeFeasibilityReasonName(PBEFeasibilityReason::OK));
    EXPECT_STREQ("GREY_DISABLED",
                 pbeFeasibilityReasonName(PBEFeasibilityReason::GREY_DISABLED));
    EXPECT_STREQ("ARTIFACT_BUILD_FAILED",
                 pbeFeasibilityReasonName(
                     PBEFeasibilityReason::ARTIFACT_BUILD_FAILED));
}

TEST(PBECompile, AnalyzeFeasibilityGreyDisabled) {
    auto grey = makePbeGrey(false);
    PBEFeasibilityResult result;
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 600, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 601, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("gamma", false, false, 602, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 603, HWLM_ALL_GROUPS, {}, {})
    };

    const bool ok = analyzePBEFeasibility(get_current_target(), lits, grey,
                                          &result, nullptr);
    EXPECT_FALSE(ok);
    EXPECT_EQ(PBEFeasibilityReason::GREY_DISABLED, result.reason);
    EXPECT_FALSE(result.canBuild);
}

TEST(PBECompile, CanBuildPbeRejectsTooFewLiterals) {
    auto grey = makePbeGrey(true);
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("a", false, false, 620, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("b", false, false, 621, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("c", false, false, 622, HWLM_ALL_GROUPS, {}, {})
    };

    EXPECT_FALSE(canBuildPBE(get_current_target(), lits, grey));
}

TEST(PBECompile, BuildPbeBlobHeaderMatchesArtifacts) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 640, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 641, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 642, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 643, HWLM_ALL_GROUPS, {}, {})
    };

    PBECompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));

    auto blob = buildPBEBlob(artifacts);
    ASSERT_NE(nullptr, blob.get());

    const auto *hdr = reinterpret_cast<const PBERuntimeHeader *>(blob.get());
    EXPECT_EQ(PBE_RUNTIME_MAGIC, hdr->magic);
    EXPECT_EQ(PBE_RUNTIME_VERSION, hdr->version);
    EXPECT_EQ(artifacts.keyBits, hdr->keyBits);
    EXPECT_EQ(artifacts.bitSelectors.size(), hdr->selectorCount);
    EXPECT_EQ(artifacts.primaryHashTable.offsets.size(), hdr->primaryCount);
    EXPECT_EQ(artifacts.primaryHashBitmap.bits.size(), hdr->primaryBitmapSize);
    EXPECT_EQ(artifacts.secondaryHashTable.size(), hdr->secondaryCount);
    EXPECT_EQ(artifacts.ruleMeta.size(), hdr->ruleMetaCount);
    EXPECT_EQ(artifacts.literalBlob.size(), hdr->literalBlobSize);
    EXPECT_EQ(artifacts.extractMode, hdr->extractMode);
    EXPECT_EQ(artifacts.windowBytes, hdr->windowBytes);
    EXPECT_EQ(artifacts.bextMask, hdr->bextMask);
    EXPECT_TRUE(std::equal(hdr->bextToKeyBit,
                           hdr->bextToKeyBit + PBE_MAX_SELECTORS,
                           artifacts.bextToKeyBit.begin()));
}

TEST(PBECompile, PrimaryBitmapMatchesNonEmptyL1Entries) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 650, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 651, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 652, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 653, HWLM_ALL_GROUPS, {}, {})
    };

    PBECompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));

    const u32 expectedBitmapBytes =
        verify_u32((artifacts.primaryHashTable.offsets.size() + 7U) / 8U);
    EXPECT_EQ(expectedBitmapBytes, artifacts.primaryHashBitmap.bits.size());

    u32 nonEmptyOffsets = 0;
    u32 setBits = 0;
    for (u32 i = 0; i < artifacts.primaryHashTable.offsets.size(); i++) {
        if (artifacts.primaryHashTable.offsets[i]) {
            nonEmptyOffsets++;
        }
    }
    for (u32 i = 0; i < artifacts.primaryHashBitmap.bits.size(); i++) {
        setBits += verify_u32(std::bitset<8>(artifacts.primaryHashBitmap.bits[i]).count());
    }
    EXPECT_EQ(nonEmptyOffsets, setBits);
}

TEST(PBECompile, TargetSveFeatureMapping) {
    hs_platform_info noneInfo = {};
    target_t noneTarget(noneInfo);
    EXPECT_FALSE(noneTarget.has_sve());
    EXPECT_FALSE(noneTarget.has_sve2());

    hs_platform_info sveInfo = {};
    sveInfo.cpu_features = HS_CPU_FEATURES_SVE;
    target_t sveTarget(sveInfo);
    EXPECT_TRUE(sveTarget.has_sve());
    EXPECT_FALSE(sveTarget.has_sve2());

    hs_platform_info sve2Info = {};
    sve2Info.cpu_features = HS_CPU_FEATURES_SVE | HS_CPU_FEATURES_SVE2;
    target_t sve2Target(sve2Info);
    EXPECT_TRUE(sve2Target.has_sve());
    EXPECT_TRUE(sve2Target.has_sve2());
    EXPECT_FALSE(sve2Target.has_sve_bitperm());

    hs_platform_info bitpermInfo = {};
    bitpermInfo.cpu_features = HS_CPU_FEATURES_SVE | HS_CPU_FEATURES_SVE2 |
                               HS_CPU_FEATURES_SVEBITPERM;
    target_t bitpermTarget(bitpermInfo);
    EXPECT_TRUE(bitpermTarget.has_sve());
    EXPECT_TRUE(bitpermTarget.has_sve2());
    EXPECT_TRUE(bitpermTarget.has_sve_bitperm());
}

TEST(PBECompile, TargetSveCompatibilityCheck) {
    hs_platform_info noneInfo = {};
    target_t noneTarget(noneInfo);

    hs_platform_info sveInfo = {};
    sveInfo.cpu_features = HS_CPU_FEATURES_SVE;
    target_t sveTarget(sveInfo);

    hs_platform_info sve2Info = {};
    sve2Info.cpu_features = HS_CPU_FEATURES_SVE | HS_CPU_FEATURES_SVE2;
    target_t sve2Target(sve2Info);

    hs_platform_info bitpermInfo = {};
    bitpermInfo.cpu_features = HS_CPU_FEATURES_SVE | HS_CPU_FEATURES_SVE2 |
                               HS_CPU_FEATURES_SVEBITPERM;
    target_t bitpermTarget(bitpermInfo);

    EXPECT_FALSE(noneTarget.can_run_on_code_built_for(sveTarget));
    EXPECT_FALSE(noneTarget.can_run_on_code_built_for(sve2Target));
    EXPECT_TRUE(sve2Target.can_run_on_code_built_for(sveTarget));
    EXPECT_FALSE(sveTarget.can_run_on_code_built_for(sve2Target));
    EXPECT_TRUE(bitpermTarget.can_run_on_code_built_for(sve2Target));
    EXPECT_FALSE(sve2Target.can_run_on_code_built_for(bitpermTarget));
}

TEST(PBECompile, SveBitPermPrereqRequiresBuildAndTargetSupport) {
    hs_platform_info noneInfo = {};
    target_t noneTarget(noneInfo);
    EXPECT_FALSE(pbeHasSveBitPermPrereq(noneTarget));

    hs_platform_info sveInfo = {};
    sveInfo.cpu_features = HS_CPU_FEATURES_SVE;
    target_t sveTarget(sveInfo);
    EXPECT_FALSE(pbeHasSveBitPermPrereq(sveTarget));

    hs_platform_info sve2Info = {};
    sve2Info.cpu_features = HS_CPU_FEATURES_SVE | HS_CPU_FEATURES_SVE2;
    target_t sve2Target(sve2Info);
    EXPECT_FALSE(pbeHasSveBitPermPrereq(sve2Target));

    hs_platform_info bitpermInfo = {};
    bitpermInfo.cpu_features = HS_CPU_FEATURES_SVE | HS_CPU_FEATURES_SVE2 |
                               HS_CPU_FEATURES_SVEBITPERM;
    target_t bitpermTarget(bitpermInfo);
#if defined(HS_BUILD_HAVE_SVEBITPERM)
    EXPECT_TRUE(pbeHasSveBitPermPrereq(bitpermTarget));
#else
    EXPECT_FALSE(pbeHasSveBitPermPrereq(bitpermTarget));
#endif
}

TEST(PBECompile, BextFastPathRequiresSveBitPerm) {
    hs_platform_info noneInfo = {};
    target_t noneTarget(noneInfo);
    EXPECT_FALSE(pbeCanUseBextFastPath(noneTarget));

    hs_platform_info sve2Info = {};
    sve2Info.cpu_features = HS_CPU_FEATURES_SVE | HS_CPU_FEATURES_SVE2;
    target_t sve2Target(sve2Info);
    EXPECT_FALSE(pbeCanUseBextFastPath(sve2Target));

    hs_platform_info bitpermInfo = {};
    bitpermInfo.cpu_features = HS_CPU_FEATURES_SVE | HS_CPU_FEATURES_SVE2 |
                               HS_CPU_FEATURES_SVEBITPERM;
    target_t bitpermTarget(bitpermInfo);
#if defined(HS_BUILD_HAVE_SVEBITPERM)
    EXPECT_TRUE(pbeCanUseBextFastPath(bitpermTarget));
#else
    EXPECT_FALSE(pbeCanUseBextFastPath(bitpermTarget));
#endif
}

TEST(PBECompile, BextMaskMatchesSelectors) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 690, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 691, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 692, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 693, HWLM_ALL_GROUPS, {}, {})
    };

    PBECompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));
    ASSERT_EQ(PBE_EXTRACT_MODE_BEXT, artifacts.extractMode);

    std::vector<std::pair<u32, u8>> expected;
    for (u32 i = 0; i < artifacts.bitSelectors.size(); i++) {
        const auto &sel = artifacts.bitSelectors[i];
        expected.emplace_back(static_cast<u32>(sel.byteOffset) * 8U +
                                  static_cast<u32>(sel.bitOffset),
                              verify_u8(i));
    }
    std::sort(expected.begin(), expected.end());

    u64a expectedMask = 0;
    for (u32 i = 0; i < expected.size(); i++) {
        expectedMask |= ((u64a)1 << expected[i].first);
        EXPECT_EQ(expected[i].second, artifacts.bextToKeyBit[i]);
    }
    EXPECT_EQ(expectedMask, artifacts.bextMask);
}

TEST(PBEExtract, BextMatchesScalar) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 700, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 701, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 702, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 703, HWLM_ALL_GROUPS, {}, {})
    };

    PBECompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));

    const std::vector<u8> history = {'x', 'y', 'z'};
    const std::vector<u8> data = {'a','l','p','h','a',' ',
                                  'B','E','T','A',' ',
                                  'd','e','l','t','a'};
    const size_t historyLen = history.size();
    const size_t totalLen = historyLen + data.size();

    for (size_t endPos = 0; endPos < totalLen; endPos++) {
        const u64a window = loadWindow64NormalizedForTest(
            history, data, historyLen, endPos, artifacts.windowBytes);
        const u32 scalarKey = extractScalarKeyFromWindowForTest(artifacts,
                                                                window);
        const u64a packed = extractPackedBitsFallbackForTest(window,
                                                             artifacts.bextMask);
        const u32 bextKey = remapPackedBitsForTest(artifacts, packed);
        EXPECT_EQ(scalarKey, bextKey) << "endPos=" << endPos;
    }
}

TEST(PBEExtract, BextHistoryBoundaryConsistency) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("abcz", false, false, 710, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("YY", true, false, 711, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("kappa", false, false, 712, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 713, HWLM_ALL_GROUPS, {}, {})
    };

    PBECompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));

    const std::vector<u8> history = {'x', 'x', 'a', 'b'};
    const std::vector<u8> data = {'c', 'z', 'Y', 'Y', 'a', 'B', 'c', 'z'};
    const size_t historyLen = history.size();

    for (size_t endPos = 0; endPos < historyLen + data.size(); endPos++) {
        const u64a window = loadWindow64NormalizedForTest(
            history, data, historyLen, endPos, artifacts.windowBytes);
        const u32 scalarKey = extractScalarKeyFromWindowForTest(artifacts,
                                                                window);
        const u64a packed = extractPackedBitsFallbackForTest(window,
                                                             artifacts.bextMask);
        const u32 bextKey = remapPackedBitsForTest(artifacts, packed);
        EXPECT_EQ(scalarKey, bextKey) << "boundary endPos=" << endPos;
    }
}

TEST(PBERuntime, Batch4MatchesNaiveDirect) {
    auto pbe = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 730, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 731, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 732, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", true, false, 733, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_PBE);

    if (!pbe || pbe->engineID != ENGINE_ID_PBE || !pbe->pbeOffset) {
        skipIfNoPbeSupport();
        return;
    }

    const std::vector<u8> data = {'a','l','p','h','a',' ',
                                  'B','E','T','A',' ',
                                  'T','H','E','T','A',' ',
                                  'A','L','P','H','A'};

    const auto naiveMatches = runPbeDirectInOrder(pbe.get(), data,
                                                  HWLM_ALL_GROUPS, true);
    const auto batchMatches = runPbeDirectInOrder(pbe.get(), data,
                                                  HWLM_ALL_GROUPS, false);
    EXPECT_EQ(naiveMatches, batchMatches);
}

TEST(PBERuntime, Batch4SparseBitmapSkipsEmptyLanes) {
    auto pbe = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 740, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 741, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 742, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 743, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_PBE);

    if (!pbe || pbe->engineID != ENGINE_ID_PBE || !pbe->pbeOffset) {
        skipIfNoPbeSupport();
        return;
    }

    const std::vector<u8> data = {'x','x','x','x','x','x','x','x',
                                  'a','l','p','h','a','x','x','x',
                                  'd','e','l','t','a','x','x','x',
                                  'o','m','e','g','a','x','x','x'};

    const auto naiveMatches = runPbeDirectInOrder(pbe.get(), data,
                                                  HWLM_ALL_GROUPS, true);
    const auto batchMatches = runPbeDirectInOrder(pbe.get(), data,
                                                  HWLM_ALL_GROUPS, false);
    EXPECT_EQ(naiveMatches, batchMatches);
}

TEST(PBERuntime, Batch4OrderStableWithWildcard) {
    auto pbe = buildFdrWithHint({
        hwlmLiteral("ab", true, false, 750, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("wxyz", false, false, 751, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("mnop", false, false, 752, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("qrst", false, false, 753, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_PBE);

    if (!pbe || pbe->engineID != ENGINE_ID_PBE || !pbe->pbeOffset) {
        skipIfNoPbeSupport();
        return;
    }

    const std::vector<u8> data = {'A','b','x','w','x','y','z'};
    const auto naiveMatches = runPbeDirectInOrder(pbe.get(), data,
                                                  HWLM_ALL_GROUPS, true);
    const auto batchMatches = runPbeDirectInOrder(pbe.get(), data,
                                                  HWLM_ALL_GROUPS, false);

    ASSERT_EQ(naiveMatches, batchMatches);
    for (size_t i = 1; i < batchMatches.size(); i++) {
        EXPECT_LE(batchMatches[i - 1].end, batchMatches[i].end);
    }
}

TEST(PBERuntime, InvalidMagicFallsBackCleanly) {
    auto pbe = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 660, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 661, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 662, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 663, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_PBE);

    if (!pbe || pbe->engineID != ENGINE_ID_PBE || !pbe->pbeOffset) {
        skipIfNoPbeSupport();
        return;
    }

    auto *hdr = reinterpret_cast<PBERuntimeHeader *>(
        reinterpret_cast<u8 *>(pbe.get()) + pbe->pbeOffset);
    const u32 savedMagic = hdr->magic;
    hdr->magic = 0;

    const hwlm_error_t rv = runPbeDirect(
        pbe.get(), {'a','l','p','h','a'}, HWLM_ALL_GROUPS);
    EXPECT_EQ(HWLM_SUCCESS, rv);
    EXPECT_TRUE(g_matches.empty());

    hdr->magic = savedMagic;
}

TEST(PBERuntime, InvalidVersionFallsBackCleanly) {
    auto pbe = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 670, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 671, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 672, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 673, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_PBE);

    if (!pbe || pbe->engineID != ENGINE_ID_PBE || !pbe->pbeOffset) {
        skipIfNoPbeSupport();
        return;
    }

    auto *hdr = reinterpret_cast<PBERuntimeHeader *>(
        reinterpret_cast<u8 *>(pbe.get()) + pbe->pbeOffset);
    const u32 savedVersion = hdr->version;
    hdr->version = savedVersion + 1;

    const hwlm_error_t rv = runPbeDirect(
        pbe.get(), {'a','l','p','h','a'}, HWLM_ALL_GROUPS);
    EXPECT_EQ(HWLM_SUCCESS, rv);
    EXPECT_TRUE(g_matches.empty());

    hdr->version = savedVersion;
}

TEST(PBERuntime, InvalidLayoutOffsetFallsBackCleanly) {
    auto pbe = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 680, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 681, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 682, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 683, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_PBE);

    if (!pbe || pbe->engineID != ENGINE_ID_PBE || !pbe->pbeOffset) {
        skipIfNoPbeSupport();
        return;
    }

    auto *hdr = reinterpret_cast<PBERuntimeHeader *>(
        reinterpret_cast<u8 *>(pbe.get()) + pbe->pbeOffset);
    const u32 savedSecondaryOffset = hdr->secondaryOffset;
    hdr->secondaryOffset = pbe->pbeSize;

    const hwlm_error_t rv = runPbeDirect(
        pbe.get(), {'a','l','p','h','a'}, HWLM_ALL_GROUPS);
    EXPECT_EQ(HWLM_SUCCESS, rv);
    EXPECT_TRUE(g_matches.empty());

    hdr->secondaryOffset = savedSecondaryOffset;
}

TEST(PBEInspect, DumpSelectorsAndHashTables) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 100, 0x1, {}, {}),
        hwlmLiteral("ALPHA", true,  false, 101, 0x3, {}, {}),
        hwlmLiteral("beta",  false, false, 102, 0x1, {}, {}),
        hwlmLiteral("delta", false, true,  103, 0x2, {}, {}),
        hwlmLiteral("gamma", false, false, 104, 0x4, {}, {}),
        hwlmLiteral("theta", true,  false, 105, 0x7, {}, {}),
    };

    PBECompileArtifacts artifacts;
    const bool ok = buildPBEArtifacts(lits, &artifacts);
    ASSERT_TRUE(ok);
    const PBEInspectStats stats = computeInspectStats(artifacts);

    std::cout << "\n========== PBE Inspect Begin ==========\n";
    std::cout << "[Rules]\n";
    for (size_t i = 0; i < lits.size(); i++) {
        const auto &lit = lits[i];
        std::cout << "  r" << i
                  << " id=" << lit.id
                  << " s=\"" << lit.s << "\""
                  << " nocase=" << lit.nocase
                  << " noruns=" << lit.noruns
                  << " groups=0x" << std::hex << lit.groups << std::dec
                  << "\n";
    }

    std::cout << "\n[Selectors]\n";
    std::cout << "  keyBits=" << artifacts.keyBits
              << " selectorCount=" << artifacts.bitSelectors.size() << "\n";
    for (size_t i = 0; i < artifacts.bitSelectors.size(); i++) {
        const auto &sel = artifacts.bitSelectors[i];
        std::cout << "  s" << i
                  << " byteOffset=" << static_cast<u32>(sel.byteOffset)
                  << " bitOffset=" << static_cast<u32>(sel.bitOffset)
                  << " suffixBitIndex="
                  << static_cast<u32>(sel.byteOffset) * 8U +
                         static_cast<u32>(sel.bitOffset)
                  << " states=[";
        for (size_t r = 0; r < lits.size(); r++) {
            const u8 st = ruleBitStateNoMask(lits[r], sel);
            const char c = (st == 2) ? 'X' : (st ? '1' : '0');
            std::cout << "r" << r << ":" << c;
            if (r + 1 != lits.size()) {
                std::cout << ", ";
            }
        }
        std::cout << "]\n";
    }

    std::cout << "\n[Extract]\n";
    std::cout << "  mode="
              << (artifacts.extractMode == PBE_EXTRACT_MODE_BEXT ? "bext"
                                                                  : "scalar")
              << " windowBytes=" << artifacts.windowBytes
              << " bextMask={dec=" << artifacts.bextMask
              << ",hex=0x" << std::hex << artifacts.bextMask << std::dec
              << ",bin=" << std::bitset<64>(artifacts.bextMask) << "}\n";
    std::cout << "  bextToKeyBit=[";
    for (size_t i = 0; i < artifacts.bitSelectors.size() && i < PBE_MAX_SELECTORS;
         i++) {
        std::cout << static_cast<u32>(artifacts.bextToKeyBit[i])
                  << (i + 1 == artifacts.bitSelectors.size() ? "" : ", ");
    }
    std::cout << "]\n";

    std::cout << "\n[Build Stats]\n";
    std::cout << "  nonEmptyL1=" << stats.nonEmptyL1
              << " bitmapBytes=" << stats.bitmapBytes
              << " exactBucketCount=" << stats.exactBucketCount
              << " multiEntryBucketCount=" << stats.multiEntryBucketCount
              << " maxL2EntriesPerKey=" << stats.maxL2EntriesPerKey << "\n";
    std::cout << "  wildcardL2Entries=" << stats.wildcardL2Entries
              << " wildcardRules=" << stats.wildcardRules
              << " totalL2Entries=" << stats.totalL2Entries
              << " totalRulesInL2=" << stats.totalRulesInL2 << "\n";

    std::map<u32, std::vector<u32>> offToL1Keys;
    for (u32 key = 0; key < artifacts.primaryHashTable.offsets.size(); key++) {
        const u32 value = artifacts.primaryHashTable.offsets[key];
        if (value) {
            const u32 off = value & PBE_L1_OFFSET_MASK;
            const u32 count = value >> PBE_L1_COUNT_SHIFT;
            for (u32 n = 0; n < count; n++) {
                offToL1Keys[off + n].push_back(key);
            }
        }
    }

    struct ActualKeyInfo {
        bool found = false;
        u32 l2Off = 0;
        u32 keyValue = 0;
        u32 keyMask = 0;
        std::vector<u32> bucketKeys;
    };
    std::vector<ActualKeyInfo> actual(lits.size());
    for (u32 i = 1; i < artifacts.secondaryHashTable.size(); i++) {
        const auto &e = artifacts.secondaryHashTable[i];
        for (u32 j = 0; j < e.ruleCount && j < PBE_RULE_SLOTS_PER_ENTRY; j++) {
            const u16 ridx = e.ruleIndex[j];
            if (ridx >= actual.size()) {
                continue;
            }
            actual[ridx].found = true;
            actual[ridx].l2Off = i;
            actual[ridx].keyValue = e.keyValue[j];
            actual[ridx].keyMask = e.keyMask[j];
            actual[ridx].bucketKeys = offToL1Keys[i];
        }
    }

    std::cout << "\n[Rule -> KeyMask/Bucket (Actual Build)]\n";
    for (size_t i = 0; i < lits.size(); i++) {
        std::cout << "  r" << i << " id=" << lits[i].id;
        if (!actual[i].found) {
            std::cout << " <not_in_L2>\n";
            continue;
        }
        std::cout << " l2Off=" << actual[i].l2Off
                  << " keyValue={dec=" << actual[i].keyValue
                  << ",hex=0x" << std::hex << actual[i].keyValue << std::dec
                  << ",bin=" << u32ToBin(actual[i].keyValue, artifacts.keyBits)
                  << "}"
                  << " keyMask={dec=" << actual[i].keyMask
                  << ",hex=0x" << std::hex << actual[i].keyMask << std::dec
                  << ",bin=" << u32ToBin(actual[i].keyMask, artifacts.keyBits)
                  << "}"
                  << " bucketKeys(" << actual[i].bucketKeys.size() << ")=";
        for (size_t k = 0; k < actual[i].bucketKeys.size(); k++) {
            const u32 bk = actual[i].bucketKeys[k];
            std::cout << "{dec=" << bk << ",hex=0x" << std::hex << bk
                      << std::dec
                      << ",bin=" << u32ToBin(bk, artifacts.keyBits) << "}";
            if (k + 1 != actual[i].bucketKeys.size()) {
                std::cout << ", ";
            }
        }
        std::cout << "\n";
    }

    std::cout << "\n[L1 Hash Table]\n";
    std::cout << "  size=" << artifacts.primaryHashTable.offsets.size() << "\n";
    size_t nonEmpty = 0;
    for (u32 key = 0; key < artifacts.primaryHashTable.offsets.size(); key++) {
        const u32 value = artifacts.primaryHashTable.offsets[key];
        if (!value) {
            continue;
        }
        nonEmpty++;
        std::cout << "  key={dec=" << key
                  << ",hex=0x" << std::hex << key << std::dec
                  << ",bin=" << u32ToBin(key, artifacts.keyBits) << "}"
                  << " -> value={dec=" << value
                  << ",hex=0x" << std::hex << value << std::dec
                  << ",offset=" << (value & PBE_L1_OFFSET_MASK)
                  << ",count=" << (value >> PBE_L1_COUNT_SHIFT) << "}\n";
    }
    std::cout << "  nonEmpty=" << nonEmpty << "\n";

    std::cout << "\n[L2 Hash Table]\n";
    std::cout << "  size=" << artifacts.secondaryHashTable.size()
              << " (entry0 is null)\n";
    for (u32 i = 1; i < artifacts.secondaryHashTable.size(); i++) {
        const auto &e = artifacts.secondaryHashTable[i];
        std::cout << "  L2[" << i << "]"
                  << " ruleCount=" << e.ruleCount
                  << " entryCapacity=" << PBE_RULE_SLOTS_PER_ENTRY
                  << " headMask=0x" << std::hex << e.headMask
                  << " tailMask=0x" << e.tailMask << std::dec
                  << " headMaskBits=" << maskToBin(e.headMask)
                  << " tailMaskBits=" << maskToBin(e.tailMask)
                  << "\n";

        for (u32 j = 0; j < e.ruleCount && j < PBE_RULE_SLOTS_PER_ENTRY; j++) {
            const u16 ridx = e.ruleIndex[j];
            std::cout << "    slot" << j
                      << ": ruleIndex=" << ridx
                      << " keyValue=0x" << std::hex << e.keyValue[j]
                      << " keyMask=0x" << e.keyMask[j] << std::dec
                      << " suffix=[";
            for (u32 k = 0; k < PBE_BYTES_PER_RULE_SLOT; k++) {
                const u8 rv = e.ruleVector[j * PBE_BYTES_PER_RULE_SLOT + k];
                std::cout << "{char='" << printableOrDot(rv)
                          << "',dec=" << static_cast<u32>(rv)
                          << ",hex=0x" << std::hex << static_cast<u32>(rv)
                          << std::dec
                          << ",bin=" << u8ToBin(rv) << "}";
                if (k + 1 != PBE_BYTES_PER_RULE_SLOT) {
                    std::cout << ", ";
                }
            }
            std::cout << "] tbl=[";
            for (u32 k = 0; k < PBE_BYTES_PER_RULE_SLOT; k++) {
                std::cout << static_cast<u32>(
                                 e.tableControl[j * PBE_BYTES_PER_RULE_SLOT + k]);
                if (k + 1 != PBE_BYTES_PER_RULE_SLOT) {
                    std::cout << ", ";
                }
            }
            std::cout << "]";
            if (ridx < lits.size()) {
                std::cout << " rule={id=" << lits[ridx].id
                          << ",s=\"" << lits[ridx].s << "\"}";
            }
            std::cout << "\n";
        }
    }

    std::cout << "========== PBE Inspect End ==========\n\n";
}

} // namespace
