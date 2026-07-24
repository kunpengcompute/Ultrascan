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

#include "fp_collector.h"
#include "fp_collector_hist.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "allocator.h"
#include "database.h"
#include "hs_runtime.h"
#include "rose/rose_internal.h"
#include "scratch.h"
#include "ue2common.h"

#if !defined(__aarch64__)
#error "false-positive feedback collector requires AArch64 NEON"
#endif

#include <arm_neon.h>

struct hs_fp_counter {
    u32 key;
    u8 used;
    u64a trigger_count;
    u64a true_trigger_count;
    u64a final_report_count;
};

struct hs_fp_collector {
    const hs_database_t *db;
    const struct RoseEngine *rose;
    struct hs_fp_counter *counters;
    u32 counter_capacity;
    u64a scan_bytes;
    u64a scan_calls;
    u64a trigger_count;
    u64a true_trigger_count;
    u64a final_report_count;
    u64a unknown_report_count;
    u64a unknown_no_active_trigger_count;
    u64a unknown_delayed_replay_count;
    u64a unknown_anchored_replay_count;
    u64a unknown_eod_or_boundary_count;
    u64a unknown_flush_combination_count;
    u64a unknown_mpv_or_nfa_queue_count;
    u64a unknown_counter_missing_count;
    u64a dropped_trigger_count;
    u8 histogram_backend;
    u32 pending_trigger_count;
    u32 pending_trigger_keys[HS_FP_TRIGGER_HISTOGRAM_BATCH_SIZE];
};

struct hs_fp_report_entry {
    u64a key;
    u32 fragment_id;
    u32 literal_count;
    u8 table;
    u8 engine;
    u8 flags;
    u8 length;
    u8 mask_length;
    u8 bytes[ROSE_FP_FRAGMENT_BYTES_MAX];
    u8 mask[ROSE_FP_FRAGMENT_BYTES_MAX];
    u8 cmp[ROSE_FP_FRAGMENT_BYTES_MAX];
    u64a trigger_count;
    u64a true_trigger_count;
    u64a final_report_count;
};

struct hs_fp_report {
    const hs_database_t *db;
    const struct RoseEngine *rose;
    struct hs_fp_report_entry *entries;
    u32 entry_count;
    u64a scan_bytes;
    u64a scan_calls;
    u64a trigger_count;
    u64a true_trigger_count;
    u64a final_report_count;
    u64a unknown_report_count;
    u64a unknown_no_active_trigger_count;
    u64a unknown_delayed_replay_count;
    u64a unknown_anchored_replay_count;
    u64a unknown_eod_or_boundary_count;
    u64a unknown_flush_combination_count;
    u64a unknown_mpv_or_nfa_queue_count;
    u64a unknown_counter_missing_count;
    u64a unknown_fragment_meta_missing_count;
    u64a dropped_trigger_count;
};

struct hs_fp_feedback_entry {
    u64a key;
    u32 fragment_id;
    u32 literal_count;
    u8 table;
    u8 engine;
    u8 flags;
    u8 length;
    u8 mask_length;
    u8 bytes[ROSE_FP_FRAGMENT_BYTES_MAX];
    u8 mask[ROSE_FP_FRAGMENT_BYTES_MAX];
    u8 cmp[ROSE_FP_FRAGMENT_BYTES_MAX];
    u64a trigger_count;
    u64a true_trigger_count;
    u64a final_report_count;
    u64a false_positive_trigger_count;
};

struct hs_fp_feedback {
    struct hs_fp_feedback_entry *entries;
    u32 bad_fragment_count;
    u64a scan_bytes;
    u64a scan_calls;
    u64a total_false_positive_trigger_count;
};

struct hs_fp_feedback_build_options {
    u64a min_trigger_count;
    u64a min_false_positive_count;
    u32 min_false_positive_rate;
    u32 min_waste_share;
    u32 max_bad_fragments;
};

static char bytes_equal_short_neon(const u8 *lhs, const u8 *rhs, size_t len) {
    assert(len <= ROSE_FP_FRAGMENT_BYTES_MAX);
    if (!len) {
        return 1;
    }

    u8 lhs_buf[ROSE_FP_FRAGMENT_BYTES_MAX] = {0};
    u8 rhs_buf[ROSE_FP_FRAGMENT_BYTES_MAX] = {0};
    u8 cmp_buf[ROSE_FP_FRAGMENT_BYTES_MAX];

    for (size_t i = 0; i < len; i++) {
        lhs_buf[i] = lhs[i];
        rhs_buf[i] = rhs[i];
    }

    const uint8x8_t lhs_vec = vld1_u8(lhs_buf);
    const uint8x8_t rhs_vec = vld1_u8(rhs_buf);
    const uint8x8_t eq = vceq_u8(lhs_vec, rhs_vec);
    vst1_u8(cmp_buf, eq);

    for (size_t i = 0; i < len; i++) {
        if (cmp_buf[i] != 0xffU) {
            return 0;
        }
    }

    return 1;
}

static u64a sat_add_u64a(u64a a, u64a b) {
    const u64a max = ~(u64a)0;
    if (max - a < b) {
        return max;
    }
    return a + b;
}

static u64a sub_or_zero_u64a(u64a a, u64a b) { return a > b ? a - b : 0; }

static char ratio_at_least(u64a num, u64a den, u64a min_num, u64a min_den) {
    if (!den) {
        return 0;
    }
    if (!min_num) {
        return 1;
    }

    const u64a max = ~(u64a)0;
    if (num <= max / min_den && den <= max / min_num) {
        return num * min_den >= den * min_num;
    }

    long double lhs = (long double)num / (long double)den;
    long double rhs = (long double)min_num / (long double)min_den;
    return lhs >= rhs;
}

static u32 hash_u32(u32 v) {
    v ^= v >> 16;
    v *= 0x7feb352dU;
    v ^= v >> 15;
    v *= 0x846ca68bU;
    v ^= v >> 16;
    return v;
}

static u32 next_power_of_two_u32(u32 v) {
    if (v <= 1) {
        return 1;
    }

    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

static u32 choose_counter_capacity(const struct RoseEngine *rose) {
    u32 wanted = rose->totalNumLiterals;
    if (wanted < 4) {
        wanted = 4;
    }
    if (wanted > (1U << 28)) {
        wanted = 1U << 28;
    } else {
        wanted *= 4;
    }
    return next_power_of_two_u32(wanted);
}

static int compare_report_entry(const void *a, const void *b) {
    const struct hs_fp_report_entry *entry_a = a;
    const struct hs_fp_report_entry *entry_b = b;

    if (entry_a->key < entry_b->key) {
        return -1;
    }
    if (entry_a->key > entry_b->key) {
        return 1;
    }
    return 0;
}

static int compare_feedback_entry_score(const void *a, const void *b) {
    const struct hs_fp_feedback_entry *entry_a = a;
    const struct hs_fp_feedback_entry *entry_b = b;

    if (entry_a->false_positive_trigger_count >
        entry_b->false_positive_trigger_count) {
        return -1;
    }
    if (entry_a->false_positive_trigger_count <
        entry_b->false_positive_trigger_count) {
        return 1;
    }
    if (entry_a->trigger_count > entry_b->trigger_count) {
        return -1;
    }
    if (entry_a->trigger_count < entry_b->trigger_count) {
        return 1;
    }
    if (entry_a->key < entry_b->key) {
        return -1;
    }
    if (entry_a->key > entry_b->key) {
        return 1;
    }
    return 0;
}

static hs_error_t
make_feedback_build_options(const hs_fp_feedback_params_t *params,
                            struct hs_fp_feedback_build_options *options) {
    static const u32 known_flags =
        HS_FP_FEEDBACK_PARAM_MIN_TRIGGER_COUNT |
        HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_COUNT |
        HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_RATE |
        HS_FP_FEEDBACK_PARAM_MIN_WASTE_SHARE |
        HS_FP_FEEDBACK_PARAM_MAX_BAD_FRAGMENTS;

    if (!options) {
        return HS_INVALID;
    }

    options->min_trigger_count = HS_FP_FEEDBACK_DEFAULT_MIN_TRIGGER_COUNT;
    options->min_false_positive_count =
        HS_FP_FEEDBACK_DEFAULT_MIN_FALSE_POSITIVE_COUNT;
    options->min_false_positive_rate =
        HS_FP_FEEDBACK_DEFAULT_MIN_FALSE_POSITIVE_RATE;
    options->min_waste_share = HS_FP_FEEDBACK_DEFAULT_MIN_WASTE_SHARE;
    options->max_bad_fragments = HS_FP_FEEDBACK_DEFAULT_MAX_BAD_FRAGMENTS;

    if (!params) {
        return HS_SUCCESS;
    }

    if (params->flags & ~known_flags) {
        return HS_INVALID;
    }

    if (params->flags & HS_FP_FEEDBACK_PARAM_MIN_TRIGGER_COUNT) {
        if (!params->min_trigger_count) {
            return HS_INVALID;
        }
        options->min_trigger_count = params->min_trigger_count;
    }

    if (params->flags & HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_COUNT) {
        if (!params->min_false_positive_count) {
            return HS_INVALID;
        }
        options->min_false_positive_count = params->min_false_positive_count;
    }

    if (params->flags & HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_RATE) {
        if (params->min_false_positive_rate > HS_FP_FEEDBACK_RATE_SCALE) {
            return HS_INVALID;
        }
        options->min_false_positive_rate = params->min_false_positive_rate;
    }

    if (params->flags & HS_FP_FEEDBACK_PARAM_MIN_WASTE_SHARE) {
        if (params->min_waste_share > HS_FP_FEEDBACK_RATE_SCALE) {
            return HS_INVALID;
        }
        options->min_waste_share = params->min_waste_share;
    }

    if (params->flags & HS_FP_FEEDBACK_PARAM_MAX_BAD_FRAGMENTS) {
        if (!params->max_bad_fragments) {
            return HS_INVALID;
        }
        options->max_bad_fragments = params->max_bad_fragments;
    }

    return HS_SUCCESS;
}

static const struct RoseFpFragmentMeta *
find_fragment_meta(const struct RoseEngine *rose, u32 key) {
    const struct RoseFpFragmentMeta *meta = getRoseFpFragmentMeta(rose);
    if (!meta) {
        return NULL;
    }

    for (u32 i = 0; i < rose->fpFragmentMetaCount; i++) {
        if (meta[i].programOffset == key) {
            return meta + i;
        }
    }

    return NULL;
}

static u32 count_used_counters(const hs_fp_collector_t *collector) {
    u32 count = 0;
    for (u32 i = 0; i < collector->counter_capacity; i++) {
        if (collector->counters[i].used) {
            count++;
        }
    }
    return count;
}

static char fill_report_entry(struct hs_fp_report_entry *entry,
                              const hs_fp_collector_t *collector,
                              const struct hs_fp_counter *counter) {
    entry->fragment_id = ROSE_OFFSET_INVALID;
    entry->trigger_count = counter->trigger_count;
    entry->true_trigger_count = counter->true_trigger_count;
    entry->final_report_count = counter->final_report_count;

    const struct RoseFpFragmentMeta *meta =
        find_fragment_meta(collector->rose, counter->key);
    if (!meta) {
        return 0;
    }

    entry->key = meta->stableKey;
    entry->fragment_id = meta->fragmentId;
    entry->literal_count = meta->literalCount;
    entry->table = meta->table;
    entry->engine = meta->engine;
    entry->flags = meta->flags;
    entry->length = meta->length;
    entry->mask_length = meta->maskLength;
    memcpy(entry->bytes, meta->bytes, sizeof(entry->bytes));
    memcpy(entry->mask, meta->mask, sizeof(entry->mask));
    memcpy(entry->cmp, meta->cmp, sizeof(entry->cmp));
    return 1;
}

static u64a count_total_false_positive_triggers(const hs_fp_report_t *report) {
    u64a total = 0;
    for (u32 i = 0; i < report->entry_count; i++) {
        const struct hs_fp_report_entry *entry = report->entries + i;
        u64a fp =
            sub_or_zero_u64a(entry->trigger_count, entry->true_trigger_count);
        total = sat_add_u64a(total, fp);
    }
    return total;
}

static char report_entry_is_bad_fragment(
    const struct hs_fp_report_entry *entry, u64a total_false_positive,
    const struct hs_fp_feedback_build_options *options) {
    if (entry->fragment_id == ROSE_OFFSET_INVALID ||
        entry->table == HS_FP_TABLE_UNKNOWN ||
        entry->engine == HS_FP_ENGINE_UNKNOWN || !entry->length) {
        return 0;
    }

    u64a fp = sub_or_zero_u64a(entry->trigger_count, entry->true_trigger_count);

    if (!fp || entry->trigger_count < options->min_trigger_count ||
        fp < options->min_false_positive_count) {
        return 0;
    }

    if (!ratio_at_least(fp, entry->trigger_count,
                        options->min_false_positive_rate,
                        HS_FP_FEEDBACK_RATE_SCALE)) {
        return 0;
    }

    if (!ratio_at_least(fp, total_false_positive, options->min_waste_share,
                        HS_FP_FEEDBACK_RATE_SCALE)) {
        return 0;
    }

    return 1;
}

static u32
count_bad_fragments(const hs_fp_report_t *report, u64a total_false_positive,
                    const struct hs_fp_feedback_build_options *options) {
    u32 count = 0;
    for (u32 i = 0; i < report->entry_count; i++) {
        if (report_entry_is_bad_fragment(report->entries + i,
                                         total_false_positive, options)) {
            count++;
        }
    }
    return count;
}

static void fill_feedback_entry(struct hs_fp_feedback_entry *dst,
                                const struct hs_fp_report_entry *src) {
    dst->key = src->key;
    dst->fragment_id = src->fragment_id;
    dst->literal_count = src->literal_count;
    dst->table = src->table;
    dst->engine = src->engine;
    dst->flags = src->flags;
    dst->length = src->length;
    dst->mask_length = src->mask_length;
    memcpy(dst->bytes, src->bytes, sizeof(dst->bytes));
    memcpy(dst->mask, src->mask, sizeof(dst->mask));
    memcpy(dst->cmp, src->cmp, sizeof(dst->cmp));
    dst->trigger_count = src->trigger_count;
    dst->true_trigger_count = src->true_trigger_count;
    dst->final_report_count = src->final_report_count;
    dst->false_positive_trigger_count =
        sub_or_zero_u64a(src->trigger_count, src->true_trigger_count);
}

static hs_error_t
build_feedback_entries(const hs_fp_report_t *report, hs_fp_feedback_t *feedback,
                       u64a total_false_positive,
                       const struct hs_fp_feedback_build_options *options) {
    feedback->bad_fragment_count =
        count_bad_fragments(report, total_false_positive, options);
    if (!feedback->bad_fragment_count) {
        return HS_SUCCESS;
    }

    feedback->entries = hs_misc_alloc(sizeof(*feedback->entries) *
                                      feedback->bad_fragment_count);
    if (!feedback->entries) {
        return HS_NOMEM;
    }
    memset(feedback->entries, 0,
           sizeof(*feedback->entries) * feedback->bad_fragment_count);

    u32 out = 0;
    for (u32 i = 0; i < report->entry_count; i++) {
        const struct hs_fp_report_entry *entry = report->entries + i;
        if (!report_entry_is_bad_fragment(entry, total_false_positive,
                                          options)) {
            continue;
        }

        fill_feedback_entry(feedback->entries + out, entry);
        out++;
    }
    assert(out == feedback->bad_fragment_count);

    if (options->max_bad_fragments &&
        feedback->bad_fragment_count > options->max_bad_fragments) {
        qsort(feedback->entries, feedback->bad_fragment_count,
              sizeof(*feedback->entries), compare_feedback_entry_score);
        feedback->bad_fragment_count = options->max_bad_fragments;
    }

    return HS_SUCCESS;
}

static hs_error_t build_report_entries(const hs_fp_collector_t *collector,
                                       hs_fp_report_t *report) {
    const u32 entry_count = count_used_counters(collector);
    if (!entry_count) {
        return HS_SUCCESS;
    }

    report->entries = hs_misc_alloc(sizeof(*report->entries) * entry_count);
    if (!report->entries) {
        return HS_NOMEM;
    }
    memset(report->entries, 0, sizeof(*report->entries) * entry_count);
    report->entry_count = entry_count;

    u32 entry = 0;
    for (u32 i = 0; i < collector->counter_capacity; i++) {
        const struct hs_fp_counter *counter = collector->counters + i;
        if (!counter->used) {
            continue;
        }

        if (!fill_report_entry(report->entries + entry, collector, counter)) {
            report->unknown_fragment_meta_missing_count =
                sat_add_u64a(report->unknown_fragment_meta_missing_count, 1);
        }
        entry++;
    }
    assert(entry == entry_count);

    qsort(report->entries, report->entry_count, sizeof(*report->entries),
          compare_report_entry);
    return HS_SUCCESS;
}

static void clear_collector_counts(hs_fp_collector_t *collector) {
    collector->scan_bytes = 0;
    collector->scan_calls = 0;
    collector->trigger_count = 0;
    collector->true_trigger_count = 0;
    collector->final_report_count = 0;
    collector->unknown_report_count = 0;
    collector->unknown_no_active_trigger_count = 0;
    collector->unknown_delayed_replay_count = 0;
    collector->unknown_anchored_replay_count = 0;
    collector->unknown_eod_or_boundary_count = 0;
    collector->unknown_flush_combination_count = 0;
    collector->unknown_mpv_or_nfa_queue_count = 0;
    collector->unknown_counter_missing_count = 0;
    collector->dropped_trigger_count = 0;
    collector->pending_trigger_count = 0;

    if (collector->counters) {
        memset(collector->counters, 0,
               sizeof(*collector->counters) * collector->counter_capacity);
    }
}

static struct hs_fp_counter *get_counter(hs_fp_collector_t *collector, u32 key,
                                         char create) {
    if (!collector || !collector->counters || !collector->counter_capacity) {
        return NULL;
    }

    const u32 mask = collector->counter_capacity - 1;
    u32 idx = hash_u32(key) & mask;
    for (u32 i = 0; i < collector->counter_capacity; i++) {
        struct hs_fp_counter *counter = collector->counters + idx;
        if (!counter->used) {
            if (!create) {
                return NULL;
            }
            counter->used = 1;
            counter->key = key;
            return counter;
        }
        if (counter->key == key) {
            return counter;
        }
        idx = (idx + 1) & mask;
    }

    return NULL;
}

static void merge_counter(hs_fp_collector_t *dst,
                          const struct hs_fp_counter *src) {
    struct hs_fp_counter *dst_counter = get_counter(dst, src->key, 1);
    if (!dst_counter) {
        dst->dropped_trigger_count =
            sat_add_u64a(dst->dropped_trigger_count, src->trigger_count);
        return;
    }

    dst_counter->trigger_count =
        sat_add_u64a(dst_counter->trigger_count, src->trigger_count);
    dst_counter->true_trigger_count =
        sat_add_u64a(dst_counter->true_trigger_count, src->true_trigger_count);
    dst_counter->final_report_count =
        sat_add_u64a(dst_counter->final_report_count, src->final_report_count);
}

static void add_counter_trigger_count(void *ctx, u32 key, u64a count) {
    hs_fp_collector_t *collector = ctx;
    struct hs_fp_counter *counter = get_counter(collector, key, 1);
    if (!counter) {
        collector->dropped_trigger_count =
            sat_add_u64a(collector->dropped_trigger_count, count);
        return;
    }

    counter->trigger_count = sat_add_u64a(counter->trigger_count, count);
}

void hs_fp_collector_flush(hs_fp_collector_t *collector) {
    if (!collector || !collector->pending_trigger_count) {
        return;
    }

    const u32 count = collector->pending_trigger_count;
    collector->pending_trigger_count = 0;
    hs_fp_histogram_count_batch(collector->histogram_backend,
                                collector->pending_trigger_keys, count,
                                add_counter_trigger_count, collector);
}

static void queue_trigger_key(hs_fp_collector_t *collector, u32 key) {
    if (collector->pending_trigger_count ==
        HS_FP_TRIGGER_HISTOGRAM_BATCH_SIZE) {
        hs_fp_collector_flush(collector);
    }

    assert(collector->pending_trigger_count <
           HS_FP_TRIGGER_HISTOGRAM_BATCH_SIZE);
    collector->pending_trigger_keys[collector->pending_trigger_count++] = key;
}

static hs_error_t
validate_collector_database(const hs_database_t *db,
                            const struct RoseEngine **rose_out) {
    hs_error_t err = validDatabase(db);
    if (err != HS_SUCCESS) {
        return err;
    }

    const struct RoseEngine *rose = hs_get_bytecode(db);
    if (!ISALIGNED_16(rose)) {
        return HS_INVALID;
    }

    *rose_out = rose;
    return HS_SUCCESS;
}

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_collector_create(const hs_database_t *db,
                                           hs_fp_collector_t **collector) {
    if (!collector) {
        return HS_INVALID;
    }
    *collector = NULL;

    const struct RoseEngine *rose = NULL;
    hs_error_t err = validate_collector_database(db, &rose);
    if (err != HS_SUCCESS) {
        return err;
    }

    hs_fp_collector_t *c = hs_misc_alloc(sizeof(*c));
    if (!c) {
        return HS_NOMEM;
    }

    memset(c, 0, sizeof(*c));
    c->db = db;
    c->rose = rose;
    c->histogram_backend = hs_fp_histogram_select_backend();

    c->counter_capacity = choose_counter_capacity(rose);
    c->counters = hs_misc_alloc(sizeof(*c->counters) * c->counter_capacity);
    if (!c->counters) {
        hs_misc_free(c);
        return HS_NOMEM;
    }
    memset(c->counters, 0, sizeof(*c->counters) * c->counter_capacity);

    *collector = c;
    return HS_SUCCESS;
}

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_collector_reset(hs_fp_collector_t *collector) {
    if (!collector) {
        return HS_INVALID;
    }

    clear_collector_counts(collector);
    return HS_SUCCESS;
}

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_collector_merge(hs_fp_collector_t *dst,
                                          const hs_fp_collector_t *src) {
    if (!dst || !src) {
        return HS_INVALID;
    }

    if (dst->rose != src->rose) {
        return HS_INVALID;
    }

    hs_fp_collector_flush(dst);

    dst->scan_bytes = sat_add_u64a(dst->scan_bytes, src->scan_bytes);
    dst->scan_calls = sat_add_u64a(dst->scan_calls, src->scan_calls);
    dst->trigger_count = sat_add_u64a(dst->trigger_count, src->trigger_count);
    dst->true_trigger_count =
        sat_add_u64a(dst->true_trigger_count, src->true_trigger_count);
    dst->final_report_count =
        sat_add_u64a(dst->final_report_count, src->final_report_count);
    dst->unknown_report_count =
        sat_add_u64a(dst->unknown_report_count, src->unknown_report_count);
    dst->unknown_no_active_trigger_count =
        sat_add_u64a(dst->unknown_no_active_trigger_count,
                     src->unknown_no_active_trigger_count);
    dst->unknown_delayed_replay_count = sat_add_u64a(
        dst->unknown_delayed_replay_count, src->unknown_delayed_replay_count);
    dst->unknown_anchored_replay_count = sat_add_u64a(
        dst->unknown_anchored_replay_count, src->unknown_anchored_replay_count);
    dst->unknown_eod_or_boundary_count = sat_add_u64a(
        dst->unknown_eod_or_boundary_count, src->unknown_eod_or_boundary_count);
    dst->unknown_flush_combination_count =
        sat_add_u64a(dst->unknown_flush_combination_count,
                     src->unknown_flush_combination_count);
    dst->unknown_mpv_or_nfa_queue_count =
        sat_add_u64a(dst->unknown_mpv_or_nfa_queue_count,
                     src->unknown_mpv_or_nfa_queue_count);
    dst->unknown_counter_missing_count = sat_add_u64a(
        dst->unknown_counter_missing_count, src->unknown_counter_missing_count);
    dst->dropped_trigger_count =
        sat_add_u64a(dst->dropped_trigger_count, src->dropped_trigger_count);

    for (u32 i = 0; i < src->counter_capacity; i++) {
        const struct hs_fp_counter *counter = src->counters + i;
        if (counter->used) {
            merge_counter(dst, counter);
        }
    }

    return HS_SUCCESS;
}

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_collector_report(const hs_fp_collector_t *collector,
                                           hs_fp_report_t **report) {
    if (!collector || !report) {
        return HS_INVALID;
    }
    *report = NULL;

    hs_fp_report_t *r = hs_misc_alloc(sizeof(*r));
    if (!r) {
        return HS_NOMEM;
    }
    memset(r, 0, sizeof(*r));

    r->db = collector->db;
    r->rose = collector->rose;
    r->scan_bytes = collector->scan_bytes;
    r->scan_calls = collector->scan_calls;
    r->trigger_count = collector->trigger_count;
    r->true_trigger_count = collector->true_trigger_count;
    r->final_report_count = collector->final_report_count;
    r->unknown_report_count = collector->unknown_report_count;
    r->unknown_no_active_trigger_count =
        collector->unknown_no_active_trigger_count;
    r->unknown_delayed_replay_count = collector->unknown_delayed_replay_count;
    r->unknown_anchored_replay_count = collector->unknown_anchored_replay_count;
    r->unknown_eod_or_boundary_count = collector->unknown_eod_or_boundary_count;
    r->unknown_flush_combination_count =
        collector->unknown_flush_combination_count;
    r->unknown_mpv_or_nfa_queue_count =
        collector->unknown_mpv_or_nfa_queue_count;
    r->unknown_counter_missing_count = collector->unknown_counter_missing_count;
    r->dropped_trigger_count = collector->dropped_trigger_count;

    hs_error_t err = build_report_entries(collector, r);
    if (err != HS_SUCCESS) {
        hs_misc_free(r);
        return err;
    }

    *report = r;
    return HS_SUCCESS;
}

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_collector_free(hs_fp_collector_t *collector) {
    if (collector) {
        hs_misc_free(collector->counters);
        hs_misc_free(collector);
    }
    return HS_SUCCESS;
}

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_report_free(hs_fp_report_t *report) {
    if (report) {
        hs_misc_free(report->entries);
        hs_misc_free(report);
    }
    return HS_SUCCESS;
}

hs_error_t HS_CDECL hs_fp_report_get_summary(const hs_fp_report_t *report,
                                             hs_fp_report_summary_t *summary) {
    if (!report || !summary) {
        return HS_INVALID;
    }

    memset(summary, 0, sizeof(*summary));
    summary->fragment_count = report->entry_count;
    summary->scan_calls = report->scan_calls;
    summary->scan_bytes = report->scan_bytes;
    summary->trigger_count = report->trigger_count;
    summary->true_trigger_count = report->true_trigger_count;
    summary->final_report_count = report->final_report_count;
    summary->false_positive_count = count_total_false_positive_triggers(report);
    summary->unknown_report_count = report->unknown_report_count;
    summary->unknown_no_active_trigger_count =
        report->unknown_no_active_trigger_count;
    summary->unknown_delayed_replay_count =
        report->unknown_delayed_replay_count;
    summary->unknown_anchored_replay_count =
        report->unknown_anchored_replay_count;
    summary->unknown_eod_or_boundary_count =
        report->unknown_eod_or_boundary_count;
    summary->unknown_flush_combination_count =
        report->unknown_flush_combination_count;
    summary->unknown_mpv_or_nfa_queue_count =
        report->unknown_mpv_or_nfa_queue_count;
    summary->unknown_counter_missing_count =
        report->unknown_counter_missing_count;
    summary->unknown_fragment_meta_missing_count =
        report->unknown_fragment_meta_missing_count;
    summary->dropped_trigger_count = report->dropped_trigger_count;
    return HS_SUCCESS;
}

hs_error_t HS_CDECL hs_fp_report_get_fragment(const hs_fp_report_t *report,
                                              unsigned int index,
                                              hs_fp_fragment_info_t *fragment) {
    if (!report || !fragment || index >= report->entry_count) {
        return HS_INVALID;
    }

    const struct hs_fp_report_entry *entry = report->entries + index;
    memset(fragment, 0, sizeof(*fragment));
    fragment->key = entry->key;
    fragment->fragment_id = entry->fragment_id;
    fragment->literal_count = entry->literal_count;
    fragment->table = entry->table;
    fragment->engine = entry->engine;
    fragment->flags = entry->flags;
    fragment->bytes = entry->bytes;
    fragment->length = entry->length;
    fragment->mask = entry->mask_length ? entry->mask : NULL;
    fragment->cmp = entry->mask_length ? entry->cmp : NULL;
    fragment->mask_length = entry->mask_length;
    fragment->trigger_count = entry->trigger_count;
    fragment->true_trigger_count = entry->true_trigger_count;
    fragment->final_report_count = entry->final_report_count;
    fragment->false_positive_count =
        sub_or_zero_u64a(entry->trigger_count, entry->true_trigger_count);
    return HS_SUCCESS;
}

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_feedback_build(const hs_fp_report_t *report,
                                         hs_fp_feedback_t **feedback) {
    return hs_fp_feedback_build_ext(report, NULL, feedback);
}

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_feedback_build_ext(
    const hs_fp_report_t *report, const hs_fp_feedback_params_t *params,
    hs_fp_feedback_t **feedback) {
    if (!report || !feedback) {
        return HS_INVALID;
    }
    *feedback = NULL;

    struct hs_fp_feedback_build_options options;
    hs_error_t err = make_feedback_build_options(params, &options);
    if (err != HS_SUCCESS) {
        return err;
    }

    hs_fp_feedback_t *f = hs_misc_alloc(sizeof(*f));
    if (!f) {
        return HS_NOMEM;
    }

    memset(f, 0, sizeof(*f));
    f->scan_bytes = report->scan_bytes;
    f->scan_calls = report->scan_calls;
    f->total_false_positive_trigger_count =
        count_total_false_positive_triggers(report);

    err = build_feedback_entries(
        report, f, f->total_false_positive_trigger_count, &options);
    if (err != HS_SUCCESS) {
        hs_misc_free(f);
        return err;
    }

    *feedback = f;
    return HS_SUCCESS;
}

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_feedback_free(hs_fp_feedback_t *feedback) {
    if (feedback) {
        hs_misc_free(feedback->entries);
        hs_misc_free(feedback);
    }
    return HS_SUCCESS;
}

hs_error_t HS_CDECL hs_fp_feedback_get_summary(
    const hs_fp_feedback_t *feedback, hs_fp_feedback_summary_t *summary) {
    if (!feedback || !summary) {
        return HS_INVALID;
    }

    memset(summary, 0, sizeof(*summary));
    summary->bad_fragment_count = feedback->bad_fragment_count;
    summary->scan_calls = feedback->scan_calls;
    summary->scan_bytes = feedback->scan_bytes;
    summary->total_false_positive_count =
        feedback->total_false_positive_trigger_count;
    return HS_SUCCESS;
}

hs_error_t HS_CDECL hs_fp_feedback_get_fragment(
    const hs_fp_feedback_t *feedback, unsigned int index,
    hs_fp_fragment_info_t *fragment) {
    if (!feedback || !fragment || index >= feedback->bad_fragment_count) {
        return HS_INVALID;
    }

    const struct hs_fp_feedback_entry *entry = feedback->entries + index;
    memset(fragment, 0, sizeof(*fragment));
    fragment->key = entry->key;
    fragment->fragment_id = entry->fragment_id;
    fragment->literal_count = entry->literal_count;
    fragment->table = entry->table;
    fragment->engine = entry->engine;
    fragment->flags = entry->flags;
    fragment->bytes = entry->bytes;
    fragment->length = entry->length;
    fragment->mask = entry->mask_length ? entry->mask : NULL;
    fragment->cmp = entry->mask_length ? entry->cmp : NULL;
    fragment->mask_length = entry->mask_length;
    fragment->trigger_count = entry->trigger_count;
    fragment->true_trigger_count = entry->true_trigger_count;
    fragment->final_report_count = entry->final_report_count;
    fragment->false_positive_count = entry->false_positive_trigger_count;
    return HS_SUCCESS;
}

hs_error_t hs_fp_feedback_clone(const hs_fp_feedback_t *src,
                                hs_fp_feedback_t **dst) {
    if (!src || !dst) {
        return HS_INVALID;
    }
    *dst = NULL;

    hs_fp_feedback_t *copy = hs_misc_alloc(sizeof(*copy));
    if (!copy) {
        return HS_NOMEM;
    }
    memset(copy, 0, sizeof(*copy));

    copy->bad_fragment_count = src->bad_fragment_count;
    copy->scan_bytes = src->scan_bytes;
    copy->scan_calls = src->scan_calls;
    copy->total_false_positive_trigger_count =
        src->total_false_positive_trigger_count;

    if (src->bad_fragment_count) {
        copy->entries =
            hs_misc_alloc(sizeof(*copy->entries) * src->bad_fragment_count);
        if (!copy->entries) {
            hs_misc_free(copy);
            return HS_NOMEM;
        }
        memcpy(copy->entries, src->entries,
               sizeof(*copy->entries) * src->bad_fragment_count);
    }

    *dst = copy;
    return HS_SUCCESS;
}

static char import_fragment_is_valid(
    const struct hs_fp_feedback_import_fragment *fragment) {
    if (!fragment || fragment->fragment_id == ROSE_OFFSET_INVALID ||
        fragment->table == HS_FP_TABLE_UNKNOWN ||
        fragment->engine == HS_FP_ENGINE_UNKNOWN || !fragment->bytes ||
        !fragment->length || fragment->length > ROSE_FP_FRAGMENT_BYTES_MAX ||
        fragment->mask_length > ROSE_FP_FRAGMENT_BYTES_MAX) {
        return 0;
    }
    if (fragment->mask_length && (!fragment->mask || !fragment->cmp)) {
        return 0;
    }
    return 1;
}

hs_error_t hs_fp_feedback_create_from_fragments(
    const struct hs_fp_feedback_import_fragment *fragments, u32 fragment_count,
    u64a scan_calls, u64a scan_bytes, u64a total_false_positive_count,
    hs_fp_feedback_t **feedback) {
    if (!feedback || (fragment_count && !fragments)) {
        return HS_INVALID;
    }
    *feedback = NULL;

    hs_fp_feedback_t *f = hs_misc_alloc(sizeof(*f));
    if (!f) {
        return HS_NOMEM;
    }
    memset(f, 0, sizeof(*f));

    f->bad_fragment_count = fragment_count;
    f->scan_calls = scan_calls;
    f->scan_bytes = scan_bytes;
    f->total_false_positive_trigger_count = total_false_positive_count;

    if (fragment_count) {
        f->entries = hs_misc_alloc(sizeof(*f->entries) * fragment_count);
        if (!f->entries) {
            hs_misc_free(f);
            return HS_NOMEM;
        }
        memset(f->entries, 0, sizeof(*f->entries) * fragment_count);
    }

    for (u32 i = 0; i < fragment_count; i++) {
        const struct hs_fp_feedback_import_fragment *src = fragments + i;
        if (!import_fragment_is_valid(src)) {
            hs_fp_feedback_free(f);
            return HS_INVALID;
        }

        struct hs_fp_feedback_entry *dst = f->entries + i;
        dst->key = src->key;
        dst->fragment_id = src->fragment_id;
        dst->literal_count = src->literal_count;
        dst->table = (u8)src->table;
        dst->engine = (u8)src->engine;
        dst->flags = (u8)src->flags;
        dst->length = (u8)src->length;
        dst->mask_length = (u8)src->mask_length;
        memcpy(dst->bytes, src->bytes, src->length);
        if (src->mask_length) {
            memcpy(dst->mask, src->mask, src->mask_length);
            memcpy(dst->cmp, src->cmp, src->mask_length);
        }
        dst->trigger_count = src->trigger_count;
        dst->true_trigger_count = src->true_trigger_count;
        dst->final_report_count = src->final_report_count;
        dst->false_positive_trigger_count = src->false_positive_count;
    }

    *feedback = f;
    return HS_SUCCESS;
}

static char
feedback_entry_matches_meta(const struct hs_fp_feedback_entry *entry,
                            const struct RoseFpFragmentMeta *meta) {
    if (entry->key != meta->stableKey || entry->table != meta->table ||
        entry->flags != meta->flags || entry->length != meta->length ||
        entry->mask_length != meta->maskLength) {
        return 0;
    }

    if (!bytes_equal_short_neon(entry->bytes, meta->bytes, entry->length)) {
        return 0;
    }

    if (entry->mask_length &&
        (!bytes_equal_short_neon(entry->mask, meta->mask, entry->mask_length) ||
         !bytes_equal_short_neon(entry->cmp, meta->cmp, entry->mask_length))) {
        return 0;
    }

    return 1;
}

u32 hs_fp_feedback_count_matches_in_rose(const hs_fp_feedback_t *feedback,
                                         const struct RoseEngine *rose,
                                         u32 *checked_count) {
    if (checked_count) {
        *checked_count = 0;
    }
    if (!feedback || !feedback->bad_fragment_count || !rose) {
        return 0;
    }

    if (checked_count) {
        *checked_count = feedback->bad_fragment_count;
    }

    const struct RoseFpFragmentMeta *meta = getRoseFpFragmentMeta(rose);
    if (!meta) {
        return 0;
    }

    u32 hit_count = 0;
    for (u32 i = 0; i < feedback->bad_fragment_count; i++) {
        const struct hs_fp_feedback_entry *entry = feedback->entries + i;
        for (u32 j = 0; j < rose->fpFragmentMetaCount; j++) {
            if (feedback_entry_matches_meta(entry, meta + j)) {
                hit_count++;
                break;
            }
        }
    }

    return hit_count;
}

char hs_fp_feedback_literal_is_bad(const hs_fp_feedback_t *feedback,
                                   const char *bytes, size_t length,
                                   char nocase) {
    if (!feedback || !feedback->bad_fragment_count || !bytes || !length) {
        return 0;
    }

    const u8 flags = nocase ? ROSE_FP_FRAGMENT_FLAG_NOCASE : 0;

    for (u32 i = 0; i < feedback->bad_fragment_count; i++) {
        const struct hs_fp_feedback_entry *entry = feedback->entries + i;
        if (!entry->length || entry->length > length || entry->mask_length ||
            ((entry->flags & ROSE_FP_FRAGMENT_FLAG_NOCASE) != flags)) {
            continue;
        }

        const char *suffix = bytes + length - entry->length;
        if (bytes_equal_short_neon(entry->bytes, (const u8 *)suffix,
                                   entry->length)) {
            return 1;
        }
    }

    return 0;
}

char hs_fp_feedback_fragment_is_bad(const hs_fp_feedback_t *feedback,
                                    const char *bytes, size_t length,
                                    char nocase, const u8 *mask, const u8 *cmp,
                                    size_t mask_length) {
    if (!feedback || !feedback->bad_fragment_count || !bytes || !length) {
        return 0;
    }
    if (mask_length && (!mask || !cmp)) {
        return 0;
    }

    const u8 nocase_flag = nocase ? ROSE_FP_FRAGMENT_FLAG_NOCASE : 0;

    for (u32 i = 0; i < feedback->bad_fragment_count; i++) {
        const struct hs_fp_feedback_entry *entry = feedback->entries + i;
        if (!entry->length || entry->length > length ||
            ((entry->flags & ROSE_FP_FRAGMENT_FLAG_NOCASE) != nocase_flag)) {
            continue;
        }

        const char *suffix = bytes + length - entry->length;
        if (!bytes_equal_short_neon(entry->bytes, (const u8 *)suffix,
                                    entry->length)) {
            continue;
        }

        if (entry->mask_length != mask_length) {
            continue;
        }

        if (mask_length &&
            (!bytes_equal_short_neon(entry->mask, mask, mask_length) ||
             !bytes_equal_short_neon(entry->cmp, cmp, mask_length))) {
            continue;
        }

        return 1;
    }

    return 0;
}

hs_error_t hs_fp_collector_check_db(const hs_fp_collector_t *collector,
                                    const hs_database_t *db) {
    if (!collector) {
        return HS_INVALID;
    }

    const struct RoseEngine *rose = NULL;
    hs_error_t err = validate_collector_database(db, &rose);
    if (err != HS_SUCCESS) {
        return err;
    }

    return hs_fp_collector_check_rose(collector, rose);
}

hs_error_t hs_fp_collector_check_rose(const hs_fp_collector_t *collector,
                                      const struct RoseEngine *rose) {
    if (!collector || !rose) {
        return HS_INVALID;
    }

    if (collector->rose != rose) {
        return HS_INVALID;
    }

    return HS_SUCCESS;
}

void hs_fp_collector_record_scan(hs_fp_collector_t *collector, size_t bytes) {
    if (!collector) {
        return;
    }

    collector->scan_bytes = sat_add_u64a(collector->scan_bytes, (u64a)bytes);
    collector->scan_calls = sat_add_u64a(collector->scan_calls, 1);
    hs_fp_collector_flush(collector);
}

void hs_fp_collector_begin_trigger(struct hs_scratch *scratch, u32 key) {
    if (!scratch) {
        return;
    }

    struct core_info *ci = &scratch->core_info;
    ci->fp_current_trigger_active = 0;
    ci->fp_current_trigger_reported = 0;
    ci->fp_current_trigger_key = 0;

    hs_fp_collector_t *collector = ci->fp_collector;
    if (!collector) {
        return;
    }

    collector->trigger_count = sat_add_u64a(collector->trigger_count, 1);
    queue_trigger_key(collector, key);
    ci->fp_current_trigger_active = 1;
    ci->fp_current_trigger_key = key;
}

void hs_fp_collector_end_trigger(struct hs_scratch *scratch) {
    if (!scratch) {
        return;
    }

    scratch->core_info.fp_current_trigger_active = 0;
    scratch->core_info.fp_current_trigger_reported = 0;
    scratch->core_info.fp_current_trigger_key = 0;
}

static void record_unknown_no_active_source(hs_fp_collector_t *collector,
                                            u8 source) {
    switch (source) {
    case HS_FP_UNKNOWN_SOURCE_DELAYED_REPLAY:
        collector->unknown_delayed_replay_count =
            sat_add_u64a(collector->unknown_delayed_replay_count, 1);
        break;
    case HS_FP_UNKNOWN_SOURCE_ANCHORED_REPLAY:
        collector->unknown_anchored_replay_count =
            sat_add_u64a(collector->unknown_anchored_replay_count, 1);
        break;
    case HS_FP_UNKNOWN_SOURCE_EOD_OR_BOUNDARY:
        collector->unknown_eod_or_boundary_count =
            sat_add_u64a(collector->unknown_eod_or_boundary_count, 1);
        break;
    case HS_FP_UNKNOWN_SOURCE_FLUSH_COMBINATION:
        collector->unknown_flush_combination_count =
            sat_add_u64a(collector->unknown_flush_combination_count, 1);
        break;
    case HS_FP_UNKNOWN_SOURCE_MPV_OR_NFA_QUEUE:
        collector->unknown_mpv_or_nfa_queue_count =
            sat_add_u64a(collector->unknown_mpv_or_nfa_queue_count, 1);
        break;
    default:
        break;
    }
}

void hs_fp_collector_record_final_report(struct hs_scratch *scratch) {
    if (!scratch) {
        return;
    }

    struct core_info *ci = &scratch->core_info;
    hs_fp_collector_t *collector = ci->fp_collector;
    if (!collector) {
        return;
    }

    collector->final_report_count =
        sat_add_u64a(collector->final_report_count, 1);

    if (!ci->fp_current_trigger_active) {
        collector->unknown_report_count =
            sat_add_u64a(collector->unknown_report_count, 1);
        collector->unknown_no_active_trigger_count =
            sat_add_u64a(collector->unknown_no_active_trigger_count, 1);
        record_unknown_no_active_source(collector, ci->fp_unknown_source);
        return;
    }

    hs_fp_collector_flush(collector);

    struct hs_fp_counter *counter =
        get_counter(collector, ci->fp_current_trigger_key, 0);
    if (!counter) {
        collector->unknown_report_count =
            sat_add_u64a(collector->unknown_report_count, 1);
        collector->unknown_counter_missing_count =
            sat_add_u64a(collector->unknown_counter_missing_count, 1);
        return;
    }

    counter->final_report_count = sat_add_u64a(counter->final_report_count, 1);
    if (!ci->fp_current_trigger_reported) {
        ci->fp_current_trigger_reported = 1;
        collector->true_trigger_count =
            sat_add_u64a(collector->true_trigger_count, 1);
        counter->true_trigger_count =
            sat_add_u64a(counter->true_trigger_count, 1);
    }
}
