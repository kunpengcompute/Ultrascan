# API Reference

<!-- md-trans-meta sourceCommit=fd215461e79615cfea45605856a99c32a14a7e4b translatedAt=2026-08-27T01:40:31.746Z pushedAt=2026-09-09T08:01:11.916Z -->

## 1 Document Scope

This document summarizes the public APIs added in Ultrascan on top of the standard Hyperscan C APIs. It is categorized into the following three segments:

- [Universal bytecode APIs](#2-universal-bytecode-apis): `fat_hs_*`

- [Grey configuration APIs](#3-grey-configuration-apis): process-level compilation parameter setting and resetting

- [Feedback-driven optimization APIs for regular expression matching](#4-feedback-driven-optimization-apis-for-regular-expression-matching): runtime collection, feedback generation, and feedback-based compilation

All APIs can be included through the public header file `hs.h`. This document does not repeat the description of the upstream general-purpose APIs such as `hs_compile()` and `hs_scan()`; when using these APIs, refer to the comments in the public header file and the upstream API documents.

>![](public_sys-resources/icon-note.gif) **NOTE:** Feedback-driven optimization for regular expression matching is a new closed-loop feedback capability in V5.8.0, and it is an independent feature from the false-positive blocking technology. This document does not replace the names or descriptions of historical features with the new ones.

### 1.1 Common Conventions

- The API return type is `hs_error_t`. Successful API calls normally return `HS_SUCCESS`; invalid arguments return `HS_INVALID`; compilation errors return `HS_COMPILER_ERROR` with error details provided through `hs_compile_error_t`.

- Upon a compilation failure, the caller should use `hs_free_compile_error()` to release the non-empty compilation error object.

- Objects marked as opaque in this document can only be created, passed, and released through the corresponding APIs. The caller must not access their internal layout.

- Unless an API explicitly transfers ownership, ownership of input objects remains with the caller.

### 1.2 Example Paths

The commands in this document follow the fixed directory layout in [Installation Guide](./installation_guide.md):

- Ultrascan source code: `/opt/Ultrascan`

- Default static library build directory: `/opt/Ultrascan/build`

- Debug build directory: `/opt/Ultrascan/build-debug`

- Feedback-driven optimization build directory: `/opt/Ultrascan/build-feedback`

## 2 Universal Bytecode APIs

The universal bytecode technology encapsulates x86 and AArch64 bytecode in the same `fat_hs_database_t` object, and is applicable to scenarios where bytecode needs to be distributed to both platform types after a single compilation. `fat_hs_database_t` is an opaque type.

```c
typedef struct fat_hs_database fat_hs_database_t;
```

### 2.1 API List

| Category | API | Description |
| --- | --- | --- |
| Compilation | `fat_hs_compile` | Compiles a single regular expression. |
| Compilation | `fat_hs_compile_multi` | Compiles multiple regular expressions. |
| Compilation | `fat_hs_compile_ext_multi` | Compiles multiple regular expressions with extended parameters. |
| Compilation | `fat_hs_compile_lit` | Compiles a single literal. |
| Compilation | `fat_hs_compile_lit_multi` | Compiles multiple literals. |
| Lifecycle | `fat_hs_free_database` | Releases a universal bytecode database. |
| Serialization | `fat_hs_serialize_database` | Serializes a universal database into a byte stream. |
| Deserialization | `fat_hs_deserialize_database` | Allocates the memory and deserializes a universal database. |
| Deserialization | `fat_hs_deserialize_database_at` | Deserializes a database in the memory provided by the caller. |
| Query | `fat_hs_database_size` | Queries the size of a universal database in memory. |
| Query | `fat_hs_serialized_database_size` | Queries the space required after deserializing serialized data. |
| Query | `fat_hs_database_info` | Queries database version and mode information. |

### 2.2 `fat_hs_compile`

Compiles a regular expression to generate a database containing both x86 and AArch64 bytecode.

```c
hs_error_t HS_CDECL fat_hs_compile(
    const char *expression, unsigned int flags, unsigned int mode,
    const hs_platform_info_t *platform, fat_hs_database_t **db,
    hs_compile_error_t **error);
```

| Parameter | Description |
| --- | --- |
| `expression` | Regular expression ending with `NULL`, which must not be empty and does not contain delimiters or embedded flags. |
| `flags` | `HS_FLAG_*` flags that apply to this expression. Multiple flags can be combined through bitwise OR. |
| `mode` | Database mode. The value can be `HS_MODE_BLOCK`, `HS_MODE_STREAM`, or `HS_MODE_VECTORED`. |
| `platform` | (Optional) Target platform information. The `NULL` value indicates that the database is generated based on the current host platform. |
| `db` | Non-empty output parameter. A successful API call returns the universal database; a failed API call sets this parameter to `NULL`. The caller uses `fat_hs_free_database()` to release the object returned upon a successful call. |
| `error` | Non-empty output parameter. A compilation failure returns error details. The caller uses `hs_free_compile_error()` to release the non-empty object. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The compilation is successful. |
| `HS_COMPILER_ERROR` | The `db`, `error`, `expression`, `mode`, or `platform` parameter is invalid, or expression parsing or compilation fails. |
| `HS_ARCH_ERROR` | The database is built with `FAT_RUNTIME` and the runtime platform does not support SSSE3. |

### 2.3 `fat_hs_compile_multi`

Compiles multiple regular expressions to generate a universal database.

```c
hs_error_t HS_CDECL fat_hs_compile_multi(
    const char *const *expressions, const unsigned int *flags,
    const unsigned int *ids, unsigned int elements, unsigned int mode,
    const hs_platform_info_t *platform, fat_hs_database_t **db,
    hs_compile_error_t **error);
```

| Parameter | Description |
| --- | --- |
| `expressions` | Array of regular expressions ending with `NULL`, which must not be empty. The number of array elements is `elements`. |
| `flags` | Array of `HS_FLAG_*` flags corresponding to each expression, which can be `NULL`, indicating that all flags are 0. |
| `ids` | Array of IDs corresponding to each expression, which can be `NULL`, indicating that the default IDs are used. |
| `elements` | Number of expressions, which must be greater than 0. |
| `mode` | Database mode. The value can be `HS_MODE_BLOCK`, `HS_MODE_STREAM`, or `HS_MODE_VECTORED`. |
| `platform` | (Optional) Target platform information. The `NULL` value indicates that the database is generated based on the current host platform. |
| `db` | Non-empty output parameter. A successful API call returns the universal database; a failed API call sets this parameter to `NULL`. The caller uses `fat_hs_free_database()` to release the object returned upon a successful call. |
| `error` | Non-empty output parameter. A compilation failure returns error details. The caller uses `hs_free_compile_error()` to release the non-empty object. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The compilation is successful. |
| `HS_COMPILER_ERROR` | The `db`, `error`, `expressions`, `elements`, `mode`, or `platform` parameter is invalid, or any expression fails to be parsed or compiled. |
| `HS_ARCH_ERROR` |  The database is built with `FAT_RUNTIME` and the runtime platform does not support SSSE3. |

### 2.4 `fat_hs_compile_ext_multi`

Compiles multiple regular expressions with extended parameters to generate a universal database.

```c
hs_error_t HS_CDECL fat_hs_compile_ext_multi(
    const char *const *expressions, const unsigned int *flags,
    const unsigned int *ids, const hs_expr_ext_t *const *ext,
    unsigned int elements, unsigned int mode,
    const hs_platform_info_t *platform, fat_hs_database_t **db,
    hs_compile_error_t **error);
```

| Parameter | Description |
| --- | --- |
| `expressions` | Array of regular expressions ending with `NULL`, which must not be empty. The number of array elements is `elements`. |
| `flags` | Array of `HS_FLAG_*` flags corresponding to each expression, which can be `NULL`, indicating that all flags are 0. |
| `ids` | Array of IDs corresponding to each expression, which can be `NULL`, indicating that the default IDs are used. |
| `ext` | Array of `hs_expr_ext_t` pointers corresponding to each expression, which can be `NULL`. Individual array elements can also be `NULL`. |
| `elements` | Number of expressions, which must be greater than 0. |
| `mode` | Database mode. The value can be `HS_MODE_BLOCK`, `HS_MODE_STREAM`, or `HS_MODE_VECTORED`. |
| `platform` | (Optional) Target platform information. The `NULL` value indicates that the database is generated based on the current host platform. |
| `db` | Non-empty output parameter. A successful API call returns the universal database; a failed API call sets this parameter to `NULL`. The caller uses `fat_hs_free_database()` to release the object returned upon a successful call. |
| `error` | Non-empty output parameter. A compilation failure returns error details. The caller uses `hs_free_compile_error()` to release the non-empty object. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The compilation is successful. |
| `HS_COMPILER_ERROR` | The `db`, `error`, `expressions`, `ext`, `elements`, `mode`, or `platform` parameter is invalid, or any expression fails to be parsed or compiled. |
| `HS_ARCH_ERROR` | The database is built with `FAT_RUNTIME` and the runtime platform does not support SSSE3. |

### 2.5 `fat_hs_compile_lit`

Compiles a literal expression to generate a universal database. This API does not interpret the byte content according to regular expression syntax.

```c
hs_error_t HS_CDECL fat_hs_compile_lit(
    const char *expression, unsigned flags, const size_t len,
    unsigned mode, const hs_platform_info_t *platform,
    fat_hs_database_t **db, hs_compile_error_t **error);
```

| Parameter | Description |
| --- | --- |
| `expression` | A literal byte sequence, which must not be empty; the data may contain `\0`. |
| `flags` | `HS_FLAG_*` flags applied to this literal. |
| `len` | Byte length of `expression`. The API reads only the first `len` bytes. |
| `mode` | Database mode. The value can be `HS_MODE_BLOCK`, `HS_MODE_STREAM`, or `HS_MODE_VECTORED`. |
| `platform` | (Optional) Target platform information. The `NULL` value indicates that the database is generated based on the current host platform. |
| `db` | Non-empty output parameter. A successful API call returns the universal database; a failed API call sets this parameter to `NULL`. The caller uses `fat_hs_free_database()` to release the object returned upon a successful call. |
| `error` | Non-empty output parameter. A compilation failure returns error details. The caller uses `hs_free_compile_error()` to release the non-empty object. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The compilation is successful. |
| `HS_COMPILER_ERROR` | The `db`, `error`, `expression`, `len`, `mode`, or `platform` parameter is invalid, or compilation fails. |
| `HS_ARCH_ERROR` | The database is built with `FAT_RUNTIME` and the runtime platform does not support SSSE3. |

### 2.6 `fat_hs_compile_lit_multi`

Compiles multiple literal expressions to generate a universal database.

```c
hs_error_t HS_CDECL fat_hs_compile_lit_multi(
    const char *const *expressions, const unsigned *flags,
    const unsigned *ids, const size_t *lens, unsigned elements,
    unsigned mode, const hs_platform_info_t *platform,
    fat_hs_database_t **db, hs_compile_error_t **error);
```

| Parameter | Description |
| --- | --- |
| `expressions` | Array of literal byte sequences, which must not be empty. The number of array elements is `elements`, and each element may contain `\0`. |
| `flags` | Array of `HS_FLAG_*` flags corresponding to each literal, which can be `NULL`, indicating that all flags are 0. |
| `ids` | Array of IDs corresponding to each literal, which can be `NULL`, indicating that the default IDs are used. |
| `lens` | Array of literal byte lengths, which must not be empty. The array has a one-to-one correspondence with `expressions`. `lens[i]` specifies the number of bytes read from `expressions[i]`. A literal may contain `\0`, and the API reads only the first `lens[i]` bytes. |
| `elements` | Number of literals, which must be greater than 0. |
| `mode` | Database mode. The value can be `HS_MODE_BLOCK`, `HS_MODE_STREAM`, or `HS_MODE_VECTORED`. |
| `platform` | (Optional) Target platform information. The `NULL` value indicates that the database is generated based on the current host platform. |
| `db` | Non-empty output parameter. A successful API call returns the universal database; a failed API call sets this parameter to `NULL`. The caller uses `fat_hs_free_database()` to release the object returned upon a successful call. |
| `error` | Non-empty output parameter. A compilation failure returns error details. The caller uses `hs_free_compile_error()` to release the non-empty object. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The compilation is successful. |
| `HS_COMPILER_ERROR` | The `db`, `error`, `expressions`, `lens`, `elements`, `mode`, or `platform` parameter is invalid, or any literal fails to be compiled. |
| `HS_ARCH_ERROR` | The database is built with `FAT_RUNTIME` and the runtime platform does not support SSSE3. |

### 2.7 `fat_hs_free_database`

Releases a database returned by the universal bytecode compilation.

```c
hs_error_t HS_CDECL fat_hs_free_database(fat_hs_database_t *db);
```

| Parameter | Description |
| --- | --- |
| `db` | Universal database to be released, which can be `NULL`. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The database has been released, or `db` is `NULL`. |
| `HS_INVALID` | `db` is non-empty but is not a valid universal database object. |

### 2.8 `fat_hs_serialize_database`

Serializes an in-memory universal database into a contiguous byte stream.

```c
hs_error_t HS_CDECL fat_hs_serialize_database(
    const fat_hs_database_t *db, char **bytes, size_t *length);
```

| Parameter | Description |
| --- | --- |
| `db` | Universal database to be serialized, which must be valid and aligned as required. |
| `bytes` | Non-empty output parameter. A successful API call returns the serialized buffer allocated by the misc allocator. For the default allocator, the buffer can be released using `free()`; for a custom misc allocator, the buffer should be released using the corresponding function. |
| `length` | Non-empty output parameter. A successful API call returns the length of the buffer that `bytes` points to. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The serialization is successful. |
| `HS_INVALID` | `db`, `bytes`, or `length` is empty, or the database magic number is invalid. |
| `HS_BAD_ALIGN` | `db` is not aligned as required. |
| `HS_DB_VERSION_ERROR` | The database version does not match. |
| `HS_NOMEM` | The misc allocator fails to allocate the output buffer. |
| `HS_BAD_ALLOC` | The buffer returned by the misc allocator does not meet the alignment requirement. |

### 2.9 `fat_hs_deserialize_database`

Allocates memory and reconstructs a universal database from a serialized byte stream.

```c
hs_error_t HS_CDECL fat_hs_deserialize_database(
    const char *bytes, const size_t length, fat_hs_database_t **db);
```

| Parameter | Description |
| --- | --- |
| `bytes` | Serialized byte stream of a universal database, which must not be empty. |
| `length` | Length of the byte stream that `bytes` points to. |
| `db` | Non-empty output parameter. A successful API call returns the new database; a failed API call sets this parameter to `NULL`. The caller uses `fat_hs_free_database()` to release the object returned upon a successful call. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The deserialization is successful. |
| `HS_INVALID` | `bytes` or `db` is empty, or the byte stream format, length, database magic number, or checksum is invalid. |
| `HS_DB_VERSION_ERROR` | The database version in the byte stream does not match. |
| `HS_NOMEM` | The database allocator fails to allocate database memory. |
| `HS_BAD_ALLOC` | The memory returned by the database allocator does not meet the alignment requirement. |

### 2.10 `fat_hs_deserialize_database_at`

Reconstructs a universal database from a serialized byte stream in the caller-provided memory.

```c
hs_error_t HS_CDECL fat_hs_deserialize_database_at(
    const char *bytes, const size_t length, fat_hs_database_t *db);
```

| Parameter | Description |
| --- | --- |
| `bytes` | Serialized byte stream of a universal database, which must not be empty. |
| `length` | Length of the byte stream that `bytes` points to. |
| `db` | Output memory preallocated by the caller, which must not be empty and must be at least eight-byte aligned. Query the required space with `fat_hs_serialized_database_size()` first. Memory ownership always belongs to the caller, and calling `fat_hs_free_database()` to release the memory is not allowed. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The deserialization is successful. |
| `HS_INVALID` | `bytes` or `db` is empty, or the byte stream format, length, database magic number, or checksum is invalid. |
| `HS_BAD_ALIGN` | `db` is not eight-byte aligned. |
| `HS_DB_VERSION_ERROR` | The database version in the byte stream does not match. |

### 2.11 `fat_hs_database_size`

Queries the size of space occupied by an in-memory universal database.

```c
hs_error_t HS_CDECL fat_hs_database_size(
    const fat_hs_database_t *db, size_t *size);
```

| Parameter | Description |
| --- | --- |
| `db` | Universal database to be queried. |
| `size` | Non-empty output parameter. A successful API call returns the number of bytes occupied by the database. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The query is successful. |
| `HS_INVALID` | `db` or `size` is empty, or the database magic number is invalid. |
| `HS_DB_VERSION_ERROR` | The database version does not match. |

### 2.12 `fat_hs_serialized_database_size`

Queries the memory space required for deserializing a serialized universal database.

```c
hs_error_t HS_CDECL fat_hs_serialized_database_size(
    const char *bytes, const size_t length, size_t *size);
```

| Parameter | Description |
| --- | --- |
| `bytes` | Serialized byte stream of a universal database. |
| `length` | Length of the byte stream that `bytes` points to. |
| `size` | Non-empty output parameter. A successful API call returns the memory size required by `fat_hs_deserialize_database_at()`. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The query is successful. |
| `HS_INVALID` | The byte stream is empty, the length or format is invalid, or `size` is empty. |
| `HS_DB_VERSION_ERROR` | The database version in the byte stream does not match. |

### 2.13 `fat_hs_database_info`

Queries the version and mode of a universal database.

```c
hs_error_t HS_CDECL fat_hs_database_info(
    const fat_hs_database_t *db, char **info);
```

| Parameter | Description |
| --- | --- |
| `db` | Universal database to be queried, which must be valid and aligned as required. |
| `info` | Non-empty output parameter. A successful API call returns an information string ending with `NULL` allocated by the misc allocator; a failed API call sets this parameter to `NULL`. For a default allocator, the object can be released using `free()`; for a custom misc allocator, it should be released using the corresponding function. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The query is successful. |
| `HS_INVALID` | `db` or `info` is empty, `db` is not aligned as required, or the database magic number is invalid. |
| `HS_NOMEM` | The misc allocator fails to allocate the memory for the information string. |
| `HS_BAD_ALLOC` | The memory returned by the misc allocator does not meet the alignment requirement. |

### 2.14 Minimal Usage Example

The following example compiles a universal database and serializes it. For actual cross-platform deployment, you can directly use `hsdump -U` to generate the file, and load it with a tool that supports universal bytecode.

```c
#include <stdio.h>
#include <stdlib.h>
#include "hs.h"

int main(void) {
    fat_hs_database_t *db = NULL;
    hs_compile_error_t *compile_error = NULL;
    char *bytes = NULL;
    size_t length = 0;

    hs_error_t err = fat_hs_compile("teakettle", 0, HS_MODE_BLOCK, NULL,
                                    &db, &compile_error);
    if (err != HS_SUCCESS) {
        fprintf(stderr, "compile failed: %s\n",
                compile_error ? compile_error->message : "unknown error");
        hs_free_compile_error(compile_error);
        return 1;
    }

    err = fat_hs_serialize_database(db, &bytes, &length);
    if (err != HS_SUCCESS || length == 0) {
        fprintf(stderr, "serialize failed: %d\n", err);
        free(bytes);
        fat_hs_free_database(db);
        return 1;
    }

    puts("fat database serialized successfully");
    free(bytes); /* Applies only to the default misc allocator. */
    fat_hs_free_database(db);
    return 0;
}
```

Save the code as `/opt/Ultrascan/fat_example.c`. The following commands directly use the newly generated static libraries, without the need for installing Ultrascan. You should first complete the default static library compilation according to [Installation Guide](./installation_guide.md), and confirm that the library files have been generated.

```bash
test -f /opt/Ultrascan/build/lib/libhs.a && \
    ls -lh /opt/Ultrascan/build/lib/libhs.a
```

The output of a successful command execution should contain `/opt/Ultrascan/build/lib/libhs.a`. The file size and time vary with the build environment. For example:

```text
-rw-r--r-- 1 root root 25M Sep 30 12:00 /opt/Ultrascan/build/lib/libhs.a
```

```bash
cc -std=c99 \
   -I/opt/Ultrascan/src \
   /opt/Ultrascan/fat_example.c \
   /opt/Ultrascan/build/lib/libhs.a \
   -lstdc++ -lm -pthread \
   -o /opt/Ultrascan/build/fat_example

/opt/Ultrascan/build/fat_example
```

The correct output is as follows:

```text
fat database serialized successfully
```

## 3 Grey Configuration APIs

Grey is a collection of internal compiler optimization parameters. The public APIs allow applications to explicitly set these parameters within the process, replacing the old implicit `config.txt` reading approach. Ultrascan no longer searches for or reads `config.txt`; to override the default values, you must proactively call the APIs in this section before compiling other APIs.

### 3.1 `hs_set_grey_overrides`

Sets the process-level Grey override string. Once set successfully, this configuration is read by subsequent compilation calls that construct a Grey configuration until it is set again or reset.

```c
hs_error_t HS_CDECL hs_set_grey_overrides(const char *overrides);
```

| Parameter | Description |
| --- | --- |
| `overrides` | The format is `key:value;key:value;...`. Passing `NULL` or an empty string is equivalent to a reset. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The string is valid and the new configuration has been saved; `NULL` or an empty string also returns success. |
| `HS_INVALID` | A format error occurs, such as an unknown `key`, a missing colon, a non-numeric value, or an out-of-bound value. In this case, the existing global configuration remains unchanged, and the `help` input for debugging is also rejected. |

Usage constraints:

- A `key` must be a boolean or numeric field in the `applyGreyOverrides()` trustlist. Common examples include `allowLily`, `allowNeoFdr`, `limitPatternCount`, and `limitPatternLength`; not all fields in the `Grey` structure can be publicly overridden.

- A `value` is parsed as an unsigned integer. For boolean switches, it is recommended that only `0` or `1` be used and negative numbers should not be passed.

- Parsing does not automatically strip whitespace characters from the `key`; it is recommended that the compact format be always used.

- Configuration storage is protected by a mutex, but the configuration is shared at the process level. To ensure that the same-batch compilation results are reproducible, this API should be set during the startup phase and concurrent modification with compilation calls should be avoided.

- Each successful call replaces the previous override string entirely and does not automatically merge the setting with the previous one.

### 3.2 `hs_reset_grey_overrides`

Clears the process-level Grey overrides so that subsequent compilations revert to the hardcoded default values in the code.

```c
hs_error_t HS_CDECL hs_reset_grey_overrides(void);
```

| Parameter | Description |
| --- | --- |
| None | This API has no parameters. |

Return value:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The overrides have been cleared. |

### 3.3 Minimal Usage Example

The following example demonstrates the complete call sequence for Grey overrides: `allowLily`, `allowNeoFdr`, and `limitPatternCount` are set first, then the rules are compiled, and finally the database is released and the process-level overrides are reset to avoid affecting subsequent compilations in the same process. This example is used to verify that the configuration string can be accepted and participate in compilation, but is not used to observe the specific engine selection result.

```c
#include <stdio.h>
#include "hs.h"

int main(void) {
    hs_database_t *db = NULL;
    hs_compile_error_t *compile_error = NULL;

    hs_error_t err = hs_set_grey_overrides(
        "allowLily:1;allowNeoFdr:1;limitPatternCount:100000;");
    if (err != HS_SUCCESS) {
        fprintf(stderr, "invalid Grey overrides\n");
        return 1;
    }

    err = hs_compile("abc", 0, HS_MODE_BLOCK, NULL, &db, &compile_error);
    if (err != HS_SUCCESS) {
        fprintf(stderr, "compile failed: %s\n",
                compile_error ? compile_error->message : "unknown error");
    } else {
        puts("Grey compile succeeded");
    }

    hs_free_compile_error(compile_error);
    hs_free_database(db);
    hs_reset_grey_overrides();
    return err == HS_SUCCESS ? 0 : 1;
}
```

Save the code as `/opt/Ultrascan/grey_example.c`. The following commands directly use the newly generated static libraries, without the need for installing Ultrascan. Confirm first that the default static libraries have been generated.

```bash
test -f /opt/Ultrascan/build/lib/libhs.a && \
    ls -lh /opt/Ultrascan/build/lib/libhs.a
```

The output of a successful command execution should contain `/opt/Ultrascan/build/lib/libhs.a`. The file size and time vary with the build environment. For example:

```text
-rw-r--r-- 1 root root 25M Sep 30 12:00 /opt/Ultrascan/build/lib/libhs.a
```

Then, execute the following commands:

```bash
cc -std=c99 \
   -I/opt/Ultrascan/src \
   /opt/Ultrascan/grey_example.c \
   /opt/Ultrascan/build/lib/libhs.a \
   -lstdc++ -lm -pthread \
   -o /opt/Ultrascan/build/grey_example

/opt/Ultrascan/build/grey_example
```

The correct output is as follows:

```text
Grey compile succeeded
```

### 3.4 Example Programs and Tool Parameters

The example programs and main compilation tools in the repository are integrated with the same public API.

| Program | Parameter |
| --- | --- |
| `examples/simplegrep` | `-G OVERRIDES` |
| `examples/pcapscan` | `-G OVERRIDES` |
| `examples/patbench` | `-g OVERRIDES` |
| `hsdump` | `-G OVERRIDES` |
| `hsbench`, `hscheck`, `hscollider`, `hspgo` | `-G OVERRIDES` |

The command-line parameter is only responsible for calling `hs_set_grey_overrides()` within the process; it does not create or modify `config.txt`.

## 4 Feedback-driven Optimization APIs for Regular Expression Matching

The feedback-driven optimization technology for regular expression matching establishes a closed feedback loop: compiling a baseline database, performing runtime collection, generating feedback, and finally using the feedback to compile a new database. The runtime collector counts the relationship between multi-pattern fragment triggering and final reporting, the feedback filters high-waste fragments, and the compiler avoids the corresponding inefficient candidate paths while ensuring that the matching semantics remain unchanged.

### 4.1 Build and Availability Scope

The complete capability is currently supported only on AArch64 and is disabled by default. Enable it explicitly at build time:

```bash
mkdir -p /opt/Ultrascan/build-feedback
cd /opt/Ultrascan/build-feedback
cmake .. -DHS_ENABLE_FP_FEEDBACK=ON
make -j
```

- Setting `HS_ENABLE_FP_FEEDBACK=ON` on a non-AArch64 platform causes an error during the CMake configuration phase.

- When the capability is disabled, the public symbols still exist to maintain link compatibility. The exact return value of each API in this case is described in the corresponding API sections.

- Normal compilation and scanning should continue to call the original APIs without the `_with_feedback` or `_with_collector` suffix.

### 4.2 Objects, Constants, and Parameters

#### 4.2.1 Opaque Objects

```c
typedef struct hs_fp_collector hs_fp_collector_t;
typedef struct hs_fp_feedback hs_fp_feedback_t;
```

- `hs_fp_collector_t` binds to a specific baseline database and accumulates runtime counters. It uses the database but does not extend the database lifetime.

- `hs_fp_feedback_t` holds an independent copy of bad fragment identities, which can continue to be used for feedback-based compilation after the collector and the baseline database are released.

#### 4.2.2 Fragment Classification Constants

| Constant | Value | Description |
| --- | ---: | --- |
| `HS_FP_TABLE_UNKNOWN` | 0 | Unknown table. |
| `HS_FP_TABLE_FLOATING` | 1 | Floating table. |
| `HS_FP_TABLE_EOD_ANCHORED` | 2 | EOD anchored table. |
| `HS_FP_TABLE_SMALL_BLOCK` | 3 | Small-block table. |
| `HS_FP_TABLE_DELAY_REBUILD` | 4 | Reserved value; not collected and not used for feedback-based compilation. |
| `HS_FP_TABLE_ANCHORED` | 5 | Reserved value; not collected and not used for feedback-based compilation. |

| Constant | Value | Description |
| --- | ---: | --- |
| `HS_FP_ENGINE_UNKNOWN` | 0 | Unknown engine type. |
| `HS_FP_ENGINE_NOODLE` | 1 | Noodle type identifier. |
| `HS_FP_ENGINE_FDR` | 2 | FDR type identifier. |
| `HS_FP_ENGINE_NEO_FDR` | 3 | NeoFDR type identifier. |
| `HS_FP_ENGINE_HAO` | 4 | Engine type identifier reserved to maintain public ABI integrity. |
| `HS_FP_ENGINE_TEDDY` | 5 | Teddy type identifier. |

| Constant | Value | Description |
| --- | ---: | --- |
| `HS_FP_FRAGMENT_FLAG_NOCASE` | `0x01` | The fragment is case-insensitive. |
| `HS_FP_FRAGMENT_FLAG_NORUNS` | `0x02` | The fragment has the `NORUNS` attribute. |
| `HS_FP_FRAGMENT_FLAG_MASKED` | `0x04` | The fragment has mask/cmp constraints. |

#### 4.2.3 Filter Parameters

```c
typedef struct hs_fp_feedback_params {
    unsigned int flags;
    unsigned long long min_trigger_count;
    unsigned long long min_false_positive_count;
    unsigned long long min_false_positive_rate;
    unsigned long long min_waste_share;
    unsigned int max_bad_fragments;
} hs_fp_feedback_params_t;
```

`flags` specifies which fields override the default values:

| Flag | Effective Field | Default Value Macro | Default Value |
| --- | --- | --- | ---: |
| `HS_FP_FEEDBACK_PARAM_MIN_TRIGGER_COUNT` | `min_trigger_count` | `HS_FP_FEEDBACK_DEFAULT_MIN_TRIGGER_COUNT` | `1000` |
| `HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_COUNT` | `min_false_positive_count` | `HS_FP_FEEDBACK_DEFAULT_MIN_FALSE_POSITIVE_COUNT` | `1000` |
| `HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_RATE` | `min_false_positive_rate` | `HS_FP_FEEDBACK_DEFAULT_MIN_FALSE_POSITIVE_RATE` | `99%` |
| `HS_FP_FEEDBACK_PARAM_MIN_WASTE_SHARE` | `min_waste_share` | `HS_FP_FEEDBACK_DEFAULT_MIN_WASTE_SHARE` | `5%` |
| `HS_FP_FEEDBACK_PARAM_MAX_BAD_FRAGMENTS` | `max_bad_fragments` | `HS_FP_FEEDBACK_DEFAULT_MAX_BAD_FRAGMENTS` | `0`, indicating no limit |

Rate fields are scaled by `HS_FP_FEEDBACK_RATE_SCALE` (value: `1000000000000ULL`). For example, `99%` is represented as `990000000000ULL`. If `params` is `NULL` or the structure is zero-initialized, all parameters adopt their default values. When the first four thresholds are enabled, a field value of `0` indicates no limit for the threshold. When `MAX_BAD_FRAGMENTS` is explicitly enabled, its value must be greater than 0.

#### 4.2.4 Dump Structure and Callback

The following definitions apply only to `hs_fp_collector_to_feedback_with_dump()`. During feedback generation, the overall statistics can be output via a summary callback, and per-fragment statistics along with their selection status are output via a fragment callback. This design facilitates logging or CSV export for further analysis. These callbacks are observers only; they do not participate in the feedback filtering process, nor do they transfer ownership of any objects.

```c
typedef struct hs_fp_fragment_info {
    unsigned long long key;
    unsigned int table;
    unsigned int engine;
    unsigned int flags;
    const unsigned char *bytes;
    size_t length;
    const unsigned char *mask;
    const unsigned char *cmp;
    size_t mask_length;
    unsigned long long trigger_count;
    unsigned long long true_trigger_count;
    unsigned long long false_positive_count;
} hs_fp_fragment_info_t;

typedef struct hs_fp_feedback_dump_summary {
    unsigned int fragment_count;
    unsigned int bad_fragment_count;
    unsigned long long trigger_count;
    unsigned long long true_trigger_count;
    unsigned long long false_positive_count;
} hs_fp_feedback_dump_summary_t;

typedef void(HS_CDECL *hs_fp_feedback_dump_summary_handler)(
    const hs_fp_feedback_dump_summary_t *summary, void *context);

typedef void(HS_CDECL *hs_fp_feedback_dump_fragment_handler)(
    const hs_fp_fragment_info_t *fragment, unsigned int selected,
    void *context);

typedef struct hs_fp_feedback_dump_callbacks {
    hs_fp_feedback_dump_summary_handler on_summary;
    hs_fp_feedback_dump_fragment_handler on_fragment;
} hs_fp_feedback_dump_callbacks_t;
```

`hs_fp_fragment_info_t` fields:

| Field | Description |
| --- | --- |
| `key` | Stable fragment key used for dump, logging, and cross-compilation observation. |
| `table` | `HS_FP_TABLE_*` value, as part of the exact identity for feedback-based compilation. |
| `engine` | `HS_FP_ENGINE_*` value, used only for classification and observation. |
| `flags` | `HS_FP_FRAGMENT_FLAG_*` bitmap. |
| `bytes`/`length` | Binary content of the fragment and its length. |
| `mask`/`cmp`/`mask_length` | (Optional) mask/cmp constraints. When no constraint is present, `mask_length` is `0` and the pointers may be `NULL`. |
| `trigger_count` | Number of times the front-end fragments are triggered. |
| `true_trigger_count` | Number of triggered events that directly produced at least one final report. A single trigger that produces multiple reports is still counted as one. |
| `false_positive_count` | Number of triggered events that did not directly produce any final report. |

`hs_fp_feedback_dump_summary_t` aggregates statistics for known fragments—those that can be mapped to metadata and have actually been triggered during the current sampling window: `fragment_count` indicates the fragment count, `bad_fragment_count` is the number of fragments selected into the feedback, and the remaining three fields are the aggregated totals of trigger counts, true-trigger counts, and false-positive counts across all such fragments, respectively.

`selected != 0` indicates that the fragment has been included in this feedback. The callback parameters and the `bytes`, `mask`, and `cmp` pointers are valid only during the callback; copy them inside the callback if you need to persist the data.

### 4.3 `hs_fp_collector_create`

Creates a runtime feedback collector for the specified baseline database. The collector uses the database, which must remain valid before the collector is released. A collector does not support concurrent writes from multiple threads.

```c
hs_error_t HS_CDECL hs_fp_collector_create(
    const hs_database_t *db, hs_fp_collector_t **collector);
```

| Parameter | Description |
| --- | --- |
| `db` | Compiled baseline database. |
| `collector` | Non-empty output parameter. If the feature is enabled, a successful API call returns a new collector. If the feature is not enabled or the API fails to be called, this parameter is set to `NULL`. The caller uses `hs_fp_collector_free()` to release the object returned upon successful calls. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The feature is enabled and the creation is successful. |
| `HS_INVALID` | If the feature is enabled, `collector` is `NULL`, the database is empty, the database magic number is incorrect, or the bytecode is not aligned as required. If the feature is not enabled, this value is returned only when `collector` is `NULL`. |
| `HS_DB_VERSION_ERROR` | The feature is enabled but the database version does not match. |
| `HS_NOMEM` | The feature is enabled but memory allocation for the collector or counter fails. |
| `HS_ARCH_ERROR` | If the feature is not enabled and `collector` is valid, this value is returned after `*collector` is set to `NULL`. |

### 4.4 `hs_fp_collector_reset`

Clears the accumulated counts of the collector while retaining its binding to the baseline database.

```c
hs_error_t HS_CDECL hs_fp_collector_reset(hs_fp_collector_t *collector);
```

| Parameter | Description |
| --- | --- |
| `collector` | Collector to be reset, which must not be empty when the feature is enabled. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The feature is enabled and the reset is successful. |
| `HS_INVALID` | The feature is enabled but `collector` is `NULL`. |
| `HS_ARCH_ERROR` | This value is directly returned if the feature is not enabled. `collector` is not validated. |

### 4.5 `hs_fp_collector_merge`

Merges multiple collectors created from the same database into a new collector. The input collectors are not released or reset.

```c
hs_error_t HS_CDECL hs_fp_collector_merge(
    hs_fp_collector_t *const *collectors, unsigned int count,
    hs_fp_collector_t **collector);
```

| Parameter | Description |
| --- | --- |
| `collectors` | Array of collectors to be merged, which must not be empty. All elements must be non-empty and must be created from exactly the same database object. |
| `count` | Number of objects in `collectors`, which must be greater than 0. |
| `collector` | Non-empty output parameter. If the feature is enabled, a successful API call returns the merged collector. If the feature is not enabled or the API fails to be called, this parameter is set to `NULL`. The caller uses `hs_fp_collector_free()` to release the object returned upon successful calls. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The feature is enabled and the merge is successful. |
| `HS_INVALID` | If the feature is enabled, `collectors` is `NULL`, `count` is `0`, `collector` is `NULL`, any input collector is empty, or the inputs come from different database objects. If the feature is not enabled, this value is returned only when `collector` is `NULL`. |
| `HS_DB_VERSION_ERROR` | The feature is enabled and the version of the database associated with the new collector does not match. |
| `HS_NOMEM` | The feature is enabled but memory allocation for the new collector or counter fails. |
| `HS_ARCH_ERROR` | If the feature is not enabled and `collector` is valid, this value is returned after `*collector` is set to `NULL`. |

### 4.6 `hs_fp_collector_free`

Releases a collector.

```c
hs_error_t HS_CDECL hs_fp_collector_free(hs_fp_collector_t *collector);
```

| Parameter | Description |
| --- | --- |
| `collector` | Collector to be released, which can be `NULL`. |

Return value:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | This value is returned regardless of whether the feature is enabled or whether `collector` is `NULL`. |

### 4.7 `hs_fp_collector_to_feedback`

Generates a feedback object from a collector for feedback-based compilation. Each call commits pending counts but does not reset the collector; concurrent writes to the same collector are not allowed during conversion. When no fragment is selected, the feedback returned upon a successful call is still valid.

```c
hs_error_t HS_CDECL hs_fp_collector_to_feedback(
    hs_fp_collector_t *collector, const hs_fp_feedback_params_t *params,
    hs_fp_feedback_t **feedback);
```

| Parameter | Description |
| --- | --- |
| `collector` | Valid feedback collector. |
| `params` | (Optional) Filter parameter. If `NULL` is passed or the structure is zero-initialized, the default thresholds are used. |
| `feedback` | Non-empty output parameter. If the feature is enabled, a successful API call returns the feedback. If the feature is not enabled or the API fails to be called, this parameter is set to `NULL`. The caller uses `hs_fp_feedback_free()` to release the object returned upon successful calls. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The feature is enabled and the feedback is generated successfully. |
| `HS_INVALID` | If the feature is enabled, `collector` is `NULL`, `feedback` is `NULL`, `flags` of the filter parameter contains unknown bits, the enabled `max_bad_fragments` is `0`, or the enabled ratio threshold is greater than `HS_FP_FEEDBACK_RATE_SCALE`. If the feature is not enabled, this value is returned only when `feedback` is `NULL`. |
| `HS_NOMEM` | The feature is enabled but memory allocation for the statistics report or feedback fails. |
| `HS_ARCH_ERROR` | If the feature is not enabled and `feedback` is valid, this value is returned after `*feedback` is set to `NULL`. |

### 4.8 `hs_fp_collector_to_feedback_with_dump`

Generates feedback from a collector, and outputs summary and fragment dump information. The callbacks only provide observation results, do not participate in filtering, and do not transfer object ownership.

```c
hs_error_t HS_CDECL hs_fp_collector_to_feedback_with_dump(
    hs_fp_collector_t *collector, const hs_fp_feedback_params_t *params,
    const hs_fp_feedback_dump_callbacks_t *callbacks, void *context,
    hs_fp_feedback_t **feedback);
```

| Parameter | Description |
| --- | --- |
| `collector` | Valid feedback collector. |
| `params` | (Optional) Filter parameter. If `NULL` is passed or the structure is zero-initialized, the default thresholds are used. |
| `callbacks` | (Optional) Dump callback collection. `NULL` indicates that no dump is output. `on_summary` and `on_fragment` can each be `NULL`. |
| `context` | User context passed to the dump callbacks. This parameter is not used when `callbacks` is `NULL`. |
| `feedback` | Non-empty output parameter. If the feature is enabled, a successful API call returns the feedback. If the feature is not enabled or the API fails to be called, this parameter is set to `NULL`. The caller uses `hs_fp_feedback_free()` to release the object returned upon successful calls. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The feature is enabled and the feedback is generated successfully. |
| `HS_INVALID` | If the feature is enabled, `collector` is `NULL`, `feedback` is `NULL`, `flags` of the filter parameter contains unknown bits, the enabled `max_bad_fragments` is `0`, or the enabled ratio threshold is greater than `HS_FP_FEEDBACK_RATE_SCALE`. If the feature is not enabled, this value is returned only when `feedback` is `NULL`. |
| `HS_NOMEM` | The feature is enabled but memory allocation for the statistics report or feedback fails. |
| `HS_ARCH_ERROR` | If the feature is not enabled and `feedback` is valid, this value is returned after `*feedback` is set to `NULL`. |

### 4.9 `hs_fp_feedback_free`

Releases the feedback object. This API can be passed with `NULL`.

```c
hs_error_t HS_CDECL hs_fp_feedback_free(hs_fp_feedback_t *feedback);
```

| Parameter | Description |
| --- | --- |
| `feedback` | Feedback to be released, which can be `NULL`. |

Return value:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | This value is returned regardless of whether the feature is enabled or whether `feedback` is `NULL`. |

### 4.10 `hs_compile_multi_with_feedback`

Compiles multiple regular expressions with feedback. There are currently no feedback-based compilation variants for single, literal, or fat databases.

```c
hs_error_t HS_CDECL hs_compile_multi_with_feedback(
    const char *const *expressions, const unsigned int *flags,
    const unsigned int *ids, unsigned int elements, unsigned int mode,
    const hs_platform_info_t *platform, const hs_fp_feedback_t *feedback,
    hs_database_t **db, hs_compile_error_t **error);
```

| Parameter | Description |
| --- | --- |
| `expressions` | Array of regular expressions ending with `NULL`. |
| `flags` | Array of `HS_FLAG_*` flags corresponding to each expression, which can be `NULL`, indicating that all flags are 0. |
| `ids` | Array of IDs corresponding to each expression, which can be `NULL`, indicating that the default IDs are used. |
| `elements` | Number of expressions, which must be greater than 0. |
| `mode` | Database mode. The value can be `HS_MODE_BLOCK`, `HS_MODE_STREAM`, or `HS_MODE_VECTORED`. |
| `platform` | (Optional) Target platform information. The `NULL` value indicates that the database is generated based on the current host platform. |
| `feedback` | (Optional) Feedback object. If the feature is enabled, the `NULL` value is equivalent to performing common compilation of multiple regular expressions; the database only reads it during the call and does not take ownership. |
| `db` | Output parameter. A successful API call returns the new database. If the feature is not enabled or the API fails to be called, this parameter is set to `NULL`. The caller uses `hs_free_database()` to release the object returned upon a successful call. |
| `error` | Output parameter. A compilation failure returns error details. The caller uses `hs_free_compile_error()` to release the non-empty object. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The feature is enabled and the compilation is successful. |
| `HS_COMPILER_ERROR` | If the feature is enabled, the compilation parameters, `mode`, or `platform` is invalid, or expression parsing or compilation fails; details are returned via `error`. |
| `HS_NOMEM` | The feature is enabled, and creating or copying the feedback-based compilation context fails. |
| `HS_ARCH_ERROR` | When the feature is not enabled, this value is returned even if `feedback` is `NULL`; `db` is set to `NULL`, and error details are returned via `error` when it is non-empty. |

### 4.11 `hs_compile_ext_multi_with_feedback`

Compiles multiple regular expressions using feedback and extended parameters.

```c
hs_error_t HS_CDECL hs_compile_ext_multi_with_feedback(
    const char *const *expressions, const unsigned int *flags,
    const unsigned int *ids, const hs_expr_ext_t *const *ext,
    unsigned int elements, unsigned int mode,
    const hs_platform_info_t *platform, const hs_fp_feedback_t *feedback,
    hs_database_t **db, hs_compile_error_t **error);
```

| Parameter | Description |
| --- | --- |
| `expressions` | Array of regular expressions ending with `NULL`. |
| `flags` | Array of `HS_FLAG_*` flags corresponding to each expression, which can be `NULL`, indicating that all flags are 0. |
| `ids` | Array of IDs corresponding to each expression, which can be `NULL`, indicating that the default IDs are used. |
| `ext` | Array of `hs_expr_ext_t` pointers corresponding to each expression, which can be `NULL`. Individual array elements can also be `NULL`. |
| `elements` | Number of expressions, which must be greater than 0. |
| `mode` | Database mode. The value can be `HS_MODE_BLOCK`, `HS_MODE_STREAM`, or `HS_MODE_VECTORED`. |
| `platform` | (Optional) Target platform information. The `NULL` value indicates that the database is generated based on the current host platform. |
| `feedback` | (Optional) Feedback object. If the feature is enabled, the `NULL` value is equivalent to performing common compilation of multiple regular expressions using extended parameters; the database only reads it during the call and does not take ownership. |
| `db` | Output parameter. A successful API call returns the new database. If the feature is not enabled or the API fails to be called, this parameter is set to `NULL`. The caller uses `hs_free_database()` to release the object returned upon a successful call. |
| `error` | Output parameter. A compilation failure returns error details. The caller uses `hs_free_compile_error()` to release the non-empty object. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The feature is enabled and the compilation is successful. |
| `HS_COMPILER_ERROR` | If the feature is enabled, the compilation parameters, `ext`, `mode`, or `platform` is invalid, or expression parsing or compilation fails; details are returned via `error`. |
| `HS_NOMEM` | The feature is enabled, and creating or copying the feedback-based compilation context fails. |
| `HS_ARCH_ERROR` | When the feature is not enabled, this value is returned even if `feedback` is `NULL`; `db` is set to `NULL`, and error details are returned via `error` when it is non-empty. |

### 4.12 `hs_scan_with_collector`

Performs block-mode scanning while recording runtime trigger information to the collector. The matching semantics and callback behavior are consistent with `hs_scan()`.

```c
hs_error_t HS_CDECL hs_scan_with_collector(
    const hs_database_t *db, const char *data, unsigned int length,
    unsigned int flags, hs_scratch_t *scratch,
    match_event_handler onEvent, void *context,
    hs_fp_collector_t *collector);
```

| Parameter | Description |
| --- | --- |
| `db` | Block-mode database bound to `collector`. |
| `data` | Data to be scanned, which must not be empty. |
| `length` | Byte length of `data`. |
| `flags` | Scanning flags, which are reserved for future use. |
| `scratch` | Per-thread scratch space compatible with `db`. |
| `onEvent` | (Optional) Matching callback. `NULL` indicates that no matches are output. |
| `context` | User context passed to `onEvent`. |
| `collector` | Valid collector bound to `db`. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The feature is enabled and the scanning is successful. The pending count for this scanning has been committed. |
| `HS_SCAN_TERMINATED` | The feature is enabled but the matching callback requests a stop. The pending count for this scanning has been committed. |
| `HS_INVALID` | The feature is enabled, but `collector`, `db`, `data`, or `scratch` is invalid, or the database does not match the collector. |
| `HS_DB_VERSION_ERROR` | The feature is enabled, but the database version does not match. |
| `HS_DB_MODE_ERROR` | The feature is enabled but the database is not in block mode. |
| `HS_SCRATCH_IN_USE` | The feature is enabled but `scratch` is being used by another call. |
| `HS_UNKNOWN_ERROR` | The feature is enabled but an internal matching error occurs during the scanning. |
| `HS_ARCH_ERROR` | If the feature is not enabled, this value is returned directly without performing the scanning. |

### 4.13 `hs_scan_vector_with_collector`

Performs vectored-mode scanning while recording runtime trigger information to the collector. The matching semantics and callback behavior are consistent with `hs_scan_vector()`.

```c
hs_error_t HS_CDECL hs_scan_vector_with_collector(
    const hs_database_t *db, const char *const *data,
    const unsigned int *length, unsigned int count, unsigned int flags,
    hs_scratch_t *scratch, match_event_handler onEvent, void *context,
    hs_fp_collector_t *collector);
```

| Parameter | Description |
| --- | --- |
| `db` | Vectored-mode database bound to `collector`. |
| `data` | Array of pointers to data blocks to be scanned, which must not be empty. |
| `length` | Array of lengths corresponding to each data block, which must not be empty. |
| `count` | Number of data blocks. |
| `flags` | Scanning flags, which are reserved for future use. |
| `scratch` | Per-thread scratch space compatible with `db`. |
| `onEvent` | (Optional) Matching callback. `NULL` indicates that no matches are output. |
| `context` | User context passed to `onEvent`. |
| `collector` | Valid collector bound to `db`. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The feature is enabled and the scanning is successful. The pending count for this scanning has been committed. |
| `HS_SCAN_TERMINATED` | The feature is enabled but the matching callback requests a stop. The pending count for this scanning has been committed. |
| `HS_INVALID` | The feature is enabled, but `collector`, `db`, `data`, `length`, or `scratch` is invalid, or the database does not match the collector. |
| `HS_DB_VERSION_ERROR` | The feature is enabled, but the database version does not match. |
| `HS_DB_MODE_ERROR` | The feature is enabled but the database is not in vectored mode. |
| `HS_SCRATCH_IN_USE` | The feature is enabled but `scratch` is being used by another call. |
| `HS_UNKNOWN_ERROR` | The feature is enabled but an internal matching error occurs during the scanning. |
| `HS_ARCH_ERROR` | If the feature is not enabled, this value is returned directly without performing the scanning. |

### 4.14 `hs_scan_stream_with_collector`

Performs streaming-mode scanning while recording runtime trigger information to the collector. The matching semantics and callback behavior are consistent with `hs_scan_stream()`; EOD workload generated during the stream close and reset phases is excluded from the collector's statistics.

```c
hs_error_t HS_CDECL hs_scan_stream_with_collector(
    hs_stream_t *id, const char *data, unsigned int length,
    unsigned int flags, hs_scratch_t *scratch,
    match_event_handler onEvent, void *context,
    hs_fp_collector_t *collector);
```

| Parameter | Description |
| --- | --- |
| `id` | Opened stream object bound to the same database/Rose instance as `collector`. |
| `data` | The data to be scanned, which must not be empty. |
| `length` | The byte length of `data`. |
| `flags` | Scanning flags, which are reserved for future use. |
| `scratch` | Per-thread scratch space compatible with the stream object. |
| `onEvent` | (Optional) Matching callback. `NULL` indicates that no matches are output. |
| `context` | User context passed to `onEvent`. |
| `collector` | Valid collector bound to the same Rose instance as the stream object. |

Return values:

| Return Value | Description |
| --- | --- |
| `HS_SUCCESS` | The feature is enabled and the scanning is successful. The pending count for this scanning has been committed. |
| `HS_SCAN_TERMINATED` | The feature is enabled but the matching callback requests a stop. The pending count for this scanning has been committed. |
| `HS_INVALID` | If the feature is enabled, `id`, `collector`, `data`, or `scratch` is invalid, or the stream object does not match the collector. If the feature is not enabled, `id == NULL` also returns this value. |
| `HS_SCRATCH_IN_USE` | The feature is enabled but `scratch` is being used by another call. |
| `HS_UNKNOWN_ERROR` | The feature is enabled but an internal matching error occurs during the scanning. The partial counts already recorded are not rolled back. |
| `HS_ARCH_ERROR` | When the feature is not enabled but `id` is valid, this value is returned directly without performing the scanning. |

### 4.15 Minimal Usage Example

The following C use case uses block mode to complete baseline compilation, collection, feedback generation, and feedback-based compilation. The example requires linking against a library built with `HS_ENABLE_FP_FEEDBACK=ON` on AArch64.

```c
#include <stdio.h>
#include <string.h>
#include "hs.h"

static int on_match(unsigned int id, unsigned long long from,
                    unsigned long long to, unsigned int flags, void *ctx) {
    (void)id; (void)from; (void)to; (void)flags; (void)ctx;
    return 0;
}

int main(void) {
    const char *expressions[] = {"foo"};
    unsigned int flags[] = {0};
    unsigned int ids[] = {1};
    hs_expr_ext_t ext0 = {0};
    const hs_expr_ext_t *ext[] = {&ext0};
    hs_fp_feedback_params_t params = {0};
    hs_database_t *baseline = NULL, *optimized = NULL;
    hs_compile_error_t *compile_error = NULL;
    hs_scratch_t *scratch = NULL;
    hs_fp_collector_t *collector = NULL;
    hs_fp_feedback_t *feedback = NULL;
    hs_error_t err;
    int rc = 1;

    /*Make the front-end literal triggerable. The final expression is not reported because min_offset is not satisfied.*/
    ext0.flags = HS_EXT_FLAG_MIN_OFFSET;
    ext0.min_offset = 10;

    err = hs_compile_ext_multi(expressions, flags, ids, ext, 1,
                               HS_MODE_BLOCK, NULL, &baseline,
                               &compile_error);
    if (err != HS_SUCCESS) goto cleanup;

    if (hs_alloc_scratch(baseline, &scratch) != HS_SUCCESS) goto cleanup;
    if (hs_fp_collector_create(baseline, &collector) != HS_SUCCESS)
        goto cleanup;

    err = hs_scan_with_collector(baseline, "foo", 3, 0, scratch,
                                 on_match, NULL, collector);
    if (err != HS_SUCCESS) goto cleanup;

    params.flags = HS_FP_FEEDBACK_PARAM_MIN_TRIGGER_COUNT |
                   HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_COUNT |
                   HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_RATE |
                   HS_FP_FEEDBACK_PARAM_MIN_WASTE_SHARE;
    params.min_trigger_count = 1;
    params.min_false_positive_count = 1;
    params.min_false_positive_rate = 0;
    params.min_waste_share = 0;

    err = hs_fp_collector_to_feedback(collector, &params, &feedback);
    if (err != HS_SUCCESS) goto cleanup;

    err = hs_compile_ext_multi_with_feedback(
        expressions, flags, ids, ext, 1, HS_MODE_BLOCK, NULL, feedback,
        &optimized, &compile_error);
    if (err != HS_SUCCESS) goto cleanup;

    puts("feedback-compiled database created");
    rc = 0;

cleanup:
    if (rc && compile_error) {
        fprintf(stderr, "compile error: %s\n", compile_error->message);
    }
    hs_free_compile_error(compile_error);
    hs_fp_feedback_free(feedback);
    hs_fp_collector_free(collector);
    hs_free_scratch(scratch);
    hs_free_database(optimized);
    hs_free_database(baseline);
    return rc;
}
```

Save the code as `/opt/Ultrascan/feedback_example.c`. This example requires first enabling `HS_ENABLE_FP_FEEDBACK` on AArch64 using the build command in this section. The following commands directly use the newly generated static libraries, without the need for installing Ultrascan. First confirm that the static libraries supporting the feedback capability have been generated:

```bash
test -f /opt/Ultrascan/build-feedback/lib/libhs.a && \
    ls -lh /opt/Ultrascan/build-feedback/lib/libhs.a
```

The output of a successful command execution should contain `/opt/Ultrascan/build-feedback/lib/libhs.a`. The file size and time vary with the build environment. For example:

```text
-rw-r--r-- 1 root root 25M Sep 30 12:00 /opt/Ultrascan/build-feedback/lib/libhs.a
```

Then, execute the following commands:

```bash
cc -std=c99 \
   -I/opt/Ultrascan/src \
   /opt/Ultrascan/feedback_example.c \
   /opt/Ultrascan/build-feedback/lib/libhs.a \
   -lstdc++ -lm -pthread \
   -o /opt/Ultrascan/build-feedback/feedback_example

/opt/Ultrascan/build-feedback/feedback_example
```

The correct output is as follows:

```text
feedback-compiled database created
```

For the complete tool workflow, see the `hspgo` section in [Quick Start](./quick_start.md).



