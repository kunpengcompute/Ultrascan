# API参考

## 1 文档范围

本文汇总Ultrascan在标准Hyperscan C API基础上新增的公开接口，按主题分为以下三章：

- [通用字节码API](#2-通用字节码api)：`fat_hs_*`系列接口。
- [Grey配置API](#3-grey配置api)：进程级编译参数设置与复位接口。
- [正则匹配反馈优化技术API](#4-正则匹配反馈优化技术api)：运行期采集、反馈生成及反馈编译接口。

所有接口均可通过公共头文件`hs.h`引入。本文不重复介绍`hs_compile()`、`hs_scan()`等上游通用接口；使用这些接口时，应同时参考公共头文件中的注释和上游API文档。

>![](public_sys-resources/icon-note.gif) **说明：** “正则匹配反馈优化技术”是V5.8.0版本新增的反馈闭环能力，与历史已有的“假阳性阻断技术”是两个独立特性。本文不会以新名称替代历史特性的名称或说明。

### 1.1 公共约定

- API返回类型为`hs_error_t`。成功通常返回`HS_SUCCESS`；参数错误返回`HS_INVALID`；编译错误通常返回`HS_COMPILER_ERROR`并通过`hs_compile_error_t`提供详情。
- 编译失败后，调用者应使用`hs_free_compile_error()`释放非空的编译错误对象。
- 文中标记为opaque的对象只能通过对应API创建、传递和释放，调用者不得访问其内部布局。
- 除非接口明确转移所有权，输入对象的所有权仍归调用者。

### 1.2 示例路径

本文命令沿用[安装指南](./installation_guide.md)中的固定目录布局：

- Ultrascan源码：`/opt/Ultrascan`
- 默认静态库构建目录：`/opt/Ultrascan/build`
- Debug构建目录：`/opt/Ultrascan/build-debug`
- 反馈优化构建目录：`/opt/Ultrascan/build-feedback`

## 2 通用字节码API

通用字节码将x86和AArch64字节码封装在同一个`fat_hs_database_t`对象中，适用于一次编译后向两类平台分发的场景。`fat_hs_database_t`是opaque类型：

```c
typedef struct fat_hs_database fat_hs_database_t;
```

### 2.1 API清单

| 分类 | API | 说明 |
| --- | --- | --- |
| 编译 | `fat_hs_compile` | 编译单个正则表达式。 |
| 编译 | `fat_hs_compile_multi` | 编译多个正则表达式。 |
| 编译 | `fat_hs_compile_ext_multi` | 使用扩展参数编译多个正则表达式。 |
| 编译 | `fat_hs_compile_lit` | 编译单个纯字面量。 |
| 编译 | `fat_hs_compile_lit_multi` | 编译多个纯字面量。 |
| 生命周期 | `fat_hs_free_database` | 释放通用字节码数据库。 |
| 序列化 | `fat_hs_serialize_database` | 将通用数据库序列化为字节流。 |
| 反序列化 | `fat_hs_deserialize_database` | 分配并反序列化通用数据库。 |
| 反序列化 | `fat_hs_deserialize_database_at` | 在调用者提供的内存中反序列化。 |
| 查询 | `fat_hs_database_size` | 查询内存中通用数据库的大小。 |
| 查询 | `fat_hs_serialized_database_size` | 查询序列化数据反序列化后所需空间。 |
| 查询 | `fat_hs_database_info` | 查询数据库版本和模式信息。 |

### 2.2 `fat_hs_compile`

编译一个正则表达式，生成同时包含x86和AArch64字节码的通用数据库。

```c
hs_error_t HS_CDECL fat_hs_compile(
    const char *expression, unsigned int flags, unsigned int mode,
    const hs_platform_info_t *platform, fat_hs_database_t **db,
    hs_compile_error_t **error);
```

| 参数 | 说明 |
| --- | --- |
| `expression` | 以NUL结尾的正则表达式，不能为空。表达式中不包含分隔符和内嵌标志。 |
| `flags` | 作用于该表达式的`HS_FLAG_*`标志。多个标志可按位或。 |
| `mode` | 数据库模式。必须选择`HS_MODE_BLOCK`、`HS_MODE_STREAM`或`HS_MODE_VECTORED`之一，并可组合适用的模式标志。 |
| `platform` | 可选目标平台信息；为`NULL`时按当前主机平台生成数据库。 |
| `db` | 非空输出参数。成功时返回通用数据库；失败时置为`NULL`。调用者使用`fat_hs_free_database()`释放成功返回的对象。 |
| `error` | 非空输出参数。编译失败时返回错误详情；调用者使用`hs_free_compile_error()`释放非空对象。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 编译成功。 |
| `HS_COMPILER_ERROR` | `db`、`error`或表达式参数不合法，模式或平台参数不合法，或表达式解析、编译失败。 |
| `HS_ARCH_ERROR` | 库以`FAT_RUNTIME`构建且运行平台不支持SSSE3。 |

### 2.3 `fat_hs_compile_multi`

编译多个正则表达式，生成通用数据库。

```c
hs_error_t HS_CDECL fat_hs_compile_multi(
    const char *const *expressions, const unsigned int *flags,
    const unsigned int *ids, unsigned int elements, unsigned int mode,
    const hs_platform_info_t *platform, fat_hs_database_t **db,
    hs_compile_error_t **error);
```

| 参数 | 说明 |
| --- | --- |
| `expressions` | 以NUL结尾的正则表达式数组，不能为空。数组包含`elements`个元素。 |
| `flags` | 每个表达式对应的`HS_FLAG_*`标志数组；可为`NULL`，表示全部标志为0。 |
| `ids` | 每个表达式对应的ID数组；可为`NULL`，表示使用默认ID。 |
| `elements` | 表达式数量，必须大于0。 |
| `mode` | 数据库模式。必须选择`HS_MODE_BLOCK`、`HS_MODE_STREAM`或`HS_MODE_VECTORED`之一，并可组合适用的模式标志。 |
| `platform` | 可选目标平台信息；为`NULL`时按当前主机平台生成数据库。 |
| `db` | 非空输出参数。成功时返回通用数据库；失败时置为`NULL`。调用者使用`fat_hs_free_database()`释放成功返回的对象。 |
| `error` | 非空输出参数。编译失败时返回错误详情；调用者使用`hs_free_compile_error()`释放非空对象。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 编译成功。 |
| `HS_COMPILER_ERROR` | `db`、`error`、表达式数组或`elements`参数不合法，模式或平台参数不合法，或任一表达式解析、编译失败。 |
| `HS_ARCH_ERROR` | 库以`FAT_RUNTIME`构建且运行平台不支持SSSE3。 |

### 2.4 `fat_hs_compile_ext_multi`

使用扩展参数编译多个正则表达式，生成通用数据库。

```c
hs_error_t HS_CDECL fat_hs_compile_ext_multi(
    const char *const *expressions, const unsigned int *flags,
    const unsigned int *ids, const hs_expr_ext_t *const *ext,
    unsigned int elements, unsigned int mode,
    const hs_platform_info_t *platform, fat_hs_database_t **db,
    hs_compile_error_t **error);
```

| 参数 | 说明 |
| --- | --- |
| `expressions` | 以NUL结尾的正则表达式数组，不能为空。数组包含`elements`个元素。 |
| `flags` | 每个表达式对应的`HS_FLAG_*`标志数组；可为`NULL`，表示全部标志为0。 |
| `ids` | 每个表达式对应的ID数组；可为`NULL`，表示使用默认ID。 |
| `ext` | 每个表达式对应的`hs_expr_ext_t`指针数组；可为`NULL`，单个数组元素也可为`NULL`。调用期间由调用者保持有效。 |
| `elements` | 表达式数量，必须大于0。 |
| `mode` | 数据库模式。必须选择`HS_MODE_BLOCK`、`HS_MODE_STREAM`或`HS_MODE_VECTORED`之一，并可组合适用的模式标志。 |
| `platform` | 可选目标平台信息；为`NULL`时按当前主机平台生成数据库。 |
| `db` | 非空输出参数。成功时返回通用数据库；失败时置为`NULL`。调用者使用`fat_hs_free_database()`释放成功返回的对象。 |
| `error` | 非空输出参数。编译失败时返回错误详情；调用者使用`hs_free_compile_error()`释放非空对象。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 编译成功。 |
| `HS_COMPILER_ERROR` | `db`、`error`、表达式数组、扩展参数、`elements`、模式或平台参数不合法，或任一表达式解析、编译失败。 |
| `HS_ARCH_ERROR` | 库以`FAT_RUNTIME`构建且运行平台不支持SSSE3。 |

### 2.5 `fat_hs_compile_lit`

编译一个纯字面量表达式，生成通用数据库。该接口不按正则语法解释字节内容。

```c
hs_error_t HS_CDECL fat_hs_compile_lit(
    const char *expression, unsigned flags, const size_t len,
    unsigned mode, const hs_platform_info_t *platform,
    fat_hs_database_t **db, hs_compile_error_t **error);
```

| 参数 | 说明 |
| --- | --- |
| `expression` | 纯字面量字节序列，不能为空；数据可包含`\0`。 |
| `flags` | 作用于该字面量的`HS_FLAG_*`标志。 |
| `len` | `expression`的字节长度，接口只读取此前`len`个字节。 |
| `mode` | 数据库模式。必须选择`HS_MODE_BLOCK`、`HS_MODE_STREAM`或`HS_MODE_VECTORED`之一，并可组合适用的模式标志。 |
| `platform` | 可选目标平台信息；为`NULL`时按当前主机平台生成数据库。 |
| `db` | 非空输出参数。成功时返回通用数据库；失败时置为`NULL`。调用者使用`fat_hs_free_database()`释放成功返回的对象。 |
| `error` | 非空输出参数。编译失败时返回错误详情；调用者使用`hs_free_compile_error()`释放非空对象。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 编译成功。 |
| `HS_COMPILER_ERROR` | `db`、`error`、字面量、长度、模式或平台参数不合法，或编译失败。 |
| `HS_ARCH_ERROR` | 库以`FAT_RUNTIME`构建且运行平台不支持SSSE3。 |

### 2.6 `fat_hs_compile_lit_multi`

编译多个纯字面量表达式，生成通用数据库。

```c
hs_error_t HS_CDECL fat_hs_compile_lit_multi(
    const char *const *expressions, const unsigned *flags,
    const unsigned *ids, const size_t *lens, unsigned elements,
    unsigned mode, const hs_platform_info_t *platform,
    fat_hs_database_t **db, hs_compile_error_t **error);
```

| 参数 | 说明 |
| --- | --- |
| `expressions` | 纯字面量字节序列数组，不能为空。数组包含`elements`个元素，各元素可包含`\0`。 |
| `flags` | 每个字面量对应的`HS_FLAG_*`标志数组；可为`NULL`，表示全部标志为0。 |
| `ids` | 每个字面量对应的ID数组；可为`NULL`，表示使用默认ID。 |
| `lens` | 每个字面量的字节长度数组，不能为空。数组与`expressions`一一对应，`lens[i]`指定从`expressions[i]`读取的字节数；字面量可包含`\0`，接口只读取前`lens[i]`个字节。 |
| `elements` | 字面量数量，必须大于0。 |
| `mode` | 数据库模式。必须选择`HS_MODE_BLOCK`、`HS_MODE_STREAM`或`HS_MODE_VECTORED`之一，并可组合适用的模式标志。 |
| `platform` | 可选目标平台信息；为`NULL`时按当前主机平台生成数据库。 |
| `db` | 非空输出参数。成功时返回通用数据库；失败时置为`NULL`。调用者使用`fat_hs_free_database()`释放成功返回的对象。 |
| `error` | 非空输出参数。编译失败时返回错误详情；调用者使用`hs_free_compile_error()`释放非空对象。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 编译成功。 |
| `HS_COMPILER_ERROR` | `db`、`error`、字面量数组、长度数组、`elements`、模式或平台参数不合法，或任一字面量编译失败。 |
| `HS_ARCH_ERROR` | 库以`FAT_RUNTIME`构建且运行平台不支持SSSE3。 |

### 2.7 `fat_hs_free_database`

释放由通用字节码编译或分配式反序列化接口返回的数据库。

```c
hs_error_t HS_CDECL fat_hs_free_database(fat_hs_database_t *db);
```

| 参数 | 说明 |
| --- | --- |
| `db` | 待释放的通用数据库；可为`NULL`。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 数据库已释放，或`db`为`NULL`。 |
| `HS_INVALID` | `db`非空但不是有效的通用数据库对象。 |

### 2.8 `fat_hs_serialize_database`

将内存中的通用数据库序列化为连续字节流。

```c
hs_error_t HS_CDECL fat_hs_serialize_database(
    const fat_hs_database_t *db, char **bytes, size_t *length);
```

| 参数 | 说明 |
| --- | --- |
| `db` | 待序列化的通用数据库，必须有效且按要求对齐。 |
| `bytes` | 非空输出参数。成功时返回由misc allocator分配的序列化缓冲区；默认分配器下可使用`free()`释放，配置自定义misc allocator时应使用相应释放函数。 |
| `length` | 非空输出参数。成功时返回`bytes`指向缓冲区的长度。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 序列化成功。 |
| `HS_INVALID` | `db`、`bytes`或`length`为空，或数据库魔数无效。 |
| `HS_BAD_ALIGN` | `db`未按要求对齐。 |
| `HS_DB_VERSION_ERROR` | 数据库版本与当前库不匹配。 |
| `HS_NOMEM` | misc allocator未能分配输出缓冲区。 |
| `HS_BAD_ALLOC` | misc allocator返回的缓冲区未满足对齐要求。 |

### 2.9 `fat_hs_deserialize_database`

分配内存并从序列化字节流重建通用数据库。

```c
hs_error_t HS_CDECL fat_hs_deserialize_database(
    const char *bytes, const size_t length, fat_hs_database_t **db);
```

| 参数 | 说明 |
| --- | --- |
| `bytes` | 通用数据库的序列化字节流，不能为空。 |
| `length` | `bytes`指向字节流的长度。 |
| `db` | 非空输出参数。成功时返回新数据库；失败时置为`NULL`。调用者使用`fat_hs_free_database()`释放成功返回的对象。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 反序列化成功。 |
| `HS_INVALID` | `bytes`或`db`为空，字节流格式、长度、数据库魔数或校验和无效。 |
| `HS_DB_VERSION_ERROR` | 字节流中的数据库版本与当前库不匹配。 |
| `HS_NOMEM` | database allocator未能分配数据库内存。 |
| `HS_BAD_ALLOC` | database allocator返回的内存未满足对齐要求。 |

### 2.10 `fat_hs_deserialize_database_at`

在调用者提供的内存中从序列化字节流重建通用数据库。

```c
hs_error_t HS_CDECL fat_hs_deserialize_database_at(
    const char *bytes, const size_t length, fat_hs_database_t *db);
```

| 参数 | 说明 |
| --- | --- |
| `bytes` | 通用数据库的序列化字节流，不能为空。 |
| `length` | `bytes`指向字节流的长度。 |
| `db` | 调用者预分配的输出内存，不能为空且至少按8字节对齐。所需空间先通过`fat_hs_serialized_database_size()`查询；内存所有权始终归调用者，不得调用`fat_hs_free_database()`释放。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 反序列化成功。 |
| `HS_INVALID` | `bytes`或`db`为空，字节流格式、长度、数据库魔数或校验和无效。 |
| `HS_BAD_ALIGN` | `db`未按8字节对齐。 |
| `HS_DB_VERSION_ERROR` | 字节流中的数据库版本与当前库不匹配。 |

### 2.11 `fat_hs_database_size`

查询内存中通用数据库占用的空间大小。

```c
hs_error_t HS_CDECL fat_hs_database_size(
    const fat_hs_database_t *db, size_t *size);
```

| 参数 | 说明 |
| --- | --- |
| `db` | 待查询的通用数据库。 |
| `size` | 非空输出参数。成功时返回数据库所占字节数。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 查询成功。 |
| `HS_INVALID` | `db`或`size`为空，或数据库魔数无效。 |
| `HS_DB_VERSION_ERROR` | 数据库版本与当前库不匹配。 |

### 2.12 `fat_hs_serialized_database_size`

查询序列化通用数据库反序列化所需的内存空间。

```c
hs_error_t HS_CDECL fat_hs_serialized_database_size(
    const char *bytes, const size_t length, size_t *size);
```

| 参数 | 说明 |
| --- | --- |
| `bytes` | 通用数据库的序列化字节流。 |
| `length` | `bytes`指向字节流的长度。 |
| `size` | 非空输出参数。成功时返回`fat_hs_deserialize_database_at()`所需的内存大小。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 查询成功。 |
| `HS_INVALID` | 字节流为空、长度或格式无效，或`size`为空。 |
| `HS_DB_VERSION_ERROR` | 字节流中的数据库版本与当前库不匹配。 |

### 2.13 `fat_hs_database_info`

查询通用数据库的版本和模式信息。

```c
hs_error_t HS_CDECL fat_hs_database_info(
    const fat_hs_database_t *db, char **info);
```

| 参数 | 说明 |
| --- | --- |
| `db` | 待查询的通用数据库，必须有效且按要求对齐。 |
| `info` | 非空输出参数。成功时返回由misc allocator分配的NUL结尾信息字符串；默认分配器下可使用`free()`释放，配置自定义misc allocator时应使用相应释放函数。失败时置为`NULL`。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 查询成功。 |
| `HS_INVALID` | `db`或`info`为空，`db`未按要求对齐，或数据库魔数无效。 |
| `HS_NOMEM` | misc allocator未能分配信息字符串。 |
| `HS_BAD_ALLOC` | misc allocator返回的内存未满足对齐要求。 |

### 2.14 最小用例

下面的示例编译一个通用数据库并将其序列化。实际跨平台部署时，可以直接使用`hsdump -U`生成文件，并由支持通用字节码的工具加载。

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
    free(bytes); /* 仅适用于默认misc allocator。 */
    fat_hs_free_database(db);
    return 0;
}
```

将代码保存为`/opt/Ultrascan/fat_example.c`。以下命令直接使用新生成的静态库，无需安装Ultrascan。应先按照[安装指南](./installation_guide.md)完成默认静态库编译，并确认库文件已经生成：

```bash
test -f /opt/Ultrascan/build/lib/libhs.a && \
    ls -lh /opt/Ultrascan/build/lib/libhs.a
```

命令成功时，输出中应包含`/opt/Ultrascan/build/lib/libhs.a`；文件大小和时间会随构建环境变化。例如：

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

正确输出：

```text
fat database serialized successfully
```

## 3 Grey配置API

Grey是编译器内部优化参数集合。公开API允许应用在进程内显式设置这些参数，取代旧的`config.txt`隐式读取方式。Ultrascan不再搜索或读取`config.txt`；如需覆盖默认值，必须在编译API之前主动调用本章接口。

### 3.1 `hs_set_grey_overrides`

设置进程级Grey覆盖字符串。设置成功后，后续构造Grey配置的编译调用都会读取这份配置，直到再次设置或复位。

```c
hs_error_t HS_CDECL hs_set_grey_overrides(const char *overrides);
```

| 参数 | 说明 |
| --- | --- |
| `overrides` | 格式为`key:value;key:value;...`。传入`NULL`或空字符串等价于复位。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 字符串有效，已保存新配置；`NULL`或空字符串也返回成功。 |
| `HS_INVALID` | 存在未知key、缺少冒号、非数字或越界数值等格式错误；原有全局配置保持不变。调试用途的`help`输入也会被拒绝。 |

使用约束：

- key必须是`applyGreyOverrides()`白名单中的布尔或数值字段。常用示例包括`allowLily`、`allowNeoFdr`、`limitPatternCount`和`limitPatternLength`；并非`Grey`结构中的所有字段都可公开覆盖。
- value按无符号整数解析。布尔开关建议只使用`0`或`1`，不要传入负数。
- 解析不会自动去除key中的空白字符；建议始终使用紧凑格式。
- 配置存储由互斥锁保护，但配置是进程级共享状态。为保证同一批编译结果可复现，应在启动阶段设置，并避免与编译调用并发修改。
- 每次成功调用都会整体替换上一份覆盖字符串，不会自动与上一次设置合并。

### 3.2 `hs_reset_grey_overrides`

清除进程级Grey覆盖，使后续编译恢复使用代码中的硬编码默认值。

```c
hs_error_t HS_CDECL hs_reset_grey_overrides(void);
```

| 参数 | 说明 |
| --- | --- |
| 无 | 该接口无参数。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 覆盖已清除；重复调用也安全。 |

### 3.3 最小用例

下面的用例演示Grey覆盖的完整调用顺序：先设置`allowLily`、`allowNeoFdr`和`limitPatternCount`，再编译规则，最后释放数据库并复位进程级覆盖，避免影响同一进程的后续编译。该用例用于验证配置字符串可被接受并参与编译，不用于观测具体引擎选择结果。

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

将代码保存为`/opt/Ultrascan/grey_example.c`。以下命令直接使用新生成的静态库，无需安装Ultrascan。先确认默认静态库已经生成：

```bash
test -f /opt/Ultrascan/build/lib/libhs.a && \
    ls -lh /opt/Ultrascan/build/lib/libhs.a
```

命令成功时，输出中应包含`/opt/Ultrascan/build/lib/libhs.a`；文件大小和时间会随构建环境变化。例如：

```text
-rw-r--r-- 1 root root 25M Sep 30 12:00 /opt/Ultrascan/build/lib/libhs.a
```

随后执行：

```bash
cc -std=c99 \
   -I/opt/Ultrascan/src \
   /opt/Ultrascan/grey_example.c \
   /opt/Ultrascan/build/lib/libhs.a \
   -lstdc++ -lm -pthread \
   -o /opt/Ultrascan/build/grey_example

/opt/Ultrascan/build/grey_example
```

正确输出：

```text
Grey compile succeeded
```

### 3.4 示例程序与工具参数

仓库中的示例程序和主要编译工具已接入同一公开API：

| 程序 | 参数 |
| --- | --- |
| `examples/simplegrep` | `-G OVERRIDES` |
| `examples/pcapscan` | `-G OVERRIDES` |
| `examples/patbench` | `-g OVERRIDES` |
| `hsdump` | `-G OVERRIDES` |
| `hsbench`、`hscheck`、`hscollider`、`hspgo` | `-G OVERRIDES` |

命令行参数仅负责在进程内调用`hs_set_grey_overrides()`；它不会创建或修改`config.txt`。

## 4 正则匹配反馈优化技术API

正则匹配反馈优化技术建立反馈闭环：先编译baseline数据库，然后进行运行期采样，再生成feedback，最后使用feedback编译新数据库。运行期collector统计多模fragment触发与最终上报之间的关系，feedback筛选高浪费fragment，编译器在保证匹配语义不变的前提下避开相应的低效候选路径。

### 4.1 构建与可用范围

完整能力当前仅支持AArch64，并且默认关闭。构建时显式开启：

```bash
mkdir -p /opt/Ultrascan/build-feedback
cd /opt/Ultrascan/build-feedback
cmake .. -DHS_ENABLE_FP_FEEDBACK=ON
make -j
```

- 在非AArch64平台设置`HS_ENABLE_FP_FEEDBACK=ON`会在CMake配置阶段报错。
- 关闭能力时公共符号仍然存在，以保持链接兼容。各接口在此状态下的精确返回值在对应API小节中说明。
- 普通编译和扫描应继续调用不带`_with_feedback`或`_with_collector`后缀的原接口。

### 4.2 对象、常量与参数

#### 4.2.1 Opaque对象

```c
typedef struct hs_fp_collector hs_fp_collector_t;
typedef struct hs_fp_feedback hs_fp_feedback_t;
```

- `hs_fp_collector_t`绑定一个具体baseline数据库并累积运行期计数。它借用数据库，不延长数据库生命周期。
- `hs_fp_feedback_t`保存独立的坏fragment身份副本，可在collector及baseline数据库释放后继续用于反馈编译。

#### 4.2.2 Fragment分类常量

| 常量 | 值 | 说明 |
| --- | ---: | --- |
| `HS_FP_TABLE_UNKNOWN` | 0 | 未知表。 |
| `HS_FP_TABLE_FLOATING` | 1 | floating表。 |
| `HS_FP_TABLE_EOD_ANCHORED` | 2 | EOD anchored表。 |
| `HS_FP_TABLE_SMALL_BLOCK` | 3 | small-block表。 |
| `HS_FP_TABLE_DELAY_REBUILD` | 4 | 保留值，不采集也不用于反馈编译。 |
| `HS_FP_TABLE_ANCHORED` | 5 | 保留值，不采集也不用于反馈编译。 |

| 常量 | 值 | 说明 |
| --- | ---: | --- |
| `HS_FP_ENGINE_UNKNOWN` | 0 | 未知引擎类型。 |
| `HS_FP_ENGINE_NOODLE` | 1 | Noodle类型标识。 |
| `HS_FP_ENGINE_FDR` | 2 | FDR类型标识。 |
| `HS_FP_ENGINE_NEO_FDR` | 3 | NeoFDR类型标识。 |
| `HS_FP_ENGINE_HAO` | 4 | 为保持公开ABI完整性保留的引擎类型标识。 |
| `HS_FP_ENGINE_TEDDY` | 5 | Teddy类型标识。 |

| 常量 | 值 | 说明 |
| --- | ---: | --- |
| `HS_FP_FRAGMENT_FLAG_NOCASE` | `0x01` | fragment不区分大小写。 |
| `HS_FP_FRAGMENT_FLAG_NORUNS` | `0x02` | fragment带NORUNS属性。 |
| `HS_FP_FRAGMENT_FLAG_MASKED` | `0x04` | fragment带mask/cmp约束。 |

#### 4.2.3 筛选参数

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

`flags`指定哪些字段覆盖默认值：

| Flag | 生效字段 | 默认值宏 | 默认值 |
| --- | --- | --- | ---: |
| `HS_FP_FEEDBACK_PARAM_MIN_TRIGGER_COUNT` | `min_trigger_count` | `HS_FP_FEEDBACK_DEFAULT_MIN_TRIGGER_COUNT` | 1000 |
| `HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_COUNT` | `min_false_positive_count` | `HS_FP_FEEDBACK_DEFAULT_MIN_FALSE_POSITIVE_COUNT` | 1000 |
| `HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_RATE` | `min_false_positive_rate` | `HS_FP_FEEDBACK_DEFAULT_MIN_FALSE_POSITIVE_RATE` | 99% |
| `HS_FP_FEEDBACK_PARAM_MIN_WASTE_SHARE` | `min_waste_share` | `HS_FP_FEEDBACK_DEFAULT_MIN_WASTE_SHARE` | 5% |
| `HS_FP_FEEDBACK_PARAM_MAX_BAD_FRAGMENTS` | `max_bad_fragments` | `HS_FP_FEEDBACK_DEFAULT_MAX_BAD_FRAGMENTS` | 0，表示不限制 |

比率字段使用`HS_FP_FEEDBACK_RATE_SCALE`（值为`1000000000000ULL`）缩放。例如99%表示为`990000000000ULL`。`params == NULL`或零初始化结构使用全部默认值。启用前四个阈值后，字段值为0表示不限制该项；显式启用`MAX_BAD_FRAGMENTS`时，其值必须大于0。

#### 4.2.4 Dump结构与回调

以下定义仅用于`hs_fp_collector_to_feedback_with_dump()`。生成feedback时，库可通过汇总回调输出整体统计，并通过fragment回调逐项输出fragment统计和入选状态，便于记录日志或导出CSV。回调只提供观察结果，不参与feedback筛选，也不转移任何对象的所有权。

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

`hs_fp_fragment_info_t`字段：

| 字段 | 说明 |
| --- | --- |
| `key` | 用于dump、日志和跨编译观察的稳定fragment key。 |
| `table` | `HS_FP_TABLE_*`值，也是反馈编译精确身份的一部分。 |
| `engine` | `HS_FP_ENGINE_*`值，仅用于分类和观察。 |
| `flags` | `HS_FP_FRAGMENT_FLAG_*`位图。 |
| `bytes`/`length` | fragment二进制内容及长度，不保证以NUL结尾。 |
| `mask`/`cmp`/`mask_length` | 可选mask/cmp约束；无约束时长度为0，指针可为`NULL`。 |
| `trigger_count` | 前端fragment触发次数。 |
| `true_trigger_count` | 触发后直接产生至少一次最终上报的次数；一次触发产生多个上报仍只计一次。 |
| `false_positive_count` | 未直接产生最终上报的触发次数。 |

`hs_fp_feedback_dump_summary_t`汇总本次采样窗口内可映射到metadata且实际触发过的known fragment：`fragment_count`为明细数，`bad_fragment_count`为入选feedback的数量，其余三个字段分别汇总trigger、true-trigger和false-positive计数。

`selected != 0`表示该fragment已进入本次feedback。回调参数及`bytes`、`mask`、`cmp`指针只在回调期间有效；如需持久化，必须在回调中复制。

### 4.3 `hs_fp_collector_create`

为指定baseline数据库创建运行期反馈collector。collector借用数据库，数据库必须在collector释放前保持有效；同一collector不支持多线程并发写。

```c
hs_error_t HS_CDECL hs_fp_collector_create(
    const hs_database_t *db, hs_fp_collector_t **collector);
```

| 参数 | 说明 |
| --- | --- |
| `db` | 已编译的baseline数据库。 |
| `collector` | 非空输出参数。启用特性时成功返回新collector；失败或特性未启用时置为`NULL`。调用者使用`hs_fp_collector_free()`释放成功返回的对象。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 功能已启用时创建成功。 |
| `HS_INVALID` | 功能已启用时，`collector == NULL`、数据库为空、数据库魔数错误或字节码未按要求对齐；功能未启用时，仅在`collector == NULL`时返回该值。 |
| `HS_DB_VERSION_ERROR` | 功能已启用时，数据库版本与当前库不匹配。 |
| `HS_NOMEM` | 功能已启用时，分配collector或计数器失败。 |
| `HS_ARCH_ERROR` | 功能未启用且`collector`有效时，将`*collector`置为`NULL`并返回该值。 |

### 4.4 `hs_fp_collector_reset`

清空collector的累计计数，但保留其与baseline数据库的绑定。

```c
hs_error_t HS_CDECL hs_fp_collector_reset(hs_fp_collector_t *collector);
```

| 参数 | 说明 |
| --- | --- |
| `collector` | 待复位的collector。启用特性时不能为空。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 功能已启用时复位成功。 |
| `HS_INVALID` | 功能已启用且`collector == NULL`。 |
| `HS_ARCH_ERROR` | 功能未启用时，不校验`collector`，直接返回该值。 |

### 4.5 `hs_fp_collector_merge`

将同一数据库创建的多个collector合并为新collector。输入collector不会被释放或复位。

```c
hs_error_t HS_CDECL hs_fp_collector_merge(
    hs_fp_collector_t *const *collectors, unsigned int count,
    hs_fp_collector_t **collector);
```

| 参数 | 说明 |
| --- | --- |
| `collectors` | 待合并的collector数组，不能为空。所有元素必须非空，且必须由完全相同的数据库对象创建。 |
| `count` | `collectors`中的对象数量，必须大于0。 |
| `collector` | 非空输出参数。启用特性时成功返回新的合并collector；失败或特性未启用时置为`NULL`。调用者使用`hs_fp_collector_free()`释放成功返回的对象。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 功能已启用时合并成功。 |
| `HS_INVALID` | 功能已启用时，`collectors == NULL`、`count == 0`、`collector == NULL`、任一输入collector为空，或输入来自不同数据库对象；功能未启用时，仅在`collector == NULL`时返回该值。 |
| `HS_DB_VERSION_ERROR` | 功能已启用时，新collector创建时关联数据库版本与当前库不匹配。 |
| `HS_NOMEM` | 功能已启用时，分配新collector或计数器失败。 |
| `HS_ARCH_ERROR` | 功能未启用且`collector`有效时，将`*collector`置为`NULL`并返回该值。 |

### 4.6 `hs_fp_collector_free`

释放collector。`NULL`安全。

```c
hs_error_t HS_CDECL hs_fp_collector_free(hs_fp_collector_t *collector);
```

| 参数 | 说明 |
| --- | --- |
| `collector` | 待释放的collector；可为`NULL`。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 无论特性是否启用、`collector`是否为`NULL`，均返回成功。 |

### 4.7 `hs_fp_collector_to_feedback`

从collector生成用于反馈编译的feedback对象。调用会提交pending计数，但不会复位collector；转换期间不得并发写入同一collector。无fragment入选时，成功返回的feedback仍然有效。

```c
hs_error_t HS_CDECL hs_fp_collector_to_feedback(
    hs_fp_collector_t *collector, const hs_fp_feedback_params_t *params,
    hs_fp_feedback_t **feedback);
```

| 参数 | 说明 |
| --- | --- |
| `collector` | 有效的反馈collector。 |
| `params` | 可选筛选参数。传入`NULL`或零初始化结构时使用默认阈值。 |
| `feedback` | 非空输出参数。启用特性时成功返回feedback；失败或特性未启用时置为`NULL`。调用者使用`hs_fp_feedback_free()`释放成功返回的对象。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 功能已启用时，feedback生成成功。 |
| `HS_INVALID` | 功能已启用时，`collector == NULL`、`feedback == NULL`、筛选参数的`flags`包含未知位、已启用的`max_bad_fragments`为0，或已启用的比例阈值大于`HS_FP_FEEDBACK_RATE_SCALE`；功能未启用时，仅在`feedback == NULL`时返回该值。 |
| `HS_NOMEM` | 功能已启用时，分配统计报告或feedback失败。 |
| `HS_ARCH_ERROR` | 功能未启用且`feedback`有效时，将`*feedback`置为`NULL`并返回该值。 |

### 4.8 `hs_fp_collector_to_feedback_with_dump`

从collector生成feedback，并可同步输出汇总和fragment dump信息。回调只提供观察结果，不参与筛选，也不转移对象所有权。

```c
hs_error_t HS_CDECL hs_fp_collector_to_feedback_with_dump(
    hs_fp_collector_t *collector, const hs_fp_feedback_params_t *params,
    const hs_fp_feedback_dump_callbacks_t *callbacks, void *context,
    hs_fp_feedback_t **feedback);
```

| 参数 | 说明 |
| --- | --- |
| `collector` | 有效的反馈collector。 |
| `params` | 可选筛选参数。传入`NULL`或零初始化结构时使用默认阈值。 |
| `callbacks` | 可选dump回调集合；为`NULL`时不输出dump。`on_summary`和`on_fragment`可分别为`NULL`。 |
| `context` | 原样传递给dump回调的用户上下文；`callbacks == NULL`时不使用。 |
| `feedback` | 非空输出参数。启用特性时成功返回feedback；失败或特性未启用时置为`NULL`。调用者使用`hs_fp_feedback_free()`释放成功返回的对象。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 功能已启用时，feedback生成成功。 |
| `HS_INVALID` | 功能已启用时，`collector == NULL`、`feedback == NULL`、筛选参数的`flags`包含未知位、已启用的`max_bad_fragments`为0，或已启用的比例阈值大于`HS_FP_FEEDBACK_RATE_SCALE`；功能未启用时，仅在`feedback == NULL`时返回该值。 |
| `HS_NOMEM` | 功能已启用时，分配统计报告或feedback失败。 |
| `HS_ARCH_ERROR` | 功能未启用且`feedback`有效时，将`*feedback`置为`NULL`并返回该值。 |

### 4.9 `hs_fp_feedback_free`

释放feedback对象。`NULL`安全。

```c
hs_error_t HS_CDECL hs_fp_feedback_free(hs_fp_feedback_t *feedback);
```

| 参数 | 说明 |
| --- | --- |
| `feedback` | 待释放的feedback；可为`NULL`。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 无论特性是否启用、`feedback`是否为`NULL`，均返回成功。 |

### 4.10 `hs_compile_multi_with_feedback`

使用feedback编译多个正则表达式。当前没有single、literal或fat数据库的反馈编译变体。

```c
hs_error_t HS_CDECL hs_compile_multi_with_feedback(
    const char *const *expressions, const unsigned int *flags,
    const unsigned int *ids, unsigned int elements, unsigned int mode,
    const hs_platform_info_t *platform, const hs_fp_feedback_t *feedback,
    hs_database_t **db, hs_compile_error_t **error);
```

| 参数 | 说明 |
| --- | --- |
| `expressions` | 以NUL结尾的正则表达式数组。 |
| `flags` | 每个表达式对应的`HS_FLAG_*`标志数组；可为`NULL`，表示全部标志为0。 |
| `ids` | 每个表达式对应的ID数组；可为`NULL`，表示使用默认ID。 |
| `elements` | 表达式数量，必须大于0。 |
| `mode` | 数据库模式。必须选择`HS_MODE_BLOCK`、`HS_MODE_STREAM`或`HS_MODE_VECTORED`之一，并可组合适用的模式标志。 |
| `platform` | 可选目标平台信息；为`NULL`时按当前主机平台生成数据库。 |
| `feedback` | 可选反馈对象。启用特性时为`NULL`等价于对应的普通multi编译；库只在调用期间读取，不接管所有权。 |
| `db` | 输出参数。成功时返回新数据库；失败或特性未启用时置为`NULL`。调用者使用`hs_free_database()`释放成功返回的对象。 |
| `error` | 输出参数。编译失败时返回错误详情；调用者使用`hs_free_compile_error()`释放非空对象。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 功能已启用时编译成功。 |
| `HS_COMPILER_ERROR` | 功能已启用时，编译参数、模式或平台参数不合法，或表达式解析、编译失败；详情通过`error`返回。 |
| `HS_NOMEM` | 功能已启用时，创建或复制反馈编译上下文失败。 |
| `HS_ARCH_ERROR` | 功能未启用时，即使`feedback == NULL`也返回该值；`db`置为`NULL`，`error`非空时返回错误详情。 |

### 4.11 `hs_compile_ext_multi_with_feedback`

使用feedback和扩展参数编译多个正则表达式。

```c
hs_error_t HS_CDECL hs_compile_ext_multi_with_feedback(
    const char *const *expressions, const unsigned int *flags,
    const unsigned int *ids, const hs_expr_ext_t *const *ext,
    unsigned int elements, unsigned int mode,
    const hs_platform_info_t *platform, const hs_fp_feedback_t *feedback,
    hs_database_t **db, hs_compile_error_t **error);
```

| 参数 | 说明 |
| --- | --- |
| `expressions` | 以NUL结尾的正则表达式数组。 |
| `flags` | 每个表达式对应的`HS_FLAG_*`标志数组；可为`NULL`，表示全部标志为0。 |
| `ids` | 每个表达式对应的ID数组；可为`NULL`，表示使用默认ID。 |
| `ext` | 每个表达式对应的`hs_expr_ext_t`指针数组；可为`NULL`，单个数组元素也可为`NULL`。调用期间由调用者保持有效。 |
| `elements` | 表达式数量，必须大于0。 |
| `mode` | 数据库模式。必须选择`HS_MODE_BLOCK`、`HS_MODE_STREAM`或`HS_MODE_VECTORED`之一，并可组合适用的模式标志。 |
| `platform` | 可选目标平台信息；为`NULL`时按当前主机平台生成数据库。 |
| `feedback` | 可选反馈对象。启用特性时为`NULL`等价于对应的普通扩展multi编译；库只在调用期间读取，不接管所有权。 |
| `db` | 输出参数。成功时返回新数据库；失败或特性未启用时置为`NULL`。调用者使用`hs_free_database()`释放成功返回的对象。 |
| `error` | 输出参数。编译失败时返回错误详情；调用者使用`hs_free_compile_error()`释放非空对象。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 功能已启用时编译成功。 |
| `HS_COMPILER_ERROR` | 功能已启用时，编译参数、扩展参数、模式或平台参数不合法，或表达式解析、编译失败；详情通过`error`返回。 |
| `HS_NOMEM` | 功能已启用时，创建或复制反馈编译上下文失败。 |
| `HS_ARCH_ERROR` | 功能未启用时，即使`feedback == NULL`也返回该值；`db`置为`NULL`，`error`非空时返回错误详情。 |

### 4.12 `hs_scan_with_collector`

执行块模式扫描，同时向collector记录运行期触发信息。匹配语义和回调行为与`hs_scan()`一致。

```c
hs_error_t HS_CDECL hs_scan_with_collector(
    const hs_database_t *db, const char *data, unsigned int length,
    unsigned int flags, hs_scratch_t *scratch,
    match_event_handler onEvent, void *context,
    hs_fp_collector_t *collector);
```

| 参数 | 说明 |
| --- | --- |
| `db` | 块模式数据库，且必须与`collector`绑定同一个数据库对象。 |
| `data` | 待扫描数据，不能为空。 |
| `length` | `data`的字节长度。 |
| `flags` | 扫描标志，当前保留供将来使用。 |
| `scratch` | 与`db`兼容的每线程scratch空间。 |
| `onEvent` | 可选匹配回调；为`NULL`时不输出匹配。 |
| `context` | 原样传递给`onEvent`的用户上下文。 |
| `collector` | 与`db`绑定的有效collector。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 功能已启用时扫描成功；本次pending计数已提交。 |
| `HS_SCAN_TERMINATED` | 功能已启用时，匹配回调请求停止；本次pending计数已提交。 |
| `HS_INVALID` | 功能已启用时，`collector`、数据库、数据或scratch无效，或数据库与collector不匹配。 |
| `HS_DB_VERSION_ERROR` | 功能已启用时，数据库版本与当前库不匹配。 |
| `HS_DB_MODE_ERROR` | 功能已启用时，数据库不是块模式。 |
| `HS_SCRATCH_IN_USE` | 功能已启用时，`scratch`正在被其他调用使用。 |
| `HS_UNKNOWN_ERROR` | 功能已启用时，扫描期间发生内部匹配错误。 |
| `HS_ARCH_ERROR` | 功能未启用时，直接返回该值，不执行扫描。 |

### 4.13 `hs_scan_vector_with_collector`

执行vectored模式扫描，同时向collector记录运行期触发信息。匹配语义和回调行为与`hs_scan_vector()`一致。

```c
hs_error_t HS_CDECL hs_scan_vector_with_collector(
    const hs_database_t *db, const char *const *data,
    const unsigned int *length, unsigned int count, unsigned int flags,
    hs_scratch_t *scratch, match_event_handler onEvent, void *context,
    hs_fp_collector_t *collector);
```

| 参数 | 说明 |
| --- | --- |
| `db` | vectored模式数据库，且必须与`collector`绑定同一个数据库对象。 |
| `data` | 待扫描数据块指针数组，不能为空。 |
| `length` | 各数据块长度数组，不能为空。 |
| `count` | 数据块数量。 |
| `flags` | 扫描标志，当前保留供将来使用。 |
| `scratch` | 与`db`兼容的每线程scratch空间。 |
| `onEvent` | 可选匹配回调；为`NULL`时不输出匹配。 |
| `context` | 原样传递给`onEvent`的用户上下文。 |
| `collector` | 与`db`绑定的有效collector。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 功能已启用时扫描成功；本次pending计数已提交。 |
| `HS_SCAN_TERMINATED` | 功能已启用时，匹配回调请求停止；本次pending计数已提交。 |
| `HS_INVALID` | 功能已启用时，`collector`、数据库、数据数组、长度数组或scratch无效，或数据库与collector不匹配。 |
| `HS_DB_VERSION_ERROR` | 功能已启用时，数据库版本与当前库不匹配。 |
| `HS_DB_MODE_ERROR` | 功能已启用时，数据库不是vectored模式。 |
| `HS_SCRATCH_IN_USE` | 功能已启用时，`scratch`正在被其他调用使用。 |
| `HS_UNKNOWN_ERROR` | 功能已启用时，扫描期间发生内部匹配错误。 |
| `HS_ARCH_ERROR` | 功能未启用时，直接返回该值，不执行扫描。 |

### 4.14 `hs_scan_stream_with_collector`

执行streaming模式扫描，同时向collector记录运行期触发信息。匹配语义和回调行为与`hs_scan_stream()`一致；stream close和reset阶段产生的EOD工作不会计入collector。

```c
hs_error_t HS_CDECL hs_scan_stream_with_collector(
    hs_stream_t *id, const char *data, unsigned int length,
    unsigned int flags, hs_scratch_t *scratch,
    match_event_handler onEvent, void *context,
    hs_fp_collector_t *collector);
```

| 参数 | 说明 |
| --- | --- |
| `id` | 已打开的stream对象；其数据库/Rose实例必须与`collector`绑定的对象相同。 |
| `data` | 待扫描数据，不能为空。 |
| `length` | `data`的字节长度。 |
| `flags` | 扫描标志，当前保留供将来使用。 |
| `scratch` | 与stream兼容的每线程scratch空间。 |
| `onEvent` | 可选匹配回调；为`NULL`时不输出匹配。 |
| `context` | 原样传递给`onEvent`的用户上下文。 |
| `collector` | 与stream绑定同一Rose实例的有效collector。 |

返回值：

| 返回值 | 说明 |
| --- | --- |
| `HS_SUCCESS` | 功能已启用时扫描成功；本次pending计数已提交。 |
| `HS_SCAN_TERMINATED` | 功能已启用时，匹配回调请求停止；本次pending计数已提交。 |
| `HS_INVALID` | 功能已启用时，`id`、`collector`、数据或scratch无效，或stream与collector不匹配；功能未启用时，`id == NULL`也返回该值。 |
| `HS_SCRATCH_IN_USE` | 功能已启用时，`scratch`正在被其他调用使用。 |
| `HS_UNKNOWN_ERROR` | 功能已启用时，扫描期间发生内部匹配错误；已记录的部分计数不会回滚。 |
| `HS_ARCH_ERROR` | 功能未启用且`id`有效时，直接返回该值，不执行扫描。 |

### 4.15 最小闭环用例

下面的C用例使用块模式完成baseline编译、采集、生成feedback和反馈编译。示例要求链接在AArch64上使用`HS_ENABLE_FP_FEEDBACK=ON`构建的库。

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

    /* 使前端literal可以触发，而最终表达式因min_offset不满足而不上报。 */
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

将代码保存为`/opt/Ultrascan/feedback_example.c`。该用例要求先在AArch64上使用本章构建命令开启`HS_ENABLE_FP_FEEDBACK`。以下命令直接使用新生成的静态库，无需安装Ultrascan。先确认支持反馈能力的静态库已经生成：

```bash
test -f /opt/Ultrascan/build-feedback/lib/libhs.a && \
    ls -lh /opt/Ultrascan/build-feedback/lib/libhs.a
```

命令成功时，输出中应包含`/opt/Ultrascan/build-feedback/lib/libhs.a`；文件大小和时间会随构建环境变化。例如：

```text
-rw-r--r-- 1 root root 25M Sep 30 12:00 /opt/Ultrascan/build-feedback/lib/libhs.a
```

随后执行：

```bash
cc -std=c99 \
   -I/opt/Ultrascan/src \
   /opt/Ultrascan/feedback_example.c \
   /opt/Ultrascan/build-feedback/lib/libhs.a \
   -lstdc++ -lm -pthread \
   -o /opt/Ultrascan/build-feedback/feedback_example

/opt/Ultrascan/build-feedback/feedback_example
```

正确输出：

```text
feedback-compiled database created
```

完整工具化流程请参考[快速入门](./quick_start.md)中的`hspgo`章节。
