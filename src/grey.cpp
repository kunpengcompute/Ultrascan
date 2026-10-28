/*
 * Copyright (c) 2015-2018, Intel Corporation
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

#include "grey.h"
#include "ue2common.h"

#include <algorithm>
#include <cstdlib> // exit
#include <string>
#include <limits.h>
#include <cstring>
#include <mutex>
#include <vector>

#define DEFAULT_MAX_HISTORY 110

using namespace std;

namespace ue2 {

// Global overrides storage, protected by mutex for thread safety.
static std::string g_overrides;
static std::mutex g_overrides_mutex;

void setGreyOverrides(const std::string &overrides) {
    std::lock_guard<std::mutex> lock(g_overrides_mutex);
    g_overrides = overrides;
}

void resetGreyOverrides() {
    std::lock_guard<std::mutex> lock(g_overrides_mutex);
    g_overrides.clear();
}

std::string getGreyOverrides() {
    std::lock_guard<std::mutex> lock(g_overrides_mutex);
    return g_overrides;
}

Grey::Grey(bool applyOverrides) :
                   optimiseComponentTree(true),
                   calcComponents(true),
                   performGraphSimplification(true),
                   prefilterReductions(true),
                   removeEdgeRedundancy(true),
                   allowGough(true),
                   allowHaigLit(true),
                   allowLitHaig(true),
                   allowLbr(true),
                   allowMcClellan(true),
                   allowSheng(true),
                   allowMcSheng(true),
                   allowNeoFdr(false),
                   allowHao(false),
                   allowPuff(true),
                   allowLiteral(true),
                   allowViolet(true),
                   allowExtendedNFA(true), /* bounded repeats of course */
                   allowLimExNFA(true),
                   allowLily(false),
                   allowAnchoredAcyclic(true),
                   allowSmallLiteralSet(true),
                   allowCastle(true),
                   allowDecoratedLiteral(true),
                   allowApproximateMatching(true),
                   allowNoodle(true),
                   fdrAllowTeddy(true),
                   fdrAllowFlood(true),
                   violetAvoidSuffixes(true),
                   violetAvoidWeakInfixes(true),
                   violetDoubleCut(true),
                   violetExtractStrongLiterals(true),
                   violetLiteralChains(true),
                   violetDoubleCutLiteralLen(3),
                   violetEarlyCleanLiteralLen(6),
                   puffImproveHead(true),
                   castleExclusive(true),
                   mergeSEP(true), /* short exhaustible passthroughs */
                   mergeRose(true), // roses inside rose
                   mergeSuffixes(true), // suffix nfas inside rose
                   mergeOutfixes(true),
                   onlyOneOutfix(false),
                   allowShermanStates(true),
                   allowMcClellan8(true),
                   allowWideStates(true), // enable wide state for McClellan8
                   highlanderPruneDFA(true),
                   minimizeDFA(true),
                   accelerateDFA(true),
                   accelerateNFA(true),
                   reverseAccelerate(true),
                   squashNFA(true),
                   compressNFAState(true),
                   numberNFAStatesWrong(false), /* debugging only */
                   highlanderSquash(true),
                   allowZombies(true),
                   floodAsPuffette(false),
                   nfaForceSize(0),
                   maxHistoryAvailable(DEFAULT_MAX_HISTORY),
                   minHistoryAvailable(0), /* debugging only */
                   maxAnchoredRegion(63), /* for rose's atable to run over */
                   minRoseLiteralLength(3),
                   minRoseNetflowLiteralLength(2),
                   maxRoseNetflowEdges(50000), /* otherwise no netflow pass. */
                   maxEditDistance(16),
                   minExtBoundedRepeatSize(32),
                   goughCopyPropagate(true),
                   goughRegisterAllocate(true),
                   shortcutLiterals(true),
                   roseGraphReduction(true),
                   roseRoleAliasing(true),
                   roseMasks(true),
                   roseConvertFloodProneSuffixes(true),
                   roseMergeRosesDuringAliasing(true),
                   roseMultiTopRoses(true),
                   roseHamsterMasks(true),
                   roseLookaroundMasks(true),
                   roseMcClellanPrefix(1),
                   roseMcClellanSuffix(1),
                   roseMcClellanOutfix(2),
                   roseTransformDelay(true),
                   earlyMcClellanPrefix(true),
                   earlyMcClellanInfix(true),
                   earlyMcClellanSuffix(true),
                   allowCountingMiracles(true),
                   allowSomChain(true),
                   somMaxRevNfaLength(126),
                   hamsterAccelForward(true),
                   hamsterAccelReverse(false),
                   miracleHistoryBonus(16),
                   equivalenceEnable(true),

                   allowSmallWrite(true), // McClellan dfas for small patterns
                   allowSmallWriteSheng(false), // allow use of Sheng for SMWR

                   smallWriteLargestBuffer(70), // largest buffer that can be
                                                // considered a small write
                                                // all blocks larger than this
                                                // are given to rose &co
                   smallWriteLargestBufferBad(35),
                   limitSmallWriteOutfixSize(1048576), // 1 MB
                   smallWriteMaxPatterns(10000),
                   smallWriteMaxLiterals(10000),
                   smallWriteMergeBatchSize(20),
                   allowTamarama(true), // Tamarama engine
                   tamaChunkSize(100),
                   dumpFlags(0),
                   limitPatternCount(8000000), // 8M patterns
                   limitPatternLength(16000),  // 16K bytes
                   limitGraphVertices(500000), // 500K vertices
                   limitGraphEdges(1000000), // 1M edges
                   limitReportCount(4*8000000),
                   limitLiteralCount(8000000), // 8M literals
                   limitLiteralLength(16000),
                   limitLiteralMatcherChars(1073741824), // 1 GB
                   limitLiteralMatcherSize(1073741824), // 1 GB
                   limitRoseRoleCount(4*8000000),
                   limitRoseEngineCount(8000000), // 8M engines
                   limitRoseAnchoredSize(1073741824), // 1 GB
                   limitEngineSize(1073741824), // 1 GB
                   limitDFASize(1073741824), // 1 GB
                   limitNFASize(1048576), // 1 MB
                   limitLBRSize(1048576), // 1 MB
                   limitApproxMatchingVertices(5000)
{
    assert(maxAnchoredRegion < 64); /* a[lm]_log_sum have limited capacity */
    
    if (!applyOverrides) {
        return;
    }
    
    // Apply globally-set overrides instead of reading from config.txt file.
    std::string overrides;
    {
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        overrides = g_overrides;
    }
    if (!overrides.empty()) {
        applyGreyOverrides(this, overrides);
    }
}

} // namespace ue2

#include <boost/lexical_cast.hpp>
using boost::lexical_cast;

namespace ue2 {

bool applyGreyOverrides(Grey *g, const string &s) {
    string::const_iterator p = s.begin();
    string::const_iterator pe = s.end();
    string help = "help:0";
    bool invalid_key_seen = false;

    Grey defaultg(false);
    Grey staged = *g;

    if (s == "help" || s == "help:") {
        printf("Valid grey overrides:\n");
        p = help.begin();
        pe = help.end();
    }

    while (p != pe) {
        string::const_iterator ke = find(p, pe, ':');

        if (ke == pe) {
            // No colon found in remaining string: treat as invalid format.
            // Skip leading semicolons (which can appear between entries).
            if (p != pe && *p != ';') {
                invalid_key_seen = true;
            }
            break;
        }

        string key(p, ke);

        string::const_iterator ve = find(ke, pe, ';');

        unsigned int value = 0;
        try {
            value = lexical_cast<unsigned int>(string(ke + 1, ve));
        } catch (boost::bad_lexical_cast &e) {
            printf("Invalid grey override key %s:%s\n", key.c_str(),
                   string(ke + 1, ve).c_str());
            invalid_key_seen = true;
            break;
        }
        bool done = false;

        /* surely there exists a nice template to go with this macro to make
         * all the boring code disappear */
#define G_UPDATE(k) do {                                                \
            if (key == ""#k) { staged.k = value; done = 1;}            \
            if (key == "help") {                                        \
                printf("\t%-30s\tdefault: %s\n", #k,                    \
                       lexical_cast<string>(defaultg.k).c_str());       \
            }                                                           \
        } while (0)

        G_UPDATE(optimiseComponentTree);
        G_UPDATE(calcComponents);
        G_UPDATE(performGraphSimplification);
        G_UPDATE(prefilterReductions);
        G_UPDATE(removeEdgeRedundancy);
        G_UPDATE(allowGough);
        G_UPDATE(allowHaigLit);
        G_UPDATE(allowLitHaig);
        G_UPDATE(allowLbr);
        G_UPDATE(allowMcClellan);
        G_UPDATE(allowSheng);
        G_UPDATE(allowMcSheng);
        G_UPDATE(allowNeoFdr);
        G_UPDATE(allowHao);
        G_UPDATE(allowPuff);
        G_UPDATE(allowLiteral);
        G_UPDATE(allowLily);
        G_UPDATE(allowViolet);
        G_UPDATE(allowExtendedNFA);
        G_UPDATE(allowLimExNFA);
        G_UPDATE(allowAnchoredAcyclic);
        G_UPDATE(allowSmallLiteralSet);
        G_UPDATE(allowCastle);
        G_UPDATE(allowDecoratedLiteral);
        G_UPDATE(allowNoodle);
        G_UPDATE(allowApproximateMatching);
        G_UPDATE(fdrAllowTeddy);
        G_UPDATE(fdrAllowFlood);
        G_UPDATE(violetAvoidSuffixes);
        G_UPDATE(violetAvoidWeakInfixes);
        G_UPDATE(violetDoubleCut);
        G_UPDATE(violetExtractStrongLiterals);
        G_UPDATE(violetLiteralChains);
        G_UPDATE(violetDoubleCutLiteralLen);
        G_UPDATE(violetEarlyCleanLiteralLen);
        G_UPDATE(puffImproveHead);
        G_UPDATE(castleExclusive);
        G_UPDATE(mergeSEP);
        G_UPDATE(mergeRose);
        G_UPDATE(mergeSuffixes);
        G_UPDATE(mergeOutfixes);
        G_UPDATE(onlyOneOutfix);
        G_UPDATE(allowShermanStates);
        G_UPDATE(allowMcClellan8);
        G_UPDATE(allowWideStates);
        G_UPDATE(highlanderPruneDFA);
        G_UPDATE(minimizeDFA);
        G_UPDATE(accelerateDFA);
        G_UPDATE(accelerateNFA);
        G_UPDATE(reverseAccelerate);
        G_UPDATE(squashNFA);
        G_UPDATE(compressNFAState);
        G_UPDATE(numberNFAStatesWrong);
        G_UPDATE(allowZombies);
        G_UPDATE(floodAsPuffette);
        G_UPDATE(nfaForceSize);
        G_UPDATE(highlanderSquash);
        G_UPDATE(maxHistoryAvailable);
        G_UPDATE(minHistoryAvailable);
        G_UPDATE(maxAnchoredRegion);
        G_UPDATE(minRoseLiteralLength);
        G_UPDATE(minRoseNetflowLiteralLength);
        G_UPDATE(maxRoseNetflowEdges);
        G_UPDATE(maxEditDistance);
        G_UPDATE(minExtBoundedRepeatSize);
        G_UPDATE(goughCopyPropagate);
        G_UPDATE(goughRegisterAllocate);
        G_UPDATE(shortcutLiterals);
        G_UPDATE(roseGraphReduction);
        G_UPDATE(roseRoleAliasing);
        G_UPDATE(roseMasks);
        G_UPDATE(roseConvertFloodProneSuffixes);
        G_UPDATE(roseMergeRosesDuringAliasing);
        G_UPDATE(roseMultiTopRoses);
        G_UPDATE(roseHamsterMasks);
        G_UPDATE(roseLookaroundMasks);
        G_UPDATE(roseMcClellanPrefix);
        G_UPDATE(roseMcClellanSuffix);
        G_UPDATE(roseMcClellanOutfix);
        G_UPDATE(roseTransformDelay);
        G_UPDATE(earlyMcClellanPrefix);
        G_UPDATE(earlyMcClellanInfix);
        G_UPDATE(earlyMcClellanSuffix);
        G_UPDATE(allowSomChain);
        G_UPDATE(allowCountingMiracles);
        G_UPDATE(somMaxRevNfaLength);
        G_UPDATE(hamsterAccelForward);
        G_UPDATE(hamsterAccelReverse);
        G_UPDATE(miracleHistoryBonus);
        G_UPDATE(equivalenceEnable);
        G_UPDATE(allowSmallWrite);
        G_UPDATE(allowSmallWriteSheng);
        G_UPDATE(smallWriteLargestBuffer);
        G_UPDATE(smallWriteLargestBufferBad);
        G_UPDATE(limitSmallWriteOutfixSize);
        G_UPDATE(smallWriteMaxPatterns);
        G_UPDATE(smallWriteMaxLiterals);
        G_UPDATE(smallWriteMergeBatchSize);
        G_UPDATE(allowTamarama);
        G_UPDATE(tamaChunkSize);
        G_UPDATE(limitPatternCount);
        G_UPDATE(limitPatternLength);
        G_UPDATE(limitGraphVertices);
        G_UPDATE(limitGraphEdges);
        G_UPDATE(limitReportCount);
        G_UPDATE(limitLiteralCount);
        G_UPDATE(limitLiteralLength);
        G_UPDATE(limitLiteralMatcherChars);
        G_UPDATE(limitLiteralMatcherSize);
        G_UPDATE(limitRoseRoleCount);
        G_UPDATE(limitRoseEngineCount);
        G_UPDATE(limitRoseAnchoredSize);
        G_UPDATE(limitEngineSize);
        G_UPDATE(limitDFASize);
        G_UPDATE(limitNFASize);
        G_UPDATE(limitLBRSize);
        G_UPDATE(limitApproxMatchingVertices);

#undef G_UPDATE
        if (key == "simple_som") {
            staged.allowHaigLit = false;
            staged.allowLitHaig = false;
            staged.allowSomChain = false;
            staged.somMaxRevNfaLength = 0;
            done = true;
        }
        if (key == "forceOutfixesNFA") {
            staged.allowAnchoredAcyclic = false;
            staged.allowCastle = false;
            staged.allowDecoratedLiteral = false;
            staged.allowGough = false;
            staged.allowHaigLit = false;
            staged.allowLbr = false;
            staged.allowLimExNFA = true;
            staged.allowLitHaig = false;
            staged.allowMcClellan = false;
            staged.allowPuff = false;
            staged.allowLiteral = false;
            staged.allowViolet = false;
            staged.allowSmallLiteralSet = false;
            staged.roseMasks = false;
            done = true;
        }
        if (key == "forceOutfixesDFA") {
            staged.allowAnchoredAcyclic = false;
            staged.allowCastle = false;
            staged.allowDecoratedLiteral = false;
            staged.allowGough = false;
            staged.allowHaigLit = false;
            staged.allowLbr = false;
            staged.allowLimExNFA = false;
            staged.allowLitHaig = false;
            staged.allowMcClellan = true;
            staged.allowPuff = false;
            staged.allowLiteral = false;
            staged.allowViolet = false;
            staged.allowSmallLiteralSet = false;
            staged.roseMasks = false;
            done = true;
        }
        if (key == "forceOutfixes") {
            staged.allowAnchoredAcyclic = false;
            staged.allowCastle = false;
            staged.allowDecoratedLiteral = false;
            staged.allowGough = true;
            staged.allowHaigLit = false;
            staged.allowLbr = false;
            staged.allowLimExNFA = true;
            staged.allowLitHaig = false;
            staged.allowMcClellan = true;
            staged.allowPuff = false;
            staged.allowLiteral = false;
            staged.allowViolet = false;
            staged.allowSmallLiteralSet = false;
            staged.roseMasks = false;
            done = true;
        }

        if (!done && key != "help") {
            printf("Invalid grey override key %s:%u\n", key.c_str(), value);
            invalid_key_seen = true;
        }

        p = ve;

        if (p != pe) {
            ++p;
        }
    }

    if (invalid_key_seen) {
        return false;
    }

    assert(staged.maxAnchoredRegion < 64); /* a[lm]_log_sum have limited capacity */
    *g = staged;
    return true;
}

} // namespace ue2
