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

#include "lily.h"

#include <vector>
#include <map>
#include "../fdr/fdr_compile_internal.h"
#include "../util/verify_types.h"
#include "../util/compare.h"
#include "../fdr/teddy_internal.h"
#include <iostream>

static REALLY_INLINE
std::vector<u8> buildLily(std::map<char, lilyReport> &lily, std::vector<u32> &reportVec, std::vector<u32> &ekeyVec, u8 &flagsQuiet)
{
    std::vector<u8> singleMask;
    std::vector<u8> lo_a(16);
    std::vector<u8> hi_a(16);
    u8 idx = 1;
    int litCount = 0;
    
    // 初始化flagsQuiet为0
    flagsQuiet = 0;
    
    for (auto it : lily) {
        u8 t = it.first;
        u8 hi = t >> 4;
        u8 lo = t & 0xf;
        if ((it.second.flags & HS_FLAG_CASELESS) &&
            (((t >= 'A') && (t <= 'Z')) || ((t >= 'a') && (t <= 'z')))) {
            hi_a[hi & 0xd] |= idx;
            hi_a[(hi & 0xd) | 0x2] |= idx;
        } else {
            hi_a[hi] |= idx;
        }
        lo_a[lo] |= idx;
        
        // 检查当前规则是否为quiet模式，如果是则设置对应位
        if (it.second.flags & HS_FLAG_QUIET) {
            flagsQuiet |= idx; // 使用idx作为位掩码
        }
        
        idx = idx << 1;
        reportVec[litCount] = it.second.external_report;
        ekeyVec[litCount] = it.second.ekey;
        litCount++;
    }

    singleMask.insert(singleMask.end(), lo_a.begin(), lo_a.end());
    singleMask.insert(singleMask.end(), hi_a.begin(), hi_a.end());

    return singleMask;
}

static
void fillNibbleMasks(const std::map<ue2::BucketIndex,
                     std::vector<ue2::LiteralIndex>> &bucketToLits,
                     const std::vector<ue2::hwlmLiteral> &lits,
                     u32 numMasks, u32 maskWidth, size_t maskLen,
                     u8 *baseMsk) {
    memset(baseMsk, 0xff, maskLen);

    for (const auto &b2l : bucketToLits) {
        const u32 &bucket_id = b2l.first;
        const std::vector<ue2::LiteralIndex> &ids = b2l.second;
        const u8 bmsk = 1U << (bucket_id % 8);

        for (const ue2::LiteralIndex &lit_id : ids) {
            const ue2::hwlmLiteral &l = lits[lit_id];
            // DEBUG_PRINTF("putting lit %u into bucket %u\n", lit_id, bucket_id);
            const u32 sz = ue2::verify_u32(l.s.size());

            // fill in masks
            for (u32 j = 0; j < numMasks; j++) {
                const u32 msk_id_lo = j * 2 * maskWidth + (bucket_id / 8);
                const u32 msk_id_hi = (j * 2 + 1) * maskWidth + (bucket_id / 8);
                const u32 lo_base = msk_id_lo * 16;
                const u32 hi_base = msk_id_hi * 16;

                // if we don't have a char at this position, fill in i
                // locations in these masks with '1'
                if (j >= sz) {
                    for (u32 n = 0; n < 16; n++) {
                        baseMsk[lo_base + n] &= ~bmsk;
                        baseMsk[hi_base + n] &= ~bmsk;
                    }
                } else {
                    u8 c = l.s[sz - 1 - j];
                    // if we do have a char at this position
                    const u32 hiShift = 4;
                    u32 n_hi = (c >> hiShift) & 0xf;
                    u32 n_lo = c & 0xf;

                    if (j < l.msk.size() && l.msk[l.msk.size() - 1 - j]) {
                        u8 m = l.msk[l.msk.size() - 1 - j];
                        u8 m_hi = (m >> hiShift) & 0xf;
                        u8 m_lo = m & 0xf;
                        u8 cmp = l.cmp[l.msk.size() - 1 - j];
                        u8 cmp_lo = cmp & 0xf;
                        u8 cmp_hi = (cmp >> hiShift) & 0xf;

                        for (u8 cm = 0; cm < 0x10; cm++) {
                            if ((cm & m_lo) == (cmp_lo & m_lo)) {
                                baseMsk[lo_base + cm] &= ~bmsk;
                            }
                            if ((cm & m_hi) == (cmp_hi & m_hi)) {
                                baseMsk[hi_base + cm] &= ~bmsk;
                            }
                        }
                    } else {
                        if (l.nocase && ourisalpha(c)) {
                            u32 cmHalfClear = (0xdf >> hiShift) & 0xf;
                            u32 cmHalfSet = (0x20 >> hiShift) & 0xf;
                            baseMsk[hi_base + (n_hi & cmHalfClear)] &= ~bmsk;
                            baseMsk[hi_base + (n_hi | cmHalfSet)] &= ~bmsk;
                        } else {
                            baseMsk[hi_base + n_hi] &= ~bmsk;
                        }
                        baseMsk[lo_base + n_lo] &= ~bmsk;
                    }
                }
            }
        }
    }
}

static REALLY_INLINE
ue2::bytecode_ptr<lilyTeddy> buildLilyForTeddy(std::map<std::string, lilyReport> &lilyForTeddy,
                                    std::priority_queue<LilyForTeddyPair, std::vector<LilyForTeddyPair>, CompareStringLength> &lilyForTeddyPQ,
                                    std::vector<u32> &reportVec, std::vector<u32> &ekeyVec, std::vector<u32> &lenVec)
{
    u32 maxLitSize = 0;
    std::vector<ue2::hwlmLiteral> lits;
    std::map<ue2::BucketIndex, std::vector<ue2::LiteralIndex>> bucketToLits;
    size_t maxRules = std::min((size_t)8, lilyForTeddyPQ.size());
    for (int i = 0;i < maxRules;i++) {
        LilyForTeddyPair p = lilyForTeddyPQ.top();
        lilyForTeddyPQ.pop();
        maxLitSize = (p.first.size() >= maxLitSize) ? p.first.size() : maxLitSize;
        lilyForTeddy.insert(p);
        lits.push_back(ue2::hwlmLiteral(p.first, false, (u32)p.second.external_report));
        reportVec[i] = (u32)p.second.external_report;
        ekeyVec[i] = (u32)p.second.ekey;
        lenVec[i] = (u32)p.first.length();
        std::vector<ue2::LiteralIndex> litIdxVec;
        litIdxVec.push_back((ue2::LiteralIndex)i);
        bucketToLits.insert(std::make_pair((ue2::BucketIndex)i, litIdxVec));
    }

    const int NUM_BUCKETS = 8; // ?
    const int NUM_MASKS = maxLitSize; // ?
    u32 maskWidth = NUM_BUCKETS / 8;

    size_t headerSize = sizeof(lilyTeddy);
    size_t maskLen = NUM_MASKS * 16 * 2 * maskWidth;
    size_t reportVecLen = 8 * sizeof(u32);
    size_t ekeyVecLen = 8 * sizeof(u32);
    size_t lenVecLen = 8 * sizeof(u32);
    size_t size = KHSEL_ROUNDUP_CL(headerSize) + KHSEL_ROUNDUP_CL(maskLen) + KHSEL_ROUNDUP_CL(reportVecLen) + KHSEL_ROUNDUP_CL(ekeyVecLen) + KHSEL_ROUNDUP_CL(lenVecLen);

    auto fdr = ue2::make_zeroed_bytecode_ptr<lilyTeddy>(size, 64);
    assert(fdr); // otherwise would have thrown std::bad_alloc
    lilyTeddy *teddy = (lilyTeddy *)fdr.get(); // ugly
    u8 *teddy_base = (u8 *)teddy;

    // Write header.
    teddy->size = size;
    teddy->engineID = 19;
    teddy->maxStringLen = ue2::verify_u32(maxLen(lits));
    teddy->numStrings = ue2::verify_u32(lits.size()); 
    // Write report vector.
    u8 *ptr = teddy_base + KHSEL_ROUNDUP_CL(headerSize) + KHSEL_ROUNDUP_CL(maskLen);
    assert(KHSEL_ISALIGNED_CL(ptr));
    teddy->lilyReportOffset = ue2::verify_u32(ptr - teddy_base);
    memcpy(ptr, &reportVec[0], reportVecLen);
    ptr += KHSEL_ROUNDUP_CL(reportVecLen);

    // Write ekey vector.
    assert(KHSEL_ISALIGNED_CL(ptr));
    teddy->lilyEkeyOffset = ue2::verify_u32(ptr - teddy_base);
    memcpy(ptr, &ekeyVec[0], ekeyVecLen);
    ptr += KHSEL_ROUNDUP_CL(ekeyVecLen);

    // Write len vector
    assert(KHSEL_ISALIGNED_CL(ptr));
    teddy->lilyLenOffset = ue2::verify_u32(ptr - teddy_base);
    memcpy(ptr, &lenVec[0], lenVecLen);
    ptr += KHSEL_ROUNDUP_CL(lenVecLen);

    // Write teddy masks.
    u8 *baseMsk = teddy_base + KHSEL_ROUNDUP_CL(headerSize);
    fillNibbleMasks(bucketToLits, lits, NUM_MASKS, maskWidth, maskLen,
                    baseMsk);

    return fdr;
}

std::vector<u8> KHSEL_BuildLily(std::map<char, lilyReport> &lily,
                                std::vector<u32> &reportVec, std::vector<u32> &ekeyVec, u8 &flagsQuiet)
{
    return buildLily(lily, reportVec, ekeyVec, flagsQuiet);
}

ue2::bytecode_ptr<lilyTeddy> KHSEL_BuildLilyForTeddy(std::map<std::string, lilyReport> &lilyForTeddy,
                                        std::priority_queue<LilyForTeddyPair, std::vector<LilyForTeddyPair>, CompareStringLength> &lilyForTeddyPQ,
                                        std::vector<u32> &reportVec, std::vector<u32> &ekeyVec, std::vector<u32> &lenVec)
{
    return buildLilyForTeddy(lilyForTeddy, lilyForTeddyPQ, reportVec, ekeyVec, lenVec);
}