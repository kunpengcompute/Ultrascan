/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * All rights reserved.
 *
 * Licensed under the 3-Clause BSD License (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://opensource.org/license/bsd-3-clause
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "../nfa/shufti.h"
#include "core_precomp.h"
#include "ue2common.h"

#define BATCH_SIZE 16
#define BYTE_SIZE_FOUR 4
#define MEMORY_FETCH 256

#define GET_LO_4(chars) and128(chars, low4bits)
#define GET_HI_4(chars) Rshift8_m128(chars, BYTE_SIZE_FOUR)

/** \brief Naive byte-by-byte implementation. */
static REALLY_INLINE const u8 *ShuftiFwdSlow(const u8 *lo, const u8 *hi,
                                             const u8 *buf, const u8 *buf_end) {

    for (; buf < buf_end; ++buf) {
        u8 c = *buf;
        if (lo[c & 0xf] & hi[c >> 0x4]) {
            break;
        }
    }
    return buf;
}

static REALLY_INLINE m128 BlockOpt(m128 mask_lo, m128 mask_hi, m128 chars,
                                   const m128 low4bits,
                                   UNUSED const m128 compare) {
    m128 c_lo = Pshufb_m128_opt(mask_lo, GET_LO_4(chars));
    m128 c_hi = Pshufb_m128_opt(mask_hi, GET_HI_4(chars));
    return and128(c_lo, c_hi);
}

static REALLY_INLINE u64a Comparemask(uint16x8_t mask) {
    return vget_lane_u64((uint64x1_t)vshrn_n_u16(mask, BYTE_SIZE_FOUR), 0);
}

static REALLY_INLINE const u8 *FirstMatchOpt(const u8 *buf, m128 mask) {
    uint32x4_t m = mask.vect_u32;
    uint64_t vmax = vgetq_lane_u64(vreinterpretq_u64_u32(vpmaxq_u32(m, m)), 0);
    if (vmax != 0) {
        u64a z = Comparemask(mask.vect_u16);
        u32 pos = CTZ64(z) / BYTE_SIZE_FOUR;
        return (const u8 *)buf + pos;
    } else {
        return NULL; // no match
    }
}

static REALLY_INLINE const u8 *FwdBlockOpt(m128 mask_lo, m128 mask_hi,
                                           m128 chars, const u8 *buf,
                                           const m128 low4bits,
                                           const m128 zeroes) {
    m128 z = BlockOpt(mask_lo, mask_hi, chars, low4bits, zeroes);
    m128 r;
    /* Every non-zero mask, including bit 7, denotes a match. */
    r.vect_u8 = vcgtq_u8(z.vect_u8, zeroes.vect_u8);

    return FirstMatchOpt(buf, r);
}

const u8 *KHSEL_ShuftiExec(m128 mask_lo, m128 mask_hi, const u8 *buf,
                           const u8 *buf_end) {
    KHSEL_RETURN_IF_NULL(buf, NULL);
    KHSEL_RETURN_IF_NULL(buf_end, NULL);
    KHSEL_RETURN_IF_GE(buf, buf_end, NULL);

    // Slow path for small cases.
    if (unlikely(buf_end - buf < BATCH_SIZE)) {
        return ShuftiFwdSlow((const u8 *)&mask_lo, (const u8 *)&mask_hi, buf,
                             buf_end);
    }

    const m128 zeroes = zeroes128();
    const m128 low4bits = set16x8(0xf);
    const u8 *rv;
    size_t min = (size_t)buf % BATCH_SIZE;

    // Preconditioning: most of the time our buffer won't be aligned.
    m128 chars = loadu128(buf);
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

    chars = loadu128(buf_end - BATCH_SIZE);
    rv = FwdBlockOpt(mask_lo, mask_hi, chars, buf_end - BATCH_SIZE, low4bits,
                     zeroes);
    if (rv) {
        return rv;
    }

    return buf_end;
}
