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

static inline bool fpFeedbackBlocksRoseFragment(const CompileContext &cc,
                                                const ue2_literal &lit,
                                                const std::vector<u8> *msk,
                                                const std::vector<u8> *cmp,
                                                unsigned int checkpoint) {
    if (!cc.fp_feedback) {
        return false;
    }

    ue2_literal final_lit = fpFeedbackFinalRoseFragment(lit);
    fpCompileRecordCheck(cc, checkpoint);

    std::vector<u8> matcher_msk = msk ? *msk : std::vector<u8>();
    std::vector<u8> matcher_cmp = cmp ? *cmp : std::vector<u8>();
    fpFeedbackBuildMatcherMask(lit, matcher_msk, matcher_cmp);

    const std::string &s = final_lit.get_string();
    const u8 *mask_ptr = matcher_msk.empty() ? nullptr : matcher_msk.data();
    const u8 *cmp_ptr = matcher_cmp.empty() ? nullptr : matcher_cmp.data();
    const size_t mask_len = matcher_msk.size();
    if (!hs_fp_feedback_fragment_is_bad(cc.fp_feedback, s.data(), s.size(),
                                        final_lit.any_nocase(), mask_ptr,
                                        cmp_ptr, mask_len)) {
        return false;
    }

    fpCompileRecordHit(cc, checkpoint);
    DEBUG_PRINTF("rejecting Rose literal fragment due to fp feedback: '%s'\n",
                 escapeString(s).c_str());
    fpCompileRecordBlocked(cc, checkpoint);
    return true;
}

static inline bool fpFeedbackBlocksRoseLiteral(const CompileContext &cc,
                                               const ue2_literal &lit,
                                               unsigned int checkpoint) {
    return fpFeedbackBlocksRoseFragment(cc, lit, nullptr, nullptr, checkpoint);
}

static inline bool fpFeedbackObservesRoseFragment(const CompileContext &cc,
                                                  const ue2_literal &lit,
                                                  const std::vector<u8> *msk,
                                                  const std::vector<u8> *cmp,
                                                  unsigned int checkpoint) {
    if (!cc.fp_feedback) {
        return false;
    }

    ue2_literal final_lit = fpFeedbackFinalRoseFragment(lit);
    fpCompileRecordCheck(cc, checkpoint);

    std::vector<u8> matcher_msk = msk ? *msk : std::vector<u8>();
    std::vector<u8> matcher_cmp = cmp ? *cmp : std::vector<u8>();
    fpFeedbackBuildMatcherMask(lit, matcher_msk, matcher_cmp);

    const std::string &s = final_lit.get_string();
    const u8 *mask_ptr = matcher_msk.empty() ? nullptr : matcher_msk.data();
    const u8 *cmp_ptr = matcher_cmp.empty() ? nullptr : matcher_cmp.data();
    const size_t mask_len = matcher_msk.size();
    if (!hs_fp_feedback_fragment_is_bad(cc.fp_feedback, s.data(), s.size(),
                                        final_lit.any_nocase(), mask_ptr,
                                        cmp_ptr, mask_len)) {
        return false;
    }

    fpCompileRecordHit(cc, checkpoint);
    fpCompileRecordPassed(cc, checkpoint);
    DEBUG_PRINTF("observed Rose literal rewrite feedback hit: '%s'\n",
                 escapeString(s).c_str());
    return true;
}

static inline bool fpFeedbackObservesRoseLiteral(const CompileContext &cc,
                                                 const ue2_literal &lit,
                                                 unsigned int checkpoint) {
    return fpFeedbackObservesRoseFragment(cc, lit, nullptr, nullptr,
                                          checkpoint);
}

} // namespace ue2

#endif
