/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Huawei Corporation nor the names of its contributors
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

#ifndef ROSE_BUILD_FP_FEEDBACK_H
#define ROSE_BUILD_FP_FEEDBACK_H

#include "fp_collector.h"
#include "hwlm/hwlm_literal.h"
#include "util/charreach_util.h"
#include "util/compile_context.h"
#include "util/ue2string.h"

#include <algorithm>
#include <string>
#include <vector>

namespace ue2 {

void normaliseLiteralMask(const ue2_literal &s, std::vector<u8> &msk,
                          std::vector<u8> &cmp);

static inline ue2_literal fpFeedbackFinalRoseFragment(const ue2_literal &lit) {
    ue2_literal final_lit(lit);
    if (final_lit.length() > HWLM_MASKLEN) {
        final_lit.erase(0, final_lit.length() - HWLM_MASKLEN);
    }
    return final_lit;
}

static inline void fpFeedbackBuildMatcherMask(const ue2_literal &lit,
                                              std::vector<u8> &msk,
                                              std::vector<u8> &cmp) {
    const size_t suffix_len = std::min(lit.length(), size_t{HWLM_MASKLEN});
    const bool mixed_suffix =
        mixed_sensitivity_in(lit.end() - suffix_len, lit.end());

    if (msk.empty() && !mixed_suffix) {
        return;
    }

    while (msk.size() < HWLM_MASKLEN) {
        msk.insert(msk.begin(), 0);
        cmp.insert(cmp.begin(), 0);
    }

    if (mixed_suffix) {
        auto it = lit.rbegin();
        for (size_t i = 0; i < suffix_len; ++i, ++it) {
            const auto &c = *it;
            if (!c.nocase) {
                const size_t offset = HWLM_MASKLEN - i - 1;
                make_and_cmp_mask(c, &msk[offset], &cmp[offset]);
            }
        }
    }

    normaliseLiteralMask(lit, msk, cmp);
}

static inline bool fpFeedbackCanMatch(const CompileContext &cc,
                                      unsigned int table) {
#ifndef HS_ENABLE_FP_FEEDBACK
    (void)cc;
    (void)table;
    return false;
#else
    return cc.fp_feedback && table != HS_FP_TABLE_UNKNOWN;
#endif
}

/**
 * Match an identity that is already in the exact form consumed by HWLM.
 * This helper deliberately has no diagnostic or blocking side effects.
 */
static inline bool fpFeedbackMatchesFinalRoseFragment(
    const CompileContext &cc, unsigned int table, const std::string &s,
    bool nocase, const std::vector<u8> &msk, const std::vector<u8> &cmp,
    u32 *feedback_index = nullptr) {
#ifndef HS_ENABLE_FP_FEEDBACK
    (void)cc;
    (void)table;
    (void)s;
    (void)nocase;
    (void)msk;
    (void)cmp;
    if (feedback_index) {
        *feedback_index = HS_FP_FEEDBACK_INDEX_INVALID;
    }
    return false;
#else
    if (feedback_index) {
        *feedback_index = HS_FP_FEEDBACK_INDEX_INVALID;
    }
    if (!fpFeedbackCanMatch(cc, table) || msk.size() != cmp.size()) {
        return false;
    }

    const u8 *mask_ptr = msk.empty() ? nullptr : msk.data();
    const u8 *cmp_ptr = cmp.empty() ? nullptr : cmp.data();
    return hs_fp_feedback_fragment_match_index(
        cc.fp_feedback, table, s.data(), s.size(), nocase, mask_ptr, cmp_ptr,
        msk.size(), feedback_index);
#endif
}

/**
 * Convert a Rose literal candidate to its final HWLM identity and match it.
 * This helper deliberately has no diagnostic or blocking side effects.
 */
static inline bool fpFeedbackMatchesRoseFragment(
    const CompileContext &cc, unsigned int table, const ue2_literal &lit,
    const std::vector<u8> *msk, const std::vector<u8> *cmp,
    u32 *feedback_index = nullptr) {
    if (!fpFeedbackCanMatch(cc, table)) {
        if (feedback_index) {
            *feedback_index = HS_FP_FEEDBACK_INDEX_INVALID;
        }
        return false;
    }

    const ue2_literal final_lit = fpFeedbackFinalRoseFragment(lit);
    std::vector<u8> matcher_msk = msk ? *msk : std::vector<u8>();
    std::vector<u8> matcher_cmp = cmp ? *cmp : std::vector<u8>();
    fpFeedbackBuildMatcherMask(lit, matcher_msk, matcher_cmp);

    return fpFeedbackMatchesFinalRoseFragment(
        cc, table, final_lit.get_string(), final_lit.any_nocase(), matcher_msk,
        matcher_cmp, feedback_index);
}

static inline bool fpFeedbackBlocksRoseFragment(
    const CompileContext &cc, unsigned int table, const ue2_literal &lit,
    const std::vector<u8> *msk, const std::vector<u8> *cmp,
    unsigned int checkpoint) {
#ifndef HS_ENABLE_FP_FEEDBACK
    (void)cc;
    (void)table;
    (void)lit;
    (void)msk;
    (void)cmp;
    (void)checkpoint;
    return false;
#else
    if (!fpFeedbackCanMatch(cc, table)) {
        return false;
    }

    fpCompileRecordCheck(cc, checkpoint);
    if (!fpFeedbackMatchesRoseFragment(cc, table, lit, msk, cmp)) {
        return false;
    }

    fpCompileRecordHit(cc, checkpoint);
    const std::string s = fpFeedbackFinalRoseFragment(lit).get_string();
    DEBUG_PRINTF("rejecting Rose literal fragment due to fp feedback: '%s'\n",
                 escapeString(s).c_str());
    fpCompileRecordBlocked(cc, checkpoint);
    return true;
#endif
}

static inline bool fpFeedbackBlocksRoseLiteral(
    const CompileContext &cc, unsigned int table, const ue2_literal &lit,
    unsigned int checkpoint) {
    return fpFeedbackBlocksRoseFragment(cc, table, lit, nullptr, nullptr,
                                        checkpoint);
}

/**
 * Resolve the table used by a literal passed directly to RoseBuild::add().
 * Anchored literals beyond the anchored matcher region necessarily fall back
 * to the floating table in tryForAnchoredVertex(). Shorter anchored literals
 * are outside the feedback collection scope and therefore remain unknown.
 */
static inline unsigned int fpFeedbackTableForDirectRoseLiteral(
    const CompileContext &cc, bool anchored, bool eod,
    const ue2_literal &lit) {
    if (eod) {
        return HS_FP_TABLE_EOD_ANCHORED;
    }
    if (!anchored || lit.length() > cc.grey.maxAnchoredRegion) {
        return HS_FP_TABLE_FLOATING;
    }
    return HS_FP_TABLE_UNKNOWN;
}

} // namespace ue2

#endif
