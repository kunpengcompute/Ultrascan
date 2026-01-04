/*
 * Copyright (c) 2020-2022 Huawei Technologies Co., Ltd.
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

#include <stdio.h>
#include <stdlib.h>
#include "khsel_core.h"
#include "core_precomp.h"

#define CONSTRUCTOR __attribute__((constructor))
#define DESTRUCTOR __attribute__((destructor))

#define ARM_CPU_IMP_HISI 0x48

static int32_t gotokhsel_initialized = 0;

FORCE_INLINE void KhselHwDetect(void)
{
    int32_t cpu_id;
    __asm__ volatile("mrs %0, MIDR_EL1":"=r"(cpu_id));
    int32_t vendor = (cpu_id >> 0x18) & 0xFF;

#if defined(KHSEL_CPU_LIMIT) && KHSEL_CPU_LIMIT != 0
    if (vendor != ARM_CPU_IMP_HISI) {
        fprintf(stderr, "KHSEL: The software is running into an error, please check CPU ID.\n");
        abort();
    }
#endif
}

KHSEL_API_LOCAL CONSTRUCTOR void GotoKhselInit(void)
{
    if (gotokhsel_initialized) {
        return;
    }
    KhselHwDetect();
    gotokhsel_initialized = 1;
}
