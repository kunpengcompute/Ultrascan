/*
 * Copyright (c) 2015-2020, Intel Corporation
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

/** \file
 * \brief Runtime code for hs_database manipulation.
 */

#ifndef FAT_DATABASE_H_D467FD6F343DDF
#define FAT_DATABASE_H_D467FD6F343DDF
#ifdef __cplusplus
extern "C"
{
#endif
#include "database_common.h"
#include "hs_compile.h" // for HS_MODE_ flags
#include "hs_version.h"
#include "ue2common.h"
#include "util/arch.h"

/*
 * a header to enclose the actual fat bytecode - useful for keeping info about the
 * compiled data.
 */
struct fat_hs_database {
    u32 magic;
    u32 version;
    u32 x86_length;
    u32 arm_length;
    u64a platform;
    u32 arm_crc32;
    u32 x86_crc32;
    u32 reserved0;
    u32 reserved1;
    u32 x86_bytecode; // offset of x86 bytecode relative to db start
    u32 arm_bytecode; // offset of arm bytecode relative to db start
    u32 padding[16];
    char bytes[];
};

static really_inline
const void *fat_hs_get_bytecode(const struct fat_hs_database *db) {
#if defined(ARCH_X86_64)
    return ((const char *)db + db->x86_bytecode);
#elif defined(ARCH_AARCH64)
    return ((const char *)db + db->arm_bytecode);
#else
    return ((const char *)db + db->x86_bytecode);
#endif
}

/**
 * Cheap database sanity checks used in block mode scan calls and streaming
 * mode open calls.
 */
static really_inline
hs_error_t fat_validDatabase(const fat_hs_database_t *db) {
    if (!db || db->magic != HS_DB_MAGIC) {
        return HS_INVALID;
    }
    if (db->version != HS_DB_VERSION) {
        return HS_DB_VERSION_ERROR;
    }

    return HS_SUCCESS;
}

hs_error_t fat_dbIsValid(const struct fat_hs_database *db);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DATABASE_H_D467FD6F343DDE */
