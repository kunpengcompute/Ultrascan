/*
 * Copyright (c) 2026, Intel Corporation
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

#include "config.h"

#include "fp_collector.h"
#include "hs.h"
#include "util/compile_context.h"
#include "util/target_info.h"
#include "gtest/gtest.h"

#include <cstdlib>
#include <cstring>

using namespace ue2;

TEST(FpCollectorInternal, MatcherBuildDiagnosticsGrowBeyondInitialCapacity) {
    // matcher_hits is allocated by the library via its misc allocator: use a
    // known allocator so the array can be freed with free().
    hs_set_misc_allocator(malloc, free);

    hs_compile_context_matcher_build_hit_info_t *matcher_hits = nullptr;
    u32 matcher_count = 0;
    u32 matcher_dropped = 0;
    u32 matcher_capacity = 0;

    CompileContext cc(false, false, get_current_target(), Grey(), nullptr,
                      nullptr, &matcher_hits, &matcher_count, &matcher_dropped,
                      &matcher_capacity);

    const u32 matcher_entries =
        HS_FP_MATCHER_BUILD_HIT_DETAIL_INITIAL_CAPACITY + 17;
    for (u32 i = 0; i < matcher_entries; i++) {
        hs_compile_context_matcher_build_hit_info_t info = {};
        info.feedback_index = i;
        info.table = HS_FP_TABLE_FLOATING;
        info.fragment_id = i;
        info.lit_id = i;
        info.source_table = HS_FP_TABLE_FLOATING;
        info.source_length = sizeof(i);
        info.source_copied_length = sizeof(i);
        info.occurrences = 1;
        memcpy(info.source_suffix, &i, sizeof(i));
        fpCompileRecordMatcherBuildHit(cc, info);
    }
    EXPECT_EQ(matcher_entries, matcher_count);
    EXPECT_GE(matcher_capacity, matcher_count);
    EXPECT_EQ(0U, matcher_dropped);
    ASSERT_NE(nullptr, matcher_hits);

    hs_compile_context_matcher_build_hit_info_t duplicate_matcher =
        matcher_hits[0];
    fpCompileRecordMatcherBuildHit(cc, duplicate_matcher);
    EXPECT_EQ(matcher_entries, matcher_count);
    EXPECT_EQ(2U, matcher_hits[0].occurrences);

    free(matcher_hits);
    hs_set_misc_allocator(nullptr, nullptr);
}
