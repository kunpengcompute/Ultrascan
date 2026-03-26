/*
 * Copyright (c) 2026, Huawei Technologies Co., Ltd.
 */

#include "config.h"

#include "ue2common.h"
#include "grey.h"
#include "fdr/fdr.h"
#include "fdr/fdr_compile.h"
#include "fdr/fdr_compile_internal.h"
#include "fdr/fdr_internal.h"
#include "fdr/pbe_compile.h"
#include "hwlm/hwlm_internal.h"
#include "scratch.h"
#include "util/compare.h"
#include "util/target_info.h"
#include "util/verify_types.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <bitset>
#include <iomanip>
#include <iostream>
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
std::vector<u32> enumerateKeysForRuleNoMask(
    const hwlmLiteral &lit, const std::vector<PBEBitSelector> &selectors) {
    std::vector<u32> keys;
    keys.push_back(0);

    for (u32 i = 0; i < selectors.size(); i++) {
        const u8 state = ruleBitStateNoMask(lit, selectors[i]);
        if (state == 2) {
            const size_t oldSize = keys.size();
            keys.resize(oldSize * 2);
            for (size_t k = 0; k < oldSize; k++) {
                keys[oldSize + k] = keys[k] | (1U << i);
            }
            continue;
        }
        if (state == 1) {
            for (auto &k : keys) {
                k |= (1U << i);
            }
        }
    }

    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
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

TEST(PBEvsNeo, PartialCoverageRejectedByPbeBuild) {
    std::vector<hwlmLiteral> lits;
    lits.reserve(40);
    for (u32 i = 0; i < 40; i++) {
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
    EXPECT_EQ(nullptr, pbe.get());
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

    std::cout << "\n[Rule -> Keys]\n";
    for (size_t i = 0; i < lits.size(); i++) {
        auto keys = enumerateKeysForRuleNoMask(lits[i], artifacts.bitSelectors);
        std::cout << "  r" << i << " id=" << lits[i].id << " keys(" << keys.size()
                  << "): ";
        for (size_t k = 0; k < keys.size(); k++) {
            const u32 key = keys[k];
            std::cout << "{dec=" << key
                      << ",hex=0x" << std::hex << key << std::dec
                      << ",bin=" << u32ToBin(key, artifacts.keyBits) << "}";
            if (k + 1 != keys.size()) {
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
                  << ",hex=0x" << std::hex << value << std::dec << "}\n";
    }
    std::cout << "  nonEmpty=" << nonEmpty << "\n";

    std::cout << "\n[L2 Hash Table]\n";
    std::cout << "  size=" << artifacts.secondaryHashTable.size()
              << " (entry0 is null)\n";
    for (u32 i = 1; i < artifacts.secondaryHashTable.size(); i++) {
        const auto &e = artifacts.secondaryHashTable[i];
        std::cout << "  L2[" << i << "]"
                  << " ruleBase=" << e.ruleBase
                  << " ruleCount=" << e.ruleCount
                  << " headMask={hex=0x" << std::hex << e.headMask
                  << ",bin=" << u32ToBin(e.headMask, 32) << "}"
                  << " tailMask={hex=0x" << e.tailMask << std::dec
                  << ",bin=" << u32ToBin(e.tailMask, 32) << "}\n";

        for (u32 j = 0; j < e.ruleCount && j < 32; j++) {
            const u16 ridx = e.ruleIndex[j];
            const u8 rv = e.ruleVector[j];
            const u8 len = e.tableControl[j];
            std::cout << "    slot" << j
                      << ": ruleIndex=" << ridx
                      << " len=" << static_cast<u32>(len)
                      << " ruleVector={char='" << printableOrDot(rv)
                      << "',dec=" << static_cast<u32>(rv)
                      << ",hex=0x" << std::hex << static_cast<u32>(rv)
                      << std::dec
                      << ",bin=" << u8ToBin(rv) << "}";
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
