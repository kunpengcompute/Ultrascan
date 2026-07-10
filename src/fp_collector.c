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

#include "fp_collector.h"

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

#define HS_FP_BAD_MIN_TRIGGER_COUNT 1000ULL
#define HS_FP_BAD_MIN_FP_RATE_NUM 99ULL
#define HS_FP_BAD_MIN_FP_RATE_DEN 100ULL
#define HS_FP_BAD_MIN_WASTE_SHARE_NUM 5ULL
#define HS_FP_BAD_MIN_WASTE_SHARE_DEN 100ULL

#define HS_FP_SERIAL_MAGIC 0x50465348U /* "HSFP", little endian. */
#define HS_FP_SERIAL_VERSION 1U
#define HS_FP_SERIAL_KIND_REPORT 1U
#define HS_FP_SERIAL_KIND_FEEDBACK 2U
#define HS_FP_SERIAL_HEADER_SIZE 16U
#define HS_FP_SERIAL_REPORT_SUMMARY_SIZE 56U
#define HS_FP_SERIAL_FEEDBACK_SUMMARY_SIZE 24U
#define HS_FP_SERIAL_ENTRY_SIZE 92U

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
    u64a dropped_trigger_count;
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

struct hs_fp_writer {
    char *p;
};

struct hs_fp_reader {
    const char *p;
    const char *end;
};

static
u64a sat_add_u64a(u64a a, u64a b) {
    const u64a max = ~(u64a)0;
    if (max - a < b) {
        return max;
    }
    return a + b;
}

static
u64a sub_or_zero_u64a(u64a a, u64a b) {
    return a > b ? a - b : 0;
}

static
void fp_write_u32(struct hs_fp_writer *w, u32 v) {
    w->p[0] = (char)(v & 0xffU);
    w->p[1] = (char)((v >> 8) & 0xffU);
    w->p[2] = (char)((v >> 16) & 0xffU);
    w->p[3] = (char)((v >> 24) & 0xffU);
    w->p += sizeof(u32);
}

static
void fp_write_u64(struct hs_fp_writer *w, u64a v) {
    for (u32 i = 0; i < 8; i++) {
        w->p[i] = (char)((v >> (i * 8)) & 0xffU);
    }
    w->p += sizeof(u64a);
}

static
void fp_write_bytes(struct hs_fp_writer *w, const u8 *bytes, size_t len) {
    memcpy(w->p, bytes, len);
    w->p += len;
}

static
char fp_read_u32(struct hs_fp_reader *r, u32 *v) {
    if ((size_t)(r->end - r->p) < sizeof(u32)) {
        return 0;
    }

    const u8 *p = (const u8 *)r->p;
    *v = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
         ((u32)p[3] << 24);
    r->p += sizeof(u32);
    return 1;
}

static
char fp_read_u64(struct hs_fp_reader *r, u64a *v) {
    if ((size_t)(r->end - r->p) < sizeof(u64a)) {
        return 0;
    }

    const u8 *p = (const u8 *)r->p;
    u64a out = 0;
    for (u32 i = 0; i < 8; i++) {
        out |= (u64a)p[i] << (i * 8);
    }
    *v = out;
    r->p += sizeof(u64a);
    return 1;
}

static
char fp_read_bytes(struct hs_fp_reader *r, u8 *bytes, size_t len) {
    if ((size_t)(r->end - r->p) < len) {
        return 0;
    }

    memcpy(bytes, r->p, len);
    r->p += len;
    return 1;
}

static
char fp_serial_size(u32 count, size_t summary_size, size_t *size) {
    const size_t base = HS_FP_SERIAL_HEADER_SIZE + summary_size;
    if (count > (SIZE_MAX - base) / HS_FP_SERIAL_ENTRY_SIZE) {
        return 0;
    }

    *size = base + (size_t)count * HS_FP_SERIAL_ENTRY_SIZE;
    return 1;
}

static
char ratio_at_least(u64a num, u64a den, u64a min_num, u64a min_den) {
    if (!den) {
        return 0;
    }

    const u64a max = ~(u64a)0;
    if (num <= max / min_den && den <= max / min_num) {
        return num * min_den >= den * min_num;
    }

    long double lhs = (long double)num / (long double)den;
    long double rhs = (long double)min_num / (long double)min_den;
    return lhs >= rhs;
}

static
u32 hash_u32(u32 v) {
    v ^= v >> 16;
    v *= 0x7feb352dU;
    v ^= v >> 15;
    v *= 0x846ca68bU;
    v ^= v >> 16;
    return v;
}

static
u32 next_power_of_two_u32(u32 v) {
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

static
u32 choose_counter_capacity(const struct RoseEngine *rose) {
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

static
int compare_report_entry(const void *a, const void *b) {
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

static
const struct RoseFpFragmentMeta *find_fragment_meta(
        const struct RoseEngine *rose, u32 key) {
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

static
u32 count_used_counters(const hs_fp_collector_t *collector) {
    u32 count = 0;
    for (u32 i = 0; i < collector->counter_capacity; i++) {
        if (collector->counters[i].used) {
            count++;
        }
    }
    return count;
}

static
void fill_report_entry(struct hs_fp_report_entry *entry,
                       const hs_fp_collector_t *collector,
                       const struct hs_fp_counter *counter) {
    entry->fragment_id = ROSE_OFFSET_INVALID;
    entry->trigger_count = counter->trigger_count;
    entry->true_trigger_count = counter->true_trigger_count;
    entry->final_report_count = counter->final_report_count;

    const struct RoseFpFragmentMeta *meta =
        find_fragment_meta(collector->rose, counter->key);
    if (!meta) {
        return;
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
}

static
u64a count_total_false_positive_triggers(const hs_fp_report_t *report) {
    u64a total = 0;
    for (u32 i = 0; i < report->entry_count; i++) {
        const struct hs_fp_report_entry *entry = report->entries + i;
        u64a fp = sub_or_zero_u64a(entry->trigger_count,
                                   entry->true_trigger_count);
        total = sat_add_u64a(total, fp);
    }
    return total;
}

static
char report_entry_is_bad_fragment(const struct hs_fp_report_entry *entry,
                                  u64a total_false_positive) {
    u64a fp = sub_or_zero_u64a(entry->trigger_count,
                               entry->true_trigger_count);

    if (entry->trigger_count < HS_FP_BAD_MIN_TRIGGER_COUNT ||
        fp < HS_FP_BAD_MIN_TRIGGER_COUNT) {
        return 0;
    }

    if (!ratio_at_least(fp, entry->trigger_count,
                        HS_FP_BAD_MIN_FP_RATE_NUM,
                        HS_FP_BAD_MIN_FP_RATE_DEN)) {
        return 0;
    }

    if (!ratio_at_least(fp, total_false_positive,
                        HS_FP_BAD_MIN_WASTE_SHARE_NUM,
                        HS_FP_BAD_MIN_WASTE_SHARE_DEN)) {
        return 0;
    }

    return 1;
}

static
u32 count_bad_fragments(const hs_fp_report_t *report,
                        u64a total_false_positive) {
    u32 count = 0;
    for (u32 i = 0; i < report->entry_count; i++) {
        if (report_entry_is_bad_fragment(report->entries + i,
                                         total_false_positive)) {
            count++;
        }
    }
    return count;
}

static
void fill_feedback_entry(struct hs_fp_feedback_entry *dst,
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

static
void write_serial_entry(struct hs_fp_writer *w, u64a key, u32 fragment_id,
                        u32 literal_count, u8 table, u8 engine, u8 flags,
                        u8 length, u8 mask_length, const u8 *bytes,
                        const u8 *mask, const u8 *cmp, u64a trigger_count,
                        u64a true_trigger_count, u64a final_report_count,
                        u64a false_positive_count) {
    fp_write_u64(w, key);
    fp_write_u32(w, fragment_id);
    fp_write_u32(w, literal_count);
    fp_write_u32(w, table);
    fp_write_u32(w, engine);
    fp_write_u32(w, flags);
    fp_write_u32(w, length);
    fp_write_u32(w, mask_length);
    fp_write_bytes(w, bytes, ROSE_FP_FRAGMENT_BYTES_MAX);
    fp_write_bytes(w, mask, ROSE_FP_FRAGMENT_BYTES_MAX);
    fp_write_bytes(w, cmp, ROSE_FP_FRAGMENT_BYTES_MAX);
    fp_write_u64(w, trigger_count);
    fp_write_u64(w, true_trigger_count);
    fp_write_u64(w, final_report_count);
    fp_write_u64(w, false_positive_count);
}

static
void write_report_entry(struct hs_fp_writer *w,
                        const struct hs_fp_report_entry *entry) {
    write_serial_entry(w, entry->key, entry->fragment_id,
                       entry->literal_count, entry->table, entry->engine,
                       entry->flags, entry->length, entry->mask_length,
                       entry->bytes, entry->mask, entry->cmp,
                       entry->trigger_count, entry->true_trigger_count,
                       entry->final_report_count,
                       sub_or_zero_u64a(entry->trigger_count,
                                        entry->true_trigger_count));
}

static
void write_feedback_entry(struct hs_fp_writer *w,
                          const struct hs_fp_feedback_entry *entry) {
    write_serial_entry(w, entry->key, entry->fragment_id,
                       entry->literal_count, entry->table, entry->engine,
                       entry->flags, entry->length, entry->mask_length,
                       entry->bytes, entry->mask, entry->cmp,
                       entry->trigger_count, entry->true_trigger_count,
                       entry->final_report_count,
                       entry->false_positive_trigger_count);
}

static
char read_serial_entry(struct hs_fp_reader *r, u64a *key, u32 *fragment_id,
                       u32 *literal_count, u8 *table, u8 *engine, u8 *flags,
                       u8 *length, u8 *mask_length, u8 *bytes, u8 *mask,
                       u8 *cmp, u64a *trigger_count,
                       u64a *true_trigger_count, u64a *final_report_count,
                       u64a *false_positive_count) {
    u32 table32 = 0;
    u32 engine32 = 0;
    u32 flags32 = 0;
    u32 length32 = 0;
    u32 mask_length32 = 0;

    if (!fp_read_u64(r, key) ||
        !fp_read_u32(r, fragment_id) ||
        !fp_read_u32(r, literal_count) ||
        !fp_read_u32(r, &table32) ||
        !fp_read_u32(r, &engine32) ||
        !fp_read_u32(r, &flags32) ||
        !fp_read_u32(r, &length32) ||
        !fp_read_u32(r, &mask_length32) ||
        !fp_read_bytes(r, bytes, ROSE_FP_FRAGMENT_BYTES_MAX) ||
        !fp_read_bytes(r, mask, ROSE_FP_FRAGMENT_BYTES_MAX) ||
        !fp_read_bytes(r, cmp, ROSE_FP_FRAGMENT_BYTES_MAX) ||
        !fp_read_u64(r, trigger_count) ||
        !fp_read_u64(r, true_trigger_count) ||
        !fp_read_u64(r, final_report_count) ||
        !fp_read_u64(r, false_positive_count)) {
        return 0;
    }

    if (table32 > 0xffU || engine32 > 0xffU || flags32 > 0xffU ||
        length32 > ROSE_FP_FRAGMENT_BYTES_MAX ||
        mask_length32 > ROSE_FP_FRAGMENT_BYTES_MAX) {
        return 0;
    }

    *table = (u8)table32;
    *engine = (u8)engine32;
    *flags = (u8)flags32;
    *length = (u8)length32;
    *mask_length = (u8)mask_length32;
    return 1;
}

static
char read_report_entry(struct hs_fp_reader *r,
                       struct hs_fp_report_entry *entry) {
    u64a false_positive_count = 0;

    memset(entry, 0, sizeof(*entry));
    return read_serial_entry(r, &entry->key, &entry->fragment_id,
                             &entry->literal_count, &entry->table,
                             &entry->engine, &entry->flags, &entry->length,
                             &entry->mask_length, entry->bytes, entry->mask,
                             entry->cmp, &entry->trigger_count,
                             &entry->true_trigger_count,
                             &entry->final_report_count,
                             &false_positive_count);
}

static
char read_feedback_entry(struct hs_fp_reader *r,
                         struct hs_fp_feedback_entry *entry) {
    memset(entry, 0, sizeof(*entry));
    return read_serial_entry(r, &entry->key, &entry->fragment_id,
                             &entry->literal_count, &entry->table,
                             &entry->engine, &entry->flags, &entry->length,
                             &entry->mask_length, entry->bytes, entry->mask,
                             entry->cmp, &entry->trigger_count,
                             &entry->true_trigger_count,
                             &entry->final_report_count,
                             &entry->false_positive_trigger_count);
}

static
hs_error_t build_feedback_entries(const hs_fp_report_t *report,
                                  hs_fp_feedback_t *feedback,
                                  u64a total_false_positive) {
    feedback->bad_fragment_count =
        count_bad_fragments(report, total_false_positive);
    if (!feedback->bad_fragment_count) {
        return HS_SUCCESS;
    }

    feedback->entries =
        hs_misc_alloc(sizeof(*feedback->entries) *
                      feedback->bad_fragment_count);
    if (!feedback->entries) {
        return HS_NOMEM;
    }
    memset(feedback->entries, 0,
           sizeof(*feedback->entries) * feedback->bad_fragment_count);

    u32 out = 0;
    for (u32 i = 0; i < report->entry_count; i++) {
        const struct hs_fp_report_entry *entry = report->entries + i;
        if (!report_entry_is_bad_fragment(entry, total_false_positive)) {
            continue;
        }

        fill_feedback_entry(feedback->entries + out, entry);
        out++;
    }
    assert(out == feedback->bad_fragment_count);
    return HS_SUCCESS;
}

static
hs_error_t build_report_entries(const hs_fp_collector_t *collector,
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

        fill_report_entry(report->entries + entry, collector, counter);
        entry++;
    }
    assert(entry == entry_count);

    qsort(report->entries, report->entry_count, sizeof(*report->entries),
          compare_report_entry);
    return HS_SUCCESS;
}

static
void clear_collector_counts(hs_fp_collector_t *collector) {
    collector->scan_bytes = 0;
    collector->scan_calls = 0;
    collector->trigger_count = 0;
    collector->true_trigger_count = 0;
    collector->final_report_count = 0;
    collector->unknown_report_count = 0;
    collector->dropped_trigger_count = 0;

    if (collector->counters) {
        memset(collector->counters, 0,
               sizeof(*collector->counters) * collector->counter_capacity);
    }
}

static
struct hs_fp_counter *get_counter(hs_fp_collector_t *collector, u32 key,
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

static
void merge_counter(hs_fp_collector_t *dst, const struct hs_fp_counter *src) {
    struct hs_fp_counter *dst_counter = get_counter(dst, src->key, 1);
    if (!dst_counter) {
        dst->dropped_trigger_count =
            sat_add_u64a(dst->dropped_trigger_count, src->trigger_count);
        return;
    }

    dst_counter->trigger_count =
        sat_add_u64a(dst_counter->trigger_count, src->trigger_count);
    dst_counter->true_trigger_count =
        sat_add_u64a(dst_counter->true_trigger_count,
                     src->true_trigger_count);
    dst_counter->final_report_count =
        sat_add_u64a(dst_counter->final_report_count,
                     src->final_report_count);
}

static
hs_error_t validate_collector_database(const hs_database_t *db,
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

    dst->scan_bytes = sat_add_u64a(dst->scan_bytes, src->scan_bytes);
    dst->scan_calls = sat_add_u64a(dst->scan_calls, src->scan_calls);
    dst->trigger_count = sat_add_u64a(dst->trigger_count, src->trigger_count);
    dst->true_trigger_count =
        sat_add_u64a(dst->true_trigger_count, src->true_trigger_count);
    dst->final_report_count =
        sat_add_u64a(dst->final_report_count, src->final_report_count);
    dst->unknown_report_count =
        sat_add_u64a(dst->unknown_report_count, src->unknown_report_count);
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

hs_error_t HS_CDECL hs_fp_report_get_summary(
        const hs_fp_report_t *report, hs_fp_report_summary_t *summary) {
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
    summary->false_positive_count =
        count_total_false_positive_triggers(report);
    summary->unknown_report_count = report->unknown_report_count;
    summary->dropped_trigger_count = report->dropped_trigger_count;
    return HS_SUCCESS;
}

hs_error_t HS_CDECL hs_fp_report_get_fragment(
        const hs_fp_report_t *report, unsigned int index,
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
hs_error_t HS_CDECL hs_fp_report_serialize(const hs_fp_report_t *report,
                                           char **bytes, size_t *length) {
    if (!report || !bytes || !length) {
        return HS_INVALID;
    }
    *bytes = NULL;
    *length = 0;

    size_t size = 0;
    if (!fp_serial_size(report->entry_count,
                        HS_FP_SERIAL_REPORT_SUMMARY_SIZE, &size)) {
        return HS_NOMEM;
    }

    char *out = hs_misc_alloc(size);
    if (!out) {
        return HS_NOMEM;
    }
    memset(out, 0, size);

    struct hs_fp_writer w = {out};
    fp_write_u32(&w, HS_FP_SERIAL_MAGIC);
    fp_write_u32(&w, HS_FP_SERIAL_VERSION);
    fp_write_u32(&w, HS_FP_SERIAL_KIND_REPORT);
    fp_write_u32(&w, report->entry_count);
    fp_write_u64(&w, report->scan_bytes);
    fp_write_u64(&w, report->scan_calls);
    fp_write_u64(&w, report->trigger_count);
    fp_write_u64(&w, report->true_trigger_count);
    fp_write_u64(&w, report->final_report_count);
    fp_write_u64(&w, report->unknown_report_count);
    fp_write_u64(&w, report->dropped_trigger_count);

    for (u32 i = 0; i < report->entry_count; i++) {
        write_report_entry(&w, report->entries + i);
    }
    assert((size_t)(w.p - out) == size);

    *bytes = out;
    *length = size;
    return HS_SUCCESS;
}

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_report_deserialize(const char *bytes, size_t length,
                                             hs_fp_report_t **report) {
    if (!bytes || !report) {
        return HS_INVALID;
    }
    *report = NULL;

    struct hs_fp_reader r = {bytes, bytes + length};
    u32 magic = 0;
    u32 version = 0;
    u32 kind = 0;
    u32 entry_count = 0;
    if (!fp_read_u32(&r, &magic) || !fp_read_u32(&r, &version) ||
        !fp_read_u32(&r, &kind) || !fp_read_u32(&r, &entry_count)) {
        return HS_INVALID;
    }
    if (magic != HS_FP_SERIAL_MAGIC ||
        kind != HS_FP_SERIAL_KIND_REPORT) {
        return HS_INVALID;
    }
    if (version != HS_FP_SERIAL_VERSION) {
        return HS_DB_VERSION_ERROR;
    }

    size_t expected_size = 0;
    if (!fp_serial_size(entry_count, HS_FP_SERIAL_REPORT_SUMMARY_SIZE,
                        &expected_size) || expected_size != length) {
        return HS_INVALID;
    }

    hs_fp_report_t *out = hs_misc_alloc(sizeof(*out));
    if (!out) {
        return HS_NOMEM;
    }
    memset(out, 0, sizeof(*out));
    out->entry_count = entry_count;

    if (!fp_read_u64(&r, &out->scan_bytes) ||
        !fp_read_u64(&r, &out->scan_calls) ||
        !fp_read_u64(&r, &out->trigger_count) ||
        !fp_read_u64(&r, &out->true_trigger_count) ||
        !fp_read_u64(&r, &out->final_report_count) ||
        !fp_read_u64(&r, &out->unknown_report_count) ||
        !fp_read_u64(&r, &out->dropped_trigger_count)) {
        hs_fp_report_free(out);
        return HS_INVALID;
    }

    if (entry_count) {
        out->entries = hs_misc_alloc(sizeof(*out->entries) * entry_count);
        if (!out->entries) {
            hs_fp_report_free(out);
            return HS_NOMEM;
        }
        memset(out->entries, 0, sizeof(*out->entries) * entry_count);

        for (u32 i = 0; i < entry_count; i++) {
            if (!read_report_entry(&r, out->entries + i)) {
                hs_fp_report_free(out);
                return HS_INVALID;
            }
        }
    }

    if (r.p != r.end) {
        hs_fp_report_free(out);
        return HS_INVALID;
    }

    *report = out;
    return HS_SUCCESS;
}

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_feedback_build(const hs_fp_report_t *report,
                                         hs_fp_feedback_t **feedback) {
    if (!report || !feedback) {
        return HS_INVALID;
    }
    *feedback = NULL;

    hs_fp_feedback_t *f = hs_misc_alloc(sizeof(*f));
    if (!f) {
        return HS_NOMEM;
    }

    memset(f, 0, sizeof(*f));
    f->scan_bytes = report->scan_bytes;
    f->scan_calls = report->scan_calls;
    f->total_false_positive_trigger_count =
        count_total_false_positive_triggers(report);

    hs_error_t err =
        build_feedback_entries(report, f,
                               f->total_false_positive_trigger_count);
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

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_feedback_serialize(
        const hs_fp_feedback_t *feedback, char **bytes, size_t *length) {
    if (!feedback || !bytes || !length) {
        return HS_INVALID;
    }
    *bytes = NULL;
    *length = 0;

    size_t size = 0;
    if (!fp_serial_size(feedback->bad_fragment_count,
                        HS_FP_SERIAL_FEEDBACK_SUMMARY_SIZE, &size)) {
        return HS_NOMEM;
    }

    char *out = hs_misc_alloc(size);
    if (!out) {
        return HS_NOMEM;
    }
    memset(out, 0, size);

    struct hs_fp_writer w = {out};
    fp_write_u32(&w, HS_FP_SERIAL_MAGIC);
    fp_write_u32(&w, HS_FP_SERIAL_VERSION);
    fp_write_u32(&w, HS_FP_SERIAL_KIND_FEEDBACK);
    fp_write_u32(&w, feedback->bad_fragment_count);
    fp_write_u64(&w, feedback->scan_bytes);
    fp_write_u64(&w, feedback->scan_calls);
    fp_write_u64(&w, feedback->total_false_positive_trigger_count);

    for (u32 i = 0; i < feedback->bad_fragment_count; i++) {
        write_feedback_entry(&w, feedback->entries + i);
    }
    assert((size_t)(w.p - out) == size);

    *bytes = out;
    *length = size;
    return HS_SUCCESS;
}

HS_PUBLIC_API
hs_error_t HS_CDECL hs_fp_feedback_deserialize(
        const char *bytes, size_t length, hs_fp_feedback_t **feedback) {
    if (!bytes || !feedback) {
        return HS_INVALID;
    }
    *feedback = NULL;

    struct hs_fp_reader r = {bytes, bytes + length};
    u32 magic = 0;
    u32 version = 0;
    u32 kind = 0;
    u32 bad_fragment_count = 0;
    if (!fp_read_u32(&r, &magic) || !fp_read_u32(&r, &version) ||
        !fp_read_u32(&r, &kind) ||
        !fp_read_u32(&r, &bad_fragment_count)) {
        return HS_INVALID;
    }
    if (magic != HS_FP_SERIAL_MAGIC ||
        kind != HS_FP_SERIAL_KIND_FEEDBACK) {
        return HS_INVALID;
    }
    if (version != HS_FP_SERIAL_VERSION) {
        return HS_DB_VERSION_ERROR;
    }

    size_t expected_size = 0;
    if (!fp_serial_size(bad_fragment_count,
                        HS_FP_SERIAL_FEEDBACK_SUMMARY_SIZE,
                        &expected_size) || expected_size != length) {
        return HS_INVALID;
    }

    hs_fp_feedback_t *out = hs_misc_alloc(sizeof(*out));
    if (!out) {
        return HS_NOMEM;
    }
    memset(out, 0, sizeof(*out));
    out->bad_fragment_count = bad_fragment_count;

    if (!fp_read_u64(&r, &out->scan_bytes) ||
        !fp_read_u64(&r, &out->scan_calls) ||
        !fp_read_u64(&r, &out->total_false_positive_trigger_count)) {
        hs_fp_feedback_free(out);
        return HS_INVALID;
    }

    if (bad_fragment_count) {
        out->entries =
            hs_misc_alloc(sizeof(*out->entries) * bad_fragment_count);
        if (!out->entries) {
            hs_fp_feedback_free(out);
            return HS_NOMEM;
        }
        memset(out->entries, 0,
               sizeof(*out->entries) * bad_fragment_count);

        for (u32 i = 0; i < bad_fragment_count; i++) {
            if (!read_feedback_entry(&r, out->entries + i)) {
                hs_fp_feedback_free(out);
                return HS_INVALID;
            }
        }
    }

    if (r.p != r.end) {
        hs_fp_feedback_free(out);
        return HS_INVALID;
    }

    *feedback = out;
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

static
char feedback_entry_matches_meta(const struct hs_fp_feedback_entry *entry,
                                 const struct RoseFpFragmentMeta *meta) {
    if (entry->key != meta->stableKey ||
        entry->table != meta->table ||
        entry->flags != meta->flags ||
        entry->length != meta->length ||
        entry->mask_length != meta->maskLength) {
        return 0;
    }

    if (memcmp(entry->bytes, meta->bytes, entry->length)) {
        return 0;
    }

    if (entry->mask_length &&
        (memcmp(entry->mask, meta->mask, entry->mask_length) ||
         memcmp(entry->cmp, meta->cmp, entry->mask_length))) {
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
        if (!entry->length || entry->length > length ||
            entry->mask_length ||
            ((entry->flags & ROSE_FP_FRAGMENT_FLAG_NOCASE) != flags)) {
            continue;
        }

        const char *suffix = bytes + length - entry->length;
        if (!memcmp(entry->bytes, suffix, entry->length)) {
            return 1;
        }
    }

    return 0;
}

char hs_fp_feedback_fragment_is_bad(const hs_fp_feedback_t *feedback,
                                    const char *bytes, size_t length,
                                    char nocase, const u8 *mask,
                                    const u8 *cmp, size_t mask_length) {
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
        if (memcmp(entry->bytes, suffix, entry->length)) {
            continue;
        }

        if (entry->mask_length != mask_length) {
            continue;
        }

        if (mask_length &&
            (memcmp(entry->mask, mask, mask_length) ||
             memcmp(entry->cmp, cmp, mask_length))) {
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

    struct hs_fp_counter *counter = get_counter(collector, key, 1);
    if (!counter) {
        collector->dropped_trigger_count =
            sat_add_u64a(collector->dropped_trigger_count, 1);
        return;
    }

    counter->trigger_count = sat_add_u64a(counter->trigger_count, 1);
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
        return;
    }

    struct hs_fp_counter *counter =
        get_counter(collector, ci->fp_current_trigger_key, 0);
    if (!counter) {
        collector->unknown_report_count =
            sat_add_u64a(collector->unknown_report_count, 1);
        return;
    }

    counter->final_report_count =
        sat_add_u64a(counter->final_report_count, 1);
    if (!ci->fp_current_trigger_reported) {
        ci->fp_current_trigger_reported = 1;
        collector->true_trigger_count =
            sat_add_u64a(collector->true_trigger_count, 1);
        counter->true_trigger_count =
            sat_add_u64a(counter->true_trigger_count, 1);
    }
}
