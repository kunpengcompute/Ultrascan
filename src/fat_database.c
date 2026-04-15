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
 * INTERRUPTION) HOWEVER CAUSED BY ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/** \file
 * \brief Runtime code for fat_hs_database manipulation.
 */

#include <stdio.h>
#include <string.h>

#include "allocator.h"
#include "hs_common.h"
#include "hs_internal.h"
#include "hs_version.h"
#include "ue2common.h"
#include "fat_database.h"
#include "crc32.h"
#include "rose/rose_internal.h"
#include "util/unaligned.h"

static really_inline
int fat_db_correctly_aligned(const void *db) {
    return ISALIGNED_N(db, alignof(unsigned long long));
}

HS_PUBLIC_API
hs_error_t HS_CDECL fat_hs_free_database(fat_hs_database_t *db) {
    if (db && db->magic != HS_DB_MAGIC) {
        return HS_INVALID;
    }
    hs_database_free(db);

    return HS_SUCCESS;
}

HS_PUBLIC_API
hs_error_t HS_CDECL fat_hs_serialize_database(const fat_hs_database_t *db, 
                                              char **bytes,
                                              size_t *serialized_length) {
    if (!db || !bytes || !serialized_length) {
        return HS_INVALID;
    }

    if (!fat_db_correctly_aligned(db)) {
        return HS_BAD_ALIGN;
    }

    hs_error_t ret = fat_validDatabase(db);
    if (ret != HS_SUCCESS) {
        return ret;
    }

    size_t total_length = db->x86_length + db->arm_length;
    size_t length = sizeof(struct fat_hs_database) + total_length + 128;

    char *out = hs_misc_alloc(length);
    ret = hs_check_alloc(out);
    if (ret != HS_SUCCESS) {
        hs_misc_free(out);
        return ret;
    }

    memset(out, 0, length);

    u32 *buf = (u32 *)out;
    *buf = db->magic;
    buf++;
    *buf = db->version;
    buf++;
    *buf = db->x86_length;
    buf++;
    *buf = db->arm_length;
    buf++;
    memcpy(buf, &db->platform, sizeof(u64a));
    buf += 2;
    *buf = db->crc32;
    buf++;
    *buf = db->reserved0;
    buf++;
    *buf = db->reserved1;
    buf++;
    *buf = db->x86_bytecode;
    buf++;
    *buf = db->arm_bytecode;
    buf++;

    // 复制 x86 字节码
    const char *x86_bytecode = (const char *)db + db->x86_bytecode;
    memcpy((char *)out + db->x86_bytecode, x86_bytecode, db->x86_length);

    // 复制 arm 字节码
    const char *arm_bytecode = (const char *)db + db->arm_bytecode;
    memcpy((char *)out + db->arm_bytecode, arm_bytecode, db->arm_length);

    *bytes = out;
    *serialized_length = length;
    return HS_SUCCESS;
}

// check that the database header's platform is compatible with the current
// runtime platform.
static
hs_error_t fat_db_check_platform(const u64a p) {
    if (p != hs_current_platform
        && p != (hs_current_platform | hs_current_platform_no_avx2)
        && p != (hs_current_platform | hs_current_platform_no_avx512)
        && p != (hs_current_platform | hs_current_platform_no_avx512vbmi)) {
        return HS_DB_PLATFORM_ERROR;
    }
    // passed all checks
    return HS_SUCCESS;
}

// Decode and check the database header, returning appropriate errors or
// HS_SUCCESS if it's OK.
static
hs_error_t fat_db_decode_header(const char **bytes, const size_t length,
                                struct fat_hs_database *header) {
    if (!*bytes) {
        return HS_INVALID;
    }

    if (length < sizeof(struct fat_hs_database)) {
        return HS_INVALID;
    }

    const u32 *buf = (const u32 *)*bytes;

    memset(header, 0, sizeof(struct fat_hs_database));

    header->magic = unaligned_load_u32(buf++);
    if (header->magic != HS_DB_MAGIC) {
        return HS_INVALID;
    }

    header->version = unaligned_load_u32(buf++);
    if (header->version != HS_DB_VERSION) {
        return HS_DB_VERSION_ERROR;
    }

    header->x86_length = unaligned_load_u32(buf++);
    header->arm_length = unaligned_load_u32(buf++);
    
    header->platform = unaligned_load_u64a(buf);
    buf += 2;
    header->crc32 = unaligned_load_u32(buf++);
    header->reserved0 = unaligned_load_u32(buf++);
    header->reserved1 = unaligned_load_u32(buf++);
    header->x86_bytecode = unaligned_load_u32(buf++);
    header->arm_bytecode = unaligned_load_u32(buf++);

    size_t expected_length = sizeof(struct fat_hs_database) 
                             + header->x86_length + header->arm_length + 128;
    if (length < expected_length) {
        DEBUG_PRINTF("bad length %zu, expecting %zu\n", length, expected_length);
        return HS_INVALID;
    }

    *bytes = (const char *)buf;

    return HS_SUCCESS;
}

// Check the CRC on a fat database
static
hs_error_t fat_db_check_crc(const fat_hs_database_t *db) {
    const char *x86_bytecode = (const char *)db + db->x86_bytecode;
    u32 crc = Crc32c_ComputeBuf(0, x86_bytecode, db->x86_length);
    if (crc != db->crc32) {
        DEBUG_PRINTF("crc mismatch! 0x%x != 0x%x\n", crc, db->crc32);
        return HS_INVALID;
    }
    return HS_SUCCESS;
}

static
void fat_db_copy_bytecode(const char *serialized, fat_hs_database_t *db) {
    // we need to align things manually (64-byte alignment for x86 bytecode)
    uintptr_t shift = (uintptr_t)db->bytes & 0x3f;
    
    // 重新计算 x86 字节码偏移（64字节对齐）
    db->x86_bytecode = offsetof(struct fat_hs_database, bytes) - shift;
    char *x86_ptr = (char *)db + db->x86_bytecode;
    assert(ISALIGNED_CL(x86_ptr));
    
    // 复制 x86 字节码
    memcpy(x86_ptr, serialized + offsetof(struct fat_hs_database, bytes), db->x86_length);

    // arm 字节码偏移（在 x86 字节码之后，64字节对齐）
    size_t arm_offset = (db->x86_bytecode + db->x86_length + 63) & ~63;
    db->arm_bytecode = arm_offset;
    char *arm_ptr = (char *)db + db->arm_bytecode;
    assert(ISALIGNED_CL(arm_ptr));
    
    // 复制 arm 字节码
    memcpy(arm_ptr, serialized + offsetof(struct fat_hs_database, bytes) + db->x86_length, db->arm_length);
}

HS_PUBLIC_API
hs_error_t HS_CDECL fat_hs_deserialize_database_at(const char *bytes,
                                                   const size_t length,
                                                   fat_hs_database_t *db) {
    if (!bytes || !db) {
        return HS_INVALID;
    }

    if (!ISALIGNED_N(db, 8)) {
        return HS_BAD_ALIGN;
    }

    fat_hs_database_t header;
    hs_error_t ret = fat_db_decode_header(&bytes, length, &header);
    if (ret != HS_SUCCESS) {
        return ret;
    }

    ret = fat_db_check_platform(header.platform);
    if (ret != HS_SUCCESS) {
        return ret;
    }

    size_t dblength = sizeof(struct fat_hs_database) 
                      + header.x86_length + header.arm_length + 128;
    memset(db, 0, dblength);

    memcpy(db, &header, sizeof(header));

    fat_db_copy_bytecode(bytes, db);

    if (fat_db_check_crc(db) != HS_SUCCESS) {
        return HS_INVALID;
    }

    return HS_SUCCESS;
}

HS_PUBLIC_API
hs_error_t HS_CDECL fat_hs_deserialize_database(const char *bytes,
                                                const size_t length,
                                                fat_hs_database_t **db) {
    if (!bytes || !db) {
        return HS_INVALID;
    }

    *db = NULL;

    fat_hs_database_t header;
    hs_error_t ret = fat_db_decode_header(&bytes, length, &header);
    if (ret != HS_SUCCESS) {
        return ret;
    }

    ret = fat_db_check_platform(header.platform);
    if (ret != HS_SUCCESS) {
        return ret;
    }

    size_t dblength = sizeof(struct fat_hs_database) 
                      + header.x86_length + header.arm_length + 128;
    struct fat_hs_database *tempdb = hs_database_alloc(dblength);
    ret = hs_check_alloc(tempdb);
    if (ret != HS_SUCCESS) {
        hs_database_free(tempdb);
        return ret;
    }

    memset(tempdb, 0, dblength);

    memcpy(tempdb, &header, sizeof(header));

    fat_db_copy_bytecode(bytes, tempdb);

    if (fat_db_check_crc(tempdb) != HS_SUCCESS) {
        hs_database_free(tempdb);
        return HS_INVALID;
    }

    *db = tempdb;
    return HS_SUCCESS;
}

HS_PUBLIC_API
hs_error_t HS_CDECL fat_hs_database_size(const fat_hs_database_t *db, 
                                         size_t *size) {
    if (!size) {
        return HS_INVALID;
    }

    hs_error_t ret = fat_validDatabase(db);
    if (unlikely(ret != HS_SUCCESS)) {
        return ret;
    }

    *size = sizeof(struct fat_hs_database) + db->x86_length + db->arm_length + 128;
    return HS_SUCCESS;
}

HS_PUBLIC_API
hs_error_t HS_CDECL fat_hs_serialized_database_size(const char *bytes,
                                                    const size_t length,
                                                    size_t *size) {
    fat_hs_database_t header;
    hs_error_t ret = fat_db_decode_header(&bytes, length, &header);
    if (ret != HS_SUCCESS) {
        return ret;
    }

    if (!size) {
        return HS_INVALID;
    }

    *size = sizeof(struct fat_hs_database) + header.x86_length + header.arm_length + 128;
    return HS_SUCCESS;
}

hs_error_t fat_dbIsValid(const struct fat_hs_database *db) {
    if (db->magic != HS_DB_MAGIC) {
        DEBUG_PRINTF("bad magic\n");
        return HS_INVALID;
    }

    if (db->version != HS_DB_VERSION) {
        DEBUG_PRINTF("bad version\n");
        return HS_DB_VERSION_ERROR;
    }

    if (fat_db_check_platform(db->platform) != HS_SUCCESS) {
        DEBUG_PRINTF("bad platform\n");
        return HS_DB_PLATFORM_ERROR;
    }

    hs_error_t rv = fat_db_check_crc(db);
    if (rv != HS_SUCCESS) {
        DEBUG_PRINTF("bad crc\n");
        return rv;
    }

    return HS_SUCCESS;
}

#if defined(_WIN32)
#define SNPRINTF_COMPAT _snprintf
#else
#define SNPRINTF_COMPAT snprintf
#endif

static
hs_error_t fat_print_database_string(char **s, u32 version, const platform_t plat,
                                     u32 raw_mode) {
    assert(s);
    *s = NULL;

    u8 release = (version >> 8) & 0xff;
    u8 minor = (version >> 16) & 0xff;
    u8 major = (version >> 24) & 0xff;

    const char *features = (plat & HS_PLATFORM_NOAVX512VBMI)
                               ? (plat & HS_PLATFORM_NOAVX512)
                                   ? (plat & HS_PLATFORM_NOAVX2) ? "" : "AVX2"
                                   : "AVX512"
                               : "AVX512VBMI";

    const char *mode = NULL;

    if (raw_mode == HS_MODE_STREAM) {
        mode = "STREAM";
    } else if (raw_mode == HS_MODE_VECTORED) {
        mode = "VECTORED";
    } else {
        assert(raw_mode == HS_MODE_BLOCK);
        mode = "BLOCK";
    }

    size_t len = 256;

    while (1) {
        char *buf = hs_misc_alloc(len);
        hs_error_t ret = hs_check_alloc(buf);
        if (ret != HS_SUCCESS) {
            hs_misc_free(buf);
            return ret;
        }

        int p_len = SNPRINTF_COMPAT(
            buf, len, "Version: %u.%u.%u Features: %s Mode: %s (FAT)",
            major, minor, release, features, mode);
        if (p_len < 0) {
            DEBUG_PRINTF("snprintf output error, returned %d\n", p_len);
            hs_misc_free(buf);
            break;
        } else if ((size_t)p_len < len) {
            assert(buf[p_len] == '\0');
            *s = buf;
            return HS_SUCCESS;
        } else {
            len = (size_t)p_len + 1;
            hs_misc_free(buf);
        }
    }

    return HS_NOMEM;
}

HS_PUBLIC_API
hs_error_t HS_CDECL fat_hs_serialized_database_info(const char *bytes,
                                                    size_t length, char **info) {
    if (!info) {
        return HS_INVALID;
    }
    *info = NULL;

    fat_hs_database_t header;
    hs_error_t ret = fat_db_decode_header(&bytes, length, &header);
    if (ret != HS_SUCCESS) {
        return ret;
    }

    const struct RoseEngine *rose = fat_hs_get_bytecode(&header);
    u32 mode = rose->mode;

    return fat_print_database_string(info, header.version, header.platform, mode);
}

HS_PUBLIC_API
hs_error_t HS_CDECL fat_hs_database_info(const fat_hs_database_t *db, char **info) {
    if (!info) {
        return HS_INVALID;
    }
    *info = NULL;

    if (!db || !fat_db_correctly_aligned(db) || db->magic != HS_DB_MAGIC) {
        return HS_INVALID;
    }

    platform_t plat = db->platform;

    const struct RoseEngine *rose = fat_hs_get_bytecode(db);

    return fat_print_database_string(info, db->version, plat, rose->mode);
}