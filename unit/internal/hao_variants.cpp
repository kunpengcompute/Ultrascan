/* Copyright (c) 2026, Huawei Technologies Co., Ltd. */

#include "config.h"
#include "database.h"
#include "fat_database.h"
#include "fdr/fdr_compile.h"
#include "fdr/fdr_compile_internal.h"
#include "fdr/fdr_engine_description.h"
#include "fdr/fdr_internal.h"
#include "fdr/hao_compile.h"
#include "fdr/hao_runtime_inline.h"
#include "fdr/hao_runtime_layout.h"
#include "grey.h"
#include "hs_compile.h"
#include "hs_internal.h"
#include "hwlm/hwlm_internal.h"
#include "rose/rose_internal.h"
#include "scratch.h"
#include "util/cpuid_flags.h"
#include "util/target_info.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#if defined(__linux__) && defined(__aarch64__) && defined(HS_BUILD_HAVE_SVE)
#include <sys/prctl.h>
#endif

using namespace ue2;

namespace {
const u64a SVE = HS_CPU_FEATURES_SVE;
const u64a BEXT = SVE | HS_CPU_FEATURES_SVE2 | HS_CPU_FEATURES_SVEBITPERM;
std::vector<std::pair<size_t, u32>> matches;
u32 stopAfter = 0;

hwlmcb_rv_t collect(size_t end, u32 id, struct hs_scratch *) {
    matches.emplace_back(end, id);
    if (stopAfter && matches.size() >= stopAfter) {
        return HWLM_TERMINATE_MATCHING;
    }
    return HWLM_CONTINUE_MATCHING;
}

int publicCollect(unsigned id, unsigned long long, unsigned long long end,
                  unsigned, void *) {
    matches.emplace_back(static_cast<size_t>(end), id);
    return 0;
}

std::vector<hwlmLiteral> rules() {
    std::vector<hwlmLiteral> lits;
    const char *hex = "0123456789ABCDEF";
    for (u32 i = 0; i < 1000; i++) {
        std::string s = "rule0000";
        for (u32 j = 0; j < 4; j++) {
            s[7 - j] = hex[(i >> (j * 4U)) & 15U];
        }
        lits.emplace_back(s, false, false, i, HWLM_ALL_GROUPS,
                          std::vector<u8>(), std::vector<u8>());
    }
    return lits;
}

target_t target(u64a features) {
    hs_platform_info_t info = {};
    info.cpu_features = features;
    return target_t(info);
}
} // namespace

TEST(HAOVariants, SelectionRequiresDataKernelAndHost) {
    HAOVariants v = {};
    const u64a hosts[] = {0, SVE, BEXT};
    const u64a kernels[] = {0, SVE, BEXT};
    for (u32 data = 0; data < 4U; data++) {
        v.dotSize = data & 1U;
        v.bextSize = data & 2U;
        for (const u64a host : hosts) {
            for (const u64a kernel : kernels) {
                const u64a available = host & kernel;
                const u32 expected =
                    v.bextSize && (available & BEXT) == BEXT ? HAO_PATH_BEXT
                    : v.dotSize && (available & SVE)         ? HAO_PATH_DOT
                                                             : HAO_PATH_FDR;
                EXPECT_EQ(expected, haoSelectPath(&v, host, kernel, 32));
                EXPECT_EQ(expected, haoSelectPath(&v, host, kernel, 64));
                EXPECT_EQ(HAO_PATH_FDR, haoSelectPath(&v, host, kernel, 16));
            }
        }
    }
    v.dotSize = v.bextSize = 1;
    EXPECT_EQ(HAO_PATH_DOT,
              haoSelectPath(&v, SVE | HS_CPU_FEATURES_SVE2, BEXT, 32));
    EXPECT_EQ(HAO_PATH_FDR, haoSelectPath(nullptr, BEXT, BEXT, 32));
}

TEST(HAOVariants, LayoutAndTableValidation) {
    Grey grey;
    grey.haoHashMode = 2;
    HAOCompileArtifacts dot;
    const std::vector<hwlmLiteral> lits = {
        hwlmLiteral("abcdefgh", false, false, 1, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("ijklmnop", false, false, 2, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("qrstuvwx", false, false, 3, HWLM_ALL_GROUPS, {}, {}),
        hwlmLiteral("yzabcdef", false, false, 4, HWLM_ALL_GROUPS, {}, {})};
    ASSERT_TRUE(
        buildHAOArtifacts(lits, &dot, HAODumpMode::None, nullptr, grey));
    auto blob = buildHAOVariants(&dot, nullptr, 0);
    ASSERT_TRUE(bool(blob));
    const HAOVariants *v = nullptr;
    ASSERT_TRUE(haoValidateVariants(blob.get(), blob.size(), &v));
    auto *hdr = reinterpret_cast<HAORuntimeHeader *>(blob.get() + v->dotOffset);
    ASSERT_TRUE(haoValidateTables(hdr));

    auto *primary = reinterpret_cast<u32 *>(reinterpret_cast<u8 *>(hdr) +
                                            hdr->primaryOffset);
    const u32 saved = primary[0];
    primary[0] = 0xffffffffU;
    EXPECT_FALSE(haoValidateTables(hdr));
    primary[0] = saved;
    auto *meta = reinterpret_cast<HAORuntimeL2Meta *>(
        reinterpret_cast<u8 *>(hdr) + hdr->l2MetaOffset);
    const u32 index = meta[1].ruleIndex[0];
    meta[1].ruleIndex[0] = hdr->ruleMetaCount;
    EXPECT_FALSE(haoValidateTables(hdr));
    meta[1].ruleIndex[0] = index;

    auto *mutableV = reinterpret_cast<HAOVariants *>(blob.get());
    const HAOVariants savedHeader = *mutableV;
    mutableV->dotOffset++;
    EXPECT_FALSE(haoValidateVariants(blob.get(), blob.size(), nullptr));
    *mutableV = savedHeader;
    mutableV->bextOffset = mutableV->dotOffset;
    mutableV->bextSize = mutableV->dotSize;
    EXPECT_FALSE(haoValidateVariants(blob.get(), blob.size(), nullptr));
    *mutableV = savedHeader;
    mutableV->fallback = HAO_VARIANTS_ENGINE_ID;
    EXPECT_FALSE(haoValidateVariants(blob.get(), blob.size(), nullptr));
    *mutableV = savedHeader;
    mutableV->version++;
    EXPECT_FALSE(haoValidateVariants(blob.get(), blob.size(), nullptr));
    EXPECT_FALSE(bool(buildHAOVariants(nullptr, nullptr, 0)));
    EXPECT_FALSE(bool(buildHAOVariants(&dot, nullptr, 2)));
}

TEST(HAOVariants, HostCoverageAndFallbackMatches) {
    const auto lits = rules();
    Grey grey;
    grey.allowHao = true;
    grey.fdrAllowTeddy = false;
    for (const u64a host : {u64a(0), SVE, BEXT}) {
        auto proto = fdrBuildProto(HWLM_ENGINE_FDR, lits, false, target(host),
                                   grey, true);
        ASSERT_TRUE(bool(proto));
        EXPECT_FALSE(bool(proto->haoArtifacts));
#if defined(__aarch64__) && defined(HS_BUILD_HAVE_SVE)
        EXPECT_EQ(bool(host & SVE), bool(proto->dotArtifacts));
#else
        EXPECT_FALSE(bool(proto->dotArtifacts));
#endif
#if defined(__aarch64__) && defined(HS_BUILD_HAVE_SVEBITPERM)
        EXPECT_EQ(bool(host & HS_CPU_FEATURES_SVEBITPERM),
                  bool(proto->bextArtifacts));
#else
        EXPECT_FALSE(bool(proto->bextArtifacts));
#endif
        auto fdr = fdrBuildTable(*proto, grey);
        ASSERT_TRUE(bool(fdr));
        std::string data =
            "prefix-" + lits[0].s + "-" + lits[999].s + "-suffix";
        hs_scratch scratch = {};
        FDR_Runtime_Args args = {};
        args.buf = reinterpret_cast<const u8 *>(data.data());
        args.len = data.size();
        args.cb = collect;
        args.scratch = &scratch;
        // FDR expects the initialized history tail supplied by fdrExec.
        const u8 history[16] = {};
        args.buf_history = history + sizeof(history);
        args.firstFloodDetect = args.buf + args.len;
        matches.clear();
        ASSERT_EQ(HWLM_SUCCESS, FdrEngineExec(fdr.get(), &args, HWLM_ALL_GROUPS,
                                              proto->fdrEng->getID()));
        const auto expected = matches;
        ASSERT_EQ(2U, expected.size());
        if (fdr->engineID != HAO_VARIANTS_ENGINE_ID) {
            continue;
        }
        const HAOVariants *v = nullptr;
        ASSERT_TRUE(haoValidateVariants(
            reinterpret_cast<const u8 *>(fdr.get()) + fdr->matcherBlobOffset,
            fdr->matcherBlobSize, &v));
        matches.clear();
        ASSERT_EQ(HWLM_SUCCESS,
                  HaoVariantsExec(fdr.get(), &args, HWLM_ALL_GROUPS));
        EXPECT_EQ(expected, matches);
        matches.clear();
        stopAfter = 1;
        const auto terminated =
            HaoVariantsExec(fdr.get(), &args, HWLM_ALL_GROUPS);
        stopAfter = 0;
        ASSERT_EQ(HWLM_TERMINATED, terminated);
        ASSERT_EQ(1U, matches.size());
        EXPECT_EQ(expected.front(), matches.front());
#if defined(__aarch64__) && defined(HS_BUILD_HAVE_SVE)
        if (v->dotSize && (cpuid_flags() & SVE) && HaoVectorLength() >= 32) {
            matches.clear();
            const auto *hdr = reinterpret_cast<const HAORuntimeHeader *>(
                reinterpret_cast<const u8 *>(v) + v->dotOffset);
            ASSERT_EQ(HWLM_SUCCESS, HaoDotExec(hdr, &args, HWLM_ALL_GROUPS));
            EXPECT_EQ(expected, matches);
        }
#endif
#if defined(__aarch64__) && defined(HS_BUILD_HAVE_SVEBITPERM)
        if (v->bextSize && (cpuid_flags() & BEXT) == BEXT &&
            HaoVectorLength() >= 32) {
            matches.clear();
            const auto *hdr = reinterpret_cast<const HAORuntimeHeader *>(
                reinterpret_cast<const u8 *>(v) + v->bextOffset);
            ASSERT_EQ(HWLM_SUCCESS, HaoBextExec(hdr, &args, HWLM_ALL_GROUPS));
            EXPECT_EQ(expected, matches);
        }
#endif
    }
}

TEST(HAOVariants, SinglePathModesRemainDistinct) {
    Grey grey;
    grey.allowHao = true;
    grey.fdrAllowTeddy = false;
    for (const u32 mode : {1U, 2U}) {
        SCOPED_TRACE(::testing::Message() << "hashMode=" << mode);
        grey.haoHashMode = mode;
        // Build against a capable target without executing unsupported ISA.
        auto proto = fdrBuildProto(HWLM_ENGINE_FDR, rules(), false,
                                   target(BEXT), grey, false);
        ASSERT_TRUE(bool(proto));
        EXPECT_FALSE(bool(proto->dotArtifacts));
        EXPECT_FALSE(bool(proto->bextArtifacts));
#if defined(__aarch64__) && defined(HS_BUILD_HAVE_SVE)
        const bool available = mode == 2U
#ifdef HS_BUILD_HAVE_SVEBITPERM
                               || mode == 1U
#endif
            ;
        ASSERT_EQ(available, bool(proto->haoArtifacts));
        if (available) {
            EXPECT_EQ(mode == 1U ? HAO_LAYOUT_HASH_BEXT : HAO_LAYOUT_HASH_DOT,
                      proto->haoArtifacts->hashMode);
        }
#else
        EXPECT_FALSE(bool(proto->haoArtifacts));
#endif
    }
}

TEST(HAOVariants, CompileIntentsRoundTripAndPublicModes) {
    const auto lits = rules();
    std::vector<const char *> expressions;
    std::vector<size_t> lengths;
    std::vector<unsigned> ids;
    for (const auto &lit : lits) {
        expressions.push_back(lit.s.c_str());
        lengths.push_back(lit.s.size());
        ids.push_back(lit.id);
    }
    struct RestoreGrey {
        std::string saved = getGreyOverrides();
        ~RestoreGrey() { setGreyOverrides(saved); }
    } restoreGrey;
    ASSERT_EQ(HS_SUCCESS,
              hs_set_grey_overrides(
                  "allowHao:1;allowLily:0;fdrAllowTeddy:0;haoHashMode:2"));
    const std::string data =
        "prefix-" + lits[0].s + "-" + lits[999].s + "-suffix";
    for (const bool literal : {false, true}) {
        for (const bool variants : {false, true}) {
            for (const unsigned mode :
                 {HS_MODE_BLOCK, HS_MODE_STREAM, HS_MODE_VECTORED}) {
                SCOPED_TRACE(::testing::Message()
                             << "literal=" << literal
                             << " variants=" << variants << " mode=" << mode);
                fat_hs_database_t *fat = nullptr;
                hs_compile_error_t *error = nullptr;
                // Exercise the public default and the internal hsbench opt-out
                // for both regex and literal compilation, without changing the
                // container.
                const auto compiled =
                    literal
                        ? (variants
                               ? fat_hs_compile_lit_multi(
                                     expressions.data(), nullptr, ids.data(),
                                     lengths.data(), expressions.size(), mode,
                                     nullptr, &fat, &error)
                               : fat_hs_compile_lit_multi_int(
                                     expressions.data(), nullptr, ids.data(),
                                     nullptr, lengths.data(),
                                     expressions.size(), mode, nullptr, &fat,
                                     &error, Grey(), false))
                        : (variants
                               ? fat_hs_compile_multi(expressions.data(),
                                                      nullptr, ids.data(),
                                                      expressions.size(), mode,
                                                      nullptr, &fat, &error)
                               : fat_hs_compile_multi_int(
                                     expressions.data(), nullptr, ids.data(),
                                     nullptr, expressions.size(), mode, nullptr,
                                     &fat, &error, Grey(), false));
                ASSERT_EQ(HS_SUCCESS, compiled)
                    << (error ? error->message : "");
                char *bytes = nullptr;
                size_t size = 0;
                ASSERT_EQ(HS_SUCCESS,
                          fat_hs_serialize_database(fat, &bytes, &size));
                fat_hs_free_database(fat);
                fat = nullptr;
                const auto decoded =
                    fat_hs_deserialize_database(bytes, size, &fat);
                free(bytes);
                ASSERT_EQ(HS_SUCCESS, decoded);
#if defined(ARCH_AARCH64)
                const u32 length = fat->arm_length;
                const u32 offset = fat->arm_bytecode;
                const u64a platform = fat->arm_platform;
                const u32 crc = fat->arm_crc32;
#else
                const u32 length = fat->x86_length;
                const u32 offset = fat->x86_bytecode;
                const u64a platform = fat->platform;
                const u32 crc = fat->x86_crc32;
#endif
                auto db = make_zeroed_bytecode_ptr<hs_database_t>(
                    ROUNDUP_CL(sizeof(hs_database_t)) + length, 64);
                db->magic = fat->magic;
                db->version = fat->version;
                db->length = length;
                db->platform = platform;
                db->crc32 = crc;
                db->bytecode = ROUNDUP_CL(sizeof(hs_database_t));
                memcpy(reinterpret_cast<u8 *>(db.get()) + db->bytecode,
                       reinterpret_cast<const u8 *>(fat) + offset, length);
                fat_hs_free_database(fat);
#if defined(ARCH_AARCH64) && defined(HS_BUILD_HAVE_SVE)
                if (cpuid_flags() & SVE) {
                    const auto *rose = static_cast<const RoseEngine *>(
                        hs_get_bytecode(db.get()));
                    ASSERT_NE(0U, rose->fmatcherOffset);
                    const auto *hwlm = reinterpret_cast<const HWLM *>(
                        reinterpret_cast<const u8 *>(rose) +
                        rose->fmatcherOffset);
                    ASSERT_EQ(HWLM_ENGINE_FDR, hwlm->type);
                    const auto *fdr =
                        static_cast<const FDR *>(HWLM_C_DATA(hwlm));
                    ASSERT_EQ(variants ? HAO_VARIANTS_ENGINE_ID : HAO_ENGINE_ID,
                              fdr->engineID);
                    if (!variants) {
                        const HAORuntimeHeader *hdr = nullptr;
                        ASSERT_TRUE(haoValidateLayout(
                            reinterpret_cast<const u8 *>(fdr) +
                                fdr->matcherBlobOffset,
                            fdr->matcherBlobSize, &hdr));
                        EXPECT_EQ(HAO_RUNTIME_HASH_DOT,
                                  haoRuntimeHeaderHashMode(hdr));
                    }
                }
#endif
                hs_scratch_t *scratch = nullptr;
                ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db.get(), &scratch));
                matches.clear();
                if (mode == HS_MODE_BLOCK) {
                    EXPECT_EQ(HS_SUCCESS,
                              hs_scan(db.get(), data.data(), data.size(), 0,
                                      scratch, publicCollect, nullptr));
                } else if (mode == HS_MODE_STREAM) {
                    hs_stream_t *stream = nullptr;
                    ASSERT_EQ(HS_SUCCESS, hs_open_stream(db.get(), 0, &stream));
                    // Split inside the first matching literal to exercise
                    // history.
                    EXPECT_EQ(HS_SUCCESS,
                              hs_scan_stream(stream, data.data(), 10, 0,
                                             scratch, publicCollect, nullptr));
                    EXPECT_EQ(HS_SUCCESS,
                              hs_scan_stream(stream, data.data() + 10,
                                             data.size() - 10, 0, scratch,
                                             publicCollect, nullptr));
                    EXPECT_EQ(HS_SUCCESS,
                              hs_close_stream(stream, scratch, publicCollect,
                                              nullptr));
                } else {
                    const char *parts[] = {data.data(), data.data() + 10};
                    const unsigned sizes[] = {
                        10, static_cast<unsigned>(data.size() - 10)};
                    EXPECT_EQ(HS_SUCCESS,
                              hs_scan_vector(db.get(), parts, sizes, 2, 0,
                                             scratch, publicCollect, nullptr));
                }
                hs_free_scratch(scratch);
                ASSERT_EQ(2U, matches.size());
                EXPECT_EQ(0U, matches[0].second);
                EXPECT_EQ(999U, matches[1].second);
            }
        }
    }
}

TEST(HAOVariants, SmallVlUsesFdr) {
#if defined(__linux__) && defined(__aarch64__) && defined(HS_BUILD_HAVE_SVE)
    if (!(cpuid_flags() & SVE)) {
        SUCCEED() << "SVE is unavailable on this host";
        return;
    }
    const int oldVl = prctl(PR_SVE_GET_VL, 0, 0, 0, 0);
    ASSERT_GE(oldVl, 0);
    struct RestoreVl {
        int value;
        ~RestoreVl() { prctl(PR_SVE_SET_VL, value, 0, 0, 0); }
    } restore{oldVl};
    ASSERT_GE(prctl(PR_SVE_SET_VL, 16, 0, 0, 0), 0);
    ASSERT_EQ(16U, HaoVectorLength());
    Grey grey;
    grey.allowHao = true;
    grey.fdrAllowTeddy = false;
    const auto lits = rules();
    auto proto = fdrBuildProto(HWLM_ENGINE_FDR, lits, false,
                               ue2::get_current_target(), grey, true);
    ASSERT_TRUE(bool(proto));
    ASSERT_TRUE(bool(proto->dotArtifacts));
    auto fdr = fdrBuildTable(*proto, grey);
    ASSERT_TRUE(bool(fdr));
    ASSERT_EQ(HAO_VARIANTS_ENGINE_ID, fdr->engineID);
    const std::string data = "prefix-" + lits[0].s + "-" + lits[999].s;
    const u8 history[16] = {};
    hs_scratch scratch = {};
    FDR_Runtime_Args args = {};
    args.buf = reinterpret_cast<const u8 *>(data.data());
    args.len = data.size();
    args.buf_history = history + sizeof(history);
    args.firstFloodDetect = args.buf + args.len;
    args.cb = collect;
    args.scratch = &scratch;
    matches.clear();
    ASSERT_EQ(HWLM_SUCCESS, FdrEngineExec(fdr.get(), &args, HWLM_ALL_GROUPS,
                                          proto->fdrEng->getID()));
    const auto expected = matches;
    ASSERT_EQ(2U, expected.size());
    matches.clear();
    ASSERT_EQ(HWLM_SUCCESS, HaoVariantsExec(fdr.get(), &args, HWLM_ALL_GROUPS));
    EXPECT_EQ(expected, matches);
    // hsbench's standalone DOT path must also fall back at 128-bit VL.
    grey.haoHashMode = 2;
    auto single = fdrBuildProto(HWLM_ENGINE_FDR, lits, false,
                                ue2::get_current_target(), grey, false);
    ASSERT_TRUE(bool(single));
    ASSERT_TRUE(bool(single->haoArtifacts));
    auto singleFdr = fdrBuildTable(*single, grey);
    ASSERT_TRUE(bool(singleFdr));
    ASSERT_EQ(HAO_ENGINE_ID, singleFdr->engineID);
    matches.clear();
    ASSERT_EQ(HWLM_SUCCESS,
              HaoEngineExec(singleFdr.get(), &args, HWLM_ALL_GROUPS));
    EXPECT_EQ(expected, matches);
#else
    SUCCEED() << "SVE VL control is unavailable in this build";
#endif
}
