/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * All rights reserved.
 *
 * Licensed under the License;
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

#include "../rose/lily.h"
#include "hs_compile.h"

#define HS_FLAG_CASELESS 1

#include <vector>
#include <map>
#include "../fdr/fdr_compile_internal.h"
#include "../fdr/teddy_common_function.h"
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



static REALLY_INLINE
ue2::bytecode_ptr<lilyTeddy> buildLilyForTeddy(std::map<std::string, lilyReport> &lilyForTeddy,
                                    std::priority_queue<LilyForTeddyPair, std::vector<LilyForTeddyPair>, CompareStringLength> &lilyForTeddyPQ,
                                    std::vector<u32> &reportVec, std::vector<u32> &ekeyVec, std::vector<u32> &lenVec)
{
    u32 maxLitSize = 0;
    std::vector<ue2::hwlmLiteral> lits;
    std::map<ue2::BucketIndex, std::vector<ue2::LiteralIndex>> bucketToLits;
    size_t maxRules = std::min((size_t)8, lilyForTeddyPQ.size());
    u8 flagsQuiet = 0;
    for (size_t i = 0; i < maxRules; i++) {
        LilyForTeddyPair p = lilyForTeddyPQ.top();
        lilyForTeddyPQ.pop();
        maxLitSize = (p.first.size() >= maxLitSize) ? p.first.size() : maxLitSize;
        lilyForTeddy.insert(p);
        const bool nocase = (p.second.flags & HS_FLAG_CASELESS) != 0;
        lits.push_back(ue2::hwlmLiteral(p.first, nocase, (u32)p.second.external_report));
        reportVec[i] = (u32)p.second.external_report;
        ekeyVec[i] = (u32)p.second.ekey;
        lenVec[i] = (u32)p.first.length();
        
        // 检查当前规则是否为quiet模式，如果是则设置对应位
        if (p.second.flags & HS_FLAG_QUIET) {
            flagsQuiet |= (1 << i); // 使用i作为位掩码
        }
        
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
    size_t quietVecLen = sizeof(u8);
    size_t size = KHSEL_ROUNDUP_CL(headerSize) + KHSEL_ROUNDUP_CL(maskLen) + KHSEL_ROUNDUP_CL(reportVecLen) + KHSEL_ROUNDUP_CL(ekeyVecLen) + KHSEL_ROUNDUP_CL(lenVecLen) + KHSEL_ROUNDUP_CL(quietVecLen);

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

    // Write quiet flags
    assert(KHSEL_ISALIGNED_CL(ptr));
    teddy->lilyQuietOffset = ue2::verify_u32(ptr - teddy_base);
    memcpy(ptr, &flagsQuiet, quietVecLen);
    ptr += KHSEL_ROUNDUP_CL(quietVecLen);

    // Write teddy masks.
    u8 *baseMsk = teddy_base + KHSEL_ROUNDUP_CL(headerSize);
    ue2::fillNibbleMasks(bucketToLits, lits, NUM_MASKS, maskWidth, maskLen,
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