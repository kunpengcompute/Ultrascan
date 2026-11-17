# 开发指南

本文档介绍Ultrascan内部实现与KHSEL内部接口，说明KHSEL已优化函数的功能、参数与返回值，适用于需要理解或维护Ultrascan内部实现的开发者。

## 文档定位

本文面向需要理解或维护Ultrascan内部实现的开发者，说明KHSEL内部接口和Hao算法的编译、运行与诊断流程。

Ultrascan新增公开API的函数定义、参数、返回值、生命周期和最小用例统一收录在[《API参考》](./api_reference.md)中。

开发指南不重复维护这些公开接口的逐项定义，以公共头文件和API参考为准。

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

Hao位于`src/fdr/`，复用HWLM/FDR的编译和运行入口。应用通过Grey参数允许编译器
选择Hao，再使用原有公开扫描接口。启用方式、选型阈值和平台要求见
[用户指南](./user_guide.md#hao多模匹配算法)。

### 编译与表布局

1. `fdrBuildProtoInternal()`先尝试Teddy，再根据`allowHao`、literal数量与平均长度
   决定是否尝试Hao。`analyzeHAOFeasibility()`进一步检查目标能力、规则覆盖及容量。
   `tryBuildHaoProto()`随后要求碰撞桶比例不超过5%，不合格的自动候选继续使用FDR；
   多变体编译分别过滤DOT/BEXT。该门槛是启发式筛选，不等于与FDR的实测性能比较。
   普通选型失败后继续尝试NeoFDR/FDR，不输出只覆盖部分literal的Hao表。
2. `hao_compile.cpp`为每条内部literal构造键展开计划和校验片段。每个校验槽保存
   最多8字节的内部literal及对应掩码；这不是对用户正则表达式长度的限制。
   大小写不敏感约束通过掩码编码，兼容的附加mask/cmp约束合并到L2校验；
   无法合并时拒绝该Hao方案。
3. BEXT根据所选bit提取哈希键。默认自动流程比较不同selector及18~22位目标键宽，
   根据桶分布、冲突、展开量和内存占用估计成本。DOT则将输入窗口按16位元素
   与系数向量做点积，结合输入掩码形成22位哈希键，并在编译期搜索掩码与系数。
4. L1位图标记非空键，主表编码L2起始偏移与entry数量。每个`HAORuntimeL2Check`
   包含4组64位规则值和掩码，共64字节，配套metadata保存规则索引与边界校验信息。
   冲突桶可以包含多个entry；完全相同的L2范围可被多个展开键共享，减少实际表占用。
5. `buildHAOBlob()`生成单个Hao布局；多变体编译由`buildHAOVariants()`将DOT/BEXT
   布局封装为`HAOVariants`，FDR回退表保留在父对象中。报告元数据与FDR桶顺序
   对齐，用于保持同一结束位置的报告顺序、group控制、NORUNS及回调终止行为。

### 多变体与运行期分派

`CompileContext::haoVariants`区分单一路径与多变体编译意图。公开`fat_hs_*`编译
接口及`hsdump`默认启用多变体意图：先构建不含Hao的回退matcher，再为当前
AArch64编译主机支持的能力分别尝试DOT和BEXT。至少一个变体成功后才附加
`HAOVariants`。`hsbench -e`显式关闭此意图，以便按`haoHashMode`比较单一路径，
同时保留通用字节码容器用于保存和加载。

`haoSelectPath()`将路径选择与内核执行分开，综合数据库中存在的变体、已编译
内核、CPU/操作系统能力和当前SVE向量长度。向量长度至少32字节且条件满足时
优先BEXT，其次DOT，否则执行FDR回退。不要将“工具链可以编译某内核”当作
“运行机可以执行该内核”；也不要假设在x86或不支持SVE的主机上编译数据库会
生成所有Hao变体。

DOT与BEXT分别由`hao_dot.c`、`hao_bext.c`实例化共享的`hao_engine.c`，采用
独立源文件编译标志；公共分派和布局校验代码保持Armv8-A+CRC基线。运行期处理
输入窗口、流历史和尾部边界，先探测位图，再访问L2并用SVE校验候选。
`hao_runtime_layout.c`负责布局、表及变体范围校验；数据库校验失败应按错误处理，
不能把损坏的字节码视为正常的能力不足回退。

### 诊断与验证

- `haoDebug:1`输出`[HAO][EngSel]`，用于检查规模与长度决策。
- `haoDebug:2`输出selector、规则覆盖、键展开、桶分布和L2复用等编译统计。
  支持`DUMP_SUPPORT`时，`hsdump`还通过`Grey::DUMP_HAO`输出
  `hao_compile_stats_<matcher>.txt`与`hao_l2map_<matcher>.txt`，
  多变体文件名包含经过规范化的DOT/BEXT标记。
- `haoDebugAuto`用于支持`DUMP_SUPPORT`的BEXT自动搜索，输出`[HAO][AutoCand]`
  候选成本和表规模。例如`haoDebugAuto:16`将默认18~22位搜索扩展为16~22位。
- `haoDebug:4`仅在以`HAO_ENABLE_RUNTIME_STATS=1`编译的构建中生效。
  该宏默认是`0`，不是独立的CMake选项。可按下面的方式生成诊断构建：

    ```bash
    mkdir -p /opt/Ultrascan/build-hao-stats
    cd /opt/Ultrascan/build-hao-stats
    cmake .. -DCMAKE_BUILD_TYPE=DEBUG \
        -DCMAKE_C_FLAGS="-DHAO_ENABLE_RUNTIME_STATS=1" \
        -DCMAKE_CXX_FLAGS="-DHAO_ENABLE_RUNTIME_STATS=1"
    make -j
    ```

    运行时传入`-G "allowHao:1;haoDebug:4;"`，进程退出时向stderr输出扫描、
    L1/L2命中、假阳性率及热点桶等统计。开关在首次进入Hao扫描时读取一次，
    因此需要在此前设置，不能在编译后、首次扫描前复位掉`haoDebug:4`。
    统计使用进程级计数与表，建议单线程诊断；测量正式吞吐时使用关闭统计的构建。

在完成AArch64构建后，可运行`./bin/unit-internal --gtest_filter='HAO*.*'`执行
相关测试；依赖实际CPU能力的测试应在对应平台运行。业务规则集的单路径与
部署文件验证见[快速入门](./quick_start.md#hao算法验证与调优)。

## 修订记录

| 文档版本 | 发布日期 | 修改说明 |
| --- | --- | --- |
| 01 | 2026-12-30 | 第一次正式发布（V5.9.0）：新增假阳性阻断过滤逻辑判断优化，新增Hao多模匹配算法特性及相关说明。 |
