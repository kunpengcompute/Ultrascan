/* Copyright (c) 2026, Huawei Technologies Co., Ltd. */

/* Test-only preload fixture: remove capabilities, never fabricate hardware. */
#define _GNU_SOURCE
#include <asm/hwcap.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

unsigned long getauxval(unsigned long type) {
    typedef unsigned long (*getauxval_fn)(unsigned long);
    getauxval_fn next = (getauxval_fn)dlsym(RTLD_NEXT, "getauxval");
    if (!next) {
        return 0;
    }
    unsigned long value = next(type);
    const char *mode = getenv("HS_TEST_CAPS");
    if (!mode) {
        return value;
    }
    const int fdr = strcmp(mode, "FDR") == 0;
    const int dot = strcmp(mode, "DOT") == 0;
    if (fdr && type == AT_HWCAP) {
        value &= ~HWCAP_SVE;
    }
    if ((fdr || dot) && type == AT_HWCAP2) {
#ifdef HWCAP2_SVE2
        value &= ~HWCAP2_SVE2;
#endif
#ifdef HWCAP2_SVEBITPERM
        value &= ~HWCAP2_SVEBITPERM;
#endif
    }
    return value;
}
