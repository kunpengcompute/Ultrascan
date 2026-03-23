#ifndef PBE_RUNTIME_H
#define PBE_RUNTIME_H

#include "ue2common.h"

#define PBE_RUNTIME_MAGIC 0x50424530U /* "PBE0" */
#define PBE_RUNTIME_VERSION 1U

struct PBERuntimeHeader {
    u32 magic;
    u32 version;
    u32 keyBits;
    u32 selectorCount;
    u32 primaryCount;
    u32 secondaryCount;
    u32 selectorsOffset;
    u32 primaryOffset;
    u32 secondaryOffset;
};

struct PBERuntimeBitSelector {
    u8 byteOffset;
    u8 bitOffset;
    u16 reserved;
};

struct PBERuntimeSecondaryHashEntry {
    u8 ruleVector[32];
    u8 tableControl[32];
    u32 headMask;
    u32 tailMask;
    u16 ruleBase;
    u16 ruleCount;
};

#endif // PBE_RUNTIME_H
