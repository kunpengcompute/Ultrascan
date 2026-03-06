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

#include "shufti_enhanced.h"
#include "ue2common.h"

#define BATCH_SIZE 16
#define BYTE_SIZE_FOUR 4
#define MEMORY_FETCH 256

#define GET_LO_4(chars) And128(chars, low4bits)
#define GET_HI_4(chars) Rshift8_m128(chars, BYTE_SIZE_FOUR)

/** \brief Naive byte-by-byte implementation. */
static REALLY_INLINE const u8 *ShuftiFwdSlow(const u8 *lo, const u8 *hi, const u8 *buf,
    const u8 *buf_end) {

    for (; buf < buf_end; ++buf) {
        u8 c = *buf;
        if (lo[c & 0xf] & hi[c >> 0x4]) {
            break;
        }
    }
    return buf;
}

static REALLY_INLINE m128 BlockOpt(m128 mask_lo, m128 mask_hi, m128 chars, const m128 low4bits,
    UNUSED const m128 compare)
{
    m128 c_lo  = Pshufb_m128_opt(mask_lo, GET_LO_4(chars));
    m128 c_hi  = Pshufb_m128_opt(mask_hi, GET_HI_4(chars));
    return And128(c_lo, c_hi);
}

static REALLY_INLINE u64a Comparemask(uint16x8_t mask)
{
    return vget_lane_u64((uint64x1_t)vshrn_n_u16(mask, BYTE_SIZE_FOUR), 0);
}

static REALLY_INLINE const u8 *FirstMatchOpt(const u8 *buf, m128 mask)
{
    uint32x4_t m = mask.vectU32;
    uint64_t vmax = vgetq_lane_u64 (vreinterpretq_u64_u32 (vpmaxq_u32(m, m)), 0);
    if (vmax != 0) {
        u64a z = Comparemask(mask.vectU16);
        u32 pos = CTZ64(z) / BYTE_SIZE_FOUR;
        return (const u8 *)buf + pos;
    } else {
        return NULL; // no match
    }
}

static REALLY_INLINE const u8 *FwdBlockOpt(m128 mask_lo, m128 mask_hi, m128 chars,
    const u8 *buf, const m128 low4bits, const m128 zeroes)
{
    m128 z = BlockOpt(mask_lo, mask_hi, chars, low4bits, zeroes);
    m128 r;
    r.vectU8 = vcgtq_s8(z.vectS8, zeroes.vectS8);

    return FirstMatchOpt(buf, r);
}

const u8 *KHSEL_ShuftiExec(m128 mask_lo, m128 mask_hi, const u8 *buf, const u8 *buf_end)
{
    KHSEL_RETURN_IF_NULL(buf, NULL);
    KHSEL_RETURN_IF_NULL(buf_end, NULL);
    KHSEL_RETURN_IF_GE(buf, buf_end, NULL);

    // Slow path for small cases.
    if (unlikely(buf_end - buf < BATCH_SIZE)) {
        return ShuftiFwdSlow((const u8 *)&mask_lo, (const u8 *)&mask_hi,
                             buf, buf_end);
    }

    const m128 zeroes = zeroes128();
    const m128 low4bits = Set16x8(0xf);
    const u8 *rv;
    size_t min = (size_t)buf % BATCH_SIZE;

    // Preconditioning: most of the time our buffer won't be aligned.
    m128 chars = Loadu128(buf);
    rv = FwdBlockOpt(mask_lo, mask_hi, chars, buf, low4bits, zeroes);
    if (rv) {
        return rv;
    }
    buf += (BATCH_SIZE - min);

    const u8 *last_block = buf_end - BATCH_SIZE;
    while (buf < last_block) {
        m128 lchars = Load128(buf);

#if defined(HAVE_NEON)
        __asm__ __volatile__("prfm pldl1keep, %0" ::"Q"(*(buf + MEMORY_FETCH)));
#endif

        rv = FwdBlockOpt(mask_lo, mask_hi, lchars, buf, low4bits, zeroes);
        if (rv) {
            return rv;
        }
        buf += BATCH_SIZE;
    }

    chars = Loadu128(buf_end - BATCH_SIZE);
    rv = FwdBlockOpt(mask_lo, mask_hi, chars, buf_end - BATCH_SIZE, low4bits, zeroes);
    if (rv) {
        return rv;
    }

    return buf_end;
}
