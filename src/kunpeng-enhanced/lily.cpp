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

std::vector<u8> KHSEL_BuildLily(std::map<char, lilyReport> &lily,
                                std::vector<u32> &reportVec, std::vector<u32> &ekeyVec, u8 &flagsQuiet)
{
    return buildLily(lily, reportVec, ekeyVec, flagsQuiet);
}