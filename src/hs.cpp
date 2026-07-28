/*
 * Copyright (c) 2015-2021, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/** \file
 * \brief Compiler front-end, including public API calls for compilation.
 */
#include "allocator.h"
#include "compiler/compiler.h"
#include "compiler/error.h"
#include "database.h"
#include "fat_database.h"
#include "fp_collector.h"
#include "grey.h"
#include "hs_compile.h"
#include "hs_internal.h"
#include "nfagraph/ng.h"
#include "nfagraph/ng_expr_info.h"
#include "parser/Parser.h"
#include "parser/parse_error.h"
#include "parser/prefilter.h"
#include "parser/shortcut_literal.h"
#include "parser/unsupported.h"
#include "ue2common.h"
#include "util/compile_error.h"
#include "util/cpuid_flags.h"
#include "util/cpuid_inline.h"
#include "util/depth.h"
#include "util/popcount.h"
#include "util/target_info.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <limits.h>
#include <string>
#include <vector>

using namespace std;
using namespace ue2;

struct hs_compile_context {
    hs_fp_feedback_t *fp_feedback;
    u32 fp_observe_checked_count;
    u32 fp_observe_hit_count;
    hs_compile_context_checkpoint_info_t
        fp_checkpoint_info[HS_FP_COMPILE_CHECKPOINT_COUNT];
};

static void resetCompileContextObserve(const hs_compile_context_t *ctx) {
    if (!ctx) {
        return;
    }

    hs_compile_context_t *mutable_ctx = const_cast<hs_compile_context_t *>(ctx);
    mutable_ctx->fp_observe_checked_count = 0;
    mutable_ctx->fp_observe_hit_count = 0;
}

static void resetCompileContextDiagnostics(const hs_compile_context_t *ctx) {
    resetCompileContextObserve(ctx);
    if (!ctx) {
        return;
    }

    hs_compile_context_t *mutable_ctx = const_cast<hs_compile_context_t *>(ctx);
    memset(mutable_ctx->fp_checkpoint_info, 0,
           sizeof(mutable_ctx->fp_checkpoint_info));
}

static void observeCompileFeedback(const hs_compile_context_t *ctx,
                                   const hs_database_t *db) {
    resetCompileContextObserve(ctx);
    if (!ctx || !ctx->fp_feedback || !db) {
        return;
    }

    const struct RoseEngine *rose =
        (const struct RoseEngine *)hs_get_bytecode(db);
    u32 checked_count = 0;
    u32 hit_count = hs_fp_feedback_count_matches_in_rose(ctx->fp_feedback, rose,
                                                         &checked_count);

    hs_compile_context_t *mutable_ctx = const_cast<hs_compile_context_t *>(ctx);
    mutable_ctx->fp_observe_checked_count = checked_count;
    mutable_ctx->fp_observe_hit_count = hit_count;
}

/** \brief Cheap check that no unexpected mode flags are on. */
static bool validModeFlags(unsigned int mode) {
    static const unsigned allModeFlags =
        HS_MODE_BLOCK | HS_MODE_STREAM | HS_MODE_VECTORED |
        HS_MODE_SOM_HORIZON_LARGE | HS_MODE_SOM_HORIZON_MEDIUM |
        HS_MODE_SOM_HORIZON_SMALL;

    return !(mode & ~allModeFlags);
}

/** \brief Validate mode flags. */
static bool checkMode(unsigned int mode, hs_compile_error **comp_error) {
    // First, check that only bits with meaning are on.
    if (!validModeFlags(mode)) {
        *comp_error = generateCompileError("Invalid parameter: "
                                           "unrecognised mode flags.",
                                           -1);
        return false;
    }

    // Our mode must be ONE of (block, streaming, vectored).
    unsigned checkmode =
        mode & (HS_MODE_STREAM | HS_MODE_BLOCK | HS_MODE_VECTORED);
    if (popcount32(checkmode) != 1) {
        *comp_error = generateCompileError(
            "Invalid parameter: mode must have one "
            "(and only one) of HS_MODE_BLOCK, HS_MODE_STREAM or "
            "HS_MODE_VECTORED set.",
            -1);
        return false;
    }

    // If you specify SOM precision, you must be in streaming mode and you only
    // get to have one.
    unsigned somMode =
        mode & (HS_MODE_SOM_HORIZON_LARGE | HS_MODE_SOM_HORIZON_MEDIUM |
                HS_MODE_SOM_HORIZON_SMALL);
    if (somMode) {
        if (!(mode & HS_MODE_STREAM)) {
            *comp_error = generateCompileError(
                "Invalid parameter: the "
                "HS_MODE_SOM_HORIZON_ mode flags may only be set in "
                "streaming mode.",
                -1);
            return false;
        }
        if ((somMode & (somMode - 1)) != 0) {
            *comp_error = generateCompileError(
                "Invalid parameter: only one "
                "HS_MODE_SOM_HORIZON_ mode flag can be set.",
                -1);
            return false;
        }
    }

    return true;
}

static bool checkPlatform(const hs_platform_info *p,
                          hs_compile_error **comp_error) {
    static constexpr u32 HS_TUNE_LAST = HS_TUNE_FAMILY_ICX;
    static constexpr u32 HS_CPU_FEATURES_ALL =
        HS_CPU_FEATURES_AVX2 | HS_CPU_FEATURES_AVX512 |
        HS_CPU_FEATURES_AVX512VBMI | HS_CPU_FEATURES_SVE |
        HS_CPU_FEATURES_SVE2 | HS_CPU_FEATURES_SVEBITPERM;

    if (!p) {
        return true;
    }

    if (p->cpu_features & ~HS_CPU_FEATURES_ALL) {
        *comp_error = generateCompileError("Invalid cpu features specified in "
                                           "the platform information.",
                                           -1);
        return false;
    }

    if ((p->cpu_features & HS_CPU_FEATURES_SVE2) &&
        !(p->cpu_features & HS_CPU_FEATURES_SVE)) {
        *comp_error = generateCompileError("SVE2 requires SVE in the platform "
                                           "information.",
                                           -1);
        return false;
    }

    if ((p->cpu_features & HS_CPU_FEATURES_SVEBITPERM) &&
        !(p->cpu_features & HS_CPU_FEATURES_SVE2)) {
        *comp_error = generateCompileError("SVEBITPERM requires SVE2 in the "
                                           "platform information.",
                                           -1);
        return false;
    }

    if (p->tune > HS_TUNE_LAST) {
        *comp_error = generateCompileError("Invalid tuning value specified in "
                                           "the platform information.",
                                           -1);
        return false;
    }

    return true;
}

static u64a currentArmCpuFeatures(void) {
    return cpuid_flags() & (HS_CPU_FEATURES_SVE | HS_CPU_FEATURES_SVE2 |
                            HS_CPU_FEATURES_SVEBITPERM);
}

/** \brief Convert from SOM mode to bytes of precision. */
static unsigned getSomPrecision(unsigned mode) {
    if (mode & HS_MODE_VECTORED) {
        /* always assume full precision for vectoring */
        return 8;
    }

    if (mode & HS_MODE_SOM_HORIZON_LARGE) {
        return 8;
    } else if (mode & HS_MODE_SOM_HORIZON_MEDIUM) {
        return 4;
    } else if (mode & HS_MODE_SOM_HORIZON_SMALL) {
        return 2;
    }
    return 0;
}

namespace ue2 {
static hs_error_t validate_fat_compile_args(fat_hs_database_t **db,
                                            hs_compile_error_t **comp_error,
                                            const char *const *expressions,
                                            unsigned elements, const Grey &g) {
    if (!comp_error) {
        if (db) {
            *db = nullptr;
        }
        return HS_COMPILER_ERROR;
    }
    if (!db) {
        *comp_error = generateCompileError("Invalid parameter: db is NULL", -1);
        return HS_COMPILER_ERROR;
    }
    if (!expressions) {
        *db = nullptr;
        *comp_error =
            generateCompileError("Invalid parameter: expressions is NULL", -1);
        return HS_COMPILER_ERROR;
    }
    if (elements == 0) {
        *db = nullptr;
        *comp_error =
            generateCompileError("Invalid parameter: elements is zero", -1);
        return HS_COMPILER_ERROR;
    }

#if defined(FAT_RUNTIME)
    if (!check_ssse3()) {
        *db = nullptr;
        *comp_error = generateCompileError("Unsupported architecture", -1);
        return HS_ARCH_ERROR;
    }
#endif

    if (elements > g.limitPatternCount) {
        *db = nullptr;
        *comp_error = generateCompileError("Number of patterns too large", -1);
        return HS_COMPILER_ERROR;
    }

    return HS_SUCCESS;
}

hs_error_t fat_hs_compile_multi_int(
    const char *const *expressions, const unsigned *flags, const unsigned *ids,
    const hs_expr_ext *const *ext, unsigned elements, unsigned mode,
    const hs_platform_info_t *platform, fat_hs_database_t **db,
    hs_compile_error_t **comp_error, const Grey &g) {
    // Check the args: note that it's OK for flags, ids or ext to be null.
    hs_error_t err =
        validate_fat_compile_args(db, comp_error, expressions, elements, g);
    if (err != HS_SUCCESS) {
        return err;
    }

    if (!checkMode(mode, comp_error)) {
        *db = nullptr;
        assert(*comp_error); // set by checkMode.
        return HS_COMPILER_ERROR;
    }

    if (!checkPlatform(platform, comp_error)) {
        *db = nullptr;
        assert(*comp_error); // set by checkPlatform.
        return HS_COMPILER_ERROR;
    }

    if (elements > g.limitPatternCount) {
        *db = nullptr;
        *comp_error = generateCompileError("Number of patterns too large", -1);
        return HS_COMPILER_ERROR;
    }

    // This function is simply a wrapper around both the parser and compiler
    bool isStreaming = mode & (HS_MODE_STREAM | HS_MODE_VECTORED);
    bool isVectored = mode & HS_MODE_VECTORED;
    unsigned somPrecision = getSomPrecision(mode);

    target_t target_info =
        platform ? target_t(*platform) : get_current_target();
    u32 count_2_4_byte_literals = 0;
    for (unsigned int i = 0; i < elements; i++) {
        try {
            // Use ParsedExpression constructor directly
            ParsedExpression pe(i, expressions[i], flags ? flags[i] : 0, 0,
                                ext ? ext[i] : nullptr);
            // Check if it's a literal using the same logic as shortcut_literal
            if (isShortLiteral(pe) > 0) {
                count_2_4_byte_literals++;
            }
        } catch (const ParseError &) {
            continue; // Skip invalid expressions, they'll be caught later
        } catch (const CompileError &) {
            continue; // Skip compilation errors, they'll be caught later
        }
    }

    try {
        // =======1. 编译 x86 字节码===============
        Grey x86_grey = g;
        x86_grey.allowLily = false;
        x86_grey.allowNeoFdr = false;

        CompileContext x86_cc(isStreaming, isVectored, target_info, x86_grey);
        NG x86_ng(x86_cc, elements, somPrecision);
        x86_ng.allowLilyForTeddy = false;

        // First pass: process all HS_FLAG_COMBINATION rules to populate
        // toLogicalKeyMap
        for (unsigned int i = 0; i < elements; i++) {
            if (flags && (flags[i] & HS_FLAG_COMBINATION)) {
                try {
                    x86_addExpression(x86_ng, i, expressions[i], flags[i],
                                      ext ? ext[i] : nullptr, ids ? ids[i] : 0);
                } catch (CompileError &e) {
                    /* Caught a parse error:
                     * throw it upstream as a CompileError with a specific index
                     */
                    e.setExpressionIndex(i);
                    throw; /* do not slice */
                }
            }
        }

        // Second pass: process all non-HS_FLAG_COMBINATION rules
        for (unsigned int i = 0; i < elements; i++) {
            if (!flags || !(flags[i] & HS_FLAG_COMBINATION)) {
                try {
                    x86_addExpression(x86_ng, i, expressions[i],
                                      flags ? flags[i] : 0,
                                      ext ? ext[i] : nullptr, ids ? ids[i] : 0);
                } catch (CompileError &e) {
                    /* Caught a parse error:
                     * throw it upstream as a CompileError with a specific index
                     */
                    e.setExpressionIndex(i);
                    throw; /* do not slice */
                }
            }
        }

        // Check sub-expression ids
        x86_ng.rm.pl.validateSubIDs(ids, expressions, flags, elements);
        // Renumber and assign lkey to reports
        x86_ng.rm.logicalKeyRenumber();

        //===== 编译arm字节码 根据config.txt获取===================
        Grey arm_grey = g;
        // 为ARM创建单独的target_info，不包含AVX2/AVX512特性
        hs_platform_info arm_platform;
        arm_platform.tune = HS_TUNE_FAMILY_GENERIC;
        arm_platform.cpu_features =
            currentArmCpuFeatures(); // 不包含AVX2，这样chooseTeddyEngine会选择ID
                                     // 11-18
        target_t arm_target_info(arm_platform);

        CompileContext arm_cc(isStreaming, isVectored, arm_target_info,
                              arm_grey);
        NG arm_ng(arm_cc, elements, somPrecision);

        if (count_2_4_byte_literals > 8) {
            DEBUG_PRINTF(
                "More than 8 2-4 rules exist, will not start lilyForTeddy\n");
            arm_ng.allowLilyForTeddy = false;
        }

        // First pass: process all HS_FLAG_COMBINATION rules to populate
        // toLogicalKeyMap
        for (unsigned int i = 0; i < elements; i++) {
            if (flags && (flags[i] & HS_FLAG_COMBINATION)) {
                try {
                    addExpression(arm_ng, i, expressions[i], flags[i],
                                  ext ? ext[i] : nullptr, ids ? ids[i] : 0);
                } catch (CompileError &e) {
                    /* Caught a parse error:
                     * throw it upstream as a CompileError with a specific index
                     */
                    e.setExpressionIndex(i);
                    throw; /* do not slice */
                }
            }
        }

        // Second pass: process all non-HS_FLAG_COMBINATION rules
        for (unsigned int i = 0; i < elements; i++) {
            if (!flags || !(flags[i] & HS_FLAG_COMBINATION)) {
                try {
                    addExpression(arm_ng, i, expressions[i],
                                  flags ? flags[i] : 0, ext ? ext[i] : nullptr,
                                  ids ? ids[i] : 0);
                } catch (CompileError &e) {
                    /* Caught a parse error:
                     * throw it upstream as a CompileError with a specific index
                     */
                    e.setExpressionIndex(i);
                    throw; /* do not slice */
                }
            }
        }
        // Check sub-expression ids
        arm_ng.rm.pl.validateSubIDs(ids, expressions, flags, elements);
        // Renumber and assign lkey to reports
        arm_ng.rm.logicalKeyRenumber();

        // ===== 构建 FAT Database========
        unsigned length = 0;
        struct fat_hs_database *out = fat_build(x86_ng, arm_ng, &length, 0);
        assert(out);
        assert(length);
        *db = out;
        *comp_error = nullptr;

        return HS_SUCCESS;
    } catch (const CompileError &e) {
        // Compiler error occurred
        *db = nullptr;
        *comp_error =
            generateCompileError(e.reason, e.hasIndex ? (int)e.index : -1);
        return HS_COMPILER_ERROR;
    } catch (const std::bad_alloc &) {
        *db = nullptr;
        *comp_error = const_cast<hs_compile_error_t *>(&hs_enomem);
        return HS_COMPILER_ERROR;
    } catch (...) {
        assert(!"Internal error, unexpected exception");
        *db = nullptr;
        *comp_error = const_cast<hs_compile_error_t *>(&hs_einternal);
        return HS_COMPILER_ERROR;
    }
}

hs_error_t fat_hs_compile_lit_multi_int(
    const char *const *expressions, const unsigned *flags, const unsigned *ids,
    const hs_expr_ext *const *ext, const size_t *lens, unsigned elements,
    unsigned mode, const hs_platform_info_t *platform, fat_hs_database_t **db,
    hs_compile_error_t **comp_error, const Grey &g) {
    // Check the args: note that it's OK for flags, ids or ext to be null.
    hs_error_t err =
        validate_fat_compile_args(db, comp_error, expressions, elements, g);
    if (err != HS_SUCCESS) {
        return err;
    }

    if (!checkMode(mode, comp_error)) {
        *db = nullptr;
        assert(*comp_error);
        return HS_COMPILER_ERROR;
    }

    if (!checkPlatform(platform, comp_error)) {
        *db = nullptr;
        assert(*comp_error);
        return HS_COMPILER_ERROR;
    }

    if (elements > g.limitPatternCount) {
        *db = nullptr;
        *comp_error = generateCompileError("Number of patterns too large", -1);
        return HS_COMPILER_ERROR;
    }

    bool isStreaming = mode & (HS_MODE_STREAM | HS_MODE_VECTORED);
    bool isVectored = mode & HS_MODE_VECTORED;
    unsigned somPrecision = getSomPrecision(mode);

    target_t target_info =
        platform ? target_t(*platform) : get_current_target();

    try {
        // =======1. 编译 x86 字节码===============
        Grey x86_grey = g;
        x86_grey.allowLily = false;
        x86_grey.allowNeoFdr = false;

        CompileContext x86_cc(isStreaming, isVectored, target_info, x86_grey);
        NG x86_ng(x86_cc, elements, somPrecision);
        x86_ng.allowLilyForTeddy = false;

        for (unsigned int i = 0; i < elements; i++) {
            try {
                addLitExpression(x86_ng, i, expressions[i],
                                 flags ? flags[i] : 0, ext ? ext[i] : nullptr,
                                 ids ? ids[i] : 0, lens[i]);
            } catch (CompileError &e) {
                e.setExpressionIndex(i);
                throw;
            }
        }

        x86_ng.rm.pl.validateSubIDs(ids, expressions, flags, elements);
        x86_ng.rm.logicalKeyRenumber();

        // =======2. 编译 arm 字节码===============
        Grey arm_grey = g;
        // 为ARM创建单独的target_info，不包含AVX2/AVX512特性
        hs_platform_info arm_platform;
        arm_platform.tune = HS_TUNE_FAMILY_GENERIC;
        arm_platform.cpu_features =
            currentArmCpuFeatures(); // 不包含AVX2，这样chooseTeddyEngine会选择ID
                                     // 11-18
        target_t arm_target_info(arm_platform);

        CompileContext arm_cc(isStreaming, isVectored, arm_target_info,
                              arm_grey);
        NG arm_ng(arm_cc, elements, somPrecision);

        for (unsigned int i = 0; i < elements; i++) {
            try {
                addLitExpression(arm_ng, i, expressions[i],
                                 flags ? flags[i] : 0, ext ? ext[i] : nullptr,
                                 ids ? ids[i] : 0, lens[i]);
            } catch (CompileError &e) {
                e.setExpressionIndex(i);
                throw;
            }
        }

        arm_ng.rm.pl.validateSubIDs(ids, expressions, flags, elements);
        arm_ng.rm.logicalKeyRenumber();

        // =======3. 构建 FAT Database========
        unsigned length = 0;
        struct fat_hs_database *out = fat_build(x86_ng, arm_ng, &length, 0);
        assert(out);
        assert(length);
        *db = out;
        *comp_error = nullptr;

        return HS_SUCCESS;
    } catch (const CompileError &e) {
        *db = nullptr;
        *comp_error =
            generateCompileError(e.reason, e.hasIndex ? (int)e.index : -1);
        return HS_COMPILER_ERROR;
    } catch (const std::bad_alloc &) {
        *db = nullptr;
        *comp_error = const_cast<hs_compile_error_t *>(&hs_enomem);
        return HS_COMPILER_ERROR;
    } catch (...) {
        assert(!"Internal error, unexpected exception");
        *db = nullptr;
        *comp_error = const_cast<hs_compile_error_t *>(&hs_einternal);
        return HS_COMPILER_ERROR;
    }
}

hs_error_t hs_compile_multi_int(const char *const *expressions,
                                const unsigned *flags, const unsigned *ids,
                                const hs_expr_ext *const *ext,
                                unsigned elements, unsigned mode,
                                const hs_platform_info_t *platform,
                                hs_database_t **db,
                                hs_compile_error_t **comp_error, const Grey &g,
                                const hs_compile_context_t *fp_ctx) {
    resetCompileContextDiagnostics(fp_ctx);

    // Check the args: note that it's OK for flags, ids or ext to be null.
    if (!comp_error) {
        if (db) {
            *db = nullptr;
        }
        // nowhere to write the string, but we can still report an error code
        return HS_COMPILER_ERROR;
    }
    if (!db) {
        *comp_error = generateCompileError("Invalid parameter: db is NULL", -1);
        return HS_COMPILER_ERROR;
    }
    if (!expressions) {
        *db = nullptr;
        *comp_error =
            generateCompileError("Invalid parameter: expressions is NULL", -1);
        return HS_COMPILER_ERROR;
    }
    if (elements == 0) {
        *db = nullptr;
        *comp_error =
            generateCompileError("Invalid parameter: elements is zero", -1);
        return HS_COMPILER_ERROR;
    }

#if defined(FAT_RUNTIME)
    if (!check_ssse3()) {
        *db = nullptr;
        *comp_error = generateCompileError("Unsupported architecture", -1);
        return HS_ARCH_ERROR;
    }
#endif

    if (!checkMode(mode, comp_error)) {
        *db = nullptr;
        assert(*comp_error); // set by checkMode.
        return HS_COMPILER_ERROR;
    }

    if (!checkPlatform(platform, comp_error)) {
        *db = nullptr;
        assert(*comp_error); // set by checkPlatform.
        return HS_COMPILER_ERROR;
    }

    if (elements > g.limitPatternCount) {
        *db = nullptr;
        *comp_error = generateCompileError("Number of patterns too large", -1);
        return HS_COMPILER_ERROR;
    }

    // This function is simply a wrapper around both the parser and compiler
    bool isStreaming = mode & (HS_MODE_STREAM | HS_MODE_VECTORED);
    bool isVectored = mode & HS_MODE_VECTORED;
    unsigned somPrecision = getSomPrecision(mode);

    target_t target_info =
        platform ? target_t(*platform) : get_current_target();
    u32 count_2_4_byte_literals = 0;
    for (unsigned int i = 0; i < elements; i++) {
        try {
            // Use ParsedExpression constructor directly
            ParsedExpression pe(i, expressions[i], flags ? flags[i] : 0, 0,
                                ext ? ext[i] : nullptr);
            // Check if it's a literal using the same logic as shortcut_literal
            if (isShortLiteral(pe) > 0) {
                count_2_4_byte_literals++;
            }
        } catch (const ParseError &) {
            continue; // Skip invalid expressions, they'll be caught later
        } catch (const CompileError &) {
            continue; // Skip compilation errors, they'll be caught later
        }
    }

    try {
        hs_compile_context_t *mutable_fp_ctx =
            const_cast<hs_compile_context_t *>(fp_ctx);
        CompileContext cc(isStreaming, isVectored, target_info, g,
                          fp_ctx ? fp_ctx->fp_feedback : nullptr,
                          mutable_fp_ctx ? mutable_fp_ctx->fp_checkpoint_info
                                         : nullptr);
        NG ng(cc, elements, somPrecision);

        if (count_2_4_byte_literals > 8) {
            DEBUG_PRINTF(
                "More than 8 2-4 rules exist, will not start lilyForTeddy\n");
            ng.allowLilyForTeddy = false;
        }

        // First pass: process all HS_FLAG_COMBINATION rules to populate
        // toLogicalKeyMap
        for (unsigned int i = 0; i < elements; i++) {
            if (flags && (flags[i] & HS_FLAG_COMBINATION)) {
                try {
                    addExpression(ng, i, expressions[i], flags[i],
                                  ext ? ext[i] : nullptr, ids ? ids[i] : 0);
                } catch (CompileError &e) {
                    /* Caught a parse error:
                     * throw it upstream as a CompileError with a specific index
                     */
                    e.setExpressionIndex(i);
                    throw; /* do not slice */
                }
            }
        }

        // Second pass: process all non-HS_FLAG_COMBINATION rules
        for (unsigned int i = 0; i < elements; i++) {
            if (!flags || !(flags[i] & HS_FLAG_COMBINATION)) {
                try {
                    addExpression(ng, i, expressions[i], flags ? flags[i] : 0,
                                  ext ? ext[i] : nullptr, ids ? ids[i] : 0);
                } catch (CompileError &e) {
                    /* Caught a parse error:
                     * throw it upstream as a CompileError with a specific index
                     */
                    e.setExpressionIndex(i);
                    throw; /* do not slice */
                }
            }
        }

        // Check sub-expression ids
        ng.rm.pl.validateSubIDs(ids, expressions, flags, elements);
        // Renumber and assign lkey to reports
        ng.rm.logicalKeyRenumber();

        unsigned length = 0;
        struct hs_database *out = build(ng, &length, 0);

        assert(out); // should have thrown exception on error
        assert(length);

        *db = out;
        *comp_error = nullptr;

        return HS_SUCCESS;
    } catch (const CompileError &e) {
        // Compiler error occurred
        *db = nullptr;
        *comp_error =
            generateCompileError(e.reason, e.hasIndex ? (int)e.index : -1);
        return HS_COMPILER_ERROR;
    } catch (const std::bad_alloc &) {
        *db = nullptr;
        *comp_error = const_cast<hs_compile_error_t *>(&hs_enomem);
        return HS_COMPILER_ERROR;
    } catch (...) {
        assert(!"Internal error, unexpected exception");
        *db = nullptr;
        *comp_error = const_cast<hs_compile_error_t *>(&hs_einternal);
        return HS_COMPILER_ERROR;
    }
}

hs_error_t
hs_compile_lit_multi_int(const char *const *expressions, const unsigned *flags,
                         const unsigned *ids, const hs_expr_ext *const *ext,
                         const size_t *lens, unsigned elements, unsigned mode,
                         const hs_platform_info_t *platform, hs_database_t **db,
                         hs_compile_error_t **comp_error, const Grey &g) {
    // Check the args: note that it's OK for flags, ids or ext to be null.
    if (!comp_error) {
        if (db) {
            *db = nullptr;
        }
        // nowhere to write the string, but we can still report an error code
        return HS_COMPILER_ERROR;
    }
    if (!db) {
        *comp_error = generateCompileError("Invalid parameter: db is NULL", -1);
        return HS_COMPILER_ERROR;
    }
    if (!expressions) {
        *db = nullptr;
        *comp_error =
            generateCompileError("Invalid parameter: expressions is NULL", -1);
        return HS_COMPILER_ERROR;
    }
    if (!lens) {
        *db = nullptr;
        *comp_error =
            generateCompileError("Invalid parameter: len is NULL", -1);
        return HS_COMPILER_ERROR;
    }
    if (elements == 0) {
        *db = nullptr;
        *comp_error =
            generateCompileError("Invalid parameter: elements is zero", -1);
        return HS_COMPILER_ERROR;
    }

#if defined(FAT_RUNTIME)
    if (!check_ssse3()) {
        *db = nullptr;
        *comp_error = generateCompileError("Unsupported architecture", -1);
        return HS_ARCH_ERROR;
    }
#endif

    if (!checkMode(mode, comp_error)) {
        *db = nullptr;
        assert(*comp_error); // set by checkMode.
        return HS_COMPILER_ERROR;
    }

    if (!checkPlatform(platform, comp_error)) {
        *db = nullptr;
        assert(*comp_error); // set by checkPlattform.
        return HS_COMPILER_ERROR;
    }

    if (elements > g.limitPatternCount) {
        *db = nullptr;
        *comp_error = generateCompileError("Number of patterns too large", -1);
        return HS_COMPILER_ERROR;
    }

    // This function is simply a wrapper around both the parser and compiler
    bool isStreaming = mode & (HS_MODE_STREAM | HS_MODE_VECTORED);
    bool isVectored = mode & HS_MODE_VECTORED;
    unsigned somPrecision = getSomPrecision(mode);

    target_t target_info =
        platform ? target_t(*platform) : get_current_target();

    try {
        CompileContext cc(isStreaming, isVectored, target_info, g);
        NG ng(cc, elements, somPrecision);

        for (unsigned int i = 0; i < elements; i++) {
            // Add this expression to the compiler
            try {
                addLitExpression(ng, i, expressions[i], flags ? flags[i] : 0,
                                 ext ? ext[i] : nullptr, ids ? ids[i] : 0,
                                 lens[i]);
            } catch (CompileError &e) {
                /* Caught a parse error;
                 * throw it upstream as a CompileError with a specific index */
                e.setExpressionIndex(i);
                throw; /* do not slice */
            }
        }

        // Check sub-expression ids
        ng.rm.pl.validateSubIDs(ids, expressions, flags, elements);
        // Renumber and assign lkey to reports
        ng.rm.logicalKeyRenumber();

        unsigned length = 0;
        struct hs_database *out = build(ng, &length, 1);

        assert(out); // should have thrown exception on error
        assert(length);

        *db = out;
        *comp_error = nullptr;

        return HS_SUCCESS;
    } catch (const CompileError &e) {
        // Compiler error occurred
        *db = nullptr;
        *comp_error =
            generateCompileError(e.reason, e.hasIndex ? (int)e.index : -1);
        return HS_COMPILER_ERROR;
    } catch (const std::bad_alloc &) {
        *db = nullptr;
        *comp_error = const_cast<hs_compile_error_t *>(&hs_enomem);
        return HS_COMPILER_ERROR;
    } catch (...) {
        assert(!"Internal errror, unexpected exception");
        *db = nullptr;
        *comp_error = const_cast<hs_compile_error_t *>(&hs_einternal);
        return HS_COMPILER_ERROR;
    }
}

} // namespace ue2

extern "C" HS_PUBLIC_API hs_error_t HS_CDECL
hs_compile(const char *expression, unsigned flags, unsigned mode,
           const hs_platform_info_t *platform, hs_database_t **db,
           hs_compile_error_t **error) {
    if (expression == nullptr) {
        *db = nullptr;
        *error =
            generateCompileError("Invalid parameter: expression is NULL", -1);
        return HS_COMPILER_ERROR;
    }

    unsigned id = 0; // single expressions get zero as an ID
    const hs_expr_ext *const *ext = nullptr; // unused for this call.

    return hs_compile_multi_int(&expression, &flags, &id, ext, 1, mode,
                                platform, db, error, Grey());
}

extern "C" HS_PUBLIC_API hs_error_t HS_CDECL
fat_hs_compile(const char *expression, unsigned flags, unsigned mode,
               const hs_platform_info_t *platform, fat_hs_database_t **db,
               hs_compile_error_t **error) {
    if (expression == nullptr) {
        *db = nullptr;
        *error =
            generateCompileError("Invalid parameter: expression is NULL", -1);
        return HS_COMPILER_ERROR;
    }

    unsigned id = 0; // single expressions get zero as an ID
    const hs_expr_ext *const *ext = nullptr; // unused for this call.

    return fat_hs_compile_multi_int(&expression, &flags, &id, ext, 1, mode,
                                    platform, db, error, Grey());
}

extern "C" hs_error_t HS_CDECL
hs_compile_context_create(hs_compile_context_t **ctx) {
    if (!ctx) {
        return HS_INVALID;
    }
    *ctx = nullptr;

#if !defined(HS_ENABLE_FP_FEEDBACK)
    return HS_ARCH_ERROR;
#else
    hs_compile_context_t *out =
        (hs_compile_context_t *)hs_misc_alloc(sizeof(*out));
    if (!out) {
        return HS_NOMEM;
    }

    memset(out, 0, sizeof(*out));
    *ctx = out;
    return HS_SUCCESS;
#endif
}

extern "C" hs_error_t HS_CDECL hs_compile_context_set_fp_feedback(
    hs_compile_context_t *ctx, const hs_fp_feedback_t *feedback) {
    if (!ctx) {
        return HS_INVALID;
    }

#if !defined(HS_ENABLE_FP_FEEDBACK)
    (void)feedback;
    return HS_ARCH_ERROR;
#else
    hs_fp_feedback_t *copy = nullptr;
    if (feedback) {
        hs_error_t err = hs_fp_feedback_clone(feedback, &copy);
        if (err != HS_SUCCESS) {
            return err;
        }
    }

    hs_fp_feedback_free(ctx->fp_feedback);
    ctx->fp_feedback = copy;
    resetCompileContextDiagnostics(ctx);
    return HS_SUCCESS;
#endif
}

extern "C" hs_error_t HS_CDECL
hs_compile_context_free(hs_compile_context_t *ctx) {
    if (ctx) {
        hs_fp_feedback_free(ctx->fp_feedback);
        hs_misc_free(ctx);
    }
    return HS_SUCCESS;
}

extern "C" unsigned int HS_CDECL
hs_compile_context_observe_checked_count(const hs_compile_context_t *ctx) {
    return ctx ? ctx->fp_observe_checked_count : 0;
}

extern "C" unsigned int HS_CDECL
hs_compile_context_observe_hit_count(const hs_compile_context_t *ctx) {
    return ctx ? ctx->fp_observe_hit_count : 0;
}

extern "C" hs_error_t HS_CDECL hs_compile_context_get_checkpoint_info(
    const hs_compile_context_t *ctx, unsigned int checkpoint,
    hs_compile_context_checkpoint_info_t *info) {
    if (!ctx || !info || checkpoint >= HS_FP_COMPILE_CHECKPOINT_COUNT) {
        return HS_INVALID;
    }

    *info = ctx->fp_checkpoint_info[checkpoint];
    return HS_SUCCESS;
}

extern "C" HS_PUBLIC_API hs_error_t HS_CDECL hs_compile_multi(
    const char *const *expressions, const unsigned *flags, const unsigned *ids,
    unsigned elements, unsigned mode, const hs_platform_info_t *platform,
    hs_database_t **db, hs_compile_error_t **error) {
    const hs_expr_ext *const *ext = nullptr; // unused for this call.
    return hs_compile_multi_int(expressions, flags, ids, ext, elements, mode,
                                platform, db, error, Grey());
}

extern "C" hs_error_t HS_CDECL hs_compile_multi_with_context(
    const char *const *expressions, const unsigned *flags, const unsigned *ids,
    unsigned elements, unsigned mode, const hs_platform_info_t *platform,
    const hs_compile_context_t *ctx, hs_database_t **db,
    hs_compile_error_t **error) {
#if !defined(HS_ENABLE_FP_FEEDBACK)
    (void)expressions;
    (void)flags;
    (void)ids;
    (void)elements;
    (void)mode;
    (void)platform;
    (void)ctx;
    if (db) {
        *db = nullptr;
    }
    if (error) {
        *error = generateCompileError(
            "False-positive feedback is not enabled for this build", -1);
    }
    return HS_ARCH_ERROR;
#else
    const hs_expr_ext *const *ext = nullptr; // unused for this call.
    hs_error_t err =
        hs_compile_multi_int(expressions, flags, ids, ext, elements, mode,
                             platform, db, error, Grey(), ctx);
    if (err == HS_SUCCESS) {
        observeCompileFeedback(ctx, db ? *db : nullptr);
    } else {
        resetCompileContextDiagnostics(ctx);
    }
    return err;
#endif
}

extern "C" HS_PUBLIC_API hs_error_t HS_CDECL hs_compile_multi_with_feedback(
    const char *const *expressions, const unsigned *flags, const unsigned *ids,
    unsigned elements, unsigned mode, const hs_platform_info_t *platform,
    const hs_fp_feedback_t *feedback, hs_database_t **db,
    hs_compile_error_t **error) {
#if !defined(HS_ENABLE_FP_FEEDBACK)
    (void)expressions;
    (void)flags;
    (void)ids;
    (void)elements;
    (void)mode;
    (void)platform;
    (void)feedback;
    if (db) {
        *db = nullptr;
    }
    if (error) {
        *error = generateCompileError(
            "False-positive feedback is not enabled for this build", -1);
    }
    return HS_ARCH_ERROR;
#else
    hs_compile_context_t *ctx = nullptr;
    hs_error_t err = hs_compile_context_create(&ctx);
    if (err != HS_SUCCESS) {
        return err;
    }

    err = hs_compile_context_set_fp_feedback(ctx, feedback);
    if (err != HS_SUCCESS) {
        hs_compile_context_free(ctx);
        return err;
    }

    err = hs_compile_multi_with_context(expressions, flags, ids, elements, mode,
                                        platform, ctx, db, error);
    hs_compile_context_free(ctx);
    return err;
#endif
}

extern "C" HS_PUBLIC_API hs_error_t HS_CDECL fat_hs_compile_multi(
    const char *const *expressions, const unsigned *flags, const unsigned *ids,
    unsigned elements, unsigned mode, const hs_platform_info_t *platform,
    fat_hs_database_t **db, hs_compile_error_t **error) {
    const hs_expr_ext *const *ext = nullptr; // unused for this call.
    return fat_hs_compile_multi_int(expressions, flags, ids, ext, elements,
                                    mode, platform, db, error, Grey());
}

extern "C" HS_PUBLIC_API hs_error_t HS_CDECL hs_compile_ext_multi(
    const char *const *expressions, const unsigned *flags, const unsigned *ids,
    const hs_expr_ext *const *ext, unsigned elements, unsigned mode,
    const hs_platform_info_t *platform, hs_database_t **db,
    hs_compile_error_t **error) {
    return hs_compile_multi_int(expressions, flags, ids, ext, elements, mode,
                                platform, db, error, Grey());
}

extern "C" hs_error_t HS_CDECL hs_compile_ext_multi_with_context(
    const char *const *expressions, const unsigned *flags, const unsigned *ids,
    const hs_expr_ext *const *ext, unsigned elements, unsigned mode,
    const hs_platform_info_t *platform, const hs_compile_context_t *ctx,
    hs_database_t **db, hs_compile_error_t **error) {
#if !defined(HS_ENABLE_FP_FEEDBACK)
    (void)expressions;
    (void)flags;
    (void)ids;
    (void)ext;
    (void)elements;
    (void)mode;
    (void)platform;
    (void)ctx;
    if (db) {
        *db = nullptr;
    }
    if (error) {
        *error = generateCompileError(
            "False-positive feedback is not enabled for this build", -1);
    }
    return HS_ARCH_ERROR;
#else
    hs_error_t err =
        hs_compile_multi_int(expressions, flags, ids, ext, elements, mode,
                             platform, db, error, Grey(), ctx);
    if (err == HS_SUCCESS) {
        observeCompileFeedback(ctx, db ? *db : nullptr);
    } else {
        resetCompileContextDiagnostics(ctx);
    }
    return err;
#endif
}

extern "C" HS_PUBLIC_API hs_error_t HS_CDECL hs_compile_ext_multi_with_feedback(
    const char *const *expressions, const unsigned *flags, const unsigned *ids,
    const hs_expr_ext *const *ext, unsigned elements, unsigned mode,
    const hs_platform_info_t *platform, const hs_fp_feedback_t *feedback,
    hs_database_t **db, hs_compile_error_t **error) {
#if !defined(HS_ENABLE_FP_FEEDBACK)
    (void)expressions;
    (void)flags;
    (void)ids;
    (void)ext;
    (void)elements;
    (void)mode;
    (void)platform;
    (void)feedback;
    if (db) {
        *db = nullptr;
    }
    if (error) {
        *error = generateCompileError(
            "False-positive feedback is not enabled for this build", -1);
    }
    return HS_ARCH_ERROR;
#else
    hs_compile_context_t *ctx = nullptr;
    hs_error_t err = hs_compile_context_create(&ctx);
    if (err != HS_SUCCESS) {
        return err;
    }

    err = hs_compile_context_set_fp_feedback(ctx, feedback);
    if (err != HS_SUCCESS) {
        hs_compile_context_free(ctx);
        return err;
    }

    err = hs_compile_ext_multi_with_context(
        expressions, flags, ids, ext, elements, mode, platform, ctx, db, error);
    hs_compile_context_free(ctx);
    return err;
#endif
}

extern "C" HS_PUBLIC_API hs_error_t HS_CDECL fat_hs_compile_ext_multi(
    const char *const *expressions, const unsigned *flags, const unsigned *ids,
    const hs_expr_ext *const *ext, unsigned elements, unsigned mode,
    const hs_platform_info_t *platform, fat_hs_database_t **db,
    hs_compile_error_t **error) {
    return fat_hs_compile_multi_int(expressions, flags, ids, ext, elements,
                                    mode, platform, db, error, Grey());
}

extern "C" HS_PUBLIC_API hs_error_t HS_CDECL
hs_compile_lit(const char *expression, unsigned flags, const size_t len,
               unsigned mode, const hs_platform_info_t *platform,
               hs_database_t **db, hs_compile_error_t **error) {
    if (expression == nullptr) {
        *db = nullptr;
        *error =
            generateCompileError("Invalid parameter: expression is NULL", -1);
        return HS_COMPILER_ERROR;
    }

    unsigned id = 0; // single expressions get zero as an ID
    const hs_expr_ext *const *ext = nullptr; // unused for this call.

    return hs_compile_lit_multi_int(&expression, &flags, &id, ext, &len, 1,
                                    mode, platform, db, error, Grey());
}

extern "C" HS_PUBLIC_API hs_error_t HS_CDECL
fat_hs_compile_lit(const char *expression, unsigned flags, const size_t len,
                   unsigned mode, const hs_platform_info_t *platform,
                   fat_hs_database_t **db, hs_compile_error_t **error) {
    if (expression == nullptr) {
        *db = nullptr;
        *error =
            generateCompileError("Invalid parameter: expression is NULL", -1);
        return HS_COMPILER_ERROR;
    }

    unsigned id = 0; // single expressions get zero as an ID
    const hs_expr_ext *const *ext = nullptr; // unused for this call.

    return fat_hs_compile_lit_multi_int(&expression, &flags, &id, ext, &len, 1,
                                        mode, platform, db, error, Grey());
}

extern "C" HS_PUBLIC_API hs_error_t HS_CDECL
hs_compile_lit_multi(const char *const *expressions, const unsigned *flags,
                     const unsigned *ids, const size_t *lens, unsigned elements,
                     unsigned mode, const hs_platform_info_t *platform,
                     hs_database_t **db, hs_compile_error_t **error) {
    const hs_expr_ext *const *ext = nullptr; // unused for this call.
    return hs_compile_lit_multi_int(expressions, flags, ids, ext, lens,
                                    elements, mode, platform, db, error,
                                    Grey());
}

extern "C" HS_PUBLIC_API hs_error_t HS_CDECL fat_hs_compile_lit_multi(
    const char *const *expressions, const unsigned *flags, const unsigned *ids,
    const size_t *lens, unsigned elements, unsigned mode,
    const hs_platform_info_t *platform, fat_hs_database_t **db,
    hs_compile_error_t **error) {
    const hs_expr_ext *const *ext = nullptr;
    return fat_hs_compile_lit_multi_int(expressions, flags, ids, ext, lens,
                                        elements, mode, platform, db, error,
                                        Grey());
}

extern "C" HS_PUBLIC_API hs_error_t HS_CDECL
hs_set_grey_overrides(const char *overrides) {
    if (overrides == nullptr || overrides[0] == '\0') {
        ue2::resetGreyOverrides();
        return HS_SUCCESS;
    }

    std::string s(overrides);

    // Reject "help" or "help:" prefix - this is a debug-only feature.
    if (s == "help" || s.compare(0, 5, "help:") == 0) {
        return HS_INVALID;
    }

    // Validate the overrides string by applying to a temporary Grey.
    ue2::Grey test_grey(false);
    if (!ue2::applyGreyOverrides(&test_grey, s)) {
        return HS_INVALID;
    }

    ue2::setGreyOverrides(s);
    return HS_SUCCESS;
}

extern "C" HS_PUBLIC_API hs_error_t HS_CDECL hs_reset_grey_overrides(void) {
    ue2::resetGreyOverrides();
    return HS_SUCCESS;
}

static hs_error_t
hs_expression_info_int(const char *expression, unsigned int flags,
                       const hs_expr_ext_t *ext, unsigned int mode,
                       hs_expr_info_t **info, hs_compile_error_t **error) {
    if (!error) {
        // nowhere to write an error, but we can still return an error code.
        return HS_COMPILER_ERROR;
    }

#if defined(FAT_RUNTIME)
    if (!check_ssse3()) {
        *error = generateCompileError("Unsupported architecture", -1);
        return HS_ARCH_ERROR;
    }
#endif

    if (!info) {
        *error = generateCompileError("Invalid parameter: info is NULL", -1);
        return HS_COMPILER_ERROR;
    }

    if (!expression) {
        *error =
            generateCompileError("Invalid parameter: expression is NULL", -1);
        return HS_COMPILER_ERROR;
    }

    if (flags & HS_FLAG_COMBINATION) {
        *error = generateCompileError("Invalid parameter: unsupported "
                                      "logical combination expression",
                                      -1);
        return HS_COMPILER_ERROR;
    }

    *info = nullptr;
    *error = nullptr;

    hs_expr_info local_info;
    memset(&local_info, 0, sizeof(local_info));

    try {
        bool isStreaming = mode & (HS_MODE_STREAM | HS_MODE_VECTORED);
        bool isVectored = mode & HS_MODE_VECTORED;

        CompileContext cc(isStreaming, isVectored, get_current_target(),
                          Grey());

        // Ensure that our pattern isn't too long (in characters).
        if (strlen(expression) > cc.grey.limitPatternLength) {
            throw ParseError("Pattern length exceeds limit.");
        }

        ReportManager rm(cc.grey);
        ParsedExpression pe(0, expression, flags, 0, ext);
        assert(pe.component);

        // Apply prefiltering transformations if desired.
        if (pe.expr.prefilter) {
            prefilterTree(pe.component, ParseMode(flags));
        }

        // Expressions containing zero-width assertions and other extended pcre
        // types aren't supported yet. This call will throw a ParseError
        // exception if the component tree contains such a construct.
        checkUnsupported(*pe.component);

        pe.component->checkEmbeddedStartAnchor(true);
        pe.component->checkEmbeddedEndAnchor(true);

        auto built_expr = buildGraph(rm, cc, pe);
        unique_ptr<NGHolder> &g = built_expr.g;
        ExpressionInfo &expr = built_expr.expr;

        if (!g) {
            DEBUG_PRINTF("NFA build failed, but no exception was thrown.\n");
            throw ParseError("Internal error.");
        }

        fillExpressionInfo(rm, cc, *g, expr, &local_info);
    } catch (const CompileError &e) {
        // Compiler error occurred
        *error = generateCompileError(e);
        return HS_COMPILER_ERROR;
    } catch (std::bad_alloc &) {
        *error = const_cast<hs_compile_error_t *>(&hs_enomem);
        return HS_COMPILER_ERROR;
    } catch (...) {
        assert(!"Internal error, unexpected exception");
        *error = const_cast<hs_compile_error_t *>(&hs_einternal);
        return HS_COMPILER_ERROR;
    }

    hs_expr_info *rv = (hs_expr_info *)hs_misc_alloc(sizeof(*rv));
    if (!rv) {
        *error = const_cast<hs_compile_error_t *>(&hs_enomem);
        return HS_COMPILER_ERROR;
    }

    *rv = local_info;
    *info = rv;
    return HS_SUCCESS;
}

extern "C" HS_PUBLIC_API hs_error_t HS_CDECL
hs_expression_info(const char *expression, unsigned int flags,
                   hs_expr_info_t **info, hs_compile_error_t **error) {
    return hs_expression_info_int(expression, flags, nullptr, HS_MODE_BLOCK,
                                  info, error);
}

extern "C" HS_PUBLIC_API hs_error_t HS_CDECL hs_expression_ext_info(
    const char *expression, unsigned int flags, const hs_expr_ext_t *ext,
    hs_expr_info_t **info, hs_compile_error_t **error) {
    return hs_expression_info_int(expression, flags, ext, HS_MODE_BLOCK, info,
                                  error);
}

extern "C" HS_PUBLIC_API hs_error_t HS_CDECL
hs_populate_platform(hs_platform_info_t *platform) {
    if (!platform) {
        return HS_INVALID;
    }

    memset(platform, 0, sizeof(*platform));

    platform->cpu_features = cpuid_flags();
    platform->tune = cpuid_tune();

    return HS_SUCCESS;
}

extern "C" HS_PUBLIC_API hs_error_t HS_CDECL
hs_free_compile_error(hs_compile_error_t *error) {
#if defined(FAT_RUNTIME)
    if (!check_ssse3()) {
        return HS_ARCH_ERROR;
    }
#endif
    freeCompileError(error);
    return HS_SUCCESS;
}
