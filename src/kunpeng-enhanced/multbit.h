/*
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * MMBit-related modifications
 * Copyright (c) 2015-2016, Intel Corporation
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

#ifndef MULTIBIT_H
#define MULTIBIT_H

#include "ue2common.h"
#include "rose_internal.h"
#include "scratch.h"
#include "simd_types.h"
#include "simd_arm.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef u64a MMB_TYPE;
#define MMB_MAX_LEVEL 6
#define MMB_MAX_BITS (1U << 31)
#define MMB_ONE (1ULL)
#define MMB_ALL_ONES (0xffffffffffffffffULL)

#ifdef MMB_TRACE_WRITES
#define MMB_TRACE(format, ...)                                                 \
    printf("MMb [%u bits @ %p] " format, total_bits, bits, ##__VA_ARGS__)
#else
#define MMB_TRACE(format, ...)                                                 \
    do {                                                                       \
    } while (0)
#endif

#define MMB_KEY_SHIFT 6
#define MMB_FLAT_MAX_BITS 256
#define MMB_KEY_BITS (sizeof(MMB_TYPE) * 8)
#define MMB_KEY_MASK (MMB_KEY_BITS - 1)

const u32 mmbitRootOffsetFromLevel[7] = {
    0,
    1,
    1 + (1 << MMB_KEY_SHIFT),
    1 + (1 << MMB_KEY_SHIFT) + (1 << MMB_KEY_SHIFT * 2),
    1 + (1 << MMB_KEY_SHIFT) + (1 << MMB_KEY_SHIFT * 2) + (1 << MMB_KEY_SHIFT * 3),
    1 + (1 << MMB_KEY_SHIFT) + (1 << MMB_KEY_SHIFT * 2) + (1 << MMB_KEY_SHIFT * 3) + (1 << MMB_KEY_SHIFT * 4),
    1 + (1 << MMB_KEY_SHIFT) + (1 << MMB_KEY_SHIFT * 2) + (1 << MMB_KEY_SHIFT * 3) + 
        (1 << MMB_KEY_SHIFT * 4) + (1 << MMB_KEY_SHIFT * 5),
};

const u8 mmbitMaxlevelDirectLut[32] = {
    5, 5, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 
    2, 2, 2, 2, 2, 2, 1, 1,
    1, 1, 1, 1, 
    0, 0, 0, 0, 0, 0
};

static REALLY_INLINE
u32 MMbit_maxlevel(u32 total_bits)
{
    u32 n = CLZ32(total_bits - 1); // subtract one as we're rounding down
    u32 max_level = mmbitMaxlevelDirectLut[n];
    return max_level;
}

static REALLY_INLINE
u32 MMbit_flat_select_byte(u32 key, UNUSED u32 total_bits)
{
    return key / 8;
}

static REALLY_INLINE
char MMbit_set_flat(u8 *bits, u32 total_bits, u32 key)
{
    bits += MMbit_flat_select_byte(key, total_bits);
    u8 mask = 1U << (key % 8);
    char was_set = !!(*bits & mask);
    *bits |= mask;
    return was_set;
}

static REALLY_INLINE
u8 *MMbit_get_level_root(u8 *bits, u32 level)
{
    return bits + mmbitRootOffsetFromLevel[level] * sizeof(MMB_TYPE);
}

static REALLY_INLINE
u32 MMbit_get_ks(u32 max_level, u32 level)
{
    return (max_level - level) * MMB_KEY_SHIFT;
}

static REALLY_INLINE
u8 *MMbit_get_byte_ptr(u8 *bits, u32 max_level, u32 level, u32 key)
{
    u8 *level_root = MMbit_get_level_root(bits, level);
    u32 ks = MMbit_get_ks(max_level, level);
    return level_root + ((u64a)key >> (ks + MMB_KEY_SHIFT - 3));
}

static REALLY_INLINE
u32 MMbit_get_key_val_byte(u32 max_level, u32 level, u32 key)
{
    return (key >> (MMbit_get_ks(max_level, level))) & 0x7;
}

static REALLY_INLINE
u8 *MMbit_get_block_ptr(u8 *bits, u32 max_level, u32 level, u32 key)
{
    u8 *level_root = MMbit_get_level_root(bits, level);
    u32 ks = MMbit_get_ks(max_level, level);
    return level_root + ((u64a)key >> (ks + MMB_KEY_SHIFT)) * sizeof(MMB_TYPE);
}

static REALLY_INLINE
MMB_TYPE MMb_single_bit(u32 bit)
{
    return MMB_ONE << bit;
}

static REALLY_INLINE
u32 MMbit_get_key_val(u32 max_level, u32 level, u32 key)
{
    return (key >> MMbit_get_ks(max_level, level)) & MMB_KEY_MASK;
}

static REALLY_INLINE
void MMb_store(u8 *bits, MMB_TYPE val)
{
    Unaligned_store_u64a(bits, val);
}

static REALLY_INLINE
char MMbit_set_big(u8 *bits, u32 total_bits, u32 key)
{
    const u32 max_level = MMbit_maxlevel(total_bits);
    u32 level = 0;
    do {
        u8 * byte_ptr = MMbit_get_byte_ptr(bits, max_level, level, key);
        u8 keymask = 1U << MMbit_get_key_val_byte(max_level, level, key);
        u8 byte = *byte_ptr;
        if (likely(!(byte & keymask))) {
            *byte_ptr = byte | keymask;
            while (level++ != max_level) {
                u8 *block_ptr_1 = MMbit_get_block_ptr(bits, max_level, level, key);
                MMB_TYPE keymask_1 = MMb_single_bit(MMbit_get_key_val(max_level, level, key));
                MMb_store(block_ptr_1, keymask_1);
            }
            return 0;
        }
    } while (level++ != max_level);
    return 1;
}

static REALLY_INLINE
u32 MMbit_is_flat_model(u32 total_bits)
{
    return total_bits <= MMB_FLAT_MAX_BITS;
}

static REALLY_INLINE
char MMbit_set_i(u8 *bits, u32 total_bits, u32 key)
{
    if (MMbit_is_flat_model(total_bits)) {
        return MMbit_set_flat(bits, total_bits, key);
    } else {
        return MMbit_set_big(bits, total_bits, key);
    }
}

static REALLY_INLINE
char MMbit_set(u8 *bits, u32 total_bits, u32 key)
{
    char status = MMbit_set_i(bits, total_bits, key);
    MMB_TRACE("SET %u (prev status: %d)\n", key, (int)status);
    return status;
}

static REALLY_INLINE
char MMbit_isset_flat(const u8 *bits, u32 total_bits, u32 key)
{
    bits += MMbit_flat_select_byte(key, total_bits);
    return !!(*bits & (1U << (key % 8U)));
}

static REALLY_INLINE
const u8 *MMbit_get_level_root_const(const u8 *bits, u32 level)
{
    return bits + mmbitRootOffsetFromLevel[level] * sizeof(MMB_TYPE);
}

static REALLY_INLINE
const u8 *MMbit_get_block_ptr_const(const u8 *bits, u32 max_level, u32 level,
                                    u32 key)
{
    const u8 *level_root = MMbit_get_level_root_const(bits, level);
    u32 ks = MMbit_get_ks(max_level, level);
    return level_root + ((u64a)key >> (ks + MMB_KEY_SHIFT)) * sizeof(MMB_TYPE);
}

static REALLY_INLINE
MMB_TYPE MMb_load(const u8 * bits)
{
    return UnalignedLoadU64a(bits);
}

static REALLY_INLINE
u32 MMb_test(MMB_TYPE val, u32 bit)
{
    return (val >> bit) & MMB_ONE;
}

static REALLY_INLINE
char MMbit_isset_big(const u8 *bits, u32 total_bits, u32 key)
{
    const u32 max_level = MMbit_maxlevel(total_bits);
    u32 level = 0;
    do {
        const u8 *block_ptr = MMbit_get_block_ptr_const(bits, max_level, level, key);
        MMB_TYPE block = MMb_load(block_ptr);
        if (!MMb_test(block, MMbit_get_key_val(max_level, level, key))) {
            return 0;
        }
    } while (level++ != max_level);
    return 1;
}

static REALLY_INLINE
char MMbit_isset(const u8 *bits, u32 total_bits, u32 key)
{
    if (MMbit_is_flat_model(total_bits)) {
        return MMbit_isset_flat(bits, total_bits, key);
    } else {
        return MMbit_isset_big(bits, total_bits, key);
    }
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MULTIBIT_H */

