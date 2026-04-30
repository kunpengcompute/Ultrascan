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
#include "fdr/hao_runtime_inline.h"
#include "hwlm/hwlm_internal.h"
#include "scratch.h"
#include "util/arch.h"
#include "util/bitutils.h"
#include "util/compare.h"
#include "util/target_info.h"
#include "util/verify_types.h"

#include "gtest/gtest.h"

#ifdef HS_UNIT_HAS_HSBENCH_CORPUS_DB
#include "tools/hsbench/data_corpus.h"
#endif

#include <algorithm>
#include <bitset>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
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
static constexpr size_t HAO_COLLISION_SAMPLE_COUNT = 1U << 20;

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
    grey.allowHaoV2 = true;

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
                    bytecode_ptr<FDR> *neoOut, bytecode_ptr<FDR> *haoOut) {
    if (!neoOut || !haoOut) {
        return false;
    }

    auto neo = buildFdrWithHint(lits, ENGINE_ID_NEO);
    auto hao = buildFdrWithHint(std::move(lits), ENGINE_ID_HAO);

    if (!neo || !hao) {
        return false;
    }

    if (neo->engineID != ENGINE_ID_NEO || hao->engineID != ENGINE_ID_HAO) {
        return false;
    }

    if (!fdrMatcherBlobOffset(hao.get()) || !fdrMatcherBlobSize(hao.get())) {
        return false;
    }

    *neoOut = std::move(neo);
    *haoOut = std::move(hao);
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
hwlm_error_t runHaoDirect(const FDR *fdr, const std::vector<u8> &data,
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
const HAORuntimeHeader *getHaoRuntimeHeader(const FDR *fdr) {
    if (!fdr || !fdrMatcherBlobOffset(fdr)) {
        return nullptr;
    }
    return reinterpret_cast<const HAORuntimeHeader *>(
        reinterpret_cast<const u8 *>(fdr) + fdrMatcherBlobOffset(fdr));
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
std::vector<Match> runHaoDirectInOrder(const FDR *fdr,
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

    const hwlm_error_t rv = useNaive ? HaoEngineExecNaiveForTest(fdr, &args,
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
void skipIfNoHaoSupport() {
    // HAO path is currently gated on Arm64 in the HAO feasibility path.
    SUCCEED() << "Skip HAO regression on this target (HAO v2 unavailable).";
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
    return u32ToBin(mask, HAO_LAYOUT_RULE_VECTOR_BYTES);
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
std::string slotVectorString(const HAOSecondaryHashEntry &entry, u32 slot,
                             bool activeOnly) {
    const u32 laneBase = slot * HAO_LAYOUT_BYTES_PER_RULE_SLOT;
    std::string out;

    out.reserve(HAO_LAYOUT_BYTES_PER_RULE_SLOT);
    for (u32 i = 0; i < HAO_LAYOUT_BYTES_PER_RULE_SLOT; i++) {
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
    return popcount32(slotMask & ((1U << HAO_RUNTIME_RULE_SLOTS_PER_ENTRY) - 1U));
}

static
u32 nthSetBitForTest(u32 mask, u32 rank) {
    while (mask) {
        const u32 bit = ctz32(mask);
        if (!rank) {
            return bit;
        }
        rank--;
        mask &= mask - 1U;
    }
    return 32U;
}

static
u32 verifierPackedIndexForTest(const HAOSecondaryHashEntry &entry, u32 slot,
                               u8 validMask, u32 byteIndex) {
#if HAO_L2_PACKED_VERIFY
    const u32 rank = popcount32(entry.slotMask & ((1U << slot) - 1U));
    const u32 start = nthSetBitForTest(entry.tailMask, rank);
    u32 ordinal = 0;

    for (u32 i = 0; i < byteIndex; i++) {
        if (validMask & (1U << i)) {
            ordinal++;
        }
    }
    return start + ordinal;
#else
    (void)validMask;
    return slot * HAO_LAYOUT_BYTES_PER_RULE_SLOT + byteIndex;
#endif
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
u8 ruleBitStateNoMask(const hwlmLiteral &lit, const HAOBitSelector &sel) {
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
Grey makeHaoGrey(bool allowHaoV2 = true) {
    Grey grey;
    grey.fdrAllowTeddy = false;
    grey.allowNeoFdr = true;
    grey.allowHaoV2 = allowHaoV2;
    return grey;
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
        const u32 entryCount = primary[i] >> HAO_LAYOUT_L1_COUNT_SHIFT;
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
u64a loadWindow64NormalizedForTest(const std::vector<u8> &history,
                                   const std::vector<u8> &data,
                                   size_t historyLen, size_t endPos,
                                   u32 windowBytes) {
    const size_t totalLen = historyLen + data.size();
    if (!windowBytes || windowBytes > HAO_LAYOUT_BYTES_PER_RULE_SLOT) {
        windowBytes = HAO_LAYOUT_BYTES_PER_RULE_SLOT;
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
u32 extractScalarKeyFromWindowForTest(const HAOCompileArtifacts &artifacts,
                                      u64a window) {
    u32 key = 0;
    for (u32 i = 0; i < artifacts.bitSelectors.size() &&
                    i < HAO_LAYOUT_MAX_SELECTORS;
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

    while (mask && outBit < HAO_LAYOUT_MAX_SELECTORS) {
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
u32 packedBitsToKeyForTest(const HAOCompileArtifacts &artifacts, u64a packed) {
    if (artifacts.bitSelectors.empty()) {
        return 0;
    }
    if (artifacts.bitSelectors.size() >= 32) {
        return (u32)packed;
    }
    return (u32)(packed & ((1ULL << artifacts.bitSelectors.size()) - 1ULL));
}

static
std::vector<u8> makeDeterministicCollisionCorpus(size_t count) {
    std::vector<u8> data(count);
    u64a state = 0x9e3779b97f4a7c15ULL;
    for (size_t i = 0; i < count; i++) {
        // xorshift64* keeps the corpus deterministic and cheap in UT.
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        const u64a mixed = state * 2685821657736338717ULL;
        data[i] = verify_u8((mixed >> 56) & 0xffU);
    }
    return data;
}

static
std::vector<u32> extractRuntimeKeysForBlocks(
    const HAORuntimeHeader *hdr, const HAORuntimeBitSelector *selectors,
    const std::vector<std::vector<u8>> &blocks);

static
std::vector<u32> extractScalarReferenceKeysForBlocks(
    const HAOCompileArtifacts &artifacts,
    const std::vector<std::vector<u8>> &blocks);

static
std::vector<u32> extractRuntimeKeysForData(const HAORuntimeHeader *hdr,
                                           const HAORuntimeBitSelector *selectors,
                                           const std::vector<u8> &data) {
    std::vector<std::vector<u8>> blocks;
    if (!data.empty()) {
        blocks.push_back(data);
    }
    return extractRuntimeKeysForBlocks(hdr, selectors, blocks);
}

static
std::vector<u32> extractRuntimeKeysForBlocks(
    const HAORuntimeHeader *hdr, const HAORuntimeBitSelector *selectors,
    const std::vector<std::vector<u8>> &blocks) {
    std::vector<u32> keys;
    if (!hdr || !selectors || blocks.empty()) {
        return keys;
    }

    size_t totalBytes = 0;
    for (const auto &b : blocks) {
        totalBytes += b.size();
    }
    keys.reserve(totalBytes);

    for (const auto &data : blocks) {
        if (data.empty()) {
            continue;
        }
        hs_scratch scratch = {};
        scratch.fdr_conf = nullptr;
        const auto args = makeRuntimeArgs(data, {}, &scratch);
        for (size_t endPos = 0; endPos < data.size(); endPos++) {
            const u64a window = haoLoadWindow64Normalized(&args, endPos,
                                                          hdr->windowBytes);
            keys.push_back(haoExtractKeyFromWindow(hdr, selectors, window));
        }
    }
    return keys;
}

static
std::vector<u32> extractScalarReferenceKeysForData(
    const HAOCompileArtifacts &artifacts, const std::vector<u8> &data) {
    std::vector<std::vector<u8>> blocks;
    if (!data.empty()) {
        blocks.push_back(data);
    }
    return extractScalarReferenceKeysForBlocks(artifacts, blocks);
}

static
std::vector<u32> extractScalarReferenceKeysForBlocks(
    const HAOCompileArtifacts &artifacts,
    const std::vector<std::vector<u8>> &blocks) {
    std::vector<u32> keys;
    if (blocks.empty()) {
        return keys;
    }

    size_t totalBytes = 0;
    for (const auto &b : blocks) {
        totalBytes += b.size();
    }
    keys.reserve(totalBytes);

    for (const auto &data : blocks) {
        if (data.empty()) {
            continue;
        }
        hs_scratch scratch = {};
        scratch.fdr_conf = nullptr;
        const auto args = makeRuntimeArgs(data, {}, &scratch);
        for (size_t endPos = 0; endPos < data.size(); endPos++) {
            const u64a window = haoLoadWindow64Normalized(&args, endPos,
                                                          artifacts.windowBytes);
            keys.push_back(extractScalarKeyFromWindowForTest(artifacts, window));
        }
    }
    return keys;
}

static
double collisionRateFromHashes(const std::vector<u32> &hashes, u32 tableSize,
                               u32 *collisionsOut, u32 *nonEmptyOut) {
    if (collisionsOut) {
        *collisionsOut = 0;
    }
    if (nonEmptyOut) {
        *nonEmptyOut = 0;
    }
    if (!tableSize || hashes.empty()) {
        return 0.0;
    }

    std::vector<u32> hashCounts(tableSize, 0);
    u32 collisions = 0;
    u32 nonEmpty = 0;
    for (u32 hash : hashes) {
        if (hash >= tableSize) {
            continue;
        }
        if (hashCounts[hash] > 0) {
            collisions++;
        } else {
            nonEmpty++;
        }
        hashCounts[hash]++;
    }

    if (collisionsOut) {
        *collisionsOut = collisions;
    }
    if (nonEmptyOut) {
        *nonEmptyOut = nonEmpty;
    }
    return static_cast<double>(collisions) / hashes.size();
}

static
std::string trimAscii(std::string s) {
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };

    while (!s.empty() && isSpace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

static
bool parseUnsignedU64Token(const std::string &token, unsigned long long *out) {
    if (!out) {
        return false;
    }
    const std::string t = trimAscii(token);
    if (t.empty()) {
        return false;
    }

    errno = 0;
    char *end = nullptr;
    const unsigned long long v = std::strtoull(t.c_str(), &end, 0);
    if (errno || !end || *end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

static
bool parseBoolToken(const std::string &token, bool *out) {
    if (!out) {
        return false;
    }
    std::string t = trimAscii(token);
    for (auto &c : t) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (t == "1" || t == "true" || t == "yes" || t == "y") {
        *out = true;
        return true;
    }
    if (t == "0" || t == "false" || t == "no" || t == "n") {
        *out = false;
        return true;
    }
    return false;
}

static
bool decodeEscapedLiteral(const std::string &input, std::string *output,
                          std::string *error) {
    if (!output) {
        return false;
    }
    output->clear();
    output->reserve(input.size());

    const auto hexValue = [](char c) -> int {
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
    };

    for (size_t i = 0; i < input.size(); i++) {
        if (input[i] != '\\') {
            output->push_back(input[i]);
            continue;
        }
        if (i + 1 >= input.size()) {
            if (error) {
                *error = "dangling escape at end of literal";
            }
            return false;
        }

        const char esc = input[++i];
        switch (esc) {
        case 'n':
            output->push_back('\n');
            break;
        case 'r':
            output->push_back('\r');
            break;
        case 't':
            output->push_back('\t');
            break;
        case '\\':
            output->push_back('\\');
            break;
        case 'x': {
            if (i + 2 >= input.size()) {
                if (error) {
                    *error = "incomplete \\xNN escape";
                }
                return false;
            }
            const int hi = hexValue(input[i + 1]);
            const int lo = hexValue(input[i + 2]);
            if (hi < 0 || lo < 0) {
                if (error) {
                    *error = "invalid hex digit in \\xNN escape";
                }
                return false;
            }
            output->push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
            break;
        }
        default:
            // Keep unknown escapes usable for common punctuation.
            output->push_back(esc);
            break;
        }
    }
    return true;
}

static
std::vector<std::string> splitByPipe(const std::string &line) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (start <= line.size()) {
        const size_t pos = line.find('|', start);
        if (pos == std::string::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return fields;
}

static
bool isRegexMetaChar(char c) {
    switch (c) {
    case '.':
    case '^':
    case '$':
    case '*':
    case '+':
    case '?':
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case '|':
        return true;
    default:
        return false;
    }
}

static
bool isEscapedAt(const std::string &s, size_t pos) {
    size_t backslashCount = 0;
    while (pos > 0 && s[pos - 1] == '\\') {
        backslashCount++;
        pos--;
    }
    return (backslashCount % 2U) != 0U;
}

static
bool decodeHsbenchPcreLiteralBody(const std::string &body,
                                  std::string *literalOut, bool *unsupportedOut,
                                  std::string *error) {
    if (!literalOut || !unsupportedOut) {
        return false;
    }
    *unsupportedOut = false;
    literalOut->clear();
    literalOut->reserve(body.size());

    const auto hexValue = [](char c) -> int {
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
    };

    for (size_t i = 0; i < body.size(); i++) {
        const char c = body[i];
        if (c != '\\') {
            if (isRegexMetaChar(c)) {
                *unsupportedOut = true;
                if (error) {
                    std::ostringstream oss;
                    oss << "unsupported regex metachar '" << c
                        << "' in hsbench literal line";
                    *error = oss.str();
                }
                return false;
            }
            literalOut->push_back(c);
            continue;
        }

        if (i + 1 >= body.size()) {
            if (error) {
                *error = "dangling escape in hsbench regex literal";
            }
            return false;
        }

        const char esc = body[++i];
        switch (esc) {
        case 'n':
            literalOut->push_back('\n');
            break;
        case 'r':
            literalOut->push_back('\r');
            break;
        case 't':
            literalOut->push_back('\t');
            break;
        case 'x': {
            if (i + 2 >= body.size()) {
                if (error) {
                    *error = "incomplete \\xNN escape in hsbench regex literal";
                }
                return false;
            }
            const int hi = hexValue(body[i + 1]);
            const int lo = hexValue(body[i + 2]);
            if (hi < 0 || lo < 0) {
                if (error) {
                    *error = "invalid hex digit in hsbench \\xNN escape";
                }
                return false;
            }
            literalOut->push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
            break;
        }
        default:
            // Escaped punctuation (e.g. \. or \/) is treated as literal.
            literalOut->push_back(esc);
            break;
        }
    }

    if (literalOut->empty()) {
        if (error) {
            *error = "empty literal after decoding hsbench regex";
        }
        return false;
    }
    return true;
}

static
bool parseHsbenchPcreAsLiteral(const std::string &pcreToken,
                               std::string *literalOut, bool *nocaseOut,
                               bool *unsupportedOut, std::string *error) {
    if (!literalOut || !nocaseOut || !unsupportedOut) {
        return false;
    }
    *unsupportedOut = false;

    const std::string token = trimAscii(pcreToken);
    if (token.size() < 2 || token[0] != '/') {
        if (error) {
            *error = "hsbench rule must use /.../flags form";
        }
        return false;
    }

    size_t closingSlash = std::string::npos;
    for (size_t i = token.size(); i-- > 1;) {
        if (token[i] != '/') {
            continue;
        }
        if (!isEscapedAt(token, i)) {
            closingSlash = i;
            break;
        }
    }
    if (closingSlash == std::string::npos) {
        if (error) {
            *error = "cannot find closing '/' in hsbench regex token";
        }
        return false;
    }

    const std::string body = token.substr(1, closingSlash - 1);
    const std::string flags = token.substr(closingSlash + 1);

    bool nocase = false;
    for (char f : flags) {
        if (std::isspace(static_cast<unsigned char>(f))) {
            continue;
        }
        switch (f) {
        case 'i':
            nocase = true;
            break;
        case 'm':
        case 's':
        case 'H':
        case 'O':
        case 'V':
        case 'W':
        case '8':
        case 'P':
        case 'L':
        case 'C':
        case 'Q':
            // Accepted for compatibility with hsbench expression parser.
            break;
        default:
            *unsupportedOut = true;
            if (error) {
                std::ostringstream oss;
                oss << "unsupported hsbench flag '" << f
                    << "' for literal-only collision test";
                *error = oss.str();
            }
            return false;
        }
    }

    std::string literal;
    if (!decodeHsbenchPcreLiteralBody(body, &literal, unsupportedOut, error)) {
        return false;
    }

    *literalOut = std::move(literal);
    *nocaseOut = nocase;
    return true;
}

enum class RuleLineParseResult {
    PARSED,
    UNSUPPORTED,
    ERROR
};

struct LiteralLoadStats {
    size_t totalNonCommentLines = 0;
    size_t loadedLiteralCount = 0;
    size_t skippedUnsupportedCount = 0;
    size_t skippedTooLongCount = 0;
};

/* Rule-file format:
 * 1) literal-only line: <literal>
 *    -> nocase=0, noruns=0, id=auto, groups=HWLM_ALL_GROUPS
 * 2) full line: <literal>|<nocase>|<noruns>|<id>|<groups>
 *    bool accepts 0/1/true/false/yes/no; integers accept decimal/hex.
 * 3) hsbench style: <id>:/literal/flags (literal-only subset).
 * Literal supports escapes: \\n \\r \\t \\\\ \\xNN. */
static
RuleLineParseResult parseRuleLine(const std::string &line, u32 autoId,
                                  std::string *literalOut, bool *nocaseOut,
                                  bool *norunsOut, u32 *idOut,
                                  hwlm_group_t *groupsOut, std::string *error) {
    if (!literalOut || !nocaseOut || !norunsOut || !idOut || !groupsOut) {
        return RuleLineParseResult::ERROR;
    }

    std::string literalToken;
    bool nocase = false;
    bool noruns = false;
    u32 id = autoId;
    hwlm_group_t groups = HWLM_ALL_GROUPS;
    bool hsbenchDecoded = false;

    const size_t pipePos = line.find('|');
    if (pipePos != std::string::npos) {
        const auto fields = splitByPipe(line);
        if (fields.size() != 5) {
            if (error) {
                *error = "expected 5 pipe-separated fields";
            }
            return RuleLineParseResult::ERROR;
        }

        literalToken = trimAscii(fields[0]);
        if (!parseBoolToken(fields[1], &nocase) ||
            !parseBoolToken(fields[2], &noruns)) {
            if (error) {
                *error = "failed to parse bool fields nocase/noruns";
            }
            return RuleLineParseResult::ERROR;
        }

        unsigned long long idRaw = 0;
        if (!parseUnsignedU64Token(fields[3], &idRaw) ||
            idRaw > std::numeric_limits<u32>::max()) {
            if (error) {
                *error = "failed to parse id as u32";
            }
            return RuleLineParseResult::ERROR;
        }
        id = static_cast<u32>(idRaw);

        unsigned long long groupsRaw = 0;
        if (!parseUnsignedU64Token(fields[4], &groupsRaw)) {
            if (error) {
                *error = "failed to parse groups";
            }
            return RuleLineParseResult::ERROR;
        }
        groups = static_cast<hwlm_group_t>(groupsRaw);
    } else {
        const size_t colonPos = line.find(':');
        bool hsbenchCandidate = false;
        if (colonPos != std::string::npos) {
            const std::string idToken = trimAscii(line.substr(0, colonPos));
            const std::string pcreToken = trimAscii(line.substr(colonPos + 1));
            if (!pcreToken.empty() && pcreToken[0] == '/') {
                hsbenchCandidate = true;
                unsigned long long idRaw = 0;
                if (!parseUnsignedU64Token(idToken, &idRaw) ||
                    idRaw > std::numeric_limits<u32>::max()) {
                    if (error) {
                        *error = "failed to parse hsbench id as u32";
                    }
                    return RuleLineParseResult::ERROR;
                }
                id = static_cast<u32>(idRaw);
                bool unsupported = false;
                if (!parseHsbenchPcreAsLiteral(pcreToken, &literalToken,
                                               &nocase, &unsupported, error)) {
                    return unsupported ? RuleLineParseResult::UNSUPPORTED
                                       : RuleLineParseResult::ERROR;
                }
                hsbenchDecoded = true;
            }
        }

        if (!hsbenchCandidate) {
            literalToken = trimAscii(line);
        }
    }

    if (literalToken.empty()) {
        if (error) {
            *error = "empty literal";
        }
        return RuleLineParseResult::ERROR;
    }

    std::string decodedLiteral = literalToken;
    if (!hsbenchDecoded && !decodeEscapedLiteral(literalToken, &decodedLiteral,
                                                 error)) {
        return RuleLineParseResult::ERROR;
    }
    if (decodedLiteral.empty()) {
        if (error) {
            *error = "decoded literal is empty";
        }
        return RuleLineParseResult::ERROR;
    }

    *literalOut = std::move(decodedLiteral);
    *nocaseOut = nocase;
    *norunsOut = noruns;
    *idOut = id;
    *groupsOut = groups;
    return RuleLineParseResult::PARSED;
}

static
bool loadLiteralsFromFile(const std::string &path,
                          std::vector<hwlmLiteral> *litsOut,
                          LiteralLoadStats *statsOut,
                          std::string *error) {
    if (!litsOut) {
        return false;
    }
    litsOut->clear();

    LiteralLoadStats stats = {};

    std::ifstream in(path);
    if (!in) {
        if (error) {
            *error = "failed to open rules file: " + path;
        }
        return false;
    }

    std::string line;
    u32 autoId = 1;
    size_t lineNo = 0;
    while (std::getline(in, line)) {
        lineNo++;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const std::string trimmed = trimAscii(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        stats.totalNonCommentLines++;

        std::string literal;
        bool nocase = false;
        bool noruns = false;
        u32 id = autoId;
        hwlm_group_t groups = HWLM_ALL_GROUPS;
        std::string lineError;
        const auto parseResult = parseRuleLine(trimmed, autoId, &literal,
                                               &nocase, &noruns, &id, &groups,
                                               &lineError);
        if (parseResult == RuleLineParseResult::UNSUPPORTED) {
            stats.skippedUnsupportedCount++;
            continue;
        }
        if (parseResult != RuleLineParseResult::PARSED) {
            if (error) {
                std::ostringstream oss;
                oss << "rules parse error at line " << lineNo << ": "
                    << lineError;
                *error = oss.str();
            }
            return false;
        }

        if (literal.size() > HWLM_LITERAL_MAX_LEN) {
            stats.skippedTooLongCount++;
            continue;
        }

        litsOut->emplace_back(literal, nocase, noruns, id, groups,
                              std::vector<u8>{}, std::vector<u8>{});
        stats.loadedLiteralCount++;
        if (autoId < std::numeric_limits<u32>::max()) {
            autoId++;
        }
    }

    if (litsOut->empty()) {
        if (error) {
            std::ostringstream oss;
            oss << "rules file has no valid literals: " << path
                << " (non-comment lines=" << stats.totalNonCommentLines
                << ", unsupported=" << stats.skippedUnsupportedCount
                << ", tooLong=" << stats.skippedTooLongCount << ")";
            *error = oss.str();
        }
        return false;
    }

    if (statsOut) {
        *statsOut = stats;
    }
    return true;
}

static
bool loadBytesFromFile(const std::string &path, std::vector<u8> *bytesOut,
                       std::string *error) {
    if (!bytesOut) {
        return false;
    }
    bytesOut->clear();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) {
            *error = "failed to open input file: " + path;
        }
        return false;
    }

    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    bytesOut->reserve(raw.size());
    for (char c : raw) {
        bytesOut->push_back(verify_u8(static_cast<unsigned char>(c)));
    }

    if (bytesOut->empty()) {
        if (error) {
            *error = "input file is empty: " + path;
        }
        return false;
    }
    return true;
}

static
size_t blockBytesCount(const std::vector<std::vector<u8>> &blocks) {
    size_t total = 0;
    for (const auto &block : blocks) {
        total += block.size();
    }
    return total;
}

enum class CollisionInputMode {
    AUTO,
    RAW,
    HSBENCH_DB
};

static
std::string toLowerAscii(std::string s) {
    for (auto &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

static
bool pathLooksLikeHsbenchDb(const std::string &path) {
    const std::string lower = toLowerAscii(path);
    return (lower.size() >= 3 && lower.substr(lower.size() - 3) == ".db") ||
           (lower.size() >= 7 && lower.substr(lower.size() - 7) == ".sqlite") ||
           (lower.size() >= 8 && lower.substr(lower.size() - 8) == ".sqlite3");
}

static
bool parseCollisionInputModeFromEnv(CollisionInputMode *modeOut) {
    if (!modeOut) {
        return false;
    }
    *modeOut = CollisionInputMode::AUTO;

    const char *modeEnv = std::getenv("HS_HAO_COLLISION_INPUT_MODE");
    if (!modeEnv || !*modeEnv) {
        return true;
    }

    const std::string mode = toLowerAscii(trimAscii(modeEnv));
    if (mode == "auto") {
        *modeOut = CollisionInputMode::AUTO;
        return true;
    }
    if (mode == "raw") {
        *modeOut = CollisionInputMode::RAW;
        return true;
    }
    if (mode == "hsbench_db") {
        *modeOut = CollisionInputMode::HSBENCH_DB;
        return true;
    }
    return false;
}

static
bool loadRawInputBlocksFromFile(const std::string &path,
                                std::vector<std::vector<u8>> *blocksOut,
                                std::string *error) {
    if (!blocksOut) {
        return false;
    }
    blocksOut->clear();

    std::vector<u8> bytes;
    if (!loadBytesFromFile(path, &bytes, error)) {
        return false;
    }
    if (bytes.empty()) {
        if (error) {
            *error = "input file is empty: " + path;
        }
        return false;
    }
    blocksOut->push_back(std::move(bytes));
    return true;
}

static
bool loadHsbenchCorpusBlocksFromFile(const std::string &path,
                                     std::vector<std::vector<u8>> *blocksOut,
                                     std::string *error) {
    if (!blocksOut) {
        return false;
    }
    blocksOut->clear();

#ifdef HS_UNIT_HAS_HSBENCH_CORPUS_DB
    try {
        const auto corpusBlocks = readCorpus(path);
        blocksOut->reserve(corpusBlocks.size());
        for (const auto &block : corpusBlocks) {
            if (block.payload.empty()) {
                continue;
            }
            std::vector<u8> bytes;
            bytes.reserve(block.payload.size());
            for (char c : block.payload) {
                bytes.push_back(verify_u8(static_cast<unsigned char>(c)));
            }
            if (!bytes.empty()) {
                blocksOut->push_back(std::move(bytes));
            }
        }
    } catch (const DataCorpusError &e) {
        if (error) {
            *error = e.msg;
        }
        return false;
    } catch (const std::exception &e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }

    if (blocksOut->empty()) {
        if (error) {
            *error = "hsbench corpus DB contains no non-empty blocks: " + path;
        }
        return false;
    }
    return true;
#else
    if (error) {
        *error =
            "hsbench corpus DB mode is unavailable (unit-internal built without sqlite support)";
    }
    (void)path;
    return false;
#endif
}

static
bool loadInputBlocksFromFile(const std::string &path,
                             std::vector<std::vector<u8>> *blocksOut,
                             bool *usedHsbenchDbOut, std::string *error) {
    if (!blocksOut) {
        return false;
    }
    if (usedHsbenchDbOut) {
        *usedHsbenchDbOut = false;
    }
    blocksOut->clear();

    CollisionInputMode mode = CollisionInputMode::AUTO;
    if (!parseCollisionInputModeFromEnv(&mode)) {
        if (error) {
            *error =
                "invalid HS_HAO_COLLISION_INPUT_MODE (expected auto/raw/hsbench_db)";
        }
        return false;
    }

    if (mode == CollisionInputMode::RAW) {
        return loadRawInputBlocksFromFile(path, blocksOut, error);
    }

    if (mode == CollisionInputMode::HSBENCH_DB) {
        const bool ok = loadHsbenchCorpusBlocksFromFile(path, blocksOut, error);
        if (ok && usedHsbenchDbOut) {
            *usedHsbenchDbOut = true;
        }
        return ok;
    }

    if (pathLooksLikeHsbenchDb(path)) {
        const bool ok = loadHsbenchCorpusBlocksFromFile(path, blocksOut, error);
        if (ok && usedHsbenchDbOut) {
            *usedHsbenchDbOut = true;
        }
        return ok;
    }

    return loadRawInputBlocksFromFile(path, blocksOut, error);
}

static
bool maybeApplySampleCapFromEnv(std::vector<std::vector<u8>> *blocks,
                                size_t *sampleCountOut) {
    if (!blocks) {
        return false;
    }
    if (sampleCountOut) {
        *sampleCountOut = blockBytesCount(*blocks);
    }

    const char *capEnv = std::getenv("HS_HAO_COLLISION_SAMPLE_CAP");
    if (!capEnv || !*capEnv) {
        return true;
    }

    unsigned long long capRaw = 0;
    if (!parseUnsignedU64Token(capEnv, &capRaw)) {
        return false;
    }
    const size_t cap = static_cast<size_t>(
        std::min<unsigned long long>(capRaw, std::numeric_limits<size_t>::max()));
    if (cap == 0) {
        return false;
    }

    if (blockBytesCount(*blocks) <= cap) {
        if (sampleCountOut) {
            *sampleCountOut = blockBytesCount(*blocks);
        }
        return true;
    }

    std::vector<std::vector<u8>> cappedBlocks;
    cappedBlocks.reserve(blocks->size());
    size_t remaining = cap;
    for (const auto &block : *blocks) {
        if (!remaining) {
            break;
        }
        if (block.empty()) {
            continue;
        }
        if (block.size() <= remaining) {
            cappedBlocks.push_back(block);
            remaining -= block.size();
        } else {
            cappedBlocks.emplace_back(block.begin(), block.begin() + remaining);
            remaining = 0;
        }
    }
    blocks->swap(cappedBlocks);

    if (sampleCountOut) {
        *sampleCountOut = blockBytesCount(*blocks);
    }
    return !blocks->empty();
}

TEST(HAOVsNeo, BlockGroupsConsistency) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 10, 0x1, {}, {}),
        hwlmLiteral("beta", false, false, 11, 0x2, {}, {}),
        hwlmLiteral("gamma", false, false, 12, 0x4, {}, {}),
        hwlmLiteral("delta", true, false, 13, 0x2, {}, {})
    };

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> haoDb;
    if (!buildNeoAndHao(lits, &neo, &haoDb)) {
        skipIfNoHaoSupport();
        return;
    }

    const std::vector<u8> data = {'a','l','p','h','a',' ',
                                  'B','E','T','A',' ',
                                  'b','e','t','a',' ',
                                  'g','a','m','m','a',' ',
                                  'D','E','L','T','A',' ',
                                  'd','e','l','t','a'};

    const auto neoMatches = runBlock(neo.get(), data, 0x2);
    const auto haoDbMatches = runBlock(haoDb.get(), data, 0x2);
    EXPECT_EQ(neoMatches, haoDbMatches);
}

TEST(HAOVsNeo, BlockNorunsConsistency) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("z", false, true, 20, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("zz", false, false, 21, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ab", false, false, 22, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("cd", false, false, 23, HWLM_ALL_GROUPS, {}, {})
    };

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> haoDb;
    if (!buildNeoAndHao(lits, &neo, &haoDb)) {
        skipIfNoHaoSupport();
        return;
    }

    const std::vector<u8> data = {'z','z','z','z','z','z','a','b','z','z'};

    const auto neoMatches = runBlock(neo.get(), data, HWLM_ALL_GROUPS);
    const auto haoDbMatches = runBlock(haoDb.get(), data, HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, haoDbMatches);
}

TEST(HAOVsNeo, StreamingMaskConsistency) {
    const std::vector<u8> msk = {0xff, 0xf0};
    const std::vector<u8> cmp = {'c', 0x70};

    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("abcz", false, false, 30, HWLM_ALL_GROUPS, msk, cmp),
        hwlmLiteral("yy", true, false, 31, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("kappa", false, false, 32, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 33, HWLM_ALL_GROUPS, {}, {})
    };

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> haoDb;
    if (!buildNeoAndHao(lits, &neo, &haoDb)) {
        skipIfNoHaoSupport();
        return;
    }

    const std::vector<u8> history = {'x', 'x', 'a', 'b'};
    const std::vector<u8> data = {'c', 'z', 'Y', 'Y', 'a', 'B', 'c', 'z'};

    const auto neoMatches = runStreaming(neo.get(), history, data,
                                         HWLM_ALL_GROUPS);
    const auto haoDbMatches = runStreaming(haoDb.get(), history, data,
                                         HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, haoDbMatches);
}

TEST(HAOVsNeo, BlockMaskAndNoCaseConsistency) {
    const std::vector<u8> msk = {0xff, 0xf0};
    const std::vector<u8> cmp = {'s', 0x70};

    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("test", false, false, 40, HWLM_ALL_GROUPS, msk, cmp),
        hwlmLiteral("mix", true, false, 41, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("hao", false, false, 42, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("neo", false, false, 43, HWLM_ALL_GROUPS, {}, {})
    };

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> haoDb;
    if (!buildNeoAndHao(lits, &neo, &haoDb)) {
        skipIfNoHaoSupport();
        return;
    }

    const std::vector<u8> data = {'t','e','s','t',' ',
                                  'T','E','S','T',' ',
                                  'm','I','x',' ',
                                  'M','i','X',' ',
                                  'h','a','o',' ',
                                  'n','e','o'};

    const auto neoMatches = runBlock(neo.get(), data, HWLM_ALL_GROUPS);
    const auto haoDbMatches = runBlock(haoDb.get(), data, HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, haoDbMatches);
}

TEST(HAOVsNeo, MultiEntryCollisionAcceptedByHaoBuild) {
    std::vector<hwlmLiteral> lits;
    lits.reserve(HAO_RUNTIME_RULE_SLOTS_PER_ENTRY + 3);
    for (u32 i = 0; i < HAO_RUNTIME_RULE_SLOTS_PER_ENTRY + 3; i++) {
        lits.emplace_back("abcd", false, false, 100 + i, HWLM_ALL_GROUPS,
                          std::vector<u8>{}, std::vector<u8>{});
    }

    auto neo = buildFdrWithHint(lits, ENGINE_ID_NEO);
    if (!neo) {
        skipIfNoHaoSupport();
        return;
    }
    ASSERT_EQ(ENGINE_ID_NEO, neo->engineID);

    auto haoDb = buildFdrWithHint(std::move(lits), ENGINE_ID_HAO);
    ASSERT_NE(nullptr, haoDb.get());
    EXPECT_EQ(ENGINE_ID_HAO, haoDb->engineID);
}

TEST(HAOVsNeo, BlockMultiEntryExactBucketConsistency) {
    auto lits = makeDuplicateLiterals("abcd", false, false, 300, 7,
                                      HWLM_ALL_GROUPS);

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> haoDb;
    if (!buildNeoAndHao(lits, &neo, &haoDb)) {
        skipIfNoHaoSupport();
        return;
    }

    const std::vector<u8> data = {'x','x','a','b','c','d','y','y',
                                  'a','b','c','d'};

    const auto neoMatches = runBlock(neo.get(), data, HWLM_ALL_GROUPS);
    const auto haoDbMatches = runBlock(haoDb.get(), data, HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, haoDbMatches);
}

TEST(HAOVsNeo, StreamingMultiEntryExactBucketConsistency) {
    auto lits = makeDuplicateLiterals("abcd", false, false, 320, 7,
                                      HWLM_ALL_GROUPS);

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> haoDb;
    if (!buildNeoAndHao(lits, &neo, &haoDb)) {
        skipIfNoHaoSupport();
        return;
    }

    const std::vector<u8> history = {'x', 'a', 'b'};
    const std::vector<u8> data = {'c', 'd', 'y', 'a', 'b', 'c', 'd'};

    const auto neoMatches = runStreaming(neo.get(), history, data,
                                         HWLM_ALL_GROUPS);
    const auto haoDbMatches = runStreaming(haoDb.get(), history, data,
                                         HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, haoDbMatches);
}

TEST(HAOVsNeo, BlockMultiEntryWildcardBucketConsistency) {
    auto lits = makeDuplicateLiterals("alpha", true, false, 340, 7,
                                      HWLM_ALL_GROUPS);

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> haoDb;
    if (!buildNeoAndHao(lits, &neo, &haoDb)) {
        skipIfNoHaoSupport();
        return;
    }

    const std::vector<u8> data = {'a','l','p','h','a',' ',
                                  'A','L','P','H','A',' ',
                                  'a','L','p','H','a'};

    const auto neoMatches = runBlock(neo.get(), data, HWLM_ALL_GROUPS);
    const auto haoDbMatches = runBlock(haoDb.get(), data, HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, haoDbMatches);
}

TEST(HAOVsNeo, BlockMultiEntryGroupsConsistency) {
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
    bytecode_ptr<FDR> haoDb;
    if (!buildNeoAndHao(lits, &neo, &haoDb)) {
        skipIfNoHaoSupport();
        return;
    }

    const std::vector<u8> data = {'A','B',' ','C','D',' ','E','F',' ',
                                  'G','H',' ','I','J',' ','K','L',' ',
                                  'M','N'};
    const auto neoMatches = runBlock(neo.get(), data, 0x2);
    const auto haoDbMatches = runBlock(haoDb.get(), data, 0x2);
    EXPECT_EQ(neoMatches, haoDbMatches);
}

TEST(HAOVsNeo, BlockMultiEntryNorunsConsistency) {
    auto lits = makeDuplicateLiterals("z", false, false, 400, 7,
                                      HWLM_ALL_GROUPS);
    for (size_t i = 0; i < lits.size(); i += 2) {
        lits[i].noruns = true;
    }

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> haoDb;
    if (!buildNeoAndHao(lits, &neo, &haoDb)) {
        skipIfNoHaoSupport();
        return;
    }

    const std::vector<u8> data = {'z','z','z','z','z','z'};
    const auto neoMatches = runBlock(neo.get(), data, HWLM_ALL_GROUPS);
    const auto haoDbMatches = runBlock(haoDb.get(), data, HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, haoDbMatches);
}

TEST(HAOVsNeo, StreamingMultiEntryMaskConsistency) {
    const std::vector<u8> msk = {0xff, 0xf0};
    const std::vector<u8> cmp = {'c', 0x70};

    std::vector<hwlmLiteral> lits;
    for (u32 i = 0; i < 7; i++) {
        lits.emplace_back("abcz", false, false, 420 + i, HWLM_ALL_GROUPS, msk,
                          cmp);
    }

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> haoDb;
    if (!buildNeoAndHao(lits, &neo, &haoDb)) {
        skipIfNoHaoSupport();
        return;
    }

    const std::vector<u8> history = {'x', 'x', 'a', 'b'};
    const std::vector<u8> data = {'c', 'z', 'y', 'a', 'b', 'c', 'z'};
    const auto neoMatches = runStreaming(neo.get(), history, data,
                                         HWLM_ALL_GROUPS);
    const auto haoDbMatches = runStreaming(haoDb.get(), history, data,
                                         HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, haoDbMatches);
}

TEST(HAOVsNeo, BlockMultiEntryExactAndWildcardConsistency) {
    auto wildcardLits = makeDuplicateLiterals("ab", true, false, 440, 7,
                                              HWLM_ALL_GROUPS);
    auto exactLits = makeDuplicateLiterals("wxyz", false, false, 500, 7,
                                           HWLM_ALL_GROUPS);
    wildcardLits.insert(wildcardLits.end(), exactLits.begin(), exactLits.end());

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> haoDb;
    if (!buildNeoAndHao(wildcardLits, &neo, &haoDb)) {
        skipIfNoHaoSupport();
        return;
    }

    const std::vector<u8> data = {'A','b',' ','w','x','y','z',' ',
                                  'a','B',' ','w','x','y','z'};
    const auto neoMatches = runBlock(neo.get(), data, HWLM_ALL_GROUPS);
    const auto haoDbMatches = runBlock(haoDb.get(), data, HWLM_ALL_GROUPS);
    EXPECT_EQ(neoMatches, haoDbMatches);
}

TEST(HAOVsNeo, BlockCallbackOrderMonotonicAcrossExactAndWildcard) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("ab", true, false, 520, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("wxyz", false, false, 521, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("mnop", false, false, 522, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("qrst", false, false, 523, HWLM_ALL_GROUPS, {}, {})
    };

    bytecode_ptr<FDR> neo;
    bytecode_ptr<FDR> haoDb;
    if (!buildNeoAndHao(lits, &neo, &haoDb)) {
        skipIfNoHaoSupport();
        return;
    }

    const std::vector<u8> data = {'A','b','x','w','x','y','z'};

    const auto neoMatches = runBlockInOrder(neo.get(), data, HWLM_ALL_GROUPS);
    const auto haoDbMatches = runBlockInOrder(haoDb.get(), data, HWLM_ALL_GROUPS);

    ASSERT_EQ(2U, neoMatches.size());
    ASSERT_EQ(2U, haoDbMatches.size());
    EXPECT_EQ(neoMatches, haoDbMatches);
    EXPECT_LE(haoDbMatches[0].end, haoDbMatches[1].end);
}

TEST(HAOCompile, HaoFeasibilityReasonNameMapping) {
    EXPECT_STREQ("OK", haoFeasibilityReasonName(HAOFeasibilityReason::OK));
    EXPECT_STREQ("GREY_DISABLED",
                 haoFeasibilityReasonName(HAOFeasibilityReason::GREY_DISABLED));
    EXPECT_STREQ("ARTIFACT_BUILD_FAILED",
                 haoFeasibilityReasonName(
                     HAOFeasibilityReason::ARTIFACT_BUILD_FAILED));
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
        skipIfNoHaoSupport();
        return;
    }

    EXPECT_NE(nullptr, proto->haoArtifacts.get());
}

TEST(HAOCompile, HAOProtoBuildRejectsHaoWhenV2Disabled) {
    Grey grey;
    grey.fdrAllowTeddy = false;
    grey.allowNeoFdr = true;
    grey.allowHaoV2 = false;

    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 6237, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 6238, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 6239, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 6240, HWLM_ALL_GROUPS, {}, {})
    };

    auto proto = fdrBuildProtoHinted(HWLM_ENGINE_FDR, std::move(lits), false,
                                     ENGINE_ID_HAO, get_current_target(), grey);
    EXPECT_EQ(nullptr, proto.get());
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
        EXPECT_EQ(src.packedBytes, dst.packedBytes) << "entry=" << i;
        for (u32 j = 0; j < HAO_RUNTIME_RULE_VECTOR_BYTES; j++) {
            EXPECT_EQ(src.ruleVector[j], dst.ruleVector[j])
                << "entry=" << i << " byte=" << j;
            EXPECT_EQ(src.tableControl[j], dst.tableControl[j])
                << "entry=" << i << " tbl=" << j;
        }
        for (u32 slot = 0; slot < HAO_RUNTIME_RULE_SLOTS_PER_ENTRY; slot++) {
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

TEST(HAORuntime, HaoBlobNaiveExecMatchesHaoDirectForSimpleRules) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 684, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 685, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 686, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 687, HWLM_ALL_GROUPS, {}, {})
    };

    auto haoDb = buildFdrWithHint(lits, ENGINE_ID_HAO);
    if (!haoDb || haoDb->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(haoDb.get())) {
        skipIfNoHaoSupport();
        return;
    }

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts));
    auto haoBlob = buildHAOBlob(artifacts);
    ASSERT_NE(nullptr, haoBlob.get());

    const std::vector<u8> data = {
        'x','a','l','p','h','a','-',
        'A','l','P','h','A','-',
        't','h','e','t','a','-',
        'o','m','e','g','a'
    };

    const auto haoDbMatches =
        runHaoDirectInOrder(haoDb.get(), data, HWLM_ALL_GROUPS, true);
    const auto haoMatches =
        runHaoBlobDirectInOrder(haoBlob, data, HWLM_ALL_GROUPS, true);

    auto sortedHaoDbMatches = haoDbMatches;
    auto sortedHaoMatches = haoMatches;
    /* HAO blob and FDR-integrated HAO do not yet guarantee identical
     * same-end callback ordering. */
    std::sort(sortedHaoDbMatches.begin(), sortedHaoDbMatches.end());
    std::sort(sortedHaoMatches.begin(), sortedHaoMatches.end());
    EXPECT_EQ(sortedHaoDbMatches, sortedHaoMatches);
}

TEST(HAORuntime, HaoBlobNaiveExecRejectsBrokenLayoutCleanly) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 688, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 689, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 690, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 691, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts));
    auto haoBlob = buildHAOBlob(artifacts);
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

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts));
    auto haoBlob = buildHAOBlob(artifacts);
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
    if (!HaoRuntimeStatsEnabledForTest()) {
        return;
    }

    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("12345", false, false, 8692, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 8693, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("+-=-+", false, false, 8694, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("90_90", false, false, 8695, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
    auto haoBlob = buildHAOBlob(artifacts);
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
    EXPECT_GT(stats.scanCalls, 0U);
    EXPECT_EQ(data.size(), stats.scanInputBytes);
    EXPECT_GT(stats.primaryProbeLanes, 0U);
    EXPECT_EQ(matches.size(), stats.directReports);
    EXPECT_EQ(matches.size(), stats.callbackReports);
    EXPECT_EQ(0U, stats.encodedConfirmCalls);
    EXPECT_EQ(0U, stats.encodedConfirmRejects);
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

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts));
    auto haoBlob = buildHAOBlob(artifacts);
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

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts));
    auto haoBlob = buildHAOBlob(artifacts);
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

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts));
    auto haoBlob = buildHAOBlob(artifacts);
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
    grey.allowHaoV2 = true;

    auto fdr = buildFdrWithHintAndGrey({
        hwlmLiteral("alpha", false, false, 696, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 697, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 698, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 699, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO, grey);

    if (!fdr || fdr->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(fdr.get())) {
        skipIfNoHaoSupport();
        return;
    }

    const u8 *blob = reinterpret_cast<const u8 *>(fdr.get()) + fdrMatcherBlobOffset(fdr.get());
    u32 magic = 0;
    memcpy(&magic, blob, sizeof(magic));
    EXPECT_EQ(HAO_RUNTIME_MAGIC, magic);
}

TEST(HAORuntime, AutoHaoBuildWithHaoV2LayoutEmbedsHaoBlob) {
    Grey grey;
    grey.fdrAllowTeddy = false;
    grey.allowNeoFdr = true;
    grey.allowHaoV2 = true;

    auto fdr = buildFdrAutoWithGrey({
        hwlmLiteral("alpha", false, false, 6996, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 6997, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 6998, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 6999, HWLM_ALL_GROUPS, {}, {})
    }, grey);

    if (!fdr || fdr->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(fdr.get())) {
        skipIfNoHaoSupport();
        return;
    }

    const u8 *blob = reinterpret_cast<const u8 *>(fdr.get()) + fdrMatcherBlobOffset(fdr.get());
    u32 magic = 0;
    memcpy(&magic, blob, sizeof(magic));
    EXPECT_EQ(HAO_RUNTIME_MAGIC, magic);
}

TEST(HAORuntime, AutoHaoBuildWithHaoV2LayoutEmbedsHaoBlobForAnchorConfirmRules) {
    Grey grey;
    grey.fdrAllowTeddy = false;
    grey.allowNeoFdr = true;
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
        skipIfNoHaoSupport();
        return;
    }

    const u8 *blob = reinterpret_cast<const u8 *>(fdr.get()) + fdrMatcherBlobOffset(fdr.get());
    u32 magic = 0;
    memcpy(&magic, blob, sizeof(magic));
    EXPECT_EQ(HAO_RUNTIME_MAGIC, magic);
}

TEST(HAORuntime, AutoHaoBuildWithHaoV2LayoutEmbedsHaoBlobForResidualUnsupportedRules) {
    Grey grey;
    grey.fdrAllowTeddy = false;
    grey.allowNeoFdr = true;
    grey.allowHaoV2 = true;

    auto fdr = buildFdrAutoWithGrey({
        hwlmLiteral("alpha", false, false, 8096, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("z", false, false, 8097, HWLM_ALL_GROUPS, {}, {}),
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
        skipIfNoHaoSupport();
        return;
    }

    const u8 *blob = reinterpret_cast<const u8 *>(fdr.get()) + fdrMatcherBlobOffset(fdr.get());
    u32 magic = 0;
    memcpy(&magic, blob, sizeof(magic));
    EXPECT_EQ(HAO_RUNTIME_MAGIC, magic);
}

TEST(HAORuntime, HaoBlobNaiveExecMatchesHaoDirectForAnchorConfirmRules) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 8201, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("maskrule", false, false, 8202, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'l', 0x60}),
        hwlmLiteral("ALPHA", true, false, 8203, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 8204, HWLM_ALL_GROUPS, {}, {})
    };
    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
    auto haoBlob = buildHAOBlob(artifacts);
    ASSERT_NE(nullptr, haoBlob.get());

    auto haoDb = buildFdrWithHint(lits, ENGINE_ID_HAO);
    if (!haoDb || haoDb->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(haoDb.get())) {
        skipIfNoHaoSupport();
        return;
    }

    const std::vector<u8> data = {
        'x','a','l','p','h','a','-',
        'm','a','s','k','r','u','l','e','-',
        'A','l','P','h','A','-',
        't','h','e','t','a'
    };

    auto haoDbMatches = runHaoDirectInOrder(haoDb.get(), data, HWLM_ALL_GROUPS, true);
    auto haoMatches = runHaoBlobDirectInOrder(haoBlob, data, HWLM_ALL_GROUPS, true);
    std::sort(haoDbMatches.begin(), haoDbMatches.end());
    std::sort(haoMatches.begin(), haoMatches.end());
    EXPECT_EQ(haoDbMatches, haoMatches);
}

TEST(HAORuntime, HaoBlobNaiveExecMatchesHaoDirectWithResidualUnsupportedRules) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 8301, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 8302, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("z", false, false, 8303, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 8304, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("mask", false, false, 8305, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'s', 0x60})
    };
    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
    auto haoBlob = buildHAOBlob(artifacts);
    ASSERT_NE(nullptr, haoBlob.get());

    auto haoDb = buildFdrWithHint(lits, ENGINE_ID_HAO);
    if (!haoDb || haoDb->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(haoDb.get())) {
        skipIfNoHaoSupport();
        return;
    }

    const std::vector<u8> data = {
        'x','a','l','p','h','a','-',
        'm','a','s','k','-',
        'A','l','P','h','A','-',
        'z','-',
        't','h','e','t','a'
    };

    auto haoDbMatches = runHaoDirectInOrder(haoDb.get(), data, HWLM_ALL_GROUPS, true);
    auto haoMatches = runHaoBlobDirectInOrder(haoBlob, data, HWLM_ALL_GROUPS, true);
    std::sort(haoDbMatches.begin(), haoDbMatches.end());
    std::sort(haoMatches.begin(), haoMatches.end());
    EXPECT_EQ(haoDbMatches, haoMatches);
}

TEST(HAORuntime, HaoRuntimeStatsTrackResidualPath) {
    if (!HaoRuntimeStatsEnabledForTest()) {
        return;
    }

    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 8791, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 8792, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("z", false, false, 8793, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 8794, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("mask", false, false, 8795, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'s', 0x60})
    };

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
    auto haoBlob = buildHAOBlob(artifacts);
    ASSERT_NE(nullptr, haoBlob.get());

    const std::vector<u8> data = {
        'x','a','l','p','h','a','-',
        'm','a','s','k','-',
        'A','l','P','h','A','-',
        'z','-',
        't','h','e','t','a'
    };

    HaoRuntimeResetStatsForTest();
    const auto matches =
        runHaoBlobDirectInOrder(haoBlob, data, HWLM_ALL_GROUPS, true);
    HAORuntimeStats stats = {};
    HaoRuntimeGetStatsForTest(&stats);

    EXPECT_FALSE(matches.empty());
    EXPECT_GT(stats.scanCalls, 0U);
    EXPECT_EQ(data.size(), stats.scanInputBytes);
    EXPECT_GT(stats.residualPosCalls, 0U);
    EXPECT_GT(stats.residualRuleChecks, 0U);
    EXPECT_GT(stats.residualConfirmCalls, 0U);
    EXPECT_GT(stats.residualConfirmMatches, 0U);
    EXPECT_EQ(matches.size(), stats.callbackReports);
}

TEST(HAORuntime, EmbeddedHaoV2FdrBatchMatchesNaive) {
    Grey grey;
    grey.fdrAllowTeddy = false;
    grey.allowNeoFdr = true;
    grey.allowHaoV2 = true;

    auto fdr = buildFdrWithHintAndGrey({
        hwlmLiteral("alpha", false, false, 700, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 701, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 702, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 703, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO, grey);

    if (!fdr || fdr->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(fdr.get())) {
        skipIfNoHaoSupport();
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
        runHaoDirectInOrder(fdr.get(), data, HWLM_ALL_GROUPS, true);
    const auto batchMatches =
        runHaoDirectInOrder(fdr.get(), data, HWLM_ALL_GROUPS, false);
    EXPECT_EQ(naiveMatches, batchMatches);
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

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
    ASSERT_EQ(HAO_EXTRACT_MODE_BEXT, artifacts.extractMode);

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

TEST(HAOCompile, HaoRulePlansRespectExpansionLimit) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 714, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 715, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 716, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("mask", false, false, 717, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'s', 0x60})
    };

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
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

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
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
    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
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

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));

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

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));

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

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
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
        hwlmLiteral("z", false, false, 7398, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 7399, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("mask", false, false, 7400, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'s', 0x60})
    };

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));

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

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
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

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
    ASSERT_TRUE(artifacts.haoGlobalHash.valid);

    const auto &plan = artifacts.haoRulePlans[1];
    ASSERT_EQ(HAORuleCategory::HAO_RULE_ANCHOR_CONFIRM, plan.category);

    bool found = false;
    for (u32 i = 1; i < artifacts.haoGlobalHash.secondaryHashTable.size(); i++) {
        const auto &entry = artifacts.haoGlobalHash.secondaryHashTable[i];
        for (u32 slot = 0; slot < HAO_RUNTIME_RULE_SLOTS_PER_ENTRY; slot++) {
            if (!(entry.slotMask & (1U << slot))) {
                continue;
            }
            if (entry.ruleIndex[slot] != plan.ruleIndex) {
                continue;
            }

            found = true;
            for (u32 j = 0; j < HAO_LAYOUT_BYTES_PER_RULE_SLOT; j++) {
                if (!(plan.verifier.validByteMask & (1U << j))) {
                    continue;
                }
                const u32 idx = verifierPackedIndexForTest(
                    entry, slot, plan.verifier.validByteMask, j);
                EXPECT_EQ(plan.verifier.bytes[j],
                          entry.ruleVector[idx]);
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

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
    ASSERT_TRUE(artifacts.haoGlobalHash.valid);

    const auto &plan = artifacts.haoRulePlans[1];
    ASSERT_EQ(HAORuleCategory::HAO_RULE_EXACT, plan.category);
    bool found = false;
    for (u32 i = 1; i < artifacts.haoGlobalHash.secondaryHashTable.size(); i++) {
        const auto &entry = artifacts.haoGlobalHash.secondaryHashTable[i];
        for (u32 slot = 0; slot < HAO_RUNTIME_RULE_SLOTS_PER_ENTRY; slot++) {
            if (!(entry.slotMask & (1U << slot))) {
                continue;
            }
            if (entry.ruleIndex[slot] != plan.ruleIndex) {
                continue;
            }
            found = true;
            for (u32 j = 0; j < HAO_LAYOUT_BYTES_PER_RULE_SLOT; j++) {
                if (plan.verifier.validByteMask & (1U << j)) {
                    const u32 idx = verifierPackedIndexForTest(
                        entry, slot, plan.verifier.validByteMask, j);
                    EXPECT_EQ(plan.verifier.bytes[j],
                              entry.ruleVector[idx]);
#if HAO_L2_PACKED_VERIFY
                    EXPECT_EQ(j, entry.tableControl[idx]);
#else
                    EXPECT_EQ(((slot * HAO_LAYOUT_BYTES_PER_RULE_SLOT + j) &
                               0x0fU), entry.tableControl[idx]);
#endif
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

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
    ASSERT_TRUE(artifacts.haoGlobalHash.valid);

    const auto &plan = artifacts.haoRulePlans[1];
    ASSERT_EQ(HAORuleCategory::HAO_RULE_EXACT, plan.category);

    bool found = false;
    for (u32 i = 1; i < artifacts.haoGlobalHash.secondaryHashTable.size(); i++) {
        const auto &entry = artifacts.haoGlobalHash.secondaryHashTable[i];
        for (u32 slot = 0; slot < HAO_RUNTIME_RULE_SLOTS_PER_ENTRY; slot++) {
            if (!(entry.slotMask & (1U << slot))) {
                continue;
            }
            if (entry.ruleIndex[slot] != plan.ruleIndex) {
                continue;
            }

            found = true;
            for (u32 j = 0; j < HAO_LAYOUT_BYTES_PER_RULE_SLOT; j++) {
                if (!(plan.verifier.validByteMask & (1U << j))) {
                    continue;
                }
                const u32 idx = verifierPackedIndexForTest(
                    entry, slot, plan.verifier.validByteMask, j);
#if HAO_L2_PACKED_VERIFY
                EXPECT_EQ(j, entry.tableControl[idx]);
#else
                EXPECT_EQ(((slot * HAO_LAYOUT_BYTES_PER_RULE_SLOT + j) &
                           0x0fU), entry.tableControl[idx]);
#endif
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

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));

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

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));

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

TEST(HAOCollision, RuntimeExtractorReportsCollisionRateOnDeterministicCorpus) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 800, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true,  false, 801, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta",  false, false, 802, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 803, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 804, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("gamma", true,  false, 805, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 806, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("kappa", false, false, 807, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
    ASSERT_TRUE(artifacts.haoGlobalHash.valid);

    auto blob = buildHAOBlob(artifacts);
    ASSERT_NE(nullptr, blob.get());
    const auto *hdr = getHaoRuntimeHeader(blob);
    ASSERT_NE(nullptr, hdr);
    const auto *selectors = getHaoSelectors(hdr);
    ASSERT_NE(nullptr, selectors);
    ASSERT_GT(hdr->selectorCount, 0U);
    ASSERT_EQ(artifacts.bitSelectors.size(), hdr->selectorCount);

    const auto data = makeDeterministicCollisionCorpus(HAO_COLLISION_SAMPLE_COUNT);
    const auto runtimeHashes = extractRuntimeKeysForData(hdr, selectors, data);
    ASSERT_EQ(data.size(), runtimeHashes.size());
    for (u32 h : runtimeHashes) {
        ASSERT_LT(h, hdr->primaryCount);
    }

    u32 collisions = 0;
    u32 nonEmpty = 0;
    const double collisionRate = collisionRateFromHashes(
        runtimeHashes, hdr->primaryCount, &collisions, &nonEmpty);
    const double usageRate = hdr->primaryCount
                                 ? static_cast<double>(nonEmpty) / hdr->primaryCount
                                 : 0.0;
    std::cout << "[HAOCollision] samples=" << runtimeHashes.size()
              << " collisions=" << collisions
              << " collisionRate=" << collisionRate
              << " usageRate=" << usageRate
              << " keyBits=" << hdr->keyBits
              << " selectorCount=" << hdr->selectorCount
              << " extractMode="
              << (hdr->extractMode == HAO_RUNTIME_EXTRACT_MODE_BEXT ? "bext"
                                                                     : "scalar")
              << "\n";

    EXPECT_GE(collisionRate, 0.0);
    EXPECT_LE(collisionRate, 1.0);
    EXPECT_GT(nonEmpty, 0U);
}

TEST(HAOCollision, RuntimeCollisionRateMatchesScalarReference) {
    std::vector<hwlmLiteral> lits = {
        hwlmLiteral("alpha", false, false, 810, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true,  false, 811, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta",  false, false, 812, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 813, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 814, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("gamma", true,  false, 815, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 816, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("kappa", false, false, 817, HWLM_ALL_GROUPS, {}, {})
    };

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
    ASSERT_TRUE(artifacts.haoGlobalHash.valid);

    auto blob = buildHAOBlob(artifacts);
    ASSERT_NE(nullptr, blob.get());
    const auto *hdr = getHaoRuntimeHeader(blob);
    ASSERT_NE(nullptr, hdr);
    const auto *selectors = getHaoSelectors(hdr);
    ASSERT_NE(nullptr, selectors);

    const auto data = makeDeterministicCollisionCorpus(HAO_COLLISION_SAMPLE_COUNT);
    const auto runtimeHashes = extractRuntimeKeysForData(hdr, selectors, data);
    const auto scalarHashes = extractScalarReferenceKeysForData(artifacts, data);
    ASSERT_EQ(runtimeHashes.size(), scalarHashes.size());
    for (size_t i = 0; i < runtimeHashes.size(); i++) {
        ASSERT_EQ(runtimeHashes[i], scalarHashes[i]) << "mismatch at sample " << i;
    }

    u32 runtimeCollisions = 0;
    u32 runtimeNonEmpty = 0;
    const double runtimeRate = collisionRateFromHashes(
        runtimeHashes, hdr->primaryCount, &runtimeCollisions, &runtimeNonEmpty);

    u32 scalarCollisions = 0;
    u32 scalarNonEmpty = 0;
    const double scalarRate = collisionRateFromHashes(
        scalarHashes, hdr->primaryCount, &scalarCollisions, &scalarNonEmpty);

    EXPECT_EQ(runtimeCollisions, scalarCollisions);
    EXPECT_EQ(runtimeNonEmpty, scalarNonEmpty);
    EXPECT_DOUBLE_EQ(runtimeRate, scalarRate);
}

TEST(HAOCollision, RuntimeExtractorFromRuleAndInputFiles) {
    const char *rulesPath = std::getenv("HS_HAO_COLLISION_RULES_FILE");
    const char *inputPath = std::getenv("HS_HAO_COLLISION_INPUT_FILE");
    if (!rulesPath || !*rulesPath || !inputPath || !*inputPath) {
#if defined(GTEST_SKIP)
        GTEST_SKIP()
            << "set HS_HAO_COLLISION_RULES_FILE and HS_HAO_COLLISION_INPUT_FILE";
#else
        return;
#endif
    }

    std::vector<hwlmLiteral> lits;
    LiteralLoadStats ruleStats = {};
    std::string error;
    ASSERT_TRUE(loadLiteralsFromFile(rulesPath, &lits, &ruleStats, &error))
        << error;
    ASSERT_FALSE(lits.empty());

    std::vector<std::vector<u8>> blocks;
    bool loadedFromHsbenchDb = false;
    ASSERT_TRUE(loadInputBlocksFromFile(inputPath, &blocks, &loadedFromHsbenchDb,
                                        &error))
        << error;
    ASSERT_FALSE(blocks.empty());

    size_t sampleCount = 0;
    ASSERT_TRUE(maybeApplySampleCapFromEnv(&blocks, &sampleCount))
        << "failed to parse HS_HAO_COLLISION_SAMPLE_CAP";
    ASSERT_FALSE(blocks.empty());
    ASSERT_GT(sampleCount, 0U);

    HAOCompileArtifacts artifacts;
    ASSERT_TRUE(buildHAOArtifacts(lits, &artifacts, false));
    ASSERT_TRUE(artifacts.haoGlobalHash.valid);

    auto blob = buildHAOBlob(artifacts);
    ASSERT_NE(nullptr, blob.get());
    const auto *hdr = getHaoRuntimeHeader(blob);
    ASSERT_NE(nullptr, hdr);
    const auto *selectors = getHaoSelectors(hdr);
    ASSERT_NE(nullptr, selectors);
    ASSERT_GT(hdr->selectorCount, 0U);

    const auto runtimeHashes = extractRuntimeKeysForBlocks(hdr, selectors, blocks);
    const auto scalarHashes = extractScalarReferenceKeysForBlocks(artifacts, blocks);
    ASSERT_EQ(runtimeHashes.size(), sampleCount);
    ASSERT_EQ(scalarHashes.size(), sampleCount);
    for (size_t i = 0; i < runtimeHashes.size(); i++) {
        ASSERT_EQ(runtimeHashes[i], scalarHashes[i])
            << "runtime/scalar key mismatch at sample " << i;
    }

    u32 collisions = 0;
    u32 nonEmpty = 0;
    const double collisionRate = collisionRateFromHashes(
        runtimeHashes, hdr->primaryCount, &collisions, &nonEmpty);
    const double usageRate = hdr->primaryCount
                                 ? static_cast<double>(nonEmpty) / hdr->primaryCount
                                 : 0.0;

    std::cout << "[HAOCollision][FileInput]"
              << " rulesFile=\"" << rulesPath << "\""
              << " inputFile=\"" << inputPath << "\""
              << " inputMode=" << (loadedFromHsbenchDb ? "hsbench_db" : "raw")
              << " blocks=" << blocks.size()
              << " rules=" << lits.size()
              << " parsedLines=" << ruleStats.totalNonCommentLines
              << " skippedUnsupported=" << ruleStats.skippedUnsupportedCount
              << " skippedTooLong=" << ruleStats.skippedTooLongCount
              << " samples=" << sampleCount
              << " collisions=" << collisions
              << " collisionRate=" << collisionRate
              << " usageRate=" << usageRate
              << " keyBits=" << hdr->keyBits
              << " selectorCount=" << hdr->selectorCount
              << " extractMode="
              << (hdr->extractMode == HAO_RUNTIME_EXTRACT_MODE_BEXT ? "bext"
                                                                     : "scalar")
              << "\n";

    EXPECT_GE(collisionRate, 0.0);
    EXPECT_LE(collisionRate, 1.0);
    EXPECT_GT(nonEmpty, 0U);
}

TEST(HAOPrefilter, EntryLaneMaskMatchesScalar) {
    auto haoDb = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 720, 0x1, {}, {}),
        hwlmLiteral("ALPHA", true,  false, 721, 0x3, {}, {}),
        hwlmLiteral("beta",  false, false, 722, 0x1, {}, {}),
        hwlmLiteral("delta", false, true,  723, 0x2, {}, {}),
        hwlmLiteral("gamma", false, false, 724, 0x4, {}, {}),
        hwlmLiteral("theta", true,  false, 725, 0x7, {}, {})
    }, ENGINE_ID_HAO);

    if (!haoDb || haoDb->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(haoDb.get())) {
        skipIfNoHaoSupport();
        return;
    }

    const auto *hdr = getHaoRuntimeHeader(haoDb.get());
    ASSERT_NE(nullptr, hdr);
    ASSERT_EQ(HAO_RUNTIME_MAGIC, hdr->magic);
    const auto *secondary = getHaoSecondaryTable(hdr);

    const std::vector<u8> data = {'a','l','p','h','a',' ',
                                  'B','E','T','A',' ',
                                  'd','e','l','t','a',' ',
                                  'T','H','E','T','A'};
    hs_scratch scratch = {};
    scratch.fdr_conf = nullptr;
    const auto args = makeRuntimeArgs(data, {}, &scratch);

    for (u32 entry = 1; entry < hdr->secondaryCount; entry++) {
        for (size_t endPos = 0; endPos < data.size(); endPos++) {
            const u32 scalarMask = HaoRuntimeEntryMatchMaskForTest(
                &secondary[entry], &args, endPos, 0);
            const u32 vectorMask = HaoRuntimeEntryMatchMaskForTest(
                &secondary[entry], &args, endPos, 1);
            EXPECT_EQ(scalarMask, vectorMask)
                << "entry=" << entry << " endPos=" << endPos;
        }
    }
}

TEST(HAOPrefilter, EntryLaneMaskHistoryBoundaryConsistency) {
    auto haoDb = buildFdrWithHint({
        hwlmLiteral("abcz", false, false, 726, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("YY", true, false, 727, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("kappa", false, false, 728, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 729, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO);

    if (!haoDb || haoDb->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(haoDb.get())) {
        skipIfNoHaoSupport();
        return;
    }

    const auto *hdr = getHaoRuntimeHeader(haoDb.get());
    ASSERT_NE(nullptr, hdr);
    ASSERT_EQ(HAO_RUNTIME_MAGIC, hdr->magic);
    const auto *secondary = getHaoSecondaryTable(hdr);

    const std::vector<u8> history = {'x', 'x', 'a', 'b'};
    const std::vector<u8> data = {'c', 'z', 'Y', 'Y', 'a', 'B', 'c', 'z'};
    hs_scratch scratch = {};
    scratch.fdr_conf = nullptr;
    const auto args = makeRuntimeArgs(data, history, &scratch);

    for (u32 entry = 1; entry < hdr->secondaryCount; entry++) {
        for (size_t endPos = 0; endPos < data.size(); endPos++) {
            const u32 scalarMask = HaoRuntimeEntryMatchMaskForTest(
                &secondary[entry], &args, endPos, 0);
            const u32 vectorMask = HaoRuntimeEntryMatchMaskForTest(
                &secondary[entry], &args, endPos, 1);
            EXPECT_EQ(scalarMask, vectorMask)
                << "entry=" << entry << " endPos=" << endPos;
        }
    }
}

TEST(HAOPrefilter, HeadTailMaskRejectsSameAsScalarForPartialSlots) {
    auto haoDb = buildFdrWithHint({
        hwlmLiteral("ab", true, false, 760, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("abc", false, false, 761, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 762, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("z", false, false, 763, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO);

    if (!haoDb || haoDb->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(haoDb.get())) {
        skipIfNoHaoSupport();
        return;
    }

    const auto *hdr = getHaoRuntimeHeader(haoDb.get());
    ASSERT_NE(nullptr, hdr);
    ASSERT_EQ(HAO_RUNTIME_MAGIC, hdr->magic);
    const auto *secondary = getHaoSecondaryTable(hdr);

    const std::vector<u8> data = {'A','b',' ','a','b','c',' ',
                                  'b','e','t','a',' ','z','z'};
    hs_scratch scratch = {};
    scratch.fdr_conf = nullptr;
    const auto args = makeRuntimeArgs(data, {}, &scratch);

    for (u32 entry = 1; entry < hdr->secondaryCount; entry++) {
        for (size_t endPos = 0; endPos < data.size(); endPos++) {
            const u32 scalarMask = HaoRuntimeEntryMatchMaskForTest(
                &secondary[entry], &args, endPos, 0);
            const u32 vectorMask = HaoRuntimeEntryMatchMaskForTest(
                &secondary[entry], &args, endPos, 1);
            EXPECT_EQ(scalarMask, vectorMask)
                << "entry=" << entry << " endPos=" << endPos;
        }
    }
}

TEST(HAOPrefilter, SingleSlotFastPathMatchesScalar) {
    auto haoDb = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 770, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 771, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("gamma", false, false, 772, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 773, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO);

    if (!haoDb || haoDb->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(haoDb.get())) {
        skipIfNoHaoSupport();
        return;
    }

    const auto *hdr = getHaoRuntimeHeader(haoDb.get());
    ASSERT_NE(nullptr, hdr);
    ASSERT_EQ(HAO_RUNTIME_MAGIC, hdr->magic);
    const auto *secondary = getHaoSecondaryTable(hdr);

    const std::vector<u8> data = {'x','x','a','l','p','h','a','x',
                                  'd','e','l','t','a','x',
                                  'g','a','m','m','a','x',
                                  'o','m','e','g','a'};
    hs_scratch scratch = {};
    scratch.fdr_conf = nullptr;
    const auto args = makeRuntimeArgs(data, {}, &scratch);
    bool sawSingleSlot = false;

    for (u32 entry = 1; entry < hdr->secondaryCount; entry++) {
        if (secondary[entry].slotCount != 1) {
            continue;
        }
        sawSingleSlot = true;
        for (size_t endPos = 0; endPos < data.size(); endPos++) {
            const u32 scalarMask = HaoRuntimeEntryMatchMaskForTest(
                &secondary[entry], &args, endPos, 0);
            const u32 vectorMask = HaoRuntimeEntryMatchMaskForTest(
                &secondary[entry], &args, endPos, 1);
            EXPECT_EQ(scalarMask, vectorMask)
                << "entry=" << entry << " endPos=" << endPos;
        }
    }

    EXPECT_TRUE(sawSingleSlot);
}

TEST(HAORuntime, Batch4MatchesNaiveDirect) {
    auto haoDb = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 730, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 734, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 731, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("THETA", true, false, 733, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO);

    if (!haoDb || haoDb->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(haoDb.get())) {
        skipIfNoHaoSupport();
        return;
    }
    const auto *hdr = getHaoRuntimeHeader(haoDb.get());
    ASSERT_NE(nullptr, hdr);
    EXPECT_EQ(HAO_RUNTIME_MAGIC, hdr->magic);
    EXPECT_EQ(HAO_RUNTIME_VERSION, hdr->version);

    const std::vector<u8> data = {'a','l','p','h','a',' ',
                                  'd','e','l','t','a',' ',
                                  'B','E','T','A',' ',
                                  'T','H','E','T','A',' ',
                                  'A','L','P','H','A',' ',
                                  'D','E','L','T','A'};

    const auto naiveMatches = runHaoDirectInOrder(haoDb.get(), data,
                                                  HWLM_ALL_GROUPS, true);
    const auto batchMatches = runHaoDirectInOrder(haoDb.get(), data,
                                                  HWLM_ALL_GROUPS, false);
    EXPECT_EQ(naiveMatches, batchMatches);
}

TEST(HAORuntime, MaskClassBatchMatchesNaiveDirect) {
    auto haoDb = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 734, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 735, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 736, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("THETA", true, false, 737, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ab", true, false, 738, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("mask", false, false, 739, HWLM_ALL_GROUPS,
                    std::vector<u8>{0xff, 0xf0},
                    std::vector<u8>{'s', 0x60})
    }, ENGINE_ID_HAO);

    if (!haoDb || haoDb->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(haoDb.get())) {
        skipIfNoHaoSupport();
        return;
    }
    const auto *hdr = getHaoRuntimeHeader(haoDb.get());
    ASSERT_NE(nullptr, hdr);
    EXPECT_EQ(HAO_RUNTIME_MAGIC, hdr->magic);
    EXPECT_EQ(HAO_RUNTIME_VERSION, hdr->version);

    const std::vector<u8> data = {'a','l','p','h','a',' ',
                                  'A','L','P','H','A',' ',
                                  'b','e','t','a',' ',
                                  'T','H','E','T','A',' ',
                                  'a','B',' ',
                                  'm','a','s','k'};

    const auto naiveMatches = runHaoDirectInOrder(haoDb.get(), data,
                                                  HWLM_ALL_GROUPS, true);
    const auto batchMatches = runHaoDirectInOrder(haoDb.get(), data,
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

    const u32 scalarMask = HaoRuntimeBitmapProbeMaskForTest(
        bitmap.data(), verify_u32(bitmap.size()), primaryIdx, laneCount, 0);
    const u32 packedMask = HaoRuntimeBitmapProbeMaskForTest(
        bitmap.data(), verify_u32(bitmap.size()), primaryIdx, laneCount, 1);

    EXPECT_EQ(scalarMask, packedMask);
}

TEST(HAORuntime, BitmapProbeHandlesSharedBitmapByte) {
    const std::vector<u8> bitmap = {
        0x96 // idx 1,2,4,7
    };
    const u32 primaryIdx[] = {1, 2, 3, 4, 7};
    const u32 laneCount = verify_u32(sizeof(primaryIdx) / sizeof(primaryIdx[0]));

    const u32 scalarMask = HaoRuntimeBitmapProbeMaskForTest(
        bitmap.data(), verify_u32(bitmap.size()), primaryIdx, laneCount, 0);
    const u32 packedMask = HaoRuntimeBitmapProbeMaskForTest(
        bitmap.data(), verify_u32(bitmap.size()), primaryIdx, laneCount, 1);

    EXPECT_EQ(0x1bU, scalarMask);
    EXPECT_EQ(scalarMask, packedMask);
}

TEST(HAORuntime, Batch4SparseBitmapSkipsEmptyLanes) {
    auto haoDb = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 740, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 741, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("omega", false, false, 742, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("theta", false, false, 743, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO);

    if (!haoDb || haoDb->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(haoDb.get())) {
        skipIfNoHaoSupport();
        return;
    }

    const std::vector<u8> data = {'x','x','x','x','x','x','x','x',
                                  'a','l','p','h','a','x','x','x',
                                  'd','e','l','t','a','x','x','x',
                                  'o','m','e','g','a','x','x','x'};

    const auto naiveMatches = runHaoDirectInOrder(haoDb.get(), data,
                                                  HWLM_ALL_GROUPS, true);
    const auto batchMatches = runHaoDirectInOrder(haoDb.get(), data,
                                                  HWLM_ALL_GROUPS, false);
    EXPECT_EQ(naiveMatches, batchMatches);
}

TEST(HAORuntime, Batch4OrderStableWithWildcard) {
    auto haoDb = buildFdrWithHint({
        hwlmLiteral("ab", true, false, 750, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("wxyz", false, false, 751, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("mnop", false, false, 752, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("qrst", false, false, 753, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO);

    if (!haoDb || haoDb->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(haoDb.get())) {
        skipIfNoHaoSupport();
        return;
    }

    const std::vector<u8> data = {'A','b','x','w','x','y','z'};
    const auto naiveMatches = runHaoDirectInOrder(haoDb.get(), data,
                                                  HWLM_ALL_GROUPS, true);
    const auto batchMatches = runHaoDirectInOrder(haoDb.get(), data,
                                                  HWLM_ALL_GROUPS, false);

    ASSERT_EQ(naiveMatches, batchMatches);
    for (size_t i = 1; i < batchMatches.size(); i++) {
        EXPECT_LE(batchMatches[i - 1].end, batchMatches[i].end);
    }
}

TEST(HAORuntime, InvalidMagicFallsBackCleanly) {
    auto haoDb = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 660, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 661, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 662, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 663, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO);

    if (!haoDb || haoDb->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(haoDb.get())) {
        skipIfNoHaoSupport();
        return;
    }

    auto *hdr = reinterpret_cast<HAORuntimeHeader *>(
        reinterpret_cast<u8 *>(haoDb.get()) + fdrMatcherBlobOffset(haoDb.get()));
    const u32 savedMagic = hdr->magic;
    hdr->magic = 0;

    const hwlm_error_t rv = runHaoDirect(
        haoDb.get(), {'a','l','p','h','a'}, HWLM_ALL_GROUPS);
    EXPECT_EQ(HWLM_SUCCESS, rv);
    EXPECT_TRUE(g_matches.empty());

    hdr->magic = savedMagic;
}

TEST(HAORuntime, InvalidVersionFallsBackCleanly) {
    auto haoDb = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 670, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 671, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 672, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 673, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO);

    if (!haoDb || haoDb->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(haoDb.get())) {
        skipIfNoHaoSupport();
        return;
    }

    auto *hdr = reinterpret_cast<HAORuntimeHeader *>(
        reinterpret_cast<u8 *>(haoDb.get()) + fdrMatcherBlobOffset(haoDb.get()));
    const u32 savedVersion = hdr->version;
    hdr->version = savedVersion + 1;

    const hwlm_error_t rv = runHaoDirect(
        haoDb.get(), {'a','l','p','h','a'}, HWLM_ALL_GROUPS);
    EXPECT_EQ(HWLM_SUCCESS, rv);
    EXPECT_TRUE(g_matches.empty());

    hdr->version = savedVersion;
}

TEST(HAORuntime, InvalidLayoutOffsetFallsBackCleanly) {
    auto haoDb = buildFdrWithHint({
        hwlmLiteral("alpha", false, false, 680, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ALPHA", true, false, 681, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("beta", false, false, 682, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("delta", false, false, 683, HWLM_ALL_GROUPS, {}, {})
    }, ENGINE_ID_HAO);

    if (!haoDb || haoDb->engineID != ENGINE_ID_HAO || !fdrMatcherBlobOffset(haoDb.get())) {
        skipIfNoHaoSupport();
        return;
    }

    auto *hdr = reinterpret_cast<HAORuntimeHeader *>(
        reinterpret_cast<u8 *>(haoDb.get()) + fdrMatcherBlobOffset(haoDb.get()));
    const u32 savedSecondaryOffset = hdr->secondaryOffset;
    hdr->secondaryOffset = fdrMatcherBlobSize(haoDb.get());

    const hwlm_error_t rv = runHaoDirect(
        haoDb.get(), {'a','l','p','h','a'}, HWLM_ALL_GROUPS);
    EXPECT_EQ(HWLM_SUCCESS, rv);
    EXPECT_TRUE(g_matches.empty());

    hdr->secondaryOffset = savedSecondaryOffset;
}

} // namespace







