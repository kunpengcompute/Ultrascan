#ifndef LILY_H_
#define LILY_H_

#include "ue2common.h"

#include <vector>
#include <string>
#include <map>
#include <queue>
#include "../fdr/fdr_internal.h"
#include "../fdr/teddy_internal.h"
#include "../util/bytecode_ptr.h"

typedef struct {
    u32 internal_id;    // 内部报告ID，用于构建过程
    u32 external_report; // 外部报告ID，用于运行时
    u32 ekey;
    unsigned flags;
} lilyReport;

using LilyForTeddyPair = std::pair<std::string, lilyReport>;

struct CompareStringLength {
    bool operator()(const LilyForTeddyPair& lhs, const LilyForTeddyPair& rhs) {
        return lhs.first.length() > rhs.first.length();
    }
};

std::vector<u8> KHSEL_BuildLily(std::map<char, lilyReport> &lily, std::vector<u32> &reportVec,
                                std::vector<u32> &ekeyVec, u8 &flagsQuiet);
ue2::bytecode_ptr<lilyTeddy> KHSEL_BuildLilyForTeddy(std::map<std::string, lilyReport> &lilyForTeddy,
                                        std::priority_queue<LilyForTeddyPair, std::vector<LilyForTeddyPair>, CompareStringLength> &lilyForTeddyPQ,
                                        std::vector<u32> &reportVec, std::vector<u32> &ekeyVec, std::vector<u32> &lenVec);

#endif