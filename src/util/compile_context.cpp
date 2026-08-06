/*
 * Copyright (c) 2015, Intel Corporation
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
 * \brief Global compile context, describes compile environment.
 */
#include "compile_context.h"
#include "allocator.h"
#include "grey.h"

#include <cstring>
#include <limits>

namespace ue2 {

#ifdef HS_ENABLE_FP_FEEDBACK
CompileContext::CompileContext(
    bool in_isStreaming, bool in_isVectored, const target_t &in_target_info,
    const Grey &in_grey, const hs_fp_feedback_t *in_fp_feedback,
    hs_compile_context_checkpoint_info_t *in_fp_checkpoint_info,
    hs_compile_context_matcher_build_hit_info_t
        **in_fp_matcher_build_hit_info,
    u32 *in_fp_matcher_build_hit_count,
    u32 *in_fp_matcher_build_hit_dropped_count,
    u32 *in_fp_matcher_build_hit_capacity)
    : streaming(in_isStreaming || in_isVectored), vectored(in_isVectored),
      target_info(in_target_info), grey(in_grey), fp_feedback(in_fp_feedback),
      fp_checkpoint_info(in_fp_checkpoint_info),
      fp_matcher_build_hit_info(in_fp_matcher_build_hit_info),
      fp_matcher_build_hit_count(in_fp_matcher_build_hit_count),
      fp_matcher_build_hit_dropped_count(in_fp_matcher_build_hit_dropped_count),
      fp_matcher_build_hit_capacity(in_fp_matcher_build_hit_capacity) {}
#else
CompileContext::CompileContext(bool in_isStreaming, bool in_isVectored,
                               const target_t &in_target_info,
                               const Grey &in_grey)
    : streaming(in_isStreaming || in_isVectored), vectored(in_isVectored),
      target_info(in_target_info), grey(in_grey) {}
#endif

#ifdef HS_ENABLE_FP_FEEDBACK
template <class T>
static bool ensureDiagnosticCapacity(T **entries, u32 count, u32 *capacity,
                                     u32 initial_capacity) {
    if (!entries || !capacity) {
        return false;
    }

    const u32 old_capacity = *capacity;
    if (*entries && count < old_capacity) {
        return true;
    }
    if (old_capacity == std::numeric_limits<u32>::max()) {
        return false;
    }

    u32 new_capacity = initial_capacity;
    if (old_capacity) {
        new_capacity =
            old_capacity > std::numeric_limits<u32>::max() / 2
                ? std::numeric_limits<u32>::max()
                : old_capacity * 2;
    }
    if (new_capacity <= count) {
        return false;
    }

    const size_t allocation_size =
        static_cast<size_t>(new_capacity) * sizeof(T);
    if (allocation_size / sizeof(T) != new_capacity) {
        return false;
    }
    T *new_entries = static_cast<T *>(
        hs_misc_alloc(allocation_size));
    if (hs_check_alloc(new_entries) != HS_SUCCESS) {
        hs_misc_free(new_entries);
        return false;
    }

    if (*entries && count) {
        memcpy(new_entries, *entries, static_cast<size_t>(count) * sizeof(T));
    }
    hs_misc_free(*entries);
    *entries = new_entries;
    *capacity = new_capacity;
    return true;
}
#endif

void fpCompileRecordMatcherBuildHit(
    const CompileContext &cc,
    const hs_compile_context_matcher_build_hit_info_t &info) {
#ifdef HS_ENABLE_FP_FEEDBACK
    if (!cc.fp_matcher_build_hit_info || !cc.fp_matcher_build_hit_count ||
        !cc.fp_matcher_build_hit_capacity) {
        return;
    }

    const u32 count = *cc.fp_matcher_build_hit_count;
    hs_compile_context_matcher_build_hit_info_t *entries =
        *cc.fp_matcher_build_hit_info;
    for (u32 i = 0; i < count; i++) {
        hs_compile_context_matcher_build_hit_info_t &existing = entries[i];
        if (existing.feedback_index == info.feedback_index &&
            existing.table == info.table &&
            existing.fragment_id == info.fragment_id &&
            existing.lit_id == info.lit_id &&
            existing.source_table == info.source_table &&
            existing.source_delay == info.source_delay &&
            existing.source_length == info.source_length &&
            existing.source_copied_length == info.source_copied_length &&
            existing.source_nocase == info.source_nocase &&
            !memcmp(existing.source_suffix, info.source_suffix,
                    info.source_copied_length)) {
            existing.occurrences += info.occurrences;
            return;
        }
    }

    if (ensureDiagnosticCapacity(
            cc.fp_matcher_build_hit_info, count,
            cc.fp_matcher_build_hit_capacity,
            HS_FP_MATCHER_BUILD_HIT_DETAIL_INITIAL_CAPACITY)) {
        (*cc.fp_matcher_build_hit_info)[count] = info;
        *cc.fp_matcher_build_hit_count = count + 1;
        return;
    }

    if (cc.fp_matcher_build_hit_dropped_count) {
        (*cc.fp_matcher_build_hit_dropped_count)++;
    }
#else
    (void)cc;
    (void)info;
#endif
}

} // namespace ue2
