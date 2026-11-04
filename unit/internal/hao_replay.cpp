/* Copyright (c) 2026, Huawei Technologies Co., Ltd. */

/* Replay a serialized fat database and write canonical callback records. */
#include "database.h"
#include "fat_database.h"
#include "fdr/fdr_internal.h"
#include "fdr/hao_runtime.h"
#include "fdr/hao_runtime_inline.h"
#include "fdr/hao_runtime_layout.h"
#include "hs.h"
#include "hwlm/hwlm_internal.h"
#include "rose/rose_internal.h"
#include "tools/hsbench/data_corpus.h"
#include "util/bytecode_ptr.h"
#include "util/cpuid_flags.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <tuple>
#include <vector>

using Event = std::tuple<unsigned, unsigned, unsigned long long,
                         unsigned long long, unsigned>;
struct Replay {
    unsigned stream = 0;
    std::vector<Event> events;
};

static int collect(unsigned id, unsigned long long from, unsigned long long to,
                   unsigned flags, void *context) {
    auto *replay = static_cast<Replay *>(context);
    replay->events.emplace_back(replay->stream, id, from, to, flags);
    return 0;
}

static bool success(hs_error_t result) {
    if (result == HS_SUCCESS) {
        return true;
    }
    std::cerr << "scan error=" << result << '\n';
    return false;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "usage: hao-replay DATABASE CORPUS RECORDS\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    fat_hs_database_t *fat = nullptr;
    if (!success(
            fat_hs_deserialize_database(bytes.data(), bytes.size(), &fat))) {
        return 1;
    }
    auto db = ue2::make_zeroed_bytecode_ptr<hs_database_t>(
        ROUNDUP_CL(sizeof(hs_database_t)) + fat->arm_length, 64);
    db->magic = fat->magic;
    db->version = fat->version;
    db->length = fat->arm_length;
    db->platform = fat->arm_platform;
    db->crc32 = fat->arm_crc32;
    db->bytecode = ROUNDUP_CL(sizeof(hs_database_t));
    memcpy(reinterpret_cast<u8 *>(db.get()) + db->bytecode,
           reinterpret_cast<const u8 *>(fat) + fat->arm_bytecode,
           fat->arm_length);
    fat_hs_free_database(fat);
    hs_scratch_t *scratch = nullptr;
    if (!success(hs_alloc_scratch(db.get(), &scratch))) {
        return 1;
    }
    const auto *rose =
        static_cast<const RoseEngine *>(hs_get_bytecode(db.get()));
    const u32 offsets[] = {rose->fmatcherOffset, rose->ematcherOffset,
                           rose->drmatcherOffset, rose->sbmatcherOffset};
    const u64a features = cpuid_flags();
    u64a kernels = 0;
    u32 vl = 0;
#ifdef HS_BUILD_HAVE_SVE
    kernels |= HS_CPU_FEATURES_SVE;
    if (features & HS_CPU_FEATURES_SVE) {
        vl = HaoVectorLength();
    }
#endif
#ifdef HS_BUILD_HAVE_SVEBITPERM
    kernels |=
        HS_CPU_FEATURES_SVE | HS_CPU_FEATURES_SVE2 | HS_CPU_FEATURES_SVEBITPERM;
#endif
    for (u32 offset : offsets) {
        if (!offset) {
            continue;
        }
        const auto *hwlm = reinterpret_cast<const HWLM *>(
            reinterpret_cast<const u8 *>(rose) + offset);
        if (hwlm->type != HWLM_ENGINE_FDR) {
            continue;
        }
        const auto *fdr = static_cast<const FDR *>(HWLM_C_DATA(hwlm));
        if (fdr->engineID == HAO_VARIANTS_ENGINE_ID) {
            const auto *v = reinterpret_cast<const HAOVariants *>(
                reinterpret_cast<const u8 *>(fdr) + fdr->matcherBlobOffset);
            const u32 path = haoSelectPath(v, features, kernels, vl);
            std::cout << "paths=FDR" << (v->dotSize ? ",DOT" : "")
                      << (v->bextSize ? ",BEXT" : "") << " selected="
                      << (path == HAO_PATH_BEXT  ? "BEXT"
                          : path == HAO_PATH_DOT ? "DOT"
                                                 : "FDR")
                      << '\n';
        } else if (fdr->engineID == 2U) { // Standalone HAO matcher.
            const HAORuntimeHeader *hdr = nullptr;
            if (!haoValidateLayout(reinterpret_cast<const u8 *>(fdr) +
                                       fdr->matcherBlobOffset,
                                   fdr->matcherBlobSize, &hdr)) {
                std::cerr << "invalid standalone HAO layout\n";
                return 1;
            }
            const bool dot =
                haoRuntimeHeaderHashMode(hdr) == HAO_RUNTIME_HASH_DOT;
            HAOVariants v = {};
            if (dot) {
                v.dotSize = fdr->matcherBlobSize;
            } else {
                v.bextSize = fdr->matcherBlobSize;
            }
            const u32 path = haoSelectPath(&v, features, kernels, vl);
            std::cout << "single=" << (dot ? "DOT" : "BEXT") << " selected="
                      << (path == HAO_PATH_BEXT  ? "BEXT"
                          : path == HAO_PATH_DOT ? "DOT"
                                                 : "FDR")
                      << '\n';
        } else if (fdr->engineID <= 1U) {
            std::cout << "paths=FDR selected=FDR\n";
        }
    }
    const auto corpus = readCorpus(argv[2]);
    Replay replay;
    if (rose->mode == HS_MODE_BLOCK) {
        for (const auto &block : corpus) {
            replay.stream = block.id;
            if (!success(hs_scan(db.get(), block.payload.data(),
                                 block.payload.size(), 0, scratch, collect,
                                 &replay))) {
                return 1;
            }
        }
    } else {
        std::map<unsigned, std::vector<const DataBlock *>> streams;
        for (const auto &block : corpus) {
            streams[block.stream_id].push_back(&block);
        }
        for (const auto &entry : streams) {
            replay.stream = entry.first;
            if (rose->mode == HS_MODE_STREAM) {
                hs_stream_t *stream = nullptr;
                if (!success(hs_open_stream(db.get(), 0, &stream))) {
                    return 1;
                }
                for (const auto *block : entry.second) {
                    if (!success(hs_scan_stream(stream, block->payload.data(),
                                                block->payload.size(), 0,
                                                scratch, collect, &replay))) {
                        return 1;
                    }
                }
                if (!success(
                        hs_close_stream(stream, scratch, collect, &replay))) {
                    return 1;
                }
            } else {
                std::vector<const char *> data;
                std::vector<unsigned> lengths;
                for (const auto *block : entry.second) {
                    data.push_back(block->payload.data());
                    lengths.push_back(block->payload.size());
                }
                if (!success(hs_scan_vector(db.get(), data.data(),
                                            lengths.data(), data.size(), 0,
                                            scratch, collect, &replay))) {
                    return 1;
                }
            }
        }
    }
    hs_free_scratch(scratch);
    std::sort(replay.events.begin(), replay.events.end());
    std::ofstream output(argv[3]);
    for (const auto &event : replay.events) {
        output << std::get<0>(event) << '\t' << std::get<1>(event) << '\t'
               << std::get<2>(event) << '\t' << std::get<3>(event) << '\t'
               << std::get<4>(event) << '\n';
    }
    std::cout << "events=" << replay.events.size() << '\n';
    return output ? 0 : 1;
}
