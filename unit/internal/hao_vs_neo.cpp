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
#include "fdr/hao_compile.h"
#include "fdr/hao_runtime.h"
#include "hwlm/hwlm_internal.h"
#include "scratch.h"
#include "util/arch.h"
#include "util/bitutils.h"
#include "util/compare.h"
#include "util/target_info.h"
#include "util/verify_types.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <bitset>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

using namespace ue2;

namespace {

static constexpr u32 ENGINE_ID_NEO = 1;
static constexpr u32 ENGINE_ID_HAO = ue2::HAO_ENGINE_ID;

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

struct HAOCompatInspectStats {
    u32 nonEmptyL1 = 0;
    u32 bitmapBytes = 0;
    u32 exactBucketCount = 0;
    u32 multiEntryBucketCount = 0;
    u32 maxL2EntriesPerKey = 0;
    u32 maskClassCount = 0;
    u32 partialClassCount = 0;
    u32 totalL2Entries = 0;
    u32 totalRulesInL2 = 0;
};

struct HAOInspectStats {
    u32 nonEmptyL1 = 0;
    u32 bitmapBytes = 0;
    u32 multiEntryBucketCount = 0;
    u32 maxL2EntriesPerKey = 0;
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
    /* Keep legacy PBE/Neo regression helpers pinned to HAO v1. */
    grey.allowHaoV2 = false;

    auto proto = fdrBuildProtoHinted(HWLM_ENGINE_FDR, std::move(lits), false,
                                     hint, get_current_target(), grey);
    if (!proto) {
        return nullptr;
    }

    return fdrBuildTable(*proto, grey);
}

static
bytecode_ptr<FDR> buildFdrWithHintAndGrey(std::vector<hwlmLiteral> lits, u32 hint,
                                         const Grey &grey) {
    auto proto = fdrBuildProtoHinted(HWLM_ENGINE_FDR, std::move(lits), false,
                                     hint, get_current_target(), grey);
    if (!proto) {
        return nullptr;
    }

    return fdrBuildTable(*proto, grey);
}

static
bytecode_ptr<FDR> buildFdrAutoWithGrey(std::vector<hwlmLiteral> lits,
                                       const Grey &grey) {
    auto proto = fdrBuildProto(HWLM_ENGINE_FDR, std::move(lits), false,
                               get_current_target(), grey);
    if (!proto) {
        return nullptr;
    }

    return fdrBuildTable(*proto, grey);
}

static
bool buildNeoAndHao(std::vector<hwlmLiteral> lits,
                    bytecode_ptr<FDR> *neoOut, bytecode_ptr<FDR> *pbeOut) {
    if (!neoOut || !pbeOut) {
        return false;
    }

    auto neo = buildFdrWithHint(lits, ENGINE_ID_NEO);
    auto pbe = buildFdrWithHint(std::move(lits), ENGINE_ID_HAO);

    if (!neo || !pbe) {
        return false;
    }

    if (neo->engineID != ENGINE_ID_NEO || pbe->engineID != ENGINE_ID_HAO) {
        return false;
    }

    if (!fdrMatcherBlobOffset(pbe.get()) || !fdrMatcherBlobSize(pbe.get())) {
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
hwlm_error_t runHaoFamilyDirect(const FDR *fdr, const std::vector<u8> &data,
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

    return HaoEngineExec(fdr, &args, groups);
}

static
const HAOCompatRuntimeHeader *getHaoCompatRuntimeHeader(const FDR *fdr) {
    if (!fdr || !fdrMatcherBlobOffset(fdr)) {
        return nullptr;
    }
    return reinterpret_cast<const HAOCompatRuntimeHeader *>(
        reinterpret_cast<const u8 *>(fdr) + fdrMatcherBlobOffset(fdr));
}

static
size_t getHaoCompatBlobSize(const HAOCompatRuntimeHeader *hdr) {
    if (!hdr) {
        return 0;
    }

    size_t end = ROUNDUP_N(sizeof(HAOCompatRuntimeHeader), alignof(u32));
    end = std::max(end, static_cast<size_t>(hdr->selectorsOffset) +
                        hdr->selectorCount *
                            sizeof(PBERuntimeBitSelector));
    end = std::max(end, static_cast<size_t>(hdr->classTableOffset) +
                        hdr->classCount * sizeof(PBERuntimeMaskClass));

    const auto *classTable = reinterpret_cast<const PBERuntimeMaskClass *>(
        reinterpret_cast<const u8 *>(hdr) + hdr->classTableOffset);
    for (u32 i = 0; i < hdr->classCount; i++) {
        end = std::max(end, static_cast<size_t>(classTable[i].primaryBitmapOffset) +
                            classTable[i].primaryBitmapSize);
        end = std::max(end, static_cast<size_t>(classTable[i].primaryOffset) +
                            classTable[i].primaryCount * sizeof(u32));
    }

    end = std::max(end, static_cast<size_t>(hdr->secondaryOffset) +
                        hdr->secondaryCount *
                            sizeof(HAOCompatRuntimeSecondaryHashEntry));
    end = std::max(end, static_cast<size_t>(hdr->ruleMetaOffset) +
                        hdr->ruleMetaCount * sizeof(PBERuntimeRuleMeta));
    end = std::max(end, static_cast<size_t>(hdr->literalBlobOffset) +
                        hdr->literalBlobSize);
    return ROUNDUP_N(end, alignof(u32));
}

static
void expectHaoCompatBlobsEqual(const bytecode_ptr<u8> &lhs,
                               const bytecode_ptr<u8> &rhs) {
    ASSERT_NE(nullptr, lhs.get());
    ASSERT_NE(nullptr, rhs.get());

    const auto *lhsHdr =
        reinterpret_cast<const HAOCompatRuntimeHeader *>(lhs.get());
    const auto *rhsHdr =
        reinterpret_cast<const HAOCompatRuntimeHeader *>(rhs.get());
    const size_t lhsSize = getHaoCompatBlobSize(lhsHdr);
    const size_t rhsSize = getHaoCompatBlobSize(rhsHdr);

    ASSERT_EQ(lhs.size(), lhsSize);
    ASSERT_EQ(rhs.size(), rhsSize);
    ASSERT_EQ(lhsSize, rhsSize);
    EXPECT_EQ(0, memcmp(lhs.get(), rhs.get(), lhsSize));
}

/* Test-only read-only entry for inspecting HAO v2 blobs before runtime switch-over. */
static
const HAORuntimeHeader *getHaoRuntimeHeader(const bytecode_ptr<u8> &blob) {
    if (!blob.get()) {
        return nullptr;
    }
    return reinterpret_cast<const HAORuntimeHeader *>(blob.get());
}

static
const HAORuntimeBitSelector *getHaoSelectors(const HAORuntimeHeader *hdr) {
    if (!hdr) {
        return nullptr;
    }
    return reinterpret_cast<const HAORuntimeBitSelector *>(
        reinterpret_cast<const u8 *>(hdr) + hdr->selectorsOffset);
}

static
const u8 *getHaoPrimaryBitmap(const HAORuntimeHeader *hdr) {
    if (!hdr) {
        return nullptr;
    }
    return reinterpret_cast<const u8 *>(
        reinterpret_cast<const u8 *>(hdr) + hdr->primaryBitmapOffset);
}

static
const u32 *getHaoPrimaryTable(const HAORuntimeHeader *hdr) {
    if (!hdr) {
        return nullptr;
    }
    return reinterpret_cast<const u32 *>(
        reinterpret_cast<const u8 *>(hdr) + hdr->primaryOffset);
}

static
const HAORuntimeSecondaryHashEntry *getHaoSecondaryTable(
    const HAORuntimeHeader *hdr) {
    if (!hdr) {
        return nullptr;
    }
    return reinterpret_cast<const HAORuntimeSecondaryHashEntry *>(
        reinterpret_cast<const u8 *>(hdr) + hdr->secondaryOffset);
}

static
const HAORuntimeRuleMeta *getHaoRuleMetaTable(const HAORuntimeHeader *hdr) {
    if (!hdr) {
        return nullptr;
    }
    return reinterpret_cast<const HAORuntimeRuleMeta *>(
        reinterpret_cast<const u8 *>(hdr) + hdr->ruleMetaOffset);
}

static
FDR_Runtime_Args makeRuntimeArgs(const std::vector<u8> &data,
                                 const std::vector<u8> &history,
                                 hs_scratch *scratch) {
    const FDR_Runtime_Args args = {
        data.data(),
        data.size(),
        history.empty() ? nullptr : history.data(),
        history.size(),
        0,
        collectCallback,
        scratch,
        nullptr,
        0
    };
    return args;
}

static
std::vector<Match> runHaoFamilyDirectInOrder(const FDR *fdr,
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

    const hwlm_error_t rv = useNaive ? HaoCompatEngineExecNaiveForTest(fdr, &args,
                                                                 groups)
                                     : HaoEngineExec(fdr, &args, groups);
    EXPECT_EQ(HWLM_SUCCESS, rv);
    return g_matches;
}

/* Blob-level direct execution helper for HAO v2 before it is wired into generic FDR execution. */
static
std::vector<Match> runHaoBlobDirectInOrder(const bytecode_ptr<u8> &blob,
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

    const hwlm_error_t rv = useNaive
                                ? HaoEngineExecBlobNaiveForTest(
                                      blob.get(), verify_u32(blob.size()),
                                      &args, groups)
                                : HaoEngineExecBlobBatchForTest(
                                      blob.get(), verify_u32(blob.size()),
                                      &args, groups);
    EXPECT_EQ(HWLM_SUCCESS, rv);
    return g_matches;
}

static
std::vector<Match> runHaoBlobDirectStreamingInOrder(
    const bytecode_ptr<u8> &blob, const std::vector<u8> &history,
    const std::vector<u8> &data, hwlm_group_t groups, bool useNaive) {
    g_matches.clear();

    hs_scratch scratch = {};
    scratch.fdr_conf = nullptr;
    const auto args = makeRuntimeArgs(data, history, &scratch);

    const hwlm_error_t rv = useNaive
                                ? HaoEngineExecBlobNaiveForTest(
                                      blob.get(), verify_u32(blob.size()),
                                      &args, groups)
                                : HaoEngineExecBlobBatchForTest(
                                      blob.get(), verify_u32(blob.size()),
                                      &args, groups);
    EXPECT_EQ(HWLM_SUCCESS, rv);
    return g_matches;
}

static
void skipIfNoHaoFamilySupport() {
    // HAO family compatibility path is currently gated on Arm64 in canBuildHAOCompat().
    SUCCEED() << "Skip HAO family regression on this target (compat layout unavailable).";
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
const char *haoCategoryName(HAORuleCategory category) {
    switch (category) {
    case HAORuleCategory::HAO_RULE_EXACT:
        return "exact";
    case HAORuleCategory::HAO_RULE_NOCASE:
        return "nocase";
    case HAORuleCategory::HAO_RULE_SMALL_CLASS_EXPAND:
        return "small-expand";
    case HAORuleCategory::HAO_RULE_ANCHOR_CONFIRM:
        return "anchor-confirm";
    case HAORuleCategory::HAO_RULE_UNSUPPORTED:
        return "unsupported";
    default:
        return "unknown";
    }
}

static
std::string slotVectorString(const PBESecondaryHashEntry &entry, u32 slot,
                             bool activeOnly) {
    const u32 laneBase = slot * PBE_BYTES_PER_RULE_SLOT;
    std::string out;

    out.reserve(PBE_BYTES_PER_RULE_SLOT);
    for (u32 i = 0; i < PBE_BYTES_PER_RULE_SLOT; i++) {
        const u8 ctrl = entry.tableControl[laneBase + i];
        const bool active = ctrl != 0x80;
        if (activeOnly && !active) {
            continue;
        }
        out.push_back(active ? printableOrDot(entry.ruleVector[laneBase + i])
                             : '.');
    }
    return out;
}

static
std::string slotVectorString(const HAOSecondaryHashEntry &entry, u32 slot,
                             bool activeOnly) {
    const u32 laneBase = slot * PBE_BYTES_PER_RULE_SLOT;
    std::string out;

    out.reserve(PBE_BYTES_PER_RULE_SLOT);
    for (u32 i = 0; i < PBE_BYTES_PER_RULE_SLOT; i++) {
        const u8 ctrl = entry.tableControl[laneBase + i];
        const bool active = ctrl != 0x80;
        if (activeOnly && !active) {
            continue;
        }
        out.push_back(active ? printableOrDot(entry.ruleVector[laneBase + i])
                             : '.');
    }
    return out;
}

static
u32 haoSlotCount(u8 slotMask) {
    return popcount32(slotMask & ((1U << PBE_RULE_SLOTS_PER_ENTRY) - 1U));
}

static
void dumpBitmapBytes(std::ostream &os, const std::vector<u8> &bytes,
                     const std::string &indent) {
    bool sawNonZero = false;

    os << indent << "bitmapBytes=" << bytes.size() << "\n";
    for (size_t i = 0; i < bytes.size(); i++) {
        if (!bytes[i]) {
            continue;
        }
        sawNonZero = true;
        os << indent << "  [" << i << "] dec=" << static_cast<u32>(bytes[i])
           << " hex=0x" << std::hex << static_cast<u32>(bytes[i]) << std::dec
           << " bits=" << u8ToBin(bytes[i]) << "\n";
    }
    if (!sawNonZero) {
        os << indent << "  <all_zero>\n";
    }
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
Grey makeHaoGrey(bool allowPbe = true) {
    Grey grey;
    grey.fdrAllowTeddy = false;
    grey.allowNeoFdr = true;
    grey.allowPbe = allowPbe;
    /* Keep legacy PBE feasibility/build helpers pinned to HAO v1. */
    grey.allowHaoV2 = false;
    return grey;
}

static
HAOCompatInspectStats computeHaoCompatInspectStats(const HAOCompatCompileArtifacts &artifacts) {
    HAOCompatInspectStats stats;
    stats.maskClassCount = verify_u32(artifacts.maskClasses.size());
    stats.totalL2Entries = artifacts.secondaryHashTable.empty()
                               ? 0
                               : verify_u32(artifacts.secondaryHashTable.size() - 1);

    for (const auto &entry : artifacts.secondaryHashTable) {
        stats.totalRulesInL2 += entry.ruleCount;
    }

    const u32 fullMask = artifacts.keyBits >= 32U
                             ? 0xffffffffU
                             : ((1U << artifacts.keyBits) - 1U);
    for (const auto &klass : artifacts.maskClasses) {
        stats.bitmapBytes += verify_u32(klass.primaryHashBitmap.bits.size());
        if (klass.classMask != fullMask) {
            stats.partialClassCount++;
        }
        for (u32 key = 0; key < klass.primaryHashTable.offsets.size(); key++) {
            const u32 value = klass.primaryHashTable.offsets[key];
            if (!value) {
                continue;
            }

            const u32 entryCount = value >> PBE_L1_COUNT_SHIFT;
            stats.nonEmptyL1++;
            stats.maxL2EntriesPerKey =
                std::max(stats.maxL2EntriesPerKey, entryCount);
            if (entryCount > 1) {
                stats.multiEntryBucketCount++;
            }
            if (klass.classMask == fullMask) {
                stats.exactBucketCount++;
            }
        }
    }

    return stats;
}

static
HAOInspectStats computeHaoInspectStats(const bytecode_ptr<u8> &blob) {
    HAOInspectStats stats;
    const auto *hdr = getHaoRuntimeHeader(blob);
    if (!hdr) {
        return stats;
    }

    const auto *bitmap = getHaoPrimaryBitmap(hdr);
    const auto *primary = getHaoPrimaryTable(hdr);
    const auto *secondary = getHaoSecondaryTable(hdr);

    stats.bitmapBytes = hdr->primaryBitmapSize;
    stats.totalL2Entries = hdr->secondaryCount ? hdr->secondaryCount - 1 : 0;

    for (u32 i = 0; i < hdr->secondaryCount; i++) {
        stats.totalRulesInL2 += haoSlotCount(secondary[i].slotMask);
    }
    /* Read-only inspection of the HAO global single-table layout for runtime validation tests. */
    for (u32 i = 0; i < hdr->primaryCount; i++) {
        if (!primary[i]) {
            continue;
        }
        const u32 entryCount = primary[i] >> PBE_L1_COUNT_SHIFT;
        stats.nonEmptyL1++;
        stats.maxL2EntriesPerKey = std::max(stats.maxL2EntriesPerKey, entryCount);
        if (entryCount > 1) {
            stats.multiEntryBucketCount++;
        }
    }

    u32 setBits = 0;
    for (u32 i = 0; i < hdr->primaryBitmapSize; i++) {
        setBits += verify_u32(std::bitset<8>(bitmap[i]).count());
    }
    EXPECT_EQ(setBits, stats.nonEmptyL1);

    return stats;
}

static
u32 fullKeyMaskForArtifacts(const HAOCompatCompileArtifacts &artifacts) {
    return artifacts.keyBits >= 32U ? 0xffffffffU
                                    : ((1U << artifacts.keyBits) - 1U);
}

static
const HAOCompatMaskClassArtifacts *findMaskClass(const HAOCompatCompileArtifacts &artifacts,
                                           u32 classMask) {
    for (const auto &klass : artifacts.maskClasses) {
        if (klass.classMask == classMask) {
            return &klass;
        }
    }
    return nullptr;
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
u32 extractScalarKeyFromWindowForTest(const HAOCompatCompileArtifacts &artifacts,
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
u32 packedBitsToKeyForTest(const HAOCompatCompileArtifacts &artifacts, u64a packed) {
    if (artifacts.bitSelectors.empty()) {
        return 0;
    }
    if (artifacts.bitSelectors.size() >= 32) {
        return (u32)packed;
    }
    return (u32)(packed & ((1ULL << artifacts.bitSelectors.size()) - 1ULL));
}

TEST(HAOCompatVsNeo, BlockGroupsConsistency) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 10, 0x1, {}, {}),
        hwlmLiteral("beta", false, false, 11, 0x2, {}, {}),
        hwlmLiteral("gamma", false, false, 12, 0x4, {}, {}),
        hwlmLiteral("delta", true, false, 13, 0x2, {}, {})
    };

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndHao(lits, &neo, &pbe)) {
        skipIfNoHaoFamilySupport();
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

TEST(HAOCompatVsNeo, BlockNorunsConsistency) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("z", false, true, 20, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("zz", false, false, 21, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ab", false, false, 22, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("cd", false, false, 23, HWLM_ALL_GROUPS, {}, {})
    };

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndHao(lits, &neo, &pbe)) {
        skipIfNoHaoFamilySupport();
        return;
    }

    const std::vector<u8> data = {'z','z','z','z','z','z','a','b','z','z'};

    const auto neoMatches = runBlock(neo.get(), data, HWLM_ALL_GROUPS);
    const auto pbeMatches = runBlock(pbe.get(), data, HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, pbeMatches);
}

TEST(HAOCompatVsNeo, StreamingMaskConsistency) {
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
    if (!buildNeoAndHao(lits, &neo, &pbe)) {
        skipIfNoHaoFamilySupport();
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

TEST(HAOCompatVsNeo, BlockMaskAndNoCaseConsistency) {
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
    if (!buildNeoAndHao(lits, &neo, &pbe)) {
        skipIfNoHaoFamilySupport();
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

TEST(HAOCompatVsNeo, MultiEntryCollisionAcceptedByPbeBuild) {
    std::vector<hwlmLiteral> lits;
    lits.reserve(PBE_RULE_SLOTS_PER_ENTRY + 3);
    for (u32 i = 0; i < PBE_RULE_SLOTS_PER_ENTRY + 3; i++) {
        lits.emplace_back("abcd", false, false, 100 + i, HWLM_ALL_GROUPS,
                          std::vector<u8>{}, std::vector<u8>{});
    }

    auto neo = buildFdrWithHint(lits, ENGINE_ID_NEO);
    if (!neo) {
        skipIfNoHaoFamilySupport();
        return;
    }
    ASSERT_EQ(ENGINE_ID_NEO, neo->engineID);

    auto pbe = buildFdrWithHint(std::move(lits), ENGINE_ID_HAO);
    ASSERT_NE(nullptr, pbe.get());
    EXPECT_EQ(ENGINE_ID_HAO, pbe->engineID);
}

TEST(HAOCompatVsNeo, PrimaryValueEncodesL2CountAndOffset) {
    std::vector<hwlmLiteral> lits;
    lits.reserve(7);
    for (u32 i = 0; i < 7; i++) {
        lits.emplace_back("abcd", false, false, 200 + i, HWLM_ALL_GROUPS,
                          std::vector<u8>{}, std::vector<u8>{});
    }

    HAOCompatCompileArtifacts artifacts;
    const bool ok = buildPBEArtifacts(lits, &artifacts, false);
    ASSERT_TRUE(ok);

    const auto *fullClass = findMaskClass(artifacts, fullKeyMaskForArtifacts(artifacts));
    ASSERT_NE(nullptr, fullClass);

    u32 foundKey = 0;
    u32 foundValue = 0;
    for (u32 key = 0; key < fullClass->primaryHashTable.offsets.size(); key++) {
        const u32 value = fullClass->primaryHashTable.offsets[key];
        if (!value) {
            continue;
        }
        foundKey = key;
        foundValue = value;
        break;
    }

    ASSERT_NE(0U, foundValue);
    EXPECT_EQ(2U, foundValue >> PBE_L1_COUNT_SHIFT);
    EXPECT_EQ(fullClass->secondaryOffset, foundValue & PBE_L1_OFFSET_MASK);
    EXPECT_EQ(3U, artifacts.secondaryHashTable.size());
    (void)foundKey;
}

TEST(HAOCompatVsNeo, BlockMultiEntryExactBucketConsistency) {
    auto lits = makeDuplicateLiterals("abcd", false, false, 300, 7,
                                      HWLM_ALL_GROUPS);

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndHao(lits, &neo, &pbe)) {
        skipIfNoHaoFamilySupport();
        return;
    }

    const std::vector<u8> data = {'x','x','a','b','c','d','y','y',
                                  'a','b','c','d'};

    const auto neoMatches = runBlock(neo.get(), data, HWLM_ALL_GROUPS);
    const auto pbeMatches = runBlock(pbe.get(), data, HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, pbeMatches);
}

TEST(HAOCompatVsNeo, StreamingMultiEntryExactBucketConsistency) {
    auto lits = makeDuplicateLiterals("abcd", false, false, 320, 7,
                                      HWLM_ALL_GROUPS);

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndHao(lits, &neo, &pbe)) {
        skipIfNoHaoFamilySupport();
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

TEST(HAOCompatVsNeo, BlockMultiEntryWildcardBucketConsistency) {
    auto lits = makeDuplicateLiterals("alpha", true, false, 340, 7,
                                      HWLM_ALL_GROUPS);

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndHao(lits, &neo, &pbe)) {
        skipIfNoHaoFamilySupport();
        return;
    }

    const std::vector<u8> data = {'a','l','p','h','a',' ',
                                  'A','L','P','H','A',' ',
                                  'a','L','p','H','a'};

    const auto neoMatches = runBlock(neo.get(), data, HWLM_ALL_GROUPS);
    const auto pbeMatches = runBlock(pbe.get(), data, HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, pbeMatches);
}

TEST(HAOCompatVsNeo, PartialMaskClassPrimaryValueEncodesTwoEntries) {
    auto lits = makeDuplicateLiterals("ab", true, false, 360, 7,
                                      HWLM_ALL_GROUPS);
    lits.emplace_back("AB", false, false, 500, HWLM_ALL_GROUPS,
                      std::vector<u8>{}, std::vector<u8>{});

    HAOCompatCompileArtifacts artifacts;
    const bool ok = buildPBEArtifacts(lits, &artifacts, false);
    ASSERT_TRUE(ok);

    const u32 fullMask = fullKeyMaskForArtifacts(artifacts);
    const HAOCompatMaskClassArtifacts *partialClass = nullptr;
    for (const auto &klass : artifacts.maskClasses) {
        if (klass.classMask != fullMask) {
            partialClass = &klass;
            break;
        }
    }

    ASSERT_NE(nullptr, partialClass);
    u32 partialValue = 0;
    for (u32 key = 0; key < partialClass->primaryHashTable.offsets.size(); key++) {
        if (partialClass->primaryHashTable.offsets[key]) {
            partialValue = partialClass->primaryHashTable.offsets[key];
            break;
        }
    }

    ASSERT_NE(0U, partialValue);
    EXPECT_EQ(2U, partialValue >> PBE_L1_COUNT_SHIFT);
    EXPECT_EQ(partialClass->secondaryOffset,
              partialValue & PBE_L1_OFFSET_MASK);
    EXPECT_EQ(4U, artifacts.secondaryHashTable.size());
}

TEST(HAOCompatVsNeo, BlockMultiEntryGroupsConsistency) {
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
    if (!buildNeoAndHao(lits, &neo, &pbe)) {
        skipIfNoHaoFamilySupport();
        return;
    }

    const std::vector<u8> data = {'A','B',' ','C','D',' ','E','F',' ',
                                  'G','H',' ','I','J',' ','K','L',' ',
                                  'M','N'};
    const auto neoMatches = runBlock(neo.get(), data, 0x2);
    const auto pbeMatches = runBlock(pbe.get(), data, 0x2);
    EXPECT_EQ(neoMatches, pbeMatches);
}

TEST(HAOCompatVsNeo, BlockMultiEntryNorunsConsistency) {
    auto lits = makeDuplicateLiterals("z", false, false, 400, 7,
                                      HWLM_ALL_GROUPS);
    for (size_t i = 0; i < lits.size(); i += 2) {
        lits[i].noruns = true;
    }

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndHao(lits, &neo, &pbe)) {
        skipIfNoHaoFamilySupport();
        return;
    }

    const std::vector<u8> data = {'z','z','z','z','z','z'};
    const auto neoMatches = runBlock(neo.get(), data, HWLM_ALL_GROUPS);
    const auto pbeMatches = runBlock(pbe.get(), data, HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, pbeMatches);
}

TEST(HAOCompatVsNeo, StreamingMultiEntryMaskConsistency) {
    const std::vector<u8> msk = {0xff, 0xf0};
    const std::vector<u8> cmp = {'c', 0x70};

    std::vector<hwlmLiteral> lits;
    for (u32 i = 0; i < 7; i++) {
        lits.emplace_back("abcz", false, false, 420 + i, HWLM_ALL_GROUPS, msk,
                          cmp);
    }

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndHao(lits, &neo, &pbe)) {
        skipIfNoHaoFamilySupport();
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

TEST(HAOCompatVsNeo, BlockMultiEntryExactAndWildcardConsistency) {
    auto wildcardLits = makeDuplicateLiterals("ab", true, false, 440, 7,
                                              HWLM_ALL_GROUPS);
    auto exactLits = makeDuplicateLiterals("wxyz", false, false, 500, 7,
                                           HWLM_ALL_GROUPS);
    wildcardLits.insert(wildcardLits.end(), exactLits.begin(), exactLits.end());

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndHao(wildcardLits, &neo, &pbe)) {
        skipIfNoHaoFamilySupport();
        return;
    }

    const std::vector<u8> data = {'A','b',' ','w','x','y','z',' ',
                                  'a','B',' ','w','x','y','z'};
    const auto neoMatches = runBlock(neo.get(), data, HWLM_ALL_GROUPS);
    const auto pbeMatches = runBlock(pbe.get(), data, HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, pbeMatches);
}

TEST(HAOCompatVsNeo, BlockCallbackOrderMonotonicAcrossExactAndWildcard) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("ab", true, false, 520, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("wxyz", false, false, 521, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("mnop", false, false, 522, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("qrst", false, false, 523, HWLM_ALL_GROUPS, {}, {})
    };

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> pbe;
    if (!buildNeoAndHao(lits, &neo, &pbe)) {
        skipIfNoHaoFamilySupport();
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

TEST(HAOCompile, FeasibilityReasonNameMapping) {
    EXPECT_STREQ("OK", pbeFeasibilityReasonName(PBEFeasibilityReason::OK));
    EXPECT_STREQ("GREY_DISABLED",
                 pbeFeasibilityReasonName(PBEFeasibilityReason::GREY_DISABLED));
    EXPECT_STREQ("ARTIFACT_BUILD_FAILED",
                 pbeFeasibilityReasonName(
                     PBEFeasibilityReason::ARTIFACT_BUILD_FAILED));
}

TEST(HAOCompile, HaoCompatFeasibilityReasonNameMatchesPbeWrapper) {
    EXPECT_STREQ(haoCompatFeasibilityReasonName(PBEFeasibilityReason::OK),
                 pbeFeasibilityReasonName(PBEFeasibilityReason::OK));
    EXPECT_STREQ(
        haoCompatFeasibilityReasonName(PBEFeasibilityReason::GREY_DISABLED),
        pbeFeasibilityReasonName(PBEFeasibilityReason::GREY_DISABLED));
    EXPECT_STREQ(
        haoCompatFeasibilityReasonName(
            PBEFeasibilityReason::ARTIFACT_BUILD_FAILED),
        pbeFeasibilityReasonName(
            PBEFeasibilityReason::ARTIFACT_BUILD_FAILED));
}

TEST(HAOCompile, HaoFeasibilityReasonNameMapping) {
    EXPECT_STREQ("OK", haoFeasibilityReasonName(HAOFeasibilityReason::OK));
    EXPECT_STREQ("GREY_DISABLED",
                 haoFeasibilityReasonName(HAOFeasibilityReason::GREY_DISABLED));
    EXPECT_STREQ("ARTIFACT_BUILD_FAILED",
                 haoFeasibilityReasonName(
                     HAOFeasibilityReason::ARTIFACT_BUILD_FAILED));
}

TEST(HAOCompile, AnalyzeFeasibilityGreyDisabled) {
    auto grey = makeHaoGrey(false);
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

TEST(HAOCompile, AnalyzeHaoCompatFeasibilityWrapsHaoCompatFeasibility) {
    auto grey = makeHaoGrey(true);
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 6034, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 6035, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 6036, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 6037, HWLM_ALL_GROUPS, {}, {})
    };

    PBEFeasibilityResult pbeResult = {};
    PBEFeasibilityResult compatResult = {};
    const bool pbeOk = analyzePBEFeasibility(get_current_target(), lits, grey,
                                             &pbeResult, nullptr);
    const bool compatOk = analyzeHAOCompatFeasibility(get_current_target(), lits,
                                                      grey, &compatResult,
                                                      nullptr);

    EXPECT_EQ(pbeOk, compatOk);
    EXPECT_EQ(pbeResult.canBuild, compatResult.canBuild);
    EXPECT_EQ(pbeResult.flags, compatResult.flags);
    EXPECT_EQ(pbeResult.reason, compatResult.reason);
}

TEST(HAOCompile, AnalyzeHaoFeasibilityGreyDisabled) {
    auto grey = makeHaoGrey(false);
    HAOFeasibilityResult result;
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 6030, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 6031, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("gamma", false, false, 6032, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 6033, HWLM_ALL_GROUPS, {}, {})
    };

    const bool ok = analyzeHAOFeasibility(get_current_target(), lits, grey,
                                          &result, nullptr);
    EXPECT_FALSE(ok);
    EXPECT_EQ(HAOFeasibilityReason::GREY_DISABLED, result.reason);
    EXPECT_FALSE(result.canBuild);
}

TEST(HAOCompile, CanBuildHaoCompatRejectsTooFewLiterals) {
    auto grey = makeHaoGrey(true);
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("a", false, false, 620, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("b", false, false, 621, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("c", false, false, 622, HWLM_ALL_GROUPS, {}, {})
    };

    EXPECT_FALSE(canBuildPBE(get_current_target(), lits, grey));
}

TEST(HAOCompile, CanBuildHaoCompatMatchesHaoCompatWrapper) {
    auto grey = makeHaoGrey(true);
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 6220, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 6221, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 6222, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 6223, HWLM_ALL_GROUPS, {}, {})
    };

    EXPECT_EQ(canBuildPBE(get_current_target(), lits, grey),
              canBuildHAOCompat(get_current_target(), lits, grey));
}

TEST(HAOCompile, CanBuildHaoRejectsTooFewLiterals) {
    auto grey = makeHaoGrey(true);
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("a", false, false, 6230, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("b", false, false, 6231, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("c", false, false, 6232, HWLM_ALL_GROUPS, {}, {})
    };

    EXPECT_FALSE(canBuildHAO(get_current_target(), lits, grey));
}

TEST(HAOCompile, HAOProtoBuildPrefersHaoV2Artifacts) {
    Grey grey;
    grey.fdrAllowTeddy = false;
    grey.allowNeoFdr = true;
    grey.allowPbe = true;
    grey.allowHaoV2 = true;

    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 6233, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 6234, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 6235, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 6236, HWLM_ALL_GROUPS, {}, {})
    };

    auto proto = fdrBuildProtoHinted(HWLM_ENGINE_FDR, std::move(lits), false,
                                     ENGINE_ID_HAO, get_current_target(), grey);
    if (!proto || !proto->fdrEng) {
        skipIfNoHaoFamilySupport();
        return;
    }

    EXPECT_TRUE(proto->useHaoV2Layout);
    EXPECT_NE(nullptr, proto->haoArtifacts.get());
    EXPECT_EQ(nullptr, proto->haoCompatArtifacts.get());
}

TEST(HAOCompile, HAOProtoBuildFallsBackToLegacyCompatWhenV2Disabled) {
    Grey grey;
    grey.fdrAllowTeddy = false;
    grey.allowNeoFdr = true;
    grey.allowPbe = true;
    grey.allowHaoV2 = false;

    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 6237, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 6238, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 6239, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 6240, HWLM_ALL_GROUPS, {}, {})
    };

    auto proto = fdrBuildProtoHinted(HWLM_ENGINE_FDR, std::move(lits), false,
                                     ENGINE_ID_HAO, get_current_target(), grey);
    if (!proto || !proto->fdrEng) {
        skipIfNoHaoFamilySupport();
        return;
    }

    EXPECT_FALSE(proto->useHaoV2Layout);
    EXPECT_EQ(nullptr, proto->haoArtifacts.get());
    EXPECT_NE(nullptr, proto->haoCompatArtifacts.get());
}

TEST(HAOCompile, BuildHaoCompatBlobHeaderMatchesArtifacts) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 640, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 641, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 642, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 643, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));

    auto blob = buildPBEBlob(artifacts);
    ASSERT_NE(nullptr, blob.get());

    const auto *hdr = reinterpret_cast<const HAOCompatRuntimeHeader *>(blob.get());
    EXPECT_EQ(PBE_RUNTIME_MAGIC, hdr->magic);
    EXPECT_EQ(PBE_RUNTIME_VERSION, hdr->version);
    EXPECT_EQ(artifacts.keyBits, hdr->keyBits);
    EXPECT_EQ(artifacts.bitSelectors.size(), hdr->selectorCount);
    EXPECT_EQ(artifacts.maskClasses.size(), hdr->classCount);
    EXPECT_EQ(artifacts.primaryHashTable.offsets.size(), hdr->primaryCount);
    EXPECT_EQ(artifacts.primaryHashBitmap.bits.size(), hdr->primaryBitmapSize);
    EXPECT_EQ(artifacts.secondaryHashTable.size(), hdr->secondaryCount);
    EXPECT_EQ(artifacts.ruleMeta.size(), hdr->ruleMetaCount);
    EXPECT_EQ(artifacts.literalBlob.size(), hdr->literalBlobSize);
    EXPECT_EQ(artifacts.extractMode, hdr->extractMode);
    EXPECT_EQ(artifacts.windowBytes, hdr->windowBytes);
    EXPECT_EQ(artifacts.bextMask, hdr->bextMask);
}

TEST(HAOCompile, BuildHaoCompatCompatWrappersMatchHaoCompatBuilders) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 6440, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 6441, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 6442, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 6443, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts wrapperArtifacts;
    HAOCompatCompileArtifacts compatArtifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &wrapperArtifacts, false));
    ASSERT_TRUE(buildHAOCompatArtifacts(lits, &compatArtifacts, false));

    EXPECT_EQ(wrapperArtifacts.keyBits, compatArtifacts.keyBits);
    EXPECT_EQ(wrapperArtifacts.flags, compatArtifacts.flags);
    EXPECT_EQ(wrapperArtifacts.extractMode, compatArtifacts.extractMode);
    EXPECT_EQ(wrapperArtifacts.windowBytes, compatArtifacts.windowBytes);
    EXPECT_EQ(wrapperArtifacts.bextMask, compatArtifacts.bextMask);
    EXPECT_EQ(wrapperArtifacts.haoBlobLayoutMode,
              compatArtifacts.haoBlobLayoutMode);
    EXPECT_EQ(wrapperArtifacts.primaryHashTable.offsets,
              compatArtifacts.primaryHashTable.offsets);
    EXPECT_EQ(wrapperArtifacts.primaryHashBitmap.bits,
              compatArtifacts.primaryHashBitmap.bits);
    EXPECT_EQ(wrapperArtifacts.literalBlob, compatArtifacts.literalBlob);

    auto wrapperBlob = buildPBEBlob(wrapperArtifacts);
    auto compatBlob = buildHAOCompatBlob(compatArtifacts);
    expectHaoCompatBlobsEqual(wrapperBlob, compatBlob);
}

TEST(HAOCompile, BuildHaoCompatBlobDefaultsToV1CompatLayout) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 660, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 661, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 662, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 663, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));
    EXPECT_EQ(HAOBlobLayoutMode::HAO_BLOB_LAYOUT_V1_COMPAT,
              artifacts.haoBlobLayoutMode);

    auto blob = buildPBEBlob(artifacts);
    ASSERT_NE(nullptr, blob.get());

    const auto *hdr = reinterpret_cast<const HAOCompatRuntimeHeader *>(blob.get());
    EXPECT_EQ(PBE_RUNTIME_MAGIC, hdr->magic);
    EXPECT_EQ(PBE_RUNTIME_VERSION, hdr->version);
}

TEST(HAOCompile, BuildHaoCompatBlobCanDispatchToHaoGlobalLayout) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 664, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("maskrule", false, false, 665, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'l', 0x60}),
        hwlmLiteral("ALPHA", true, false, 666, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 667, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));
    ASSERT_TRUE(artifacts.haoGlobalHash.valid);
    // Switch explicitly to the HAO v2 layout while still reusing buildHAOCompatBlob() through the compatibility wrapper.
    artifacts.haoBlobLayoutMode = HAOBlobLayoutMode::HAO_BLOB_LAYOUT_V2_GLOBAL;
    auto blob = buildPBEBlob(artifacts);
    ASSERT_NE(nullptr, blob.get());

    const auto *hdr = getHaoRuntimeHeader(blob);
    ASSERT_NE(nullptr, hdr);
    EXPECT_EQ(HAO_RUNTIME_MAGIC, hdr->magic);
    EXPECT_EQ(HAO_RUNTIME_VERSION, hdr->version);
    EXPECT_EQ(artifacts.haoGlobalHash.keyBits, hdr->keyBits);
    EXPECT_EQ(artifacts.haoGlobalHash.primaryHashTable.offsets.size(),
              hdr->primaryCount);
}

TEST(HAOCompile, BuildHaoGlobalBlobHeaderMatchesArtifacts) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 644, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 645, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("maskrule", false, false, 646, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'l', 0x60}),
        hwlmLiteral("delta", false, false, 647, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
    ASSERT_TRUE(artifacts.haoGlobalHash.valid);

    auto blob = buildHAOBlob(artifacts);
    ASSERT_NE(nullptr, blob.get());

    const auto *hdr = reinterpret_cast<const HAORuntimeHeader *>(blob.get());
    EXPECT_EQ(HAO_RUNTIME_MAGIC, hdr->magic);
    EXPECT_EQ(HAO_RUNTIME_VERSION, hdr->version);
    EXPECT_EQ(artifacts.haoGlobalHash.flags, hdr->flags);
    EXPECT_EQ(artifacts.haoGlobalHash.keyBits, hdr->keyBits);
    EXPECT_EQ(artifacts.bitSelectors.size(), hdr->selectorCount);
    EXPECT_EQ(artifacts.haoGlobalHash.primaryHashTable.offsets.size(),
              hdr->primaryCount);
    EXPECT_EQ(artifacts.haoGlobalHash.primaryHashBitmap.bits.size(),
              hdr->primaryBitmapSize);
    EXPECT_EQ(artifacts.haoGlobalHash.secondaryHashTable.size(),
              hdr->secondaryCount);
    EXPECT_EQ(artifacts.ruleMeta.size(), hdr->ruleMetaCount);
    EXPECT_EQ(artifacts.literalBlob.size(), hdr->literalBlobSize);
    EXPECT_EQ(artifacts.extractMode, hdr->extractMode);
    EXPECT_EQ(artifacts.windowBytes, hdr->windowBytes);
    EXPECT_EQ(artifacts.bextMask, hdr->bextMask);
}

TEST(HAOCompile, BuildHaoGlobalBlobStoresRulePlanMeta) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 648, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("maskrule", false, false, 649, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'l', 0x60}),
        hwlmLiteral("ALPHA", true, false, 650, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 651, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
    ASSERT_TRUE(artifacts.haoGlobalHash.valid);

    auto blob = buildHAOBlob(artifacts);
    ASSERT_NE(nullptr, blob.get());

    const auto *hdr = reinterpret_cast<const HAORuntimeHeader *>(blob.get());
    const auto *meta = reinterpret_cast<const HAORuntimeRuleMeta *>(
        reinterpret_cast<const u8 *>(hdr) + hdr->ruleMetaOffset);

    const auto &plan = artifacts.haoRulePlans[1];
    const auto &srcMeta = artifacts.ruleMeta[1];

    EXPECT_EQ(static_cast<u8>(plan.category), meta[1].category);
    EXPECT_EQ(plan.flags, meta[1].planFlags);
    EXPECT_EQ(plan.verifier.validByteMask, meta[1].verifierValidByteMask);
    EXPECT_EQ(plan.verifier.anchorOffset, meta[1].anchorOffset);
    EXPECT_EQ(plan.verifier.anchorLength, meta[1].anchorLength);
    EXPECT_EQ(plan.verifier.flags, meta[1].verifierFlags);
    EXPECT_EQ(srcMeta.litOffset, meta[1].litOffset);
    for (u32 i = 0; i < 8; i++) {
        EXPECT_EQ(srcMeta.lit[i], meta[1].lit[i]);
        EXPECT_EQ(srcMeta.msk[i], meta[1].msk[i]);
        EXPECT_EQ(srcMeta.cmp[i], meta[1].cmp[i]);
    }
}

TEST(HAOCompile, BuildHaoGlobalBlobSelectorsAndPrimaryTableMatchArtifacts) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 652, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 653, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("maskrule", false, false, 654, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'l', 0x60}),
        hwlmLiteral("theta", false, false, 655, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
    ASSERT_TRUE(artifacts.haoGlobalHash.valid);

    auto blob = buildHAOBlob(artifacts);
    ASSERT_NE(nullptr, blob.get());

    const auto *hdr = getHaoRuntimeHeader(blob);
    ASSERT_NE(nullptr, hdr);
    const auto *selectors = getHaoSelectors(hdr);
    const auto *primary = getHaoPrimaryTable(hdr);
    const auto *bitmap = getHaoPrimaryBitmap(hdr);

    for (u32 i = 0; i < hdr->selectorCount; i++) {
        EXPECT_EQ(artifacts.bitSelectors[i].byteOffset, selectors[i].byteOffset);
        EXPECT_EQ(artifacts.bitSelectors[i].bitOffset, selectors[i].bitOffset);
    }
    for (u32 i = 0; i < hdr->primaryCount; i++) {
        EXPECT_EQ(artifacts.haoGlobalHash.primaryHashTable.offsets[i], primary[i]);
    }
    for (u32 i = 0; i < hdr->primaryBitmapSize; i++) {
        EXPECT_EQ(artifacts.haoGlobalHash.primaryHashBitmap.bits[i], bitmap[i]);
    }

    const HAOInspectStats stats = computeHaoInspectStats(blob);
    EXPECT_EQ(artifacts.haoGlobalHash.stats.nonEmptyPrimary, stats.nonEmptyL1);
    EXPECT_EQ(artifacts.haoGlobalHash.stats.totalSecondaryEntries,
              stats.totalL2Entries);
}

TEST(HAOCompile, BuildHaoGlobalBlobSecondaryEntriesMatchArtifacts) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 656, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("maskrule", false, false, 657, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'l', 0x60}),
        hwlmLiteral("ALPHA", true, false, 658, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 659, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
    ASSERT_TRUE(artifacts.haoGlobalHash.valid);

    auto blob = buildHAOBlob(artifacts);
    ASSERT_NE(nullptr, blob.get());

    const auto *hdr = getHaoRuntimeHeader(blob);
    ASSERT_NE(nullptr, hdr);
    const auto *secondary = getHaoSecondaryTable(hdr);

    ASSERT_EQ(artifacts.haoGlobalHash.secondaryHashTable.size(),
              static_cast<size_t>(hdr->secondaryCount));
    for (u32 i = 0; i < hdr->secondaryCount; i++) {
        const auto &src = artifacts.haoGlobalHash.secondaryHashTable[i];
        const auto &dst = secondary[i];
        EXPECT_EQ(src.slotMask, dst.slotMask) << "entry=" << i;
        EXPECT_EQ(src.headMask, dst.headMask) << "entry=" << i;
        EXPECT_EQ(src.tailMask, dst.tailMask) << "entry=" << i;
        for (u32 j = 0; j < PBE_RUNTIME_RULE_VECTOR_BYTES; j++) {
            EXPECT_EQ(src.ruleVector[j], dst.ruleVector[j])
                << "entry=" << i << " byte=" << j;
            EXPECT_EQ(src.tableControl[j], dst.tableControl[j])
                << "entry=" << i << " tbl=" << j;
        }
        for (u32 slot = 0; slot < PBE_RUNTIME_RULE_SLOTS_PER_ENTRY; slot++) {
            EXPECT_EQ(src.ruleIndex[slot], dst.ruleIndex[slot])
                << "entry=" << i << " slot=" << slot;
        }
    }
}

TEST(HAOCompile, HaoRuntimeValidateLayoutAcceptsGeneratedBlob) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 668, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("maskrule", false, false, 669, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'l', 0x60}),
        hwlmLiteral("ALPHA", true, false, 670, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 671, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
    ASSERT_TRUE(artifacts.haoGlobalHash.valid);

    auto blob = buildHAOBlob(artifacts);
    ASSERT_NE(nullptr, blob.get());
    EXPECT_TRUE(HaoRuntimeValidateLayoutForTest(blob.get(),
                                                verify_u32(blob.size())));

    HAORuntimeInspectSummary summary = {};
    ASSERT_TRUE(HaoRuntimeInspectBlobForTest(blob.get(),
                                            verify_u32(blob.size()),
                                            &summary));
    EXPECT_EQ(artifacts.haoGlobalHash.stats.nonEmptyPrimary,
              summary.nonEmptyPrimary);
    EXPECT_EQ(artifacts.haoGlobalHash.stats.totalSecondaryEntries + 1U,
              summary.secondaryCount);
    EXPECT_EQ(artifacts.haoGlobalHash.stats.maxEntriesPerKey,
              summary.maxEntriesPerKey);
    EXPECT_EQ(artifacts.ruleMeta.size(), summary.ruleMetaCount);
}

TEST(HAOCompile, HaoRuntimeValidateLayoutRejectsBrokenSecondaryOffset) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 672, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("maskrule", false, false, 673, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'l', 0x60}),
        hwlmLiteral("ALPHA", true, false, 674, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 675, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
    ASSERT_TRUE(artifacts.haoGlobalHash.valid);

    auto blob = buildHAOBlob(artifacts);
    ASSERT_NE(nullptr, blob.get());

    auto *hdr = reinterpret_cast<HAORuntimeHeader *>(blob.get());
    const u32 savedSecondaryOffset = hdr->secondaryOffset;
    hdr->secondaryOffset = verify_u32(blob.size());
    EXPECT_FALSE(HaoRuntimeValidateLayoutForTest(blob.get(),
                                                 verify_u32(blob.size())));
    hdr->secondaryOffset = savedSecondaryOffset;
    EXPECT_TRUE(HaoRuntimeValidateLayoutForTest(blob.get(),
                                                verify_u32(blob.size())));
}

TEST(HAORuntime, HaoBlobNaiveExecMatchesHaoCompatDirectForSimpleRules) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 684, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 685, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 686, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 687, HWLM_ALL_GROUPS, {}, {})
    };

    auto pbe = buildFdrWithHint(lits, ENGINE_ID_HAO);
    if (!pbe || pbe->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(pbe.get())) {
        skipIfNoHaoFamilySupport();
        return;
    }

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts));
    auto haoBlob = buildHAOGlobalBlob(artifacts);
    ASSERT_NE(nullptr, haoBlob.get());

    const std::vector<u8> data = {
        'x','a','l','p','h','a','-',
        'A','l','P','h','A','-',
        't','h','e','t','a','-',
        'o','m','e','g','a'
    };

    const auto pbeMatches =
        runHaoFamilyDirectInOrder(pbe.get(), data, HWLM_ALL_GROUPS, true);
    const auto haoMatches =
        runHaoBlobDirectInOrder(haoBlob, data, HWLM_ALL_GROUPS, true);

    auto sortedPbeMatches = pbeMatches;
    auto sortedHaoMatches = haoMatches;
    /* HAO v2 does not yet guarantee identical same-end callback ordering as HAO v1. */
    std::sort(sortedPbeMatches.begin(), sortedPbeMatches.end());
    std::sort(sortedHaoMatches.begin(), sortedHaoMatches.end());
    EXPECT_EQ(sortedPbeMatches, sortedHaoMatches);
}

TEST(HAORuntime, HaoBlobNaiveExecRejectsBrokenLayoutCleanly) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 688, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 689, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 690, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 691, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts));
    auto haoBlob = buildHAOGlobalBlob(artifacts);
    ASSERT_NE(nullptr, haoBlob.get());

    auto *hdr = reinterpret_cast<HAORuntimeHeader *>(haoBlob.get());
    const u32 savedPrimaryOffset = hdr->primaryOffset;
    hdr->primaryOffset = verify_u32(haoBlob.size());

    g_matches.clear();
    hs_scratch scratch = {};
    scratch.fdr_conf = nullptr;
    const std::vector<u8> data = {'a', 'l', 'p', 'h', 'a'};
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

    EXPECT_EQ(HWLM_SUCCESS, HaoEngineExecBlobNaiveForTest(
                                haoBlob.get(), verify_u32(haoBlob.size()),
                                &args, HWLM_ALL_GROUPS));
    EXPECT_TRUE(g_matches.empty());

    hdr->primaryOffset = savedPrimaryOffset;
}

TEST(HAORuntime, HaoBlobBatchExecMatchesNaiveForSimpleRules) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 692, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 693, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 694, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 695, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts));
    auto haoBlob = buildHAOGlobalBlob(artifacts);
    ASSERT_NE(nullptr, haoBlob.get());

    const std::vector<u8> data = {
        'x','a','l','p','h','a','-',
        'A','l','P','h','A','-',
        't','h','e','t','a','-',
        'o','m','e','g','a','-',
        'a','l','p','h','a'
    };

    const auto naiveMatches =
        runHaoBlobDirectInOrder(haoBlob, data, HWLM_ALL_GROUPS, true);
    const auto batchMatches =
        runHaoBlobDirectInOrder(haoBlob, data, HWLM_ALL_GROUPS, false);
    EXPECT_EQ(naiveMatches, batchMatches);
}

TEST(HAORuntime, HaoRuntimeStatsTrackDirectReportPath) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("12345", false, false, 8692, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 8693, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("+-=-+", false, false, 8694, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("90_90", false, false, 8695, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));
    auto haoBlob = buildHAOGlobalBlob(artifacts);
    ASSERT_NE(nullptr, haoBlob.get());

    const std::vector<u8> data = {
        'x','1','2','3','4','5','-',
        'A','l','P','h','A','-',
        '+','-','=','-','+','-',
        '9','0','_','9','0','-',
        '1','2','3','4','5'
    };

    HaoRuntimeResetStatsForTest();
    const auto matches =
        runHaoBlobDirectInOrder(haoBlob, data, HWLM_ALL_GROUPS, false);
    HAORuntimeStats stats = {};
    HaoRuntimeGetStatsForTest(&stats);

    EXPECT_FALSE(matches.empty());
    EXPECT_GT(stats.primaryProbeLanes, 0U);
    EXPECT_EQ(matches.size(), stats.directReports);
    EXPECT_EQ(0U, stats.encodedConfirmCalls);
    EXPECT_EQ(0U, stats.residualRuleChecks);
    EXPECT_EQ(0U, stats.residualPosCalls);
}

TEST(HAORuntime, HaoBlobBatchExecMatchesNaiveAcrossBlockBoundaries) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 704, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 705, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 706, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 707, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts));
    auto haoBlob = buildHAOGlobalBlob(artifacts);
    ASSERT_NE(nullptr, haoBlob.get());

    const std::string text =
        "xxxxalpha-xxxxAlPhA-xxxxtheta-xxxxomega-xxxxalpha-xxxxomega";
    const std::vector<u8> data(text.begin(), text.end());

    const auto naiveMatches =
        runHaoBlobDirectInOrder(haoBlob, data, HWLM_ALL_GROUPS, true);
    const auto batchMatches =
        runHaoBlobDirectInOrder(haoBlob, data, HWLM_ALL_GROUPS, false);
    EXPECT_EQ(naiveMatches, batchMatches);
}

TEST(HAORuntime, HaoBlobBatchExecMatchesNaiveAcrossHistoryBoundary) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 708, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 709, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 710, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 711, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts));
    auto haoBlob = buildHAOGlobalBlob(artifacts);
    ASSERT_NE(nullptr, haoBlob.get());

    const std::vector<u8> history = {
        'x','x','x','a','l','p','h','a','-','A','l','P','h'
    };
    const std::vector<u8> data = {
        'A','-','t','h','e','t','a','-','o','m','e','g','a'
    };

    const auto naiveMatches = runHaoBlobDirectStreamingInOrder(
        haoBlob, history, data, HWLM_ALL_GROUPS, true);
    const auto batchMatches = runHaoBlobDirectStreamingInOrder(
        haoBlob, history, data, HWLM_ALL_GROUPS, false);
    EXPECT_EQ(naiveMatches, batchMatches);
}

TEST(HAORuntime, HaoBlobBatchExecMatchesNaiveAcrossHistoryAndMultipleBlocks) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 712, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 713, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 714, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 715, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts));
    auto haoBlob = buildHAOGlobalBlob(artifacts);
    ASSERT_NE(nullptr, haoBlob.get());

    const std::vector<u8> history = {
        'x','x','a','l','p','h','a','-','A','l','P','h','A','-'
    };
    const std::string text =
        "xxxxtheta-xxxxomega-xxxxalpha-xxxxAlPhA-xxxxomega";
    const std::vector<u8> data(text.begin(), text.end());

    const auto naiveMatches = runHaoBlobDirectStreamingInOrder(
        haoBlob, history, data, HWLM_ALL_GROUPS, true);
    const auto batchMatches = runHaoBlobDirectStreamingInOrder(
        haoBlob, history, data, HWLM_ALL_GROUPS, false);
    EXPECT_EQ(naiveMatches, batchMatches);
}

TEST(HAORuntime, BuildFdrWithHaoV2LayoutEmbedsHaoBlob) {
    Grey grey;
    grey.fdrAllowTeddy = false;
    grey.allowNeoFdr = true;
    grey.allowPbe = true;
    grey.allowHaoV2 = true;

    auto fdr = buildFdrWithHintAndGrey({
        hwlmLiteral("alpha", false, false, 696, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 697, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 698, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 699, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO, grey);

    if (!fdr || fdr->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(fdr.get())) {
        skipIfNoHaoFamilySupport();
        return;
    }

    const u8 *blob = reinterpret_cast<const u8 *>(fdr.get()) + fdrMatcherBlobOffset(fdr.get());
    u32 magic = 0;
    memcpy(&magic, blob, sizeof(magic));
    EXPECT_EQ(HAO_RUNTIME_MAGIC, magic);
}

TEST(HAORuntime, AutoHaoFamilyBuildWithHaoV2LayoutEmbedsHaoBlob) {
    Grey grey;
    grey.fdrAllowTeddy = false;
    grey.allowNeoFdr = true;
    grey.allowPbe = true;
    grey.allowHaoV2 = true;

    auto fdr = buildFdrAutoWithGrey({
        hwlmLiteral("alpha", false, false, 6996, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 6997, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 6998, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 6999, HWLM_ALL_GROUPS, {}, {})
    }, grey);

    if (!fdr || fdr->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(fdr.get())) {
        skipIfNoHaoFamilySupport();
        return;
    }

    const u8 *blob = reinterpret_cast<const u8 *>(fdr.get()) + fdrMatcherBlobOffset(fdr.get());
    u32 magic = 0;
    memcpy(&magic, blob, sizeof(magic));
    EXPECT_EQ(HAO_RUNTIME_MAGIC, magic);
}

TEST(HAORuntime, AutoHaoFamilyBuildWithHaoV2LayoutEmbedsHaoBlobForAnchorConfirmRules) {
    Grey grey;
    grey.fdrAllowTeddy = false;
    grey.allowNeoFdr = true;
    grey.allowPbe = true;
    grey.allowHaoV2 = true;

    auto fdr = buildFdrAutoWithGrey({
        hwlmLiteral("alpha", false, false, 7996, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("maskrule", false, false, 7997, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'l', 0x60}),
        hwlmLiteral("theta", false, false, 7998, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 7999, HWLM_ALL_GROUPS, {}, {})
    }, grey);

    if (!fdr || fdr->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(fdr.get())) {
        skipIfNoHaoFamilySupport();
        return;
    }

    const u8 *blob = reinterpret_cast<const u8 *>(fdr.get()) + fdrMatcherBlobOffset(fdr.get());
    u32 magic = 0;
    memcpy(&magic, blob, sizeof(magic));
    EXPECT_EQ(HAO_RUNTIME_MAGIC, magic);
}

TEST(HAORuntime, AutoHaoFamilyBuildWithHaoV2LayoutEmbedsHaoBlobForResidualUnsupportedRules) {
    Grey grey;
    grey.fdrAllowTeddy = false;
    grey.allowNeoFdr = true;
    grey.allowPbe = true;
    grey.allowHaoV2 = true;

    auto fdr = buildFdrAutoWithGrey({
        hwlmLiteral("alpha", false, false, 8096, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 8097, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 8098, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 8099, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 8100, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("sigma", false, false, 8101, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("kappa", false, false, 8102, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("mask", false, false, 8103, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'s', 0x60})
    }, grey);

    if (!fdr || fdr->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(fdr.get())) {
        skipIfNoHaoFamilySupport();
        return;
    }

    const u8 *blob = reinterpret_cast<const u8 *>(fdr.get()) + fdrMatcherBlobOffset(fdr.get());
    u32 magic = 0;
    memcpy(&magic, blob, sizeof(magic));
    EXPECT_EQ(HAO_RUNTIME_MAGIC, magic);
}

TEST(HAORuntime, HaoBlobNaiveExecMatchesHaoCompatDirectForAnchorConfirmRules) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 8201, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("maskrule", false, false, 8202, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'l', 0x60}),
        hwlmLiteral("ALPHA", true, false, 8203, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 8204, HWLM_ALL_GROUPS, {}, {})
    };
    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));
    auto haoBlob = buildHAOGlobalBlob(artifacts);
    ASSERT_NE(nullptr, haoBlob.get());

    auto pbe = buildFdrWithHint(lits, ENGINE_ID_HAO);
    if (!pbe || pbe->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(pbe.get())) {
        skipIfNoHaoFamilySupport();
        return;
    }

    const std::vector<u8> data = {
        'x','a','l','p','h','a','-',
        'm','a','s','k','r','u','l','e','-',
        'A','l','P','h','A','-',
        't','h','e','t','a'
    };

    auto pbeMatches = runHaoFamilyDirectInOrder(pbe.get(), data, HWLM_ALL_GROUPS, true);
    auto haoMatches = runHaoBlobDirectInOrder(haoBlob, data, HWLM_ALL_GROUPS, true);
    std::sort(pbeMatches.begin(), pbeMatches.end());
    std::sort(haoMatches.begin(), haoMatches.end());
    EXPECT_EQ(pbeMatches, haoMatches);
}

TEST(HAORuntime, HaoBlobNaiveExecMatchesHaoCompatDirectWithResidualUnsupportedRules) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 8301, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 8302, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 8303, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 8304, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("mask", false, false, 8305, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'s', 0x60})
    };
    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));
    auto haoBlob = buildHAOGlobalBlob(artifacts);
    ASSERT_NE(nullptr, haoBlob.get());

    auto pbe = buildFdrWithHint(lits, ENGINE_ID_HAO);
    if (!pbe || pbe->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(pbe.get())) {
        skipIfNoHaoFamilySupport();
        return;
    }

    const std::vector<u8> data = {
        'x','a','l','p','h','a','-',
        'm','a','s','k','-',
        'A','l','P','h','A','-',
        'b','e','t','a','-',
        't','h','e','t','a'
    };

    auto pbeMatches = runHaoFamilyDirectInOrder(pbe.get(), data, HWLM_ALL_GROUPS, true);
    auto haoMatches = runHaoBlobDirectInOrder(haoBlob, data, HWLM_ALL_GROUPS, true);
    std::sort(pbeMatches.begin(), pbeMatches.end());
    std::sort(haoMatches.begin(), haoMatches.end());
    EXPECT_EQ(pbeMatches, haoMatches);
}

TEST(HAORuntime, HaoRuntimeStatsTrackResidualPath) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 8791, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 8792, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 8793, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 8794, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("mask", false, false, 8795, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'s', 0x60})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));
    auto haoBlob = buildHAOGlobalBlob(artifacts);
    ASSERT_NE(nullptr, haoBlob.get());

    const std::vector<u8> data = {
        'x','a','l','p','h','a','-',
        'm','a','s','k','-',
        'A','l','P','h','A','-',
        'b','e','t','a','-',
        't','h','e','t','a'
    };

    HaoRuntimeResetStatsForTest();
    const auto matches =
        runHaoBlobDirectInOrder(haoBlob, data, HWLM_ALL_GROUPS, true);
    HAORuntimeStats stats = {};
    HaoRuntimeGetStatsForTest(&stats);

    EXPECT_FALSE(matches.empty());
    EXPECT_GT(stats.residualPosCalls, 0U);
    EXPECT_GT(stats.residualRuleChecks, 0U);
    EXPECT_GT(stats.residualConfirmCalls, 0U);
    EXPECT_GT(stats.residualConfirmMatches, 0U);
}

TEST(HAORuntime, EmbeddedHaoV2FdrBatchMatchesNaive) {
    Grey grey;
    grey.fdrAllowTeddy = false;
    grey.allowNeoFdr = true;
    grey.allowPbe = true;
    grey.allowHaoV2 = true;

    auto fdr = buildFdrWithHintAndGrey({
        hwlmLiteral("alpha", false, false, 700, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 701, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 702, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 703, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO, grey);

    if (!fdr || fdr->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(fdr.get())) {
        skipIfNoHaoFamilySupport();
        return;
    }

    const std::vector<u8> data = {
        'x','a','l','p','h','a','-',
        'A','l','P','h','A','-',
        't','h','e','t','a','-',
        'o','m','e','g','a','-',
        'a','l','p','h','a'
    };

    const auto naiveMatches =
        runHaoFamilyDirectInOrder(fdr.get(), data, HWLM_ALL_GROUPS, true);
    const auto batchMatches =
        runHaoFamilyDirectInOrder(fdr.get(), data, HWLM_ALL_GROUPS, false);
    EXPECT_EQ(naiveMatches, batchMatches);
}

TEST(HAOCompile, PrimaryBitmapMatchesNonEmptyL1Entries) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 650, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 651, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 652, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 653, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));

    u32 nonEmptyOffsets = 0;
    u32 setBits = 0;
    u32 expectedBitmapBytes = 0;
    for (const auto &klass : artifacts.maskClasses) {
        expectedBitmapBytes +=
            verify_u32((klass.primaryHashTable.offsets.size() + 7U) / 8U);
        for (u32 i = 0; i < klass.primaryHashTable.offsets.size(); i++) {
            if (klass.primaryHashTable.offsets[i]) {
                nonEmptyOffsets++;
            }
        }
        for (u32 i = 0; i < klass.primaryHashBitmap.bits.size(); i++) {
            setBits += verify_u32(
                std::bitset<8>(klass.primaryHashBitmap.bits[i]).count());
        }
    }
    EXPECT_EQ(expectedBitmapBytes, computeHaoCompatInspectStats(artifacts).bitmapBytes);
    EXPECT_EQ(nonEmptyOffsets, setBits);
}

TEST(HAOCompile, TargetSveFeatureMapping) {
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

TEST(HAOCompile, TargetSveCompatibilityCheck) {
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

TEST(HAOCompile, SveBitPermPrereqRequiresBuildAndTargetSupport) {
    hs_platform_info noneInfo = {};
    target_t noneTarget(noneInfo);
    EXPECT_FALSE(haoHasSveBitPermPrereq(noneTarget));

    hs_platform_info sveInfo = {};
    sveInfo.cpu_features = HS_CPU_FEATURES_SVE;
    target_t sveTarget(sveInfo);
    EXPECT_FALSE(haoHasSveBitPermPrereq(sveTarget));

    hs_platform_info sve2Info = {};
    sve2Info.cpu_features = HS_CPU_FEATURES_SVE | HS_CPU_FEATURES_SVE2;
    target_t sve2Target(sve2Info);
    EXPECT_FALSE(haoHasSveBitPermPrereq(sve2Target));

    hs_platform_info bitpermInfo = {};
    bitpermInfo.cpu_features = HS_CPU_FEATURES_SVE | HS_CPU_FEATURES_SVE2 |
                               HS_CPU_FEATURES_SVEBITPERM;
    target_t bitpermTarget(bitpermInfo);
#if defined(HS_BUILD_HAVE_SVEBITPERM)
    EXPECT_TRUE(haoHasSveBitPermPrereq(bitpermTarget));
#else
    EXPECT_FALSE(haoHasSveBitPermPrereq(bitpermTarget));
#endif
}

TEST(HAOCompile, BextFastPathRequiresSveBitPerm) {
    hs_platform_info noneInfo = {};
    target_t noneTarget(noneInfo);
    EXPECT_FALSE(haoCanUseBextFastPath(noneTarget));

    hs_platform_info sve2Info = {};
    sve2Info.cpu_features = HS_CPU_FEATURES_SVE | HS_CPU_FEATURES_SVE2;
    target_t sve2Target(sve2Info);
    EXPECT_FALSE(haoCanUseBextFastPath(sve2Target));

    hs_platform_info bitpermInfo = {};
    bitpermInfo.cpu_features = HS_CPU_FEATURES_SVE | HS_CPU_FEATURES_SVE2 |
                               HS_CPU_FEATURES_SVEBITPERM;
    target_t bitpermTarget(bitpermInfo);
#if defined(HS_BUILD_HAVE_SVEBITPERM)
    EXPECT_TRUE(haoCanUseBextFastPath(bitpermTarget));
#else
    EXPECT_FALSE(haoCanUseBextFastPath(bitpermTarget));
#endif
}

TEST(HAOCompile, BextMaskMatchesSelectors) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 690, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 691, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 692, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 693, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));
    ASSERT_EQ(PBE_EXTRACT_MODE_BEXT, artifacts.extractMode);

    std::vector<u32> expectedBits;
    for (u32 i = 0; i < artifacts.bitSelectors.size(); i++) {
        const auto &sel = artifacts.bitSelectors[i];
        expectedBits.push_back(static_cast<u32>(sel.byteOffset) * 8U +
                               static_cast<u32>(sel.bitOffset));
    }

    u64a expectedMask = 0;
    for (u32 i = 0; i < expectedBits.size(); i++) {
        expectedMask |= ((u64a)1 << expectedBits[i]);
        if (i) {
            EXPECT_LT(expectedBits[i - 1], expectedBits[i]);
        }
    }
    EXPECT_EQ(expectedMask, artifacts.bextMask);
}

TEST(HAOCompile, MaskClassesBuildFromDistinctKeyMasks) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 694, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 695, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 696, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", true, false, 697, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));
    ASSERT_FALSE(artifacts.maskClasses.empty());

    std::set<u32> seenMasks;
    for (const auto &klass : artifacts.maskClasses) {
        EXPECT_TRUE(seenMasks.insert(klass.classMask).second);
        EXPECT_EQ(klass.classKeyBits, popcount32(klass.classMask));
    }
}

TEST(HAOCompile, PartialMaskRulesDoNotCollapseToSingleZeroBucket) {
    auto lits = makeDuplicateLiterals("ab", true, false, 698, 7,
                                      HWLM_ALL_GROUPS);
    lits.emplace_back("AB", false, false, 699, HWLM_ALL_GROUPS,
                      std::vector<u8>{}, std::vector<u8>{});

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));

    const u32 fullMask = fullKeyMaskForArtifacts(artifacts);
    bool sawPartialClass = false;
    for (const auto &klass : artifacts.maskClasses) {
        if (klass.classMask != fullMask) {
            sawPartialClass = true;
            EXPECT_NE(0U, klass.classMask);
            EXPECT_GT(klass.secondaryCount, 0U);
        }
    }
    EXPECT_TRUE(sawPartialClass);
}

TEST(HAOCompile, MaskClassCountWithinRuntimeLimit) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 699, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 700, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 701, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("THETA", true, false, 702, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ab", true, false, 703, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("mask", false, false, 704, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'s', 0x60})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));
    EXPECT_LE(artifacts.maskClasses.size(),
              static_cast<size_t>(PBE_MAX_MASK_CLASSES));

    auto blob = buildPBEBlob(artifacts);
    ASSERT_NE(nullptr, blob.get());
    const auto *hdr = reinterpret_cast<const HAOCompatRuntimeHeader *>(blob.get());
    EXPECT_LE(hdr->classCount, PBE_RUNTIME_MAX_MASK_CLASSES);
}

TEST(HAOCompile, HotMaskClassesPreferHigherRuleCoverage) {
    auto partialLits = makeDuplicateLiterals("ab", true, false, 705, 8,
                                             HWLM_ALL_GROUPS);
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 713, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 714, HWLM_ALL_GROUPS, {}, {})
    };
    lits.insert(lits.end(), partialLits.begin(), partialLits.end());

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));

    bool sawHotPartialClass = false;
    const u32 fullMask = fullKeyMaskForArtifacts(artifacts);
    for (const auto &klass : artifacts.maskClasses) {
        if ((klass.flags & PBE_MASK_CLASS_FLAG_HOT) &&
            klass.classMask != fullMask) {
            sawHotPartialClass = true;
        }
    }
    EXPECT_TRUE(sawHotPartialClass);

    auto blob = buildPBEBlob(artifacts);
    ASSERT_NE(nullptr, blob.get());
    const auto *hdr = reinterpret_cast<const HAOCompatRuntimeHeader *>(blob.get());
    const auto *classTable = reinterpret_cast<const PBERuntimeMaskClass *>(
        reinterpret_cast<const u8 *>(hdr) + hdr->classTableOffset);
    bool sawHotRuntimeClass = false;
    for (u32 i = 0; i < hdr->classCount; i++) {
        if (classTable[i].flags & PBE_RUNTIME_MASK_CLASS_FLAG_HOT) {
            sawHotRuntimeClass = true;
            break;
        }
    }
    EXPECT_TRUE(sawHotRuntimeClass);
}

TEST(HAOCompile, HaoRulePlansRespectExpansionLimit) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 714, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 715, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 716, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("mask", false, false, 717, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'s', 0x60})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));
    ASSERT_EQ(lits.size(), artifacts.haoRulePlans.size());

    u32 countedExpandedKeys = 0;
    for (const auto &plan : artifacts.haoRulePlans) {
        if (plan.category == HAORuleCategory::HAO_RULE_UNSUPPORTED) {
            EXPECT_GT(plan.keyExpansion.selectedAmbigBits,
                      HAO_MAX_KEY_AMBIG_BITS);
            EXPECT_EQ(0U, plan.keyExpansion.expandedKeyCount);
            continue;
        }

        EXPECT_LE(plan.keyExpansion.selectedAmbigBits,
                  HAO_MAX_KEY_AMBIG_BITS);
        EXPECT_EQ(1U << plan.keyExpansion.selectedAmbigBits,
                  plan.keyExpansion.expandedKeyCount);
        countedExpandedKeys += plan.keyExpansion.expandedKeyCount;
    }

    EXPECT_EQ(countedExpandedKeys, artifacts.haoSummary.totalExpandedKeys);
}

TEST(HAOCompile, HaoNocasePlanIsNormalized) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 718, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 719, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 720, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("gamma", false, false, 721, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));
    ASSERT_EQ(lits.size(), artifacts.haoRulePlans.size());

    const auto &plan = artifacts.haoRulePlans[1];
    EXPECT_EQ(HAORuleCategory::HAO_RULE_NOCASE, plan.category);
    EXPECT_TRUE(plan.flags & HAO_RULE_PLAN_FLAG_NORMALIZED);
    EXPECT_TRUE(plan.verifier.flags & HAO_RULE_PLAN_FLAG_NORMALIZED);
    EXPECT_FALSE(plan.needFullConfirm);
}

TEST(HAOCompile, HaoMaskRulesBecomeAnchorConfirm) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 722, HWLM_ALL_GROUPS, {}, {}),
        // Use an 8-byte rule to avoid tripping the selected-bit ambiguity limit.
        // This keeps the test focused on the supplementary-mask -> anchor-confirm path.
        hwlmLiteral("maskrule", false, false, 723, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'l', 0x60}),
        hwlmLiteral("delta", false, false, 724, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 725, HWLM_ALL_GROUPS, {}, {}),
    };
    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));
    ASSERT_EQ(lits.size(), artifacts.haoRulePlans.size());

    const auto &plan = artifacts.haoRulePlans[1];
    EXPECT_EQ(HAORuleCategory::HAO_RULE_ANCHOR_CONFIRM, plan.category);
    EXPECT_TRUE(plan.needFullConfirm);
    EXPECT_TRUE(plan.flags & HAO_RULE_PLAN_FLAG_NEEDS_CONFIRM);
    EXPECT_TRUE(plan.flags & HAO_RULE_PLAN_FLAG_HAS_SUPPLEMENTARY_MASK);
    EXPECT_TRUE(plan.verifier.flags & HAO_RULE_PLAN_FLAG_ANCHOR_FRAGMENT);
}

TEST(HAOCompile, HaoSummaryTracksCoverageAndAnchors) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 726, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 727, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("maskrule", false, false, 728, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'l', 0x60}),
        hwlmLiteral("theta", false, false, 729, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));

    EXPECT_EQ(lits.size(), artifacts.haoSummary.totalRules);
    EXPECT_EQ(artifacts.haoSummary.fastPathRules +
              artifacts.haoSummary.unsupportedRules,
              artifacts.haoSummary.totalRules);

    u32 anchorCount = 0;
    for (const auto &plan : artifacts.haoRulePlans) {
        if (plan.category == HAORuleCategory::HAO_RULE_ANCHOR_CONFIRM) {
            anchorCount++;
        }
    }
    EXPECT_EQ(anchorCount, artifacts.haoSummary.anchorConfirmRules);
}

TEST(HAOCompile, HaoSummaryTracksDirectReportRules) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("12345", false, false, 7296, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 7297, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("+-=-+", false, false, 7298, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("90_90", false, false, 7299, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));

    EXPECT_EQ(3U, artifacts.haoSummary.exactRules);
    EXPECT_EQ(1U, artifacts.haoSummary.nocaseRules);
    EXPECT_EQ(lits.size(), artifacts.haoSummary.directReportRules);
    EXPECT_EQ(0U, artifacts.haoSummary.residualRules);
    EXPECT_EQ(0U, artifacts.haoSummary.fastPathConfirmRules);
}

TEST(HAOCompile, HaoDirectReportSafetyFollowsSelectorCoverage) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 7300, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 7301, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("12345", false, false, 7302, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 7303, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));
    ASSERT_EQ(lits.size(), artifacts.haoRulePlans.size());

    for (u32 i = 0; i < lits.size(); i++) {
        const auto &lit = lits[i];
        const auto &plan = artifacts.haoRulePlans[i];
        bool expectedSafe = false;

        if (!lit.noruns && lit.msk.empty() && lit.cmp.empty() &&
            plan.verifier.anchorOffset == 0 &&
            plan.verifier.anchorLength == lit.s.size()) {
            if (plan.category == HAORuleCategory::HAO_RULE_NOCASE) {
                expectedSafe = true;
            } else if (plan.category == HAORuleCategory::HAO_RULE_EXACT) {
                expectedSafe = true;
                for (u32 byteFromEnd = 0; byteFromEnd < lit.s.size();
                     byteFromEnd++) {
                    const u8 c =
                        verify_u8(lit.s[lit.s.size() - byteFromEnd - 1]);
                    bool foundCaseBit = false;

                    if (!ourisalpha(c)) {
                        continue;
                    }
                    for (const auto &sel : artifacts.bitSelectors) {
                        if (sel.byteOffset == byteFromEnd &&
                            sel.bitOffset == 5U) {
                            foundCaseBit = true;
                            break;
                        }
                    }
                    if (!foundCaseBit) {
                        expectedSafe = false;
                        break;
                    }
                }
            }
        }

        EXPECT_EQ(expectedSafe,
                  !!(plan.flags & HAO_RULE_PLAN_FLAG_DIRECT_REPORT_SAFE))
            << "ruleIndex=" << i << " literal=" << lit.s;
    }
}

TEST(HAOCompile, HaoSummaryTracksResidualUnsupportedRules) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 7396, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 7397, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 7398, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 7399, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("mask", false, false, 7400, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'s', 0x60})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));

    EXPECT_GT(artifacts.haoSummary.residualRules, 0U);
    EXPECT_GT(artifacts.haoSummary.residualUnsupportedRules, 0U);
}

TEST(HAOCompile, HaoGlobalHashBuildsSinglePrimarySpace) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 730, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 731, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("maskrule", false, false, 732, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'l', 0x60}),
        hwlmLiteral("theta", false, false, 733, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));
    ASSERT_TRUE(artifacts.haoGlobalHash.valid);

    u32 nonResidualExpandedKeys = 0;
    for (const auto &plan : artifacts.haoRulePlans) {
        if (plan.category == HAORuleCategory::HAO_RULE_UNSUPPORTED) {
            continue;
        }
        nonResidualExpandedKeys += plan.keyExpansion.expandedKeyCount;
    }

    const u32 primaryCount = 1U << artifacts.keyBits;
    EXPECT_EQ(artifacts.keyBits, artifacts.haoGlobalHash.keyBits);
    EXPECT_EQ(primaryCount,
              artifacts.haoGlobalHash.primaryHashTable.offsets.size());
    EXPECT_EQ((primaryCount + 7U) / 8U,
              artifacts.haoGlobalHash.primaryHashBitmap.bits.size());
    EXPECT_EQ(nonResidualExpandedKeys,
              artifacts.haoGlobalHash.stats.totalExpandedKeysInBuckets);
    EXPECT_GT(artifacts.haoGlobalHash.stats.nonEmptyPrimary, 0U);
}

TEST(HAOCompile, HaoGlobalHashStoresAnchorConfirmFragments) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 742, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("maskrule", false, false, 743, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'l', 0x60}),
        hwlmLiteral("delta", false, false, 744, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 745, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));
    ASSERT_TRUE(artifacts.haoGlobalHash.valid);

    const auto &plan = artifacts.haoRulePlans[1];
    ASSERT_EQ(HAORuleCategory::HAO_RULE_ANCHOR_CONFIRM, plan.category);

    bool found = false;
    for (u32 i = 1; i < artifacts.haoGlobalHash.secondaryHashTable.size(); i++) {
        const auto &entry = artifacts.haoGlobalHash.secondaryHashTable[i];
        for (u32 slot = 0; slot < PBE_RULE_SLOTS_PER_ENTRY; slot++) {
            if (!(entry.slotMask & (1U << slot))) {
                continue;
            }
            if (entry.ruleIndex[slot] != plan.ruleIndex) {
                continue;
            }

            found = true;
            const u32 laneBase = slot * PBE_BYTES_PER_RULE_SLOT;
            for (u32 j = 0; j < PBE_BYTES_PER_RULE_SLOT; j++) {
                if (!(plan.verifier.validByteMask & (1U << j))) {
                    continue;
                }
                EXPECT_EQ(plan.verifier.bytes[j],
                          entry.ruleVector[laneBase + j]);
            }
        }
    }

    EXPECT_TRUE(found);
}

TEST(HAOCompile, HaoGlobalHashStoresVerifierFragments) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 734, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 735, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 736, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 737, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));
    ASSERT_TRUE(artifacts.haoGlobalHash.valid);

    const auto &plan = artifacts.haoRulePlans[1];
    ASSERT_EQ(HAORuleCategory::HAO_RULE_EXACT, plan.category);
    bool found = false;
    for (u32 i = 1; i < artifacts.haoGlobalHash.secondaryHashTable.size(); i++) {
        const auto &entry = artifacts.haoGlobalHash.secondaryHashTable[i];
        for (u32 slot = 0; slot < PBE_RULE_SLOTS_PER_ENTRY; slot++) {
            if (!(entry.slotMask & (1U << slot))) {
                continue;
            }
            if (entry.ruleIndex[slot] != plan.ruleIndex) {
                continue;
            }
            found = true;
            const u32 laneBase = slot * PBE_BYTES_PER_RULE_SLOT;
            for (u32 j = 0; j < PBE_BYTES_PER_RULE_SLOT; j++) {
                if (plan.verifier.validByteMask & (1U << j)) {
                    EXPECT_EQ(plan.verifier.bytes[j],
                              entry.ruleVector[laneBase + j]);
                    EXPECT_EQ(((laneBase + j) & 0x0fU),
                              entry.tableControl[laneBase + j]);
                }
            }
        }
    }

    EXPECT_TRUE(found);
}

TEST(HAOCompile, HaoGlobalHashTableControlEncodesShuffleBytes) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 738, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 739, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 740, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 741, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
    ASSERT_TRUE(buildPBEArtifacts(lits, &artifacts, false));
    ASSERT_TRUE(artifacts.haoGlobalHash.valid);

    const auto &plan = artifacts.haoRulePlans[1];
    ASSERT_EQ(HAORuleCategory::HAO_RULE_EXACT, plan.category);

    bool found = false;
    for (u32 i = 1; i < artifacts.haoGlobalHash.secondaryHashTable.size(); i++) {
        const auto &entry = artifacts.haoGlobalHash.secondaryHashTable[i];
        for (u32 slot = 0; slot < PBE_RULE_SLOTS_PER_ENTRY; slot++) {
            if (!(entry.slotMask & (1U << slot))) {
                continue;
            }
            if (entry.ruleIndex[slot] != plan.ruleIndex) {
                continue;
            }

            found = true;
            const u32 laneBase = slot * PBE_BYTES_PER_RULE_SLOT;
            for (u32 j = 0; j < PBE_BYTES_PER_RULE_SLOT; j++) {
                if (!(plan.verifier.validByteMask & (1U << j))) {
                    continue;
                }
                EXPECT_EQ(((laneBase + j) & 0x0fU),
                          entry.tableControl[laneBase + j]);
            }
        }
    }

    EXPECT_TRUE(found);
}

TEST(HAOExtract, BextMatchesScalar) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 700, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 701, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 702, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 703, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
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
        const u32 bextKey = packedBitsToKeyForTest(artifacts, packed);
        EXPECT_EQ(scalarKey, bextKey) << "endPos=" << endPos;
    }
}

TEST(HAOExtract, BextHistoryBoundaryConsistency) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("abcz", false, false, 710, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("YY", true, false, 711, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("kappa", false, false, 712, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 713, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompatCompileArtifacts artifacts;
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
        const u32 bextKey = packedBitsToKeyForTest(artifacts, packed);
        EXPECT_EQ(scalarKey, bextKey) << "boundary endPos=" << endPos;
    }
}

TEST(HAOPrefilter, EntryLaneMaskMatchesScalar) {
    auto pbe = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 720, 0x1, {}, {}),
        hwlmLiteral("ALPHA", true,  false, 721, 0x3, {}, {}),
        hwlmLiteral("beta",  false, false, 722, 0x1, {}, {}),
        hwlmLiteral("delta", false, true,  723, 0x2, {}, {}),
        hwlmLiteral("gamma", false, false, 724, 0x4, {}, {}),
        hwlmLiteral("theta", true,  false, 725, 0x7, {}, {})
    }, ENGINE_ID_HAO);

    if (!pbe || pbe->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(pbe.get())) {
        skipIfNoHaoFamilySupport();
        return;
    }

    const auto *hdr = getHaoCompatRuntimeHeader(pbe.get());
    ASSERT_NE(nullptr, hdr);
    const auto *secondary = reinterpret_cast<const HAOCompatRuntimeSecondaryHashEntry *>(
        reinterpret_cast<const u8 *>(hdr) + hdr->secondaryOffset);

    const std::vector<u8> data = {'a','l','p','h','a',' ',
                                  'B','E','T','A',' ',
                                  'd','e','l','t','a',' ',
                                  'T','H','E','T','A'};
    hs_scratch scratch = {};
    scratch.fdr_conf = nullptr;
    const auto args = makeRuntimeArgs(data, {}, &scratch);

    for (u32 entry = 1; entry < hdr->secondaryCount; entry++) {
        for (size_t endPos = 0; endPos < data.size(); endPos++) {
            const u32 scalarMask = HaoCompatRuntimeEntryMatchMaskForTest(
                &secondary[entry], &args, endPos, 0);
            const u32 vectorMask = HaoCompatRuntimeEntryMatchMaskForTest(
                &secondary[entry], &args, endPos, 1);
            EXPECT_EQ(scalarMask, vectorMask)
                << "entry=" << entry << " endPos=" << endPos;
        }
    }
}

TEST(HAOPrefilter, EntryLaneMaskHistoryBoundaryConsistency) {
    auto pbe = buildFdrWithHint({
        hwlmLiteral("abcz", false, false, 726, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("YY", true, false, 727, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("kappa", false, false, 728, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 729, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO);

    if (!pbe || pbe->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(pbe.get())) {
        skipIfNoHaoFamilySupport();
        return;
    }

    const auto *hdr = getHaoCompatRuntimeHeader(pbe.get());
    ASSERT_NE(nullptr, hdr);
    const auto *secondary = reinterpret_cast<const HAOCompatRuntimeSecondaryHashEntry *>(
        reinterpret_cast<const u8 *>(hdr) + hdr->secondaryOffset);

    const std::vector<u8> history = {'x', 'x', 'a', 'b'};
    const std::vector<u8> data = {'c', 'z', 'Y', 'Y', 'a', 'B', 'c', 'z'};
    hs_scratch scratch = {};
    scratch.fdr_conf = nullptr;
    const auto args = makeRuntimeArgs(data, history, &scratch);

    for (u32 entry = 1; entry < hdr->secondaryCount; entry++) {
        for (size_t endPos = 0; endPos < data.size(); endPos++) {
            const u32 scalarMask = HaoCompatRuntimeEntryMatchMaskForTest(
                &secondary[entry], &args, endPos, 0);
            const u32 vectorMask = HaoCompatRuntimeEntryMatchMaskForTest(
                &secondary[entry], &args, endPos, 1);
            EXPECT_EQ(scalarMask, vectorMask)
                << "entry=" << entry << " endPos=" << endPos;
        }
    }
}

TEST(HAOPrefilter, HeadTailMaskRejectsSameAsScalarForPartialSlots) {
    auto pbe = buildFdrWithHint({
        hwlmLiteral("ab", true, false, 760, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("abc", false, false, 761, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 762, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("z", false, false, 763, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO);

    if (!pbe || pbe->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(pbe.get())) {
        skipIfNoHaoFamilySupport();
        return;
    }

    const auto *hdr = getHaoCompatRuntimeHeader(pbe.get());
    ASSERT_NE(nullptr, hdr);
    const auto *secondary = reinterpret_cast<const HAOCompatRuntimeSecondaryHashEntry *>(
        reinterpret_cast<const u8 *>(hdr) + hdr->secondaryOffset);

    const std::vector<u8> data = {'A','b',' ','a','b','c',' ',
                                  'b','e','t','a',' ','z','z'};
    hs_scratch scratch = {};
    scratch.fdr_conf = nullptr;
    const auto args = makeRuntimeArgs(data, {}, &scratch);

    for (u32 entry = 1; entry < hdr->secondaryCount; entry++) {
        for (size_t endPos = 0; endPos < data.size(); endPos++) {
            const u32 scalarMask = HaoCompatRuntimeEntryMatchMaskForTest(
                &secondary[entry], &args, endPos, 0);
            const u32 vectorMask = HaoCompatRuntimeEntryMatchMaskForTest(
                &secondary[entry], &args, endPos, 1);
            EXPECT_EQ(scalarMask, vectorMask)
                << "entry=" << entry << " endPos=" << endPos;
        }
    }
}

TEST(HAOPrefilter, SingleSlotFastPathMatchesScalar) {
    auto pbe = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 770, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 771, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("gamma", false, false, 772, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 773, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO);

    if (!pbe || pbe->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(pbe.get())) {
        skipIfNoHaoFamilySupport();
        return;
    }

    const auto *hdr = getHaoCompatRuntimeHeader(pbe.get());
    ASSERT_NE(nullptr, hdr);
    const auto *secondary = reinterpret_cast<const HAOCompatRuntimeSecondaryHashEntry *>(
        reinterpret_cast<const u8 *>(hdr) + hdr->secondaryOffset);

    const std::vector<u8> data = {'x','x','a','l','p','h','a','x',
                                  'd','e','l','t','a','x',
                                  'g','a','m','m','a','x',
                                  'o','m','e','g','a'};
    hs_scratch scratch = {};
    scratch.fdr_conf = nullptr;
    const auto args = makeRuntimeArgs(data, {}, &scratch);
    bool sawSingleSlot = false;

    for (u32 entry = 1; entry < hdr->secondaryCount; entry++) {
        if (secondary[entry].ruleCount != 1) {
            continue;
        }
        sawSingleSlot = true;
        for (size_t endPos = 0; endPos < data.size(); endPos++) {
            const u32 scalarMask = HaoCompatRuntimeEntryMatchMaskForTest(
                &secondary[entry], &args, endPos, 0);
            const u32 vectorMask = HaoCompatRuntimeEntryMatchMaskForTest(
                &secondary[entry], &args, endPos, 1);
            EXPECT_EQ(scalarMask, vectorMask)
                << "entry=" << entry << " endPos=" << endPos;
        }
    }

    EXPECT_TRUE(sawSingleSlot);
}

TEST(HAORuntime, Batch4MatchesNaiveDirect) {
    auto pbe = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 730, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 734, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 731, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("THETA", true, false, 733, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO);

    if (!pbe || pbe->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(pbe.get())) {
        skipIfNoHaoFamilySupport();
        return;
    }
    const auto *hdr = getHaoCompatRuntimeHeader(pbe.get());
    ASSERT_NE(nullptr, hdr);
    EXPECT_LE(hdr->classCount, 2U);

    const std::vector<u8> data = {'a','l','p','h','a',' ',
                                  'd','e','l','t','a',' ',
                                  'B','E','T','A',' ',
                                  'T','H','E','T','A',' ',
                                  'A','L','P','H','A',' ',
                                  'D','E','L','T','A'};

    const auto naiveMatches = runHaoFamilyDirectInOrder(pbe.get(), data,
                                                  HWLM_ALL_GROUPS, true);
    const auto batchMatches = runHaoFamilyDirectInOrder(pbe.get(), data,
                                                  HWLM_ALL_GROUPS, false);
    EXPECT_EQ(naiveMatches, batchMatches);
}

TEST(HAORuntime, MaskClassBatchMatchesNaiveDirect) {
    auto pbe = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 734, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 735, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 736, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("THETA", true, false, 737, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ab", true, false, 738, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("mask", false, false, 739, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'s', 0x60})
    }, ENGINE_ID_HAO);

    if (!pbe || pbe->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(pbe.get())) {
        skipIfNoHaoFamilySupport();
        return;
    }
    const auto *hdr = getHaoCompatRuntimeHeader(pbe.get());
    ASSERT_NE(nullptr, hdr);
    EXPECT_GT(hdr->classCount, 2U);

    const std::vector<u8> data = {'a','l','p','h','a',' ',
                                  'A','L','P','H','A',' ',
                                  'b','e','t','a',' ',
                                  'T','H','E','T','A',' ',
                                  'a','B',' ',
                                  'm','a','s','k'};

    const auto naiveMatches = runHaoFamilyDirectInOrder(pbe.get(), data,
                                                  HWLM_ALL_GROUPS, true);
    const auto batchMatches = runHaoFamilyDirectInOrder(pbe.get(), data,
                                                  HWLM_ALL_GROUPS, false);
    EXPECT_EQ(naiveMatches, batchMatches);
}

TEST(HAORuntime, BitmapProbePackedMatchesScalar) {
    const std::vector<u8> bitmap = {
        0x96, // idx 1,2,4,7
        0x21, // idx 8,13
        0x00,
        0x80  // idx 31
    };
    const u32 primaryIdx[] = {1, 2, 3, 4, 7, 8, 13, 14, 31, 99};
    const u32 laneCount = verify_u32(sizeof(primaryIdx) / sizeof(primaryIdx[0]));

    const u32 scalarMask = HaoCompatRuntimeBitmapProbeMaskForTest(
        bitmap.data(), verify_u32(bitmap.size()), primaryIdx, laneCount, 0);
    const u32 packedMask = HaoCompatRuntimeBitmapProbeMaskForTest(
        bitmap.data(), verify_u32(bitmap.size()), primaryIdx, laneCount, 1);

    EXPECT_EQ(scalarMask, packedMask);
}

TEST(HAORuntime, BitmapProbeHandlesSharedBitmapByte) {
    const std::vector<u8> bitmap = {
        0x96 // idx 1,2,4,7
    };
    const u32 primaryIdx[] = {1, 2, 3, 4, 7};
    const u32 laneCount = verify_u32(sizeof(primaryIdx) / sizeof(primaryIdx[0]));

    const u32 scalarMask = HaoCompatRuntimeBitmapProbeMaskForTest(
        bitmap.data(), verify_u32(bitmap.size()), primaryIdx, laneCount, 0);
    const u32 packedMask = HaoCompatRuntimeBitmapProbeMaskForTest(
        bitmap.data(), verify_u32(bitmap.size()), primaryIdx, laneCount, 1);

    EXPECT_EQ(0x1bU, scalarMask);
    EXPECT_EQ(scalarMask, packedMask);
}

TEST(HAORuntime, Batch4SparseBitmapSkipsEmptyLanes) {
    auto pbe = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 740, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 741, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 742, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 743, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO);

    if (!pbe || pbe->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(pbe.get())) {
        skipIfNoHaoFamilySupport();
        return;
    }

    const std::vector<u8> data = {'x','x','x','x','x','x','x','x',
                                  'a','l','p','h','a','x','x','x',
                                  'd','e','l','t','a','x','x','x',
                                  'o','m','e','g','a','x','x','x'};

    const auto naiveMatches = runHaoFamilyDirectInOrder(pbe.get(), data,
                                                  HWLM_ALL_GROUPS, true);
    const auto batchMatches = runHaoFamilyDirectInOrder(pbe.get(), data,
                                                  HWLM_ALL_GROUPS, false);
    EXPECT_EQ(naiveMatches, batchMatches);
}

TEST(HAORuntime, Batch4OrderStableWithWildcard) {
    auto pbe = buildFdrWithHint({
        hwlmLiteral("ab", true, false, 750, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("wxyz", false, false, 751, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("mnop", false, false, 752, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("qrst", false, false, 753, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO);

    if (!pbe || pbe->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(pbe.get())) {
        skipIfNoHaoFamilySupport();
        return;
    }

    const std::vector<u8> data = {'A','b','x','w','x','y','z'};
    const auto naiveMatches = runHaoFamilyDirectInOrder(pbe.get(), data,
                                                  HWLM_ALL_GROUPS, true);
    const auto batchMatches = runHaoFamilyDirectInOrder(pbe.get(), data,
                                                  HWLM_ALL_GROUPS, false);

    ASSERT_EQ(naiveMatches, batchMatches);
    for (size_t i = 1; i < batchMatches.size(); i++) {
        EXPECT_LE(batchMatches[i - 1].end, batchMatches[i].end);
    }
}

TEST(HAORuntime, InvalidMagicFallsBackCleanly) {
    auto pbe = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 660, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 661, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 662, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 663, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO);

    if (!pbe || pbe->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(pbe.get())) {
        skipIfNoHaoFamilySupport();
        return;
    }

    auto *hdr = reinterpret_cast<HAOCompatRuntimeHeader *>(
        reinterpret_cast<u8 *>(pbe.get()) + fdrMatcherBlobOffset(pbe.get()));
    const u32 savedMagic = hdr->magic;
    hdr->magic = 0;

    const hwlm_error_t rv = runHaoFamilyDirect(
        pbe.get(), {'a','l','p','h','a'}, HWLM_ALL_GROUPS);
    EXPECT_EQ(HWLM_SUCCESS, rv);
    EXPECT_TRUE(g_matches.empty());

    hdr->magic = savedMagic;
}

TEST(HAORuntime, InvalidVersionFallsBackCleanly) {
    auto pbe = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 670, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 671, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 672, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 673, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO);

    if (!pbe || pbe->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(pbe.get())) {
        skipIfNoHaoFamilySupport();
        return;
    }

    auto *hdr = reinterpret_cast<HAOCompatRuntimeHeader *>(
        reinterpret_cast<u8 *>(pbe.get()) + fdrMatcherBlobOffset(pbe.get()));
    const u32 savedVersion = hdr->version;
    hdr->version = savedVersion + 1;

    const hwlm_error_t rv = runHaoFamilyDirect(
        pbe.get(), {'a','l','p','h','a'}, HWLM_ALL_GROUPS);
    EXPECT_EQ(HWLM_SUCCESS, rv);
    EXPECT_TRUE(g_matches.empty());

    hdr->version = savedVersion;
}

TEST(HAORuntime, InvalidLayoutOffsetFallsBackCleanly) {
    auto pbe = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 680, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 681, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 682, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 683, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO);

    if (!pbe || pbe->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(pbe.get())) {
        skipIfNoHaoFamilySupport();
        return;
    }

    auto *hdr = reinterpret_cast<HAOCompatRuntimeHeader *>(
        reinterpret_cast<u8 *>(pbe.get()) + fdrMatcherBlobOffset(pbe.get()));
    const u32 savedSecondaryOffset = hdr->secondaryOffset;
    hdr->secondaryOffset = fdrMatcherBlobSize(pbe.get());

    const hwlm_error_t rv = runHaoFamilyDirect(
        pbe.get(), {'a','l','p','h','a'}, HWLM_ALL_GROUPS);
    EXPECT_EQ(HWLM_SUCCESS, rv);
    EXPECT_TRUE(g_matches.empty());

    hdr->secondaryOffset = savedSecondaryOffset;
}

TEST(HAOInspect, DumpSelectorsAndHashTables) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("literal8", false, false, 100, 0x1, {}, {}),
        hwlmLiteral("CaseRule", true,  false, 101, 0x3, {}, {}),
        hwlmLiteral("ab",       false, false, 102, 0x1, {}, {}),
        hwlmLiteral("maskrule", false, false, 103, 0x2,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'l', 0x60}),
        hwlmLiteral("theta999", false, false, 104, 0x4, {}, {}),
    };

    HAOCompatCompileArtifacts artifacts;
    const bool ok = buildPBEArtifacts(lits, &artifacts);
    ASSERT_TRUE(ok);
    const HAOCompatInspectStats stats = computeHaoCompatInspectStats(artifacts);

    std::cout << "\n========== HAO Inspect Begin ==========\n";
    std::cout << "[Rules]\n";
    for (size_t i = 0; i < lits.size(); i++) {
        const auto &lit = lits[i];
        const auto &plan = artifacts.haoRulePlans[i];
        const bool residual =
            plan.category == HAORuleCategory::HAO_RULE_UNSUPPORTED ||
            ((plan.flags & HAO_RULE_PLAN_FLAG_NEEDS_CONFIRM) &&
             plan.category != HAORuleCategory::HAO_RULE_ANCHOR_CONFIRM);
        const char *kind = "exact";
        if (!lit.msk.empty() || !lit.cmp.empty()) {
            kind = "fuzzy-mask";
        } else if (lit.nocase) {
            kind = "nocase";
        } else if (lit.s.size() < PBE_BYTES_PER_RULE_SLOT) {
            kind = "short-wildcard-like";
        }
        std::cout << "  r" << i
                  << " id=" << lit.id
                  << " s=\"" << lit.s << "\""
                  << " kind=" << kind
                  << " nocase=" << lit.nocase
                  << " noruns=" << lit.noruns
                  << " haoCategory=" << haoCategoryName(plan.category)
                  << " directReportSafe="
                  << !!(plan.flags & HAO_RULE_PLAN_FLAG_DIRECT_REPORT_SAFE)
                  << " residual=" << residual
                  << " selectedAmbigBits="
                  << plan.keyExpansion.selectedAmbigBits
                  << " expandedKeyCount="
                  << plan.keyExpansion.expandedKeyCount
                  << " groups=0x" << std::hex << lit.groups << std::dec
                  << "\n";
    }

    std::cout << "\n[Selectors]\n";
    std::cout << "  keyBits=" << artifacts.keyBits
              << " selectorCount=" << artifacts.bitSelectors.size()
              << " selectedBits=[";
    for (size_t i = 0; i < artifacts.bitSelectors.size(); i++) {
        const auto &sel = artifacts.bitSelectors[i];
        std::cout << (static_cast<u32>(sel.byteOffset) * 8U +
                      static_cast<u32>(sel.bitOffset));
        if (i + 1 != artifacts.bitSelectors.size()) {
            std::cout << ", ";
        }
    }
    std::cout << "]\n";
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

    std::cout << "\n[Build Stats]\n";
    std::cout << "  nonEmptyL1=" << stats.nonEmptyL1
              << " bitmapBytes=" << stats.bitmapBytes
              << " exactBucketCount=" << stats.exactBucketCount
              << " multiEntryBucketCount=" << stats.multiEntryBucketCount
              << " maxL2EntriesPerKey=" << stats.maxL2EntriesPerKey << "\n";
    std::cout << "  maskClassCount=" << stats.maskClassCount
              << " partialClassCount=" << stats.partialClassCount
              << " totalL2Entries=" << stats.totalL2Entries
              << " totalRulesInL2=" << stats.totalRulesInL2 << "\n";

    std::cout << "\n[HAO Global Bitmap / L1]\n";
    std::cout << "  keyBits=" << artifacts.haoGlobalHash.keyBits
              << " nonEmptyL1=" << artifacts.haoGlobalHash.stats.nonEmptyPrimary
              << " totalSecondaryEntries="
              << artifacts.haoGlobalHash.stats.totalSecondaryEntries
              << " maxEntriesPerKey="
              << artifacts.haoGlobalHash.stats.maxEntriesPerKey << "\n";
    dumpBitmapBytes(std::cout, artifacts.haoGlobalHash.primaryHashBitmap.bits,
                    "  ");
    for (u32 key = 0; key < artifacts.haoGlobalHash.primaryHashTable.offsets.size();
         key++) {
        const u32 value = artifacts.haoGlobalHash.primaryHashTable.offsets[key];
        if (!value) {
            continue;
        }
        std::cout << "  key={dec=" << key
                  << ",hex=0x" << std::hex << key << std::dec
                  << ",bin=" << u32ToBin(key, artifacts.haoGlobalHash.keyBits)
                  << "} -> value={dec=" << value
                  << ",hex=0x" << std::hex << value << std::dec
                  << ",offset=" << (value & PBE_L1_OFFSET_MASK)
                  << ",count=" << (value >> PBE_L1_COUNT_SHIFT) << "}\n";
    }

    std::cout << "\n[HAO Global L2]\n";
    std::cout << "  size=" << artifacts.haoGlobalHash.secondaryHashTable.size()
              << " (entry0 is null)\n";
    for (u32 i = 1; i < artifacts.haoGlobalHash.secondaryHashTable.size(); i++) {
        const auto &e = artifacts.haoGlobalHash.secondaryHashTable[i];
        const u32 ruleCount = haoSlotCount(e.slotMask);
        std::cout << "  HAO-L2[" << i << "]"
                  << " ruleCount=" << ruleCount
                  << " entryCapacity=" << PBE_RULE_SLOTS_PER_ENTRY
                  << " headMask=0x" << std::hex << e.headMask
                  << " tailMask=0x" << e.tailMask << std::dec
                  << " headMaskBits=" << maskToBin(e.headMask)
                  << " tailMaskBits=" << maskToBin(e.tailMask) << "\n";
        std::cout << "    slotMask=0x" << std::hex
                  << static_cast<u32>(e.slotMask) << std::dec << "\n";
        for (u32 j = 0; j < PBE_RULE_SLOTS_PER_ENTRY; j++) {
            if (!(e.slotMask & (1U << j))) {
                continue;
            }
            const u16 ridx = e.ruleIndex[j];
            std::cout << "    slot" << j
                      << ": ruleIndex=" << ridx
                      << " vectorFull=\"" << slotVectorString(e, j, false) << "\""
                      << " vectorActive=\"" << slotVectorString(e, j, true) << "\""
                      << " tbl=[";
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

    std::map<u32, std::vector<u32>> offToL1Keys;
    std::map<u32, u32> offToClassMask;
    std::map<u32, u32> offToClassKeyBits;
    for (const auto &klass : artifacts.maskClasses) {
        for (u32 key = 0; key < klass.primaryHashTable.offsets.size(); key++) {
            const u32 value = klass.primaryHashTable.offsets[key];
            if (value) {
                const u32 off = value & PBE_L1_OFFSET_MASK;
                const u32 count = value >> PBE_L1_COUNT_SHIFT;
                for (u32 n = 0; n < count; n++) {
                    offToL1Keys[off + n].push_back(key);
                    offToClassMask[off + n] = klass.classMask;
                    offToClassKeyBits[off + n] = klass.classKeyBits;
                }
            }
        }
    }

    struct ActualKeyInfo {
        bool found = false;
        u32 l2Off = 0;
        u32 classMask = 0;
        u32 classKeyBits = 0;
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
            actual[ridx].classMask = offToClassMask[i];
            actual[ridx].classKeyBits = offToClassKeyBits[i];
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
                  << " classMask={dec=" << actual[i].classMask
                  << ",hex=0x" << std::hex << actual[i].classMask << std::dec
                  << ",bin=" << u32ToBin(actual[i].classMask, artifacts.keyBits)
                  << "}"
                  << " keyValue={dec=" << actual[i].keyValue
                  << ",hex=0x" << std::hex << actual[i].keyValue << std::dec
                  << ",bin=" << u32ToBin(actual[i].keyValue,
                                         actual[i].classKeyBits)
                  << "}"
                  << " keyMask={dec=" << actual[i].keyMask
                  << ",hex=0x" << std::hex << actual[i].keyMask << std::dec
                  << ",bin=" << u32ToBin(actual[i].keyMask,
                                         actual[i].classKeyBits)
                  << "}"
                  << " bucketKeys(" << actual[i].bucketKeys.size() << ")=";
        for (size_t k = 0; k < actual[i].bucketKeys.size(); k++) {
            const u32 bk = actual[i].bucketKeys[k];
            std::cout << "{dec=" << bk << ",hex=0x" << std::hex << bk
                      << std::dec
                      << ",bin=" << u32ToBin(bk, actual[i].classKeyBits) << "}";
            if (k + 1 != actual[i].bucketKeys.size()) {
                std::cout << ", ";
            }
        }
        std::cout << "\n";
    }

    std::cout << "\n[Mask Classes / L1 Hash Tables]\n";
    for (const auto &klass : artifacts.maskClasses) {
        std::cout << "  classId=" << klass.classId
                  << " classMask={dec=" << klass.classMask
                  << ",hex=0x" << std::hex << klass.classMask << std::dec
                  << ",bin=" << u32ToBin(klass.classMask, artifacts.keyBits)
                  << "}"
                  << " classKeyBits=" << klass.classKeyBits
                  << " primarySize=" << klass.primaryHashTable.offsets.size()
                  << " secondaryOffset=" << klass.secondaryOffset
                  << " secondaryCount=" << klass.secondaryCount << "\n";
        dumpBitmapBytes(std::cout, klass.primaryHashBitmap.bits, "    ");
        size_t nonEmpty = 0;
        for (u32 key = 0; key < klass.primaryHashTable.offsets.size(); key++) {
            const u32 value = klass.primaryHashTable.offsets[key];
            if (!value) {
                continue;
            }
            nonEmpty++;
            std::cout << "    key={dec=" << key
                      << ",hex=0x" << std::hex << key << std::dec
                      << ",bin=" << u32ToBin(key, klass.classKeyBits) << "}"
                      << " -> value={dec=" << value
                      << ",hex=0x" << std::hex << value << std::dec
                      << ",offset=" << (value & PBE_L1_OFFSET_MASK)
                      << ",count=" << (value >> PBE_L1_COUNT_SHIFT) << "}\n";
        }
        std::cout << "    nonEmpty=" << nonEmpty << "\n";
    }

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
                      << " vectorFull=\"" << slotVectorString(e, j, false) << "\""
                      << " vectorActive=\"" << slotVectorString(e, j, true) << "\""
                      << " tbl=[";
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

    std::cout << "========== HAO Inspect End ==========\n\n";
}

} // namespace
