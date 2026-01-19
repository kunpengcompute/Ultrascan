/*
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
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
#include "replaceAll_acc.h"
#include <cstring>

#define GET_LO_4(chars) And128(chars, low4bits)
#define GET_HI_4(chars) Rshift8_m128(chars, 0x4)

#define CONSTRUCTOR __attribute__((constructor))

static ShuftiMask g_mask;

static REALLY_INLINE m128 Or128(m128 a, m128 b)
{
    m128 rst;
    rst.vect_s32 = vorrq_s32(a.vect_s32, b.vect_s32);
    return rst;
}

static REALLY_INLINE m128 And128(m128 a, m128 b)
{
    m128 result;
    result.vect_s32 = vandq_s32(a.vect_s32, b.vect_s32);
    return result;
}

static REALLY_INLINE m128 Xor128(m128 a, m128 b)
{
    m128 result;
    result.vect_s32 = veorq_s32(a.vect_s32, b.vect_s32);
    return result;
}

static REALLY_INLINE m128 zeroes128(void)
{
    m128 result;
    result.vect_s32 = vdupq_n_s32(0x0);
    return result;
}

static REALLY_INLINE m128 set16x8(u8 c)
{
    m128 result;
    result.vect_s8 = vdupq_n_s8(c);
    return result;
}

// unaligned load
static REALLY_INLINE m128 loadu128(const void *inPtr)
{
    m128 rst;
    rst.vect_s32 = vld1q_s32((const int32_t *)inPtr);
    return rst;
}

// aligned load
static REALLY_INLINE m128 load128(const void *inPtr)
{
    inPtr = assume_aligned(inPtr, 16);
    m128 rst;
    rst.vect_s32 = vld1q_s32((const int32_t *)inPtr);
    return rst;
}

static REALLY_INLINE m128 Rshift8_m128(m128 a, int imm8)
{
    if (imm8 == 0) {
        return a;
    }
    m128 result;
    result.vect_u8 = vshrq_n_u8(a.vect_u8, imm8);
    return result;
}

static REALLY_INLINE m128 Pshufb_m128_opt(m128 a, m128 b)
{
    m128 result;
    __asm__ __volatile__("tbl %0.16b, {%1.16b}, %2.16b        \n\t" : "=w"(result) : "w"(a), "w"(b) : "v3");
    return result;
}

static REALLY_INLINE u32 ctz64(u64a x)
{
    return (u32)__builtin_ctzll(x);
}

CONSTRUCTOR ShuftiMask BuildExclusionMask()
{
    ShuftiMask mask;
    uint8_t hi_bytes[16] = {0};
    uint8_t lo_bytes[16] = {0};
    constexpr uint8_t allowed[] = {
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
        0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A,
        0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54,
        0x55, 0x56, 0x57, 0x58, 0x59, 0x5A,
        0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A,
        0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74,
        0x75, 0x76, 0x77, 0x78, 0x79, 0x7A,
        '_', '/', '.'
    };

    for (int c = 0; c < 0x80; ++c) {
        for (auto a : allowed) {
            if (c == a) {
                uint8_t h = (c >> 0x4) & 0x0F;
                uint8_t l = c & 0x0F;
                hi_bytes[h] |= (1 << (h % 0x8));
                lo_bytes[l] |= hi_bytes[h];
            } else {
                continue;
            }
        }
    }

    mask.hi_mask = vld1q_u8(hi_bytes);
    mask.lo_mask = vld1q_u8(lo_bytes);
    g_mask = mask;
    return mask;
}

static REALLY_INLINE m128 BlockOpt(m128 mask_lo, m128 mask_hi, m128 chars, const m128 low4bits)
{
    m128 c_lo = Pshufb_m128_opt(mask_lo, GET_LO_4(chars));
    m128 c_hi = Pshufb_m128_opt(mask_hi, GET_HI_4(chars));
    return And128(c_lo, c_hi);
}

static REALLY_INLINE u64a Comparemask(uint16x8_t mask)
{
    return vget_lane_u64((uint64x1_t)vshrn_n_u16(mask, 0x4), 0);
}

static REALLY_INLINE void FirstMatchOpt(m128 mask, u8 *rst)
{
    m128 a;
    a.vect_u32 = vdupq_n_u32(0x01010101);

    m128 z = And128(a, mask);
    z = Xor128(z, a);

    vst1q_u8(rst, z.vect_u8);
}

static REALLY_INLINE void fwdBlockOpt(
    m128 mask_lo, m128 mask_hi, m128 chars, const m128 low4bits, const m128 zeroes, u8 *rst)
{
    m128 z = BlockOpt(mask_lo, mask_hi, chars, low4bits);
    m128 r;
    r.vect_u8 = vcgtq_u8(z.vect_u8, zeroes.vect_u8);
    FirstMatchOpt(r, rst);
}

static REALLY_INLINE const u8 *shuftiFwdSlow(const u8 *lo, const u8 *hi, const u8 *buf, const u8 *buf_end)
{
    for (; buf < buf_end; ++buf) {
        u8 c = *buf;
        if (lo[c & 0xf] & hi[c >> 4]) {
            break;
        }
    }
    return buf;
}

static REALLY_INLINE uint64_t shuftiExec(m128 mask_lo, m128 mask_hi, const u8 *buf, const u8 *buf_end, u8 *rst_mask)
{
    int len = buf_end - buf;
    uint64_t vmax = 0;
    const m128 zeroes = zeroes128();
    const m128 low4bits = set16x8(0xf);
    u8 *rv_tmp = rst_mask;

    size_t min = (size_t)buf % 16;

    m128 chars = loadu128(buf);
    fwdBlockOpt(mask_lo, mask_hi, chars, low4bits, zeroes, rv_tmp);

    m128 m;
    m.vect_u8 = vld1q_u8(rv_tmp);
    vmax |= vgetq_lane_u64(vreinterpretq_u64_u32(vpmaxq_u32(m.vect_u32, m.vect_u32)), 0);

    if (len <= 16) {
        return vmax;
    }

    buf += (16 - min);
    rv_tmp += (16 - min);

    const u8 *last_block = buf_end - 16;
    while (buf < last_block) {
        m128 lchars = load128(buf);

#if defined(HAVE_NEON)
        __asm__ __volatile__("prfm pldl1keep, %0" ::"Q"(*(buf + 256)));
#endif

        fwdBlockOpt(mask_lo, mask_hi, lchars, low4bits, zeroes, rv_tmp);
        m.vect_u8 = vld1q_u8(rv_tmp);
        vmax |= vgetq_lane_u64(vreinterpretq_u64_u32(vpmaxq_u32(m.vect_u32, m.vect_u32)), 0);
        buf += 16;
        rv_tmp += 16;
    }

    chars = loadu128(buf);
    fwdBlockOpt(mask_lo, mask_hi, chars, low4bits, zeroes, rv_tmp);
    m.vect_u8 = vld1q_u8(rv_tmp);
    vmax |= vgetq_lane_u64(vreinterpretq_u64_u32(vpmaxq_u32(m.vect_u32, m.vect_u32)), 0);

    return vmax;
}

static REALLY_INLINE std::string Replace(std::string input, std::string target, uint8_t *bitmap)
{
    int input_size = input.size();
    if (input.size() == 0) {
        return NULL;
    }
    std::string res_string;

    const int MAX_GROUPS = input_size;
    int indices[MAX_GROUPS];
    int lengths[MAX_GROUPS];

    int groupCount = 0;
    int start = -1;
    for (int i = 0; i < input_size; ++i) {
        if (bitmap[i] == 1) {
            if (start == -1) {
                start = i;
            }
        } else {
            if (start != -1) {
                indices[groupCount] = start;
                lengths[groupCount] = i - start;
                groupCount++;
                start = -1;
            }
        }
    }

    if (start != -1) {
        indices[groupCount] = start;
        lengths[groupCount] = input_size - start;
        groupCount++;
    }

    int origin_string_index = 0;
    for (int i = 0; i < groupCount; i++) {
        res_string += input.substr(origin_string_index, indices[i] - origin_string_index);
        res_string += target;
        origin_string_index = indices[i] + lengths[i];
    }
    if (origin_string_index < input_size) {
        res_string += input.substr(origin_string_index, input_size - origin_string_index);
    }
    return res_string;
}

std::string ReplaceAllAcc(const std::string& input, const std::string& replacement)
{
    if (input.size() > 0) {
        int input_size = input.size();
        m128 lo;
        m128 hi;
        lo.vect_u8 = g_mask.lo_mask;
        hi.vect_u8 = g_mask.hi_mask;
        uint8_t rst_mask[input_size + 17] = {0};
        const uint8_t *dataIn = reinterpret_cast<const uint8_t *>(input.data());
        if (shuftiExec(lo, hi, (uint8_t *)dataIn, (uint8_t *)dataIn + input_size, rst_mask) > 0) {
            return Replace(input, replacement, rst_mask);
        }
    }
    return input;
}