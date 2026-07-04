/*
 * Copyright (c) 2024 Huawei Technologies Co., Ltd.
 * Optimized fdr
 * Copyright (c) 2015-2017, Intel Corporation
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

#include "core_precomp.h"
#include "../fdr/fdr_confirm_runtime.h"
#include "../fdr/fdr_loadval.h"
#include "../fdr/fdr_enhanced.h"
#include "../hwlm/hwlm.h"
#include "../fdr/fdr_internal.h"
#include "../util/simd_arm.h"

#define SHIFT_BYTES_ONE 8
#define SHIFT_BYTES_TWO 16
#define SHIFT_BYTES_THREE 24
#define SHIFT_BYTES_FOUR 32
#define SHIFT_BYTES_FIVE 40
#define SHIFT_BYTES_SIX 48
#define SHIFT_BYTES_SEVEN 56

static REALLY_INLINE void KHSELGetConfStrideOne(const u8 *itPtr, u64a domainMaskAdjusted,
    const u64a *ft, u64a conf[2], u64a s[2])
{
    u64a m8 = s[1];

    u64a current_data_0 = UnalignedLoadU64a(itPtr);
    u64a current_data_8 = UnalignedLoadU64a(itPtr + SHIFT_BYTES_ONE); // Load data 8 * 8 bytes after itPtr.
    const size_t crossByteFirst = 7;
    const size_t crossByteSecnd = 15;

    u64a v7 = (UnalignedLoadU16(itPtr + crossByteFirst)) & domainMaskAdjusted; // Load data 7 * 8 bytes after itPtr.
    u64a v15 = (UnalignedLoadU16(itPtr + crossByteSecnd)) & domainMaskAdjusted; // Load data 15 * 8 bytes after itPtr.
    u64a v0 = (current_data_0) & domainMaskAdjusted;
    u64a v8 = (current_data_8) & domainMaskAdjusted;
    u64a v1 = (current_data_0 >> SHIFT_BYTES_ONE) & domainMaskAdjusted;
    u64a v9 = (current_data_8 >> SHIFT_BYTES_ONE) & domainMaskAdjusted;
    u64a v2 = (current_data_0 >> SHIFT_BYTES_TWO) & domainMaskAdjusted;
    u64a v10 = (current_data_8 >> SHIFT_BYTES_TWO) & domainMaskAdjusted;
    u64a v3 = (current_data_0 >> SHIFT_BYTES_THREE) & domainMaskAdjusted;
    u64a v11 = (current_data_8 >> SHIFT_BYTES_THREE) & domainMaskAdjusted;
    u64a v4 = (current_data_0 >> SHIFT_BYTES_FOUR) & domainMaskAdjusted;
    u64a v12 = (current_data_8 >> SHIFT_BYTES_FOUR) & domainMaskAdjusted;
    u64a v5 = (current_data_0 >> SHIFT_BYTES_FIVE) & domainMaskAdjusted;
    u64a v13 = (current_data_8 >> SHIFT_BYTES_FIVE) & domainMaskAdjusted;
    u64a v6 = (current_data_0 >> SHIFT_BYTES_SIX) & domainMaskAdjusted;
    u64a v14 = (current_data_8 >> SHIFT_BYTES_SIX) & domainMaskAdjusted;

    u64a st0 = UnalignedLoadU64a(ft + v0);
    u64a st1 = UnalignedLoadU64a(ft + v1);
    u64a st2 = UnalignedLoadU64a(ft + v2);
    u64a st3 = UnalignedLoadU64a(ft + v3);
    u64a st4 = UnalignedLoadU64a(ft + v4);
    u64a st5 = UnalignedLoadU64a(ft + v5);
    u64a st6 = UnalignedLoadU64a(ft + v6);
    u64a st7 = UnalignedLoadU64a(ft + v7);
    u64a st8 = UnalignedLoadU64a(ft + v8);
    u64a st9 = UnalignedLoadU64a(ft + v9);
    u64a st10 = UnalignedLoadU64a(ft + v10);
    u64a st11 = UnalignedLoadU64a(ft + v11);
    u64a st12 = UnalignedLoadU64a(ft + v12);
    u64a st13 = UnalignedLoadU64a(ft + v13);
    u64a st14 = UnalignedLoadU64a(ft + v14);
    u64a st15 = UnalignedLoadU64a(ft + v15);

    u64a tst1 = st1 >> SHIFT_BYTES_SEVEN; // rightshift 7byte
    u64a tst2 = st2 >> SHIFT_BYTES_SIX; // 6
    u64a tst3 = st3 >> SHIFT_BYTES_FIVE;
    u64a tst4 = st4 >> SHIFT_BYTES_FOUR;
    u64a tst5 = st5 >> SHIFT_BYTES_THREE;
    u64a tst6 = st6 >> SHIFT_BYTES_TWO;
    u64a tst7 = st7 >> SHIFT_BYTES_ONE; // 1byte
    u64a tst9 = st9 >> SHIFT_BYTES_SEVEN;
    u64a tst10 = st10 >> SHIFT_BYTES_SIX;
    u64a tst11 = st11 >> SHIFT_BYTES_FIVE;
    u64a tst12 = st12 >> SHIFT_BYTES_FOUR;
    u64a tst13 = st13 >> SHIFT_BYTES_THREE;
    u64a tst14 = st14 >> SHIFT_BYTES_TWO;
    u64a tst15 = st15 >> SHIFT_BYTES_ONE;

    st1 <<= SHIFT_BYTES_ONE;
    st2 <<= SHIFT_BYTES_TWO;
    st3 <<= SHIFT_BYTES_THREE;
    st4 <<= SHIFT_BYTES_FOUR;
    st5 <<= SHIFT_BYTES_FIVE;
    st6 <<= SHIFT_BYTES_SIX;
    st7 <<= SHIFT_BYTES_SEVEN;
    st9 <<= SHIFT_BYTES_ONE;
    st10 <<= SHIFT_BYTES_TWO;
    st11 <<= SHIFT_BYTES_THREE;
    st12 <<= SHIFT_BYTES_FOUR;
    st13 <<= SHIFT_BYTES_FIVE;
    st14 <<= SHIFT_BYTES_SIX;
    st15 <<= SHIFT_BYTES_SEVEN;

    conf[0] = s[0] | st0 | st1 | st2 | st3 | st4 | st5 | st6 | st7;
    m8 = m8 | tst1 | tst2 | tst3 | tst4 | tst5 | tst6 | tst7;
    conf[1] = m8 | st8 | st9 | st10 | st11 | st12 | st13 | st14 | st15;

    s[0] = tst9 | tst10 | tst11 | tst12 | tst13 | tst14 | tst15;
    s[1] = 0;

    conf[0] ^= ~0ULL;
    conf[1] ^= ~0ULL;
}

static REALLY_INLINE void KHSELGetConfStrideBi(const u8 *itPtr, u64a domainMaskAdjusted,
    const u64a *ft, u64a conf[2], u64a s[2])
{
    u64a current_data_0 = UnalignedLoadU64a(itPtr);

    u64a m0 = s[0];
    u64a m8 = s[1];

    u64a v0 = current_data_0 & domainMaskAdjusted;
    u64a v2 = (current_data_0 >> SHIFT_BYTES_TWO) & domainMaskAdjusted;
    u64a v4 = (current_data_0 >> SHIFT_BYTES_FOUR) & domainMaskAdjusted;
    u64a v6 = (current_data_0 >> SHIFT_BYTES_SIX) & domainMaskAdjusted;

    u64a st0 = UnalignedLoadU64a(ft + v0);
    u64a st2 = UnalignedLoadU64a(ft + v2);
    u64a st4 = UnalignedLoadU64a(ft + v4);
    u64a st6 = UnalignedLoadU64a(ft + v6);

    u64a tst2 = st2 >> SHIFT_BYTES_SIX;
    u64a tst4 = st4 >> SHIFT_BYTES_FOUR;
    u64a tst6 = st6 >> SHIFT_BYTES_TWO;

    st2 <<= SHIFT_BYTES_TWO;
    st4 <<= SHIFT_BYTES_FOUR;
    st6 <<= SHIFT_BYTES_SIX;

    current_data_0 = UnalignedLoadU64a(itPtr + SHIFT_BYTES_ONE);
    m8 = tst2 | tst4 | tst6 | m8;
    conf[0]= st0 | st2 | st4 | st6 | m0;

    v0 = current_data_0 & domainMaskAdjusted;
    v2 = (current_data_0 >> SHIFT_BYTES_TWO) & domainMaskAdjusted;
    v4 = (current_data_0 >> SHIFT_BYTES_FOUR) & domainMaskAdjusted;
    v6 = (current_data_0 >> SHIFT_BYTES_SIX) & domainMaskAdjusted;

    st0 = UnalignedLoadU64a(ft + v0);
    st2 = UnalignedLoadU64a(ft + v2);
    st4 = UnalignedLoadU64a(ft + v4);
    st6 = UnalignedLoadU64a(ft + v6);

    tst2 = st2 >> SHIFT_BYTES_SIX;
    tst4 = st4 >> SHIFT_BYTES_FOUR;
    tst6 = st6 >> SHIFT_BYTES_TWO;

    st2 <<= SHIFT_BYTES_TWO;
    st4 <<= SHIFT_BYTES_FOUR;
    st6 <<= SHIFT_BYTES_SIX;

    s[1] = 0;
    s[0] = tst2 | tst4 | tst6;
    conf[1] = st0 | st2 | st4 | st6 | m8;

    conf[0] ^= ~0ULL;
    conf[1] ^= ~0ULL;
}

static REALLY_INLINE void KHSELGetConfStrideQua(const u8 *itPtr, u64a domainMaskAdjusted,
    const u64a *ft, u64a conf[2], u64a s[2])
{
    u64a m8 = s[1];
    
    u64a current_data_0 = UnalignedLoadU64a(itPtr);
    u64a current_data_8 = UnalignedLoadU64a(itPtr + SHIFT_BYTES_ONE);
    u64a v0 = (current_data_0) & domainMaskAdjusted;
    u64a v8 = (current_data_8) & domainMaskAdjusted;
    u64a v4 = (current_data_0 >> SHIFT_BYTES_FOUR) & domainMaskAdjusted;
    u64a v12 = (current_data_8 >> SHIFT_BYTES_FOUR) & domainMaskAdjusted;

    u64a st0 = UnalignedLoadU64a(ft + v0);
    u64a st4 = UnalignedLoadU64a(ft + v4);
    u64a st8 = UnalignedLoadU64a(ft + v8);
    u64a st12 = UnalignedLoadU64a(ft + v12);

    u64a tst4 = st4 >> SHIFT_BYTES_FOUR;
    u64a tst12 = st12 >> SHIFT_BYTES_FOUR;
    
    st4 <<= SHIFT_BYTES_FOUR; // Shift left 32 bits to get lower bits of st4.
    st12 <<= SHIFT_BYTES_FOUR; // Shift left 32 bits to get lower bits of st12.
    
    conf[0] = s[0] | st0 | st4;

    m8 = tst4 | m8;
    conf[1] = m8 | st8 | st12;

    s[0] = tst12;
    s[1] = 0;

    conf[0] ^= ~0ULL;
    conf[1] ^= ~0ULL;
}

#define KHSEL_ITER_BYTES          16
#define KHSEL_ZONE_TOTAL_SIZE     64
#define KHSEL_ZONE_MAX            3

struct zone {
    u8 KHSEL_ALIGN_CL_DIRECTIVE buf[KHSEL_ZONE_TOTAL_SIZE];
    u8 shift;
    const u8 *start;
    const u8 *end;
    ptrdiff_t zone_pointer_adjust;
    const u8 *floodPtr;
};

static const KHSEL_ALIGN_CL_DIRECTIVE u8 khsel_zone_or_mask[KHSEL_ITER_BYTES+1][KHSEL_ITER_BYTES] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00 },
    { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00 },
    { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00 },
    { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
};

static REALLY_INLINE m128 KHSEL_GetInitState(const struct FDR *fdr, u8 len_history, const u64a *ft,
                  const struct zone *inZ) {
    m128 state;
    if (len_history) {
        /* +1: the zones ensure that we can read the byte at z->end */
        u32 tmp = lv_u16(inZ->start + inZ->shift - 1, inZ->buf, inZ->end + 1);
        tmp &= fdr->domainMask;
        state = load_m128_from_u64a(ft + tmp);
        state = RshiftOnebyte_m128(state);
    } else {
        state = fdr->start;
    }
    return state;
}

#define SHIFT_BYTES_ONE 8
#define SHIFT_BYTES_TWO 16
#define SHIFT_BYTES_THREE 24
#define SHIFT_BYTES_FOUR 32
#define SHIFT_BYTES_FIVE 40
#define SHIFT_BYTES_SIX 48
#define SHIFT_BYTES_SEVEN 56

static REALLY_INLINE void KHSEL_DoConfirmFdr(u64a *conf, u8 offset, hwlmcb_rv_t *control,
                    const u32 *confBase, const struct FDR_Runtime_Args *a,
                    const u8 *ptr, u32 *last_match_id, struct zone *z)
{
    const u8 bucket = 8;
    if (likely(!*conf)) {
        return;
    }

    const u8 *ptr_main = (const u8 *)((uintptr_t)ptr + z->zone_pointer_adjust);
    const u8 *confLoc = ptr;
    do  {
        u32 bit = FindAndClearLSB_64(conf);
        u32 byteNum = bit / bucket + offset;
        u32 bitRemId = bit % bucket;
        u32 index = bitRemId;
        u32 cfBase = confBase[index];
        if (unlikely(!cfBase)) {
            continue;
        }
        const struct FDRConfirm *fdrc = (const struct FDRConfirm *)
                                        ((const u8 *)confBase + cfBase);
        if (unlikely(!(fdrc->groups & *control))) {
            continue;
        }
        u64a confVal = UnalignedLoadU64a(confLoc + byteNum - sizeof(u64a) + 1);
        confWithBit(fdrc, a, ptr_main - a->buf + byteNum, control,
                    last_match_id, confVal, conf, bit);
    } while (unlikely(!!*conf));
}

static REALLY_INLINE void KHSEL_CreateMainZone(const u8 *flood, const u8 *begin, const u8 *end,
                    struct zone *inZ)
{
    inZ->zone_pointer_adjust = 0; /* zone buffer is the main buffer */
    inZ->start = begin;
    inZ->end = end;
    inZ->floodPtr = flood;
    inZ->shift = 0;
}

static REALLY_INLINE void KHSEL_CreateShortZone(const u8 *buf, const u8 *hend, const u8 *begin,
                     const u8 *end, struct zone *inZ)
{
    inZ->floodPtr = inZ->buf + KHSEL_ZONE_TOTAL_SIZE;

    ptrdiff_t z_len = end - begin;

    inZ->shift = KHSEL_ITER_BYTES - z_len; /* ignore bytes outside region specified */

    static const size_t ZONE_SHORT_DATA_OFFSET = 16; /* after history */

    *(m128 *)inZ->buf = loadu128(hend - sizeof(m128));

    /* The amount of data we have to copy from main buffer. */
    size_t copy_len = KHSEL_MIN((size_t)(end - buf),
                          KHSEL_ITER_BYTES + sizeof(CONF_TYPE));

    u8 *zone_data = inZ->buf + ZONE_SHORT_DATA_OFFSET;
    switch (copy_len) {
    case 1:
        *zone_data = *(end - 1);
        break;
    case 2:
        *(u16 *)zone_data = UnalignedLoadU16(end - 2);
        break;
    case 3:
        *(u16 *)zone_data = UnalignedLoadU16(end - 3);
        *(zone_data + 2) = *(end - 1);
        break;
    case 4:
        *(u32 *)zone_data = Unaligned_load_u32(end - 4);
        break;
    case 5:
    case 6:
    case 7:
        /* perform copy with 2 overlapping 4-byte chunks from buf. */
        *(u32 *)zone_data = Unaligned_load_u32(end - copy_len);
        Unaligned_store_u32(zone_data + copy_len - sizeof(u32),
                            Unaligned_load_u32(end - sizeof(u32)));
        break;
    case 8:
        *(u64a *)zone_data = UnalignedLoadU64a(end - 8);
        break;
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
        *(u64a *)zone_data = UnalignedLoadU64a(end - copy_len);
        Unaligned_store_u64a(zone_data + copy_len - sizeof(u64a),
                             UnalignedLoadU64a(end - sizeof(u64a)));
        break;
    case 16:
        *(m128 *)zone_data = loadu128(end - SHIFT_BYTES_TWO);
        break;
    default:
        *(u64a *)zone_data = UnalignedLoadU64a(end - copy_len);
        storeu128(zone_data + copy_len - sizeof(m128),
                  loadu128(end - sizeof(m128)));
        break;
    }

    u8 *z_end = inZ->buf + ZONE_SHORT_DATA_OFFSET + copy_len;
    *z_end = 0;

    inZ->end = z_end;
    inZ->start = z_end - KHSEL_ITER_BYTES;
    inZ->zone_pointer_adjust = (ptrdiff_t)((uintptr_t)end - (uintptr_t)z_end);
}

static REALLY_INLINE void KHSEL_CreateStartZone(const u8 *buf, const u8 *hend, const u8 *begin,
                     struct zone *inZ)
{
    static const size_t ZONE_START_BEGIN = sizeof(CONF_TYPE);
    const u8 *end = begin + KHSEL_ITER_BYTES;
    inZ->floodPtr = inZ->buf + KHSEL_ZONE_TOTAL_SIZE;
    inZ->shift = 0;
    Unaligned_store_u64a(inZ->buf, UnalignedLoadU64a(hend - sizeof(u64a)));
    size_t copy_len = KHSEL_MIN((size_t)(end - buf),
                          KHSEL_ITER_BYTES + sizeof(CONF_TYPE));
    inZ->buf[ZONE_START_BEGIN + copy_len] = *end;
    u8 *z_end = inZ->buf + ZONE_START_BEGIN + copy_len;
    inZ->end = z_end;
    inZ->start = z_end - KHSEL_ITER_BYTES;
    Unaligned_store_u64a(inZ->buf + ZONE_START_BEGIN,
                         UnalignedLoadU64a(end - copy_len));
    storeu128(z_end - sizeof(m128), loadu128(end - sizeof(m128)));
    inZ->zone_pointer_adjust = (ptrdiff_t)((uintptr_t)end - (uintptr_t)z_end);
}

static REALLY_INLINE void KHSEL_CreateEndZone(const u8 *buf, const u8 *begin, const u8 *end,
                   struct zone *inZ)
{
    inZ->floodPtr = inZ->buf + KHSEL_ZONE_TOTAL_SIZE;
    ptrdiff_t z_len = end - begin;
    size_t iter_bytes_second = 0;
    size_t z_len_first = z_len;
    if (unlikely(z_len > KHSEL_ITER_BYTES)) {
        z_len_first = z_len - KHSEL_ITER_BYTES;
        iter_bytes_second = KHSEL_ITER_BYTES;
    }
    inZ->shift = KHSEL_ITER_BYTES - z_len_first;

    const u8 *end_first = end - iter_bytes_second;
    size_t copy_len_first = KHSEL_MIN((size_t)(end_first - buf),
                                KHSEL_ITER_BYTES + sizeof(CONF_TYPE));
    size_t total_copy_len = copy_len_first + iter_bytes_second;
    inZ->buf[total_copy_len] = 0;

    u8 *z_end = inZ->buf + total_copy_len;
    inZ->end = z_end;
    inZ->start = z_end - KHSEL_ITER_BYTES - iter_bytes_second;

    u8 *z_end_first = z_end - iter_bytes_second;
    /* copy the first 8 bytes of the valid region */
    Unaligned_store_u64a(inZ->buf,
                         UnalignedLoadU64a(end_first - copy_len_first));

    /* copy the last 16 bytes, may overlap with the previous 8 byte write */
    storeu128(z_end_first - sizeof(m128), loadu128(end_first - sizeof(m128)));
    if (unlikely(iter_bytes_second)) {
        storeu128(z_end - sizeof(m128), loadu128(end - sizeof(m128)));
    }

    inZ->zone_pointer_adjust = (ptrdiff_t)((uintptr_t)end - (uintptr_t)z_end);
}

static REALLY_INLINE size_t KHSEL_PrepareZones(const u8 *buf, size_t len, const u8 *hend,
                    size_t start, const u8 *flood, struct zone *zoneArr)
{
    const u8 *ptr = buf + start;
    size_t remaining = len - start;
    if (unlikely(remaining <= KHSEL_ITER_BYTES)) {
        /* enough bytes to make only one zone */
        KHSEL_CreateShortZone(buf, hend, ptr, buf + len, &zoneArr[0]);
        return 1;
    }
    size_t numZone = 0;
    KHSEL_CreateStartZone(buf, hend, ptr, &zoneArr[numZone++]);
    ptr += KHSEL_ITER_BYTES;
    const u8 *main_end = buf + start + KHSEL_ROUNDDOWN_N(len - start - 0x3, KHSEL_ITER_BYTES);
    if (main_end > ptr) {
        KHSEL_CreateMainZone(flood, ptr, main_end, &zoneArr[numZone++]);
        ptr = main_end;
    }
    /* create a zone with rest of the data from the main buffer */
    KHSEL_CreateEndZone(buf, ptr, buf + len, &zoneArr[numZone++]);
    return numZone;
}

#define INVALID_MATCH_ID (~0U)
#define FLOOD_MINIMUM_SIZE 256
#define FLOOD_BACKOFF_START 32

#if defined(HAVE_NEON)
#define PREFETCH __asm__ __volatile__("prfm pldl1keep, %0" ::"Q"(*(itPtr + 256)))
#define P2ALIGN   __asm__ __volatile__(".p2align 6")
#else
#define PREFETCH __builtin_prefetch(itPtr + KHSEL_ITER_BYTES)
#define P2ALIGN
#endif

#define KHSEL_FDR_MAIN_LOOP(zz, s, get_conf_fn)                             \
    do {                                                                    \
        P2ALIGN;                                                            \
        const u8 *start_ptr = (zz)->start;                                  \
        const u8 *end_ptr = (zz)->end - KHSEL_ITER_BYTES;                   \
                                                                            \
        for (const u8 *itPtr = start_ptr; itPtr <= end_ptr;                 \
            itPtr += KHSEL_ITER_BYTES) {                                    \
            PREFETCH;                                                       \
            u64a conf[2];                                                   \
            get_conf_fn(itPtr, domain_mask_flipped,                         \
                       ft, conf, (u64a *)&(s));                             \
            KHSEL_DoConfirmFdr(&conf[0], 0, &control, confBase, a, itPtr,   \
                           &last_match_id, (zz));                           \
            KHSEL_DoConfirmFdr(&conf[1], 8, &control, confBase, a, itPtr,   \
                           &last_match_id, (zz));                           \
            if (unlikely(control == KHSEL_HWLM_TERMINATE_MATCHING)) {       \
                return KHSEL_HWLM_TERMINATED;                               \
            }                                                               \
        } /* end for loop */                                                \
    } while (0)                                                             \

hwlm_error_t KHSEL_FdrEngineExec(const struct FDR *fdr,
    const struct FDR_Runtime_Args *a, hwlm_group_t control)
{
    KHSEL_RETURN_IF_NULL(fdr, KHSEL_HWLM_TERMINATED);
    KHSEL_RETURN_IF_NULL(a, KHSEL_HWLM_TERMINATED);
    u32 last_match_id = INVALID_MATCH_ID;
    u32 domain_mask_flipped = fdr->domainMask;
    u8 stride = fdr->stride;
    const u64a *ft =
        (const u64a *)((const u8 *)fdr + KHSEL_ROUNDUP_CL(sizeof(struct FDR)));
    const u32 *confBase = (const u32 *)((const u8 *)fdr + fdr->confOffset);
    struct zone zones[KHSEL_ZONE_MAX];

    size_t numZone = KHSEL_PrepareZones(a->buf, a->len, a->buf_history + a->len_history,
        a->start_offset, a->firstFloodDetect, zones);
    m128 state = KHSEL_GetInitState(fdr, a->len_history, ft, &zones[0]);

    for (size_t curZone = 0; curZone < numZone; curZone++) {
        struct zone *z = &zones[curZone];
        u8 shift = z->shift;
        state = variable_byte_shift_m128(state, shift);
        state = or128(state, Load128(khsel_zone_or_mask[shift]));
        switch (stride) {
        case 0x1:
            KHSEL_FDR_MAIN_LOOP(z, state, KHSELGetConfStrideOne);
            break;
        case 0x2:
            KHSEL_FDR_MAIN_LOOP(z, state, KHSELGetConfStrideBi);
            break;
        case 0x4:
            KHSEL_FDR_MAIN_LOOP(z, state, KHSELGetConfStrideQua);
            break;
        default:
            break;
        }
    }

    return KHSEL_HWLM_SUCCESS;
}

hwlm_error_t KHSEL_NeoFdrEngineExec(const struct FDR *fdr,
    const struct FDR_Runtime_Args *a, hwlm_group_t control)
{
    KHSEL_RETURN_IF_NULL(fdr, KHSEL_HWLM_TERMINATED);
    KHSEL_RETURN_IF_NULL(a, KHSEL_HWLM_TERMINATED);
    u32 last_match_id = INVALID_MATCH_ID;
    u32 domain_mask_flipped = fdr->domainMask;
    u8 stride = fdr->stride;
    const u64a *ft =
        (const u64a *)((const u8 *)fdr + KHSEL_ROUNDUP_CL(sizeof(struct FDR)));
    const u32 *confBase = (const u32 *)((const u8 *)fdr + fdr->confOffset);
    struct zone zones[KHSEL_ZONE_MAX];

    size_t numZone = KHSEL_PrepareZones(a->buf, a->len, a->buf_history + a->len_history,
        a->start_offset, a->firstFloodDetect, zones);
    m128 state = KHSEL_GetInitState(fdr, a->len_history, ft, &zones[0]);

    for (size_t curZone = 0; curZone < numZone; curZone++) {
        struct zone *z = &zones[curZone];
        u8 shift = z->shift;
        state = variable_byte_shift_m128(state, shift);
        state = or128(state, Load128(khsel_zone_or_mask[shift]));
        switch (stride) {
        case 0x1:
            KHSEL_FDR_MAIN_LOOP(z, state, KHSELGetConfStrideOne);
            break;
        case 0x2:
            KHSEL_FDR_MAIN_LOOP(z, state, KHSELGetConfStrideBi);
            break;
        case 0x4:
            KHSEL_FDR_MAIN_LOOP(z, state, KHSELGetConfStrideQua);
            break;
        default:
            break;
        }
    }

    return KHSEL_HWLM_SUCCESS;
}