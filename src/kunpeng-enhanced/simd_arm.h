/*
 * Copyright (c) 2024 Huawei Technologies Co., Ltd.
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

#ifndef SIMD_ARM_H
#define SIMD_ARM_H

#include "../ue2common.h"
#include "arm_neon.h"
#include "core_precomp.h"

#ifdef __cplusplus
#ifdef HAVE_CXX_BUILTIN_ASSUME_ALIGNED
#define assume_aligned(a, b) __builtin_assume_aligned((a), (b))
#endif
#else
#ifdef HAVE_CC_BUILTIN_ASSUME_ALIGNED
#define assume_aligned(a, b) __builtin_assume_aligned((a), (b))
#endif
#endif

// Fallback to identity case.
#ifndef assume_aligned
#define assume_aligned(a, b) (a)
#endif

KHSEL_ALIGN_CL_DIRECTIVE static const char khsel_vbs_mask_data[] = {
    0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0,
    0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0,

    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,

    0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0,
    0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0,
};

#define CTZ64(z) (u32)__builtin_ctzll(z)
#define CLZ32(z) (u32)__builtin_clz(z)

static REALLY_INLINE m128 zeroes128(void) {
    m128 rst;
    rst.vect_s32 = vdupq_n_s32(0x0);
    return rst;
}

static REALLY_INLINE m128 ones128(void) {
    m128 result;
    result.vect_s32 = vdupq_n_s32(0xFFFFFFFF);
    return result;
}

static REALLY_INLINE m128 Or128(m128 a, m128 b) {
    m128 rst;
    rst.vect_s32 = vorrq_s32(a.vect_s32, b.vect_s32);
    return rst;
}

static REALLY_INLINE m128 And128(m128 a, m128 b) {
    m128 result;
    result.vect_s32 = vandq_s32(a.vect_s32, b.vect_s32);
    return result;
}

static REALLY_INLINE m128 Set16x8(u8 c)
{
    m128 rst;
    rst.vect_s8 = vdupq_n_s8(c);
    return rst;
}

static REALLY_INLINE u8 Compare128(m128 a, m128 b) {
    u8 result = 0xF;
    uint8x16_t mask = vceqq_u8(a.vect_u8, b.vect_u8);
    u8 res[16];
    vst1q_u8(res, mask);
    for (int i = 0;i < 16;i++) {
        result &= res[i];
    }
    return result;
}

/// Perform an unaligned 64-bit load
static REALLY_INLINE u64a UnalignedLoadU64a(const void *inPtr) {
    struct unaligned { u64a u; };
    const struct unaligned *uptr = (const struct unaligned *)inPtr;
    return uptr->u;
}

/// Perform an unaligned 16-bit load
static REALLY_INLINE u16 UnalignedLoadU16(const void *inPtr) {
    struct unaligned { u16 u; };
    const struct unaligned *uptr = (const struct unaligned *)inPtr;
    return uptr->u;
}

/// Perform an unaligned 32-bit load
static REALLY_INLINE u32 Unaligned_load_u32(const void *inPtr) {
    struct unaligned { u32 u; };
    const struct unaligned *uptr = (const struct unaligned *)inPtr;
    return uptr->u;
}

/// Perform an unaligned 32-bit store
static REALLY_INLINE void Unaligned_store_u32(void *inPtr, u32 val) {
    struct unaligned { u32 u; };
    struct unaligned *uptr = (struct unaligned *)inPtr;
    uptr->u = val;
}

/// Perform an unaligned 64-bit store
static REALLY_INLINE void Unaligned_store_u64a(void *inPtr, u64a val) {
    struct unaligned { u64a u; };
    struct unaligned *uptr = (struct unaligned *)inPtr;
    uptr->u = val;
}

/* another form of movq */
static REALLY_INLINE m128 Load_m128_from_u64a(const u64a *p) {
    m128 rst;
    __asm__ __volatile__("ldr %d0, %1         \n\t"
                         : "=w"(rst)
                         : "Utv"(*p)
                         : /* No clobbers */
    );
    return rst;
}

// unaligned load
static REALLY_INLINE m128 Loadu128(const void *inPtr) {
    m128 rst;
    rst.vect_s32 = vld1q_s32((const int32_t *)inPtr);
    return rst;
}

// aligned load
static REALLY_INLINE m128 Load128(const void *inPtr) {
    inPtr = assume_aligned(inPtr, 16);
    m128 rst;
    rst.vect_s32 = vld1q_s32((const int32_t *)inPtr);
    return rst;
}

// unaligned store
static REALLY_INLINE void Storeu128(void *inPtr, m128 a) {
    vst1q_s32((int32_t *)inPtr, a.vect_s32);
}

static REALLY_INLINE m128 pshufb_m128(m128 a, m128 b) {
    m128 rst;
    __asm__ __volatile__("movi v3.16b, 0x8f                   \n\t"
                         "and v3.16b, v3.16b, %2.16b          \n\t"
                         "tbl %0.16b, {%1.16b}, v3.16b        \n\t"
                         : "=w"(rst)
                         : "w"(a), "w"(b)
                         : "v3");
    return rst;
}

static REALLY_INLINE m128 RshiftOnebyte_m128(m128 a) {
    m128 zeroVect;
    zeroVect.vect_s8 = vdupq_n_s8(0);
    m128 rst;
    rst.vect_s8 =  vextq_s8(a.vect_s8, zeroVect.vect_s8, 1);
    return rst;
}

static REALLY_INLINE m128 Rshift8_m128(m128 a, int imm8) {
    if (unlikely(imm8 == 0)) {
        return a;
    }
    m128 result;
    result.vect_u8 = vshrq_n_u8(a.vect_u8, imm8);
    return result;
}

static REALLY_INLINE m128 Rshift64_m128(m128 a, int imm8) {
    assert(imm8 >= 0 && imm8 <= 63);
    if (unlikely(imm8 == 0)) {
        return a;
    }
    m128 result;
    result.vect_u64 = vshrq_n_u64(a.vect_u64, imm8);
    return result;
}

static REALLY_INLINE m128 Extbyte_m128(m128 a, m128 b, int imm8) {
    assert(imm8 >= 0 && imm8 <= 15);
    m128 result;
    switch (imm8) {
        case 0:
            result.vect_s8 = vextq_s8(a.vect_s8, b.vect_s8, 0);
            break;
        case 1:
            result.vect_s8 = vextq_s8(a.vect_s8, b.vect_s8, 1);
            break;
        case 2:
            result.vect_s8 = vextq_s8(a.vect_s8, b.vect_s8, 2);
            break;
        case 3:
            result.vect_s8 = vextq_s8(a.vect_s8, b.vect_s8, 3);
            break;
        case 4:
            result.vect_s8 = vextq_s8(a.vect_s8, b.vect_s8, 4);
            break;
        case 5:
            result.vect_s8 = vextq_s8(a.vect_s8, b.vect_s8, 5);
            break;
        case 6:
            result.vect_s8 = vextq_s8(a.vect_s8, b.vect_s8, 6);
            break;
        case 7:
            result.vect_s8 = vextq_s8(a.vect_s8, b.vect_s8, 7);
            break;
        case 8:
            result.vect_s8 = vextq_s8(a.vect_s8, b.vect_s8, 8);
            break;
        case 9:
            result.vect_s8 = vextq_s8(a.vect_s8, b.vect_s8, 9);
            break;
        case 10:
            result.vect_s8 = vextq_s8(a.vect_s8, b.vect_s8, 10);
            break;
        case 11:
            result.vect_s8 = vextq_s8(a.vect_s8, b.vect_s8, 11);
            break;
        case 12:
            result.vect_s8 = vextq_s8(a.vect_s8, b.vect_s8, 12);
            break;
        case 13:
            result.vect_s8 = vextq_s8(a.vect_s8, b.vect_s8, 13);
            break;
        case 14:
            result.vect_s8 = vextq_s8(a.vect_s8, b.vect_s8, 14);
            break;
        case 15:
            result.vect_s8 = vextq_s8(a.vect_s8, b.vect_s8, 15);
            break;
        default:
            break;
    }
    return result;
}

static REALLY_INLINE m128 Palignr(m128 a, m128 b, int count) {
    m128 result;
    count = count & 0xff;
    if (likely(count < 16)) {
        result = Extbyte_m128(b, a, count);
    } else if (count < 32) {
        m128 zeroVect;
        zeroVect.vect_s8 = vdupq_n_s8(0x0);
        result = Extbyte_m128(a, zeroVect, count - 16);
    } else {
        result.vect_s32 = vdupq_n_s32(0);
    }
    return result;
}

static REALLY_INLINE m128 Variable_byte_shift_m128(m128 in, s32 amount) {
    assert(amount >= -16 && amount <= 16);
    m128 shift_mask = Loadu128(khsel_vbs_mask_data + 16 - amount);
    return pshufb_m128(in, shift_mask);
}

static REALLY_INLINE u32 FindAndClearLSB_64(u64a *v) {
    u64a val = *v, offset;
    offset = CTZ64(val);
    *v = val & (val - 1);
    return (u32)offset;
}

static REALLY_INLINE m128 Pshufb_m128_opt(m128 a, m128 b) {
    m128 result;
    __asm__ __volatile__("tbl %0.16b, {%1.16b}, %2.16b        \n\t"
                         : "=w"(result)
                         : "w"(a), "w"(b)
                         : "v3");
    return result;
}

#endif /* SIMD_ARM_H */
