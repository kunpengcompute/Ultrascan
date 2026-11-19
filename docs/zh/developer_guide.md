# 开发指南

本文面向Ultrascan内部实现的开发与维护人员，介绍KHSEL内部函数的功能、参数与返回值，以及Hao算法的编译流程、数据布局、运行期路径选择和诊断方法。

Ultrascan新增公开API的函数定义、参数、返回值、生命周期和最小用例统一收录在《[API参考](./api_reference.md)》中。开发指南不重复维护这些公开接口的逐项定义，以公共头文件和API参考为准。

## KHSEL内部函数说明

>![须知](public_sys-resources/icon-notice.gif) **须知：**
>KHSEL中的函数（`KHSEL_xxx`）是Ultrascan内部接口，应用程序无需显式调用。

KHSEL已优化函数如下：

| 名称 | 说明 |
| --- | --- |
| KHSEL_BuildLily | 单字节短规则匹配的编译函数。 |
| KHSEL_LilyRunExec | 单字节短规则匹配的运行函数。 |
| KHSEL_BuildLilyForTeddy | 2~4字节短规则匹配的编译函数。 |
| KHSEL_LilyForTeddyRunExec | 2~4字节短规则匹配的运行函数。 |

KHSEL源码已集成在Ultrascan仓库的`src/kunpeng-enhanced`目录中，不需要单独安装KHSEL软件包。

### `KHSEL_BuildLily`

函数功能：基于规则集内的单字节短规则进行规则编译，输出编译后的mask。

```c++
std::vector<u8> KHSEL_BuildLily(
    std::map<char, lilyReport> &lily,
    std::vector<u32> &reportVec,
    std::vector<u32> &ekeyVec,
    u8 &flagsQuiet);
```

| 参数 | 描述 | 输入/输出 |
| --- | --- | --- |
| lily | 单字节短规则集合。 | 输入 |
| reportVec | 单字节短规则对应的report ID。 | 输出 |
| ekeyVec | 单字节短规则对应的ekey。 | 输出 |
| flagsQuiet | 单字节短规则中quiet模式的位掩码，每位对应一条规则。 | 输出 |

返回规则编译后的mask。

### `KHSEL_LilyRunExec`

函数功能：基于编译输出的单字节规则mask，对运行期输入数据进行匹配。

```c
hs_error_t KHSEL_LilyRunExec(const struct RoseEngine *rose,
                             hs_scratch_t *scratch);
```

| 参数 | 描述 | 输入/输出 |
| --- | --- | --- |
| rose | 保存编译期输出结果的RoseEngine对象，必须非空。 | 输入 |
| scratch | 本次扫描使用的临时内存空间，必须非空。 | 输入 |

返回值说明：

| 返回值 | 说明 |
| --- | --- |
| 0（KHSEL_MATCHING_SUCCESS） | 匹配正常完成。 |
| 1（KHSEL_MATCHING_TERMINATED） | 匹配被提前终止（例如用户回调函数请求停止匹配）。 |

### `KHSEL_BuildLilyForTeddy`

函数功能：基于规则集内的2~4字节短规则进行规则编译，输出编译后的lilyTeddy字节码。

```c++
ue2::bytecode_ptr<lilyTeddy> KHSEL_BuildLilyForTeddy(
    std::map<std::string, lilyReport> &lilyForTeddy,
    std::priority_queue<LilyForTeddyPair,
                        std::vector<LilyForTeddyPair>,
                        CompareStringLength> &lilyForTeddyPQ,
    std::vector<u32> &reportVec,
    std::vector<u32> &ekeyVec,
    std::vector<u32> &lenVec);
```

| 参数 | 描述 | 输入/输出 |
| --- | --- | --- |
| lilyForTeddy | 2~4字节规则集合。 | 输入 |
| lilyForTeddyPQ | 按规则长度排序的优先队列。 | 输入 |
| reportVec | 规则对应的report ID。 | 输出 |
| ekeyVec | 规则对应的ekey。 | 输出 |
| lenVec | 规则对应的长度。 | 输出 |

返回规则编译后的lilyTeddy字节码。

### `KHSEL_LilyForTeddyRunExec`

函数功能：基于编译输出的mask，对运行期输入数据进行匹配。

```c
hs_error_t KHSEL_LilyForTeddyRunExec(const struct RoseEngine *rose,
                                     hs_scratch_t *scratch);
```

| 参数 | 描述 | 输入/输出 |
| --- | --- | --- |
| rose | 保存编译期输出结果的RoseEngine对象，必须非空。 | 输入 |
| scratch | 本次扫描使用的临时内存空间，必须非空。 | 输入 |

返回值说明：

| 返回值 | 说明 |
| --- | --- |
| 0（KHSEL_MATCHING_SUCCESS） | 匹配正常完成。 |
| 1（KHSEL_MATCHING_TERMINATED） | 匹配被提前终止（例如用户回调函数请求停止匹配）。 |

## Hao算法实现

Hao位于`src/fdr/`，用于匹配编译器提取的字符串片段（字面量）。内部匹配引擎负责扫描这些片段，再由后续Rose解释器完成规则匹配。

启用方式、选型阈值和平台要求，请参见《[用户指南](./user_guide.md#hao多模匹配算法)》。以下函数均为内部接口，由引擎调用；应用继续使用原有公开API。

### 编译与表布局

| 阶段 | 主要入口 | 作用 |
| --- | --- | --- |
| 选型 | fdrBuildProtoInternal | 普通选型先尝试Teddy，再判断Hao候选。 |
| 检查 | analyzeHAOFeasibility | 检查目标能力、规则覆盖和表容量。 |
| 准入 | tryBuildHaoProto | 自动候选要求碰撞桶比例不超过5%。 |
| 序列化 | buildHAOBlob / buildHAOVariants | 输出单路径或多变体字节码。 |

碰撞桶比例为`collisionBuckets / nonEmptyPrimary`，多变体编译分别检查DOT/BEXT。该条件用于启发式选型，不保证实测性能优于FDR；候选被拒绝后继续其他引擎选型。

| 路径 | 哈希方式 | 默认键宽 |
| --- | --- | --- |
| DOT | 16位元素与系数向量做点积。 | 22位 |
| BEXT | 按选定位提取哈希键。 | 自动搜索18~22位 |

| 数据结构 | 用途 |
| --- | --- |
| 位图 | 标记有候选规则的哈希键。 |
| L1一级哈希表 | 保存L2起始偏移和表项数。 |
| L2二级表 | 每项保存4组规则值和校验掩码。 |
| 元数据 | 保存规则索引、边界、报告ID和分组信息。 |

每个校验槽最多保存8字节的内部片段，不限制用户正则表达式长度。编译器合并大小写不敏感及附加掩码约束；无法完整覆盖规则时拒绝该方案。相同L2范围可复用，报告元数据按FDR桶顺序对齐。

### 多变体与运行期分派

`CompileContext::haoVariants`区分编译意图。通过选型且成功构建Hao时，布局如下：

| 编译入口 | Hao布局 |
| --- | --- |
| 普通hs_compile系列接口、hsbench -e | 按haoHashMode选择单个DOT或BEXT布局。 |
| 公开fat_hs_*编译接口、hsdump | 保留FDR回退，附加主机支持的DOT/BEXT变体。 |

通用字节码仅包含编译主机支持且成功构建的Hao变体；x86部分不启用Hao。FDR回退表保存在父对象中，`HAOVariants`记录变体偏移及回退引擎编号。

运行期要求数据库含对应变体、库已构建对应内核，且CPU/操作系统支持所需指令。满足条件时按下表顺序选择：

| 优先级 | 路径 | 指令及向量长度要求 |
| --- | --- | --- |
| 1 | BEXT | SVE、SVE2、SVE2 BitPerm；至少32字节。 |
| 2 | DOT | SVE；至少32字节。 |
| 3 | FDR | Hao执行条件不足时回退。 |

DOT/BEXT分别由`hao_dot.c`、`hao_bext.c`实例化共享的`hao_engine.c`，公共分派代码保持Armv8-A+CRC基线。扫描依次完成哈希计算、位图过滤、L1查询、L2校验及回调报告，并处理流历史和尾部边界。

### 关键内部接口

编译期函数位于`ue2`命名空间，声明见`src/fdr/hao_compile.h`；运行期函数声明见`src/fdr/hao_runtime.h`。

#### `analyzeHAOFeasibility`

函数功能：检查Hao构建可行性，并可输出编译中间结果。

```c++
bool analyzeHAOFeasibility(
    const target_t &target,
    const std::vector<hwlmLiteral> &lits,
    const Grey &grey,
    HAOFeasibilityResult *result,
    HAOCompileArtifacts *artifacts,
    const std::vector<u32> *reportOrder = nullptr);
```

| 参数 | 描述 | 输入/输出 |
| --- | --- | --- |
| target | 目标平台能力。 | 输入 |
| lits | 内部字面量集合。 | 输入 |
| grey | 编译配置。 | 输入 |
| result | 可行性结果和原因，可为空。 | 输出 |
| artifacts | 编译中间结果，可为空。 | 输出 |
| reportOrder | 可选的报告顺序。 | 输入 |

返回`true`表示可构建，`false`表示不可构建；5%碰撞桶准入由调用方`tryBuildHaoProto()`另行检查。

#### `buildHAOBlob`

函数功能：将单个DOT或BEXT编译结果序列化。

```c++
bytecode_ptr<u8> buildHAOBlob(const HAOCompileArtifacts &artifacts);
```

| 参数 | 描述 | 输入/输出 |
| --- | --- | --- |
| artifacts | 已构建的Hao编译结果。 | 输入 |

返回持有字节码所有权的`bytecode_ptr<u8>`；输入不合法时返回空指针。

#### `buildHAOVariants`

函数功能：封装可用的DOT/BEXT布局和FDR回退引擎编号。

```c++
bytecode_ptr<u8> buildHAOVariants(
    const HAOCompileArtifacts *dot,
    const HAOCompileArtifacts *bext,
    u32 fallback);
```

| 参数 | 描述 | 输入/输出 |
| --- | --- | --- |
| dot | DOT编译结果，可为空。 | 输入 |
| bext | BEXT编译结果，可为空。 | 输入 |
| fallback | 父对象中的回退引擎编号。 | 输入 |

返回`HAOVariants`字节码。至少一个变体必须有效；参数不合法或子布局构建失败时返回空指针。FDR表不复制到变体字节码中。

#### `haoSelectPath`

函数功能：选择执行路径，不执行扫描。

```c
u32 haoSelectPath(const struct HAOVariants *variants,
                  u64a features, u64a kernels, u32 vl);
```

| 参数 | 描述 | 输入/输出 |
| --- | --- | --- |
| variants | 已校验的变体描述。 | 输入 |
| features | CPU/操作系统可用能力位。 | 输入 |
| kernels | 库中已构建的内核能力位。 | 输入 |
| vl | 当前SVE向量长度，单位为字节。 | 输入 |

返回`HAO_PATH_BEXT`、`HAO_PATH_DOT`或`HAO_PATH_FDR`。`variants`为空或`vl`小于32时返回`HAO_PATH_FDR`。

#### `HaoEngineExec`与`HaoVariantsExec`

函数功能：分别执行单路径布局和多变体布局；Hao条件不足时调用FDR回退。

```c
hwlm_error_t HaoEngineExec(const struct FDR *fdr,
                           const struct FDR_Runtime_Args *a,
                           hwlm_group_t control);

hwlm_error_t HaoVariantsExec(const struct FDR *fdr,
                             const struct FDR_Runtime_Args *a,
                             hwlm_group_t control);
```

| 参数 | 描述 | 输入/输出 |
| --- | --- | --- |
| fdr | 保存Hao布局和回退表的父对象。 | 输入 |
| a | 输入数据、历史数据及回调等扫描参数。 | 输入 |
| control | 本次扫描启用的规则分组位掩码。 | 输入 |

有效输入下返回底层扫描状态，`HWLM_SUCCESS`表示扫描完成，`HWLM_TERMINATED`表示终止。输入或布局无效时，`HaoVariantsExec()`返回`HWLM_TERMINATED`，`HaoEngineExec()`保留返回`HWLM_SUCCESS`的既有行为；损坏数据库应在加载时通过`haoValidateDatabase()`拒绝。

#### `HaoDotExec`与`HaoBextExec`

函数功能：分别执行DOT和BEXT扫描内核。

```c
hwlm_error_t HaoDotExec(const struct HAORuntimeHeader *hdr,
                        const struct FDR_Runtime_Args *a,
                        hwlm_group_t control);

hwlm_error_t HaoBextExec(const struct HAORuntimeHeader *hdr,
                         const struct FDR_Runtime_Args *a,
                         hwlm_group_t control);
```

| 参数 | 描述 | 输入/输出 |
| --- | --- | --- |
| hdr | 已校验的单路径Hao布局。 | 输入 |
| a | 本次扫描参数。 | 输入 |
| control | 启用的规则分组位掩码。 | 输入 |

返回`HWLM_SUCCESS`或`HWLM_TERMINATED`等内部扫描状态。调用前必须完成指令能力和向量长度检查；两个入口分别受`HS_BUILD_HAVE_SVE`和`HS_BUILD_HAVE_SVEBITPERM`构建宏控制。

### 诊断与验证

| 参数 | 用途 | 条件 |
| --- | --- | --- |
| haoDebug:1 | 输出选型及碰撞桶准入结果。 | 编译数据库时设置。 |
| haoDebug:2 | 输出覆盖、桶分布和L2复用统计。 | 编译数据库时设置。 |
| haoDebugAuto | 输出BEXT自动搜索候选。 | 需要DUMP_SUPPORT。 |
| haoDebug:4 | 输出扫描、L1/L2命中和热点统计。 | 需要HAO_ENABLE_RUNTIME_STATS=1。 |

`haoDebugAuto:16`可将默认18~22位搜索扩展为16~22位。支持`DUMP_SUPPORT`且启用`Grey::DUMP_HAO`时，`hsdump`输出`hao_compile_stats_<matcher>.txt`和`hao_l2map_<matcher>.txt`；多变体文件名带规范化的DOT/BEXT标记。

`HAO_ENABLE_RUNTIME_STATS`默认是`0`，不是独立CMake选项。诊断构建命令如下：

```bash
mkdir -p /opt/Ultrascan/build-hao-stats
cd /opt/Ultrascan/build-hao-stats
cmake .. -DCMAKE_BUILD_TYPE=DEBUG \
    -DCMAKE_C_FLAGS="-DHAO_ENABLE_RUNTIME_STATS=1" \
    -DCMAKE_CXX_FLAGS="-DHAO_ENABLE_RUNTIME_STATS=1"
make -j
```

运行时传入`-G "allowHao:1;haoDebug:4;"`。英文半角双引号用于防止Shell拆分分号。该开关在首次Hao扫描时读取，须保留到首次扫描；进程退出时向stderr输出统计。统计采用进程级计数与表，建议单线程诊断；正式吞吐测试应关闭统计。

完成AArch64构建后，执行`./bin/unit-internal --gtest_filter='HAO*.*'`验证相关功能。业务规则集的单路径与部署文件验证方法，请参见《[快速入门](./quick_start.md#hao算法验证与调优)》中的“Hao算法验证与调优”章节。

## 修订记录

| 文档版本 | 发布日期 | 修改说明 |
| --- | --- | --- |
| 01 | 2026-12-30 | 第一次正式发布 |
