#ifndef LILY_H_
#define LILY_H_

#include "ue2common.h"

#include <vector>
#include <map>

typedef struct {
    u32 ReportID;
    u32 ekey;
    unsigned flags;
} lilyReport;

std::vector<u8> KHSEL_BuildLily(std::map<char, lilyReport> &lily, std::vector<u32> &reportVec, std::vector<u32> &ekeyVec);

#endif