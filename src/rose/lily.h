#ifndef LILY_H_
#define LILY_H_

#include "ue2common.h"

#include <vector>
#include <map>

typedef struct {
    u32 internal_id;    // 内部报告ID，用于构建过程
    u32 external_report; // 外部报告ID，用于运行时
    u32 ekey;
    unsigned flags;
} lilyReport;

std::vector<u8> KHSEL_BuildLily(std::map<char, lilyReport> &lily, std::vector<u32> &reportVec, std::vector<u32> &ekeyVec);

#endif