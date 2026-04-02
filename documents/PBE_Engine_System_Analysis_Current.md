# PBE 引擎系统梳理（当前实现）

## 1. 文档目的
本文档只描述**当前代码实际生效的 PBE 实现**，目标是把下面几件事系统梳理清楚：

1. PBE 在整体编译链路中的位置。
2. 编译期每一步在做什么、为什么这么做。
3. 运行期每一步在做什么、为什么这么做。
4. 当前使用到的核心结构体、字段含义及设计原因。
5. 当前使用到的核心函数、输入输出及职责边界。
6. 当前实现已经完成的部分、仍然存在的性能瓶颈和限制。

说明：

1. 本文档以当前仓库代码为准。
2. 本文档以“当前主线实现”为主，不把历史尝试方案作为主线展开。
3. 本文档统一使用中文描述，必要时保留少量英文函数名、宏名、结构体名，便于对照源码。

---

## 2. 相关源码文件

### 2.1 编译期
1. `src/fdr/fdr_compile.cpp`
   - PBE 在整体引擎选择链路中的接入点。
2. `src/fdr/pbe_compile.h`
   - PBE 编译期结构体、常量、接口声明。
3. `src/fdr/pbe_compile.cpp`
   - PBE 编译期构建逻辑。

### 2.2 运行期
1. `src/fdr/pbe_runtime.h`
   - PBE runtime blob 布局定义。
2. `src/fdr/pbe_engine.c`
   - PBE 运行期执行主逻辑。
3. `src/fdr/pbe_extract_sve2_bitperm.c`
   - `SVEBITPERM` 硬件位提取辅助实现。

### 2.3 测试与文档
1. `unit/internal/pbe_vs_neo.cpp`
   - PBE 与 Neo 的一致性测试、runtime 测试、prefilter 测试、inspect 测试。
2. `documents/PBE_Design_Compile_Runtime.md`
   - 设计与阶段性演进记录。
3. `documents/PBE_Implementation_Report_Current.md`
   - 增量实现记录。

---

## 3. PBE 在整体编译链路中的位置

### 3.1 整体选择顺序
当前 `FDR` 编译链路里，PBE 的位置是：

1. 先尝试 Teddy。
2. Teddy 未命中后，如果灰度和平台条件允许，则尝试 PBE。
3. PBE 不可行时，再回到 NeoFdr / Fdr。

当前主线要求是：

1. **能走 PBE 就直接走 PBE**。
2. 不再允许“编译期说可行、运行期再回 Neo 兜底”的旧式双重分流。

### 3.2 进入 PBE 的前置条件
PBE 的前置条件由 `analyzePBEFeasibility(...)` / `canBuildPBE(...)` 统一判断，当前主要包括：

1. `grey.allowPbe == true`
2. 当前编译目标为 `AArch64`
3. 规则数不少于 4 条
4. 规则数不超过 `u16` 能表示的范围
5. 编译期可以成功构建完整的 PBE artifacts
6. 编译结果中不允许出现 `PARTIAL_COVERAGE`

也就是说，当前的 PBE 入口是一个**强约束入口**：

1. `canBuildPBE(...) == true` 才允许后续进入 PBE。
2. 一旦进入，就要求后续必须生成可执行的 PBE blob。

---

## 4. 当前 PBE 总体架构

当前 PBE 可以拆成三层：

1. **编译期层**
   - 从规则集构建 `selectors`、`L1`、`L2`、`ruleMeta`、`literalBlob`
2. **序列化层**
   - 把编译期产物编码成 runtime blob
3. **运行期层**
   - 对输入数据提取 key
   - 路由到 L1 / L2
   - 做预筛
   - 做精确确认
   - 调用回调

当前数据流可以概括为：

```text
规则集 lits
  -> 位选择 selectors
  -> 每条规则的 keyValue/keyMask
  -> 一级哈希表 L1
  -> 一级位图 bitmap
  -> 二级哈希表 L2
  -> ruleMeta + literalBlob
  -> runtime blob
  -> 运行期读取 blob
  -> window64 / key 提取
  -> bitmap / L1 / L2 路由
  -> laneMask 预筛
  -> exact confirm
  -> callback
```

---

## 5. 编译期整体流程

本节按照真实代码路径，梳理从规则集到 PBE blob 的全过程。

### 5.1 `fdrBuildProtoInternal(...)` 阶段
位置：`src/fdr/fdr_compile.cpp`

这一步的目标是：**决定当前规则集是否采用 PBE 作为引擎类型**。

执行顺序如下：

1. 选择候选引擎描述 `pbeDes`
2. 用 PBE 描述把规则分配到 bucket
3. 调用 `addIncludedInfo(...)`
4. 调用 `analyzePBEFeasibility(...)`
5. 若可行，则构造 `HWLMProto`，并把 `PBECompileArtifacts` 挂到 `proto->pbeArtifacts`

为什么这样做：

1. PBE 不是所有规则集都适合，必须先做一次完整可行性分析。
2. 可行性分析需要真正构建一次编译期 artifacts，才能知道：
   - 是否能选出有效 selectors
   - L1 / L2 是否可构建
   - 是否会触发 partial coverage
3. 把 `PBECompileArtifacts` 先保存在 proto 里，便于后续 table 阶段复用思路和调试。

### 5.2 `analyzePBEFeasibility(...)`
位置：`src/fdr/pbe_compile.cpp`

这是 PBE 编译入口的统一前置判断函数。

它的职责是：

1. 判断灰度是否允许
2. 判断平台是否允许
3. 判断规则规模是否允许
4. 真正尝试构建一次 `PBECompileArtifacts`
5. 根据构建结果给出：
   - `canBuild`
   - `reason`
   - `flags`

为什么单独做成这个函数：

1. 让 PBE 的“可行性判断”只有一个入口，避免 proto 阶段和 table 阶段口径不一致。
2. 方便统一输出失败原因，例如：
   - `GREY_DISABLED`
   - `ARCH_UNSUPPORTED`
   - `TOO_FEW_LITERALS`
   - `PARTIAL_ENTRY_OVERFLOW`

### 5.3 `buildPBEArtifacts(...)`
位置：`src/fdr/pbe_compile.cpp`

这是 PBE 编译期的核心构建函数。

当前执行顺序是：

1. 初始化并清空 `PBECompileArtifacts`
2. 调用 `selectBitSelectors(...)`
3. 调用 `buildExtractDescriptor(...)`
4. 调用 `buildHashTables(...)`
5. 调用 `buildPrimaryBitmap(...)`
6. 调用 `buildRuleMeta(...)`
7. 根据 `enableDump` 决定是否打印调试输出

为什么这么分步：

1. `selectors` 是整个 hash 体系的起点。
2. `extract descriptor` 是运行期 key 提取的描述数据。
3. `L1/L2` 是路由结构。
4. `bitmap` 是 L1 的辅助结构，用于先判空再查 L1。
5. `ruleMeta/literalBlob` 是运行期确认阶段的载荷。

### 5.4 `selectBitSelectors(...)`
位置：`src/fdr/pbe_compile.cpp`

职责：

1. 从规则尾部 8 字节的 64 个 bit 中选择最多 22 个 bit 作为 selector。
2. 当前主线策略固定 `keyBits = 22`。

为什么要做 selector 选择：

1. PBE 的 key 不是直接取整个字符串，而是只取“最有区分度的若干 bit”。
2. 这样可以把规则映射到有限位宽的 hash key 上。

当前实现里 selector 的来源是：

1. 基于规则 bit 状态统计
2. 过滤掉没有区分度的位
3. 优先保留有信息量的位

### 5.5 `buildExtractDescriptor(...)`
位置：`src/fdr/pbe_compile.cpp`

职责：

1. 根据当前 selector 列表生成运行期提取描述：
   - `extractMode`
   - `windowBytes`
   - `bextMask`

为什么要这样设计：

1. 运行期不能每次重新理解 selector 列表。
2. `bextMask` 用于把 selector 映射成“位压缩提取”的描述。
3. 当前主线已经把 selector 顺序重排为源 bit 升序，因此 `BEXT` 的 packed 结果可以直接作为最终 key，不再需要额外 remap 表。

### 5.6 `buildHashTables(...)`
位置：`src/fdr/pbe_compile.cpp`

职责：

1. 根据规则的 `keyValue/keyMask` 构建 L1 与 L2
2. 把同一个 L1 key 下的规则装进一个或多个连续 L2 entry
3. 在出现无法完整覆盖时设置 `flags`

当前主线策略：

1. `keyBits` 固定 22 位
2. L1 表项数固定 `2^22`
3. L1 value 是一个打包后的 `u32`
   - 低 18 位：L2 起始偏移
   - 高位：连续 L2 项数
4. L2 每项固定承载最多 4 条规则
5. 若同 key 规则数超过 4，则分配多个连续 L2 项

为什么这样做：

1. 让 L1 成为固定宽度的直接路由表
2. 让 L2 成为定长规则块，方便运行期做固定布局预筛

### 5.7 `buildPrimaryBitmap(...)`
位置：`src/fdr/pbe_compile.cpp`

职责：

1. 基于 `L1 offsets` 单独生成一级哈希位图
2. 每个非空 L1 项对应 1 bit

为什么 bitmap 要独立存在：

1. 它不是 L1 主表的一部分，而是 L1 的辅助索引结构。
2. 运行期可以先查 bitmap，再决定是否读取 L1 主表。
3. 这样能避免大量空表项上的无效 L1 访问。

### 5.8 `buildRuleMeta(...)`
位置：`src/fdr/pbe_compile.cpp`

职责：

1. 为每条规则构建运行期确认需要的元信息
2. 同时构建 `literalBlob`

为什么需要 `ruleMeta + literalBlob`：

1. L2 只适合做预筛，不适合保存完整规则语义。
2. 真正的精确匹配仍然需要：
   - 完整规则字节
   - groups
   - nocase 标记
   - mask/cmp

### 5.9 `buildPBEBlob(...)`
位置：`src/fdr/pbe_compile.cpp`

职责：

1. 按 runtime layout 把 artifacts 序列化为一段连续内存
2. 填充 `PBERuntimeHeader`
3. 依次写入：
   - selectors
   - primary bitmap
   - primary hash table
   - secondary hash table
   - rule meta
   - literal blob

为什么要单独做 runtime blob：

1. 编译期使用的是 C++ 容器结构，不能直接给运行期使用。
2. 运行期需要的是固定布局、可偏移寻址的只读数据块。

### 5.10 `fdrBuildTableInternal(...)`
位置：`src/fdr/fdr_compile.cpp`

职责：

1. 在 `proto` 已经选择 PBE 后，再用最终 `proto.lits` 重建一遍 artifacts
2. 调用 `buildPBEBlob(...)`
3. 把 blob 交给 `FDRCompiler`
4. 最终写入 `FDR::pbeOffset / pbeSize`

为什么 table 阶段还要重建：

1. `proto.lits` 在后续阶段可能被写入最终运行期 ID 等信息。
2. 如果直接复用 proto 阶段的旧 artifacts，可能出现 rule id 过期问题。
3. 所以当前策略是：
   - proto 阶段决定“是否可走 PBE”
   - table 阶段基于最终规则重新构建真正落盘的 blob

---

## 6. 运行期整体流程

本节按照当前 runtime 实现，梳理从 `FDR` 执行到 callback 的全过程。

### 6.1 `PbeEngineExec(...)`
位置：`src/fdr/pbe_engine.c`

职责：

1. 作为 PBE 运行期入口
2. 调用 `pbeExecWithPath(...)`

当前默认策略：

1. 默认走 `Batch4` 路径
2. `Naive` 路径只保留给测试与对照

### 6.2 `pbeExecWithPath(...)`
位置：`src/fdr/pbe_engine.c`

职责：

1. 从 `FDR` 中定位 PBE blob
2. 调用 `pbeValidateLayout(...)`
3. 检查 `PARTIAL_COVERAGE` 等运行期禁止条件
4. 根据 `useBatch4` 选择：
   - `pbeRunBatch4(...)`
   - `pbeRunNaive(...)`

为什么要做这一层：

1. 统一 layout 校验逻辑
2. 统一 runtime 入口
3. 方便对照测试

### 6.3 `pbeValidateLayout(...)`
位置：`src/fdr/pbe_engine.c`

职责：

1. 校验 `magic`
2. 校验 `version`
3. 校验 `selectorCount/primaryCount/secondaryCount`
4. 校验各段 offset 和 size 是否在 `pbeSize` 范围内

为什么必须做：

1. PBE blob 是偏移布局，一旦 offset 错误会直接越界。
2. 运行期必须先确认 blob 合法，再进入扫描。

### 6.4 `pbeRunBatch4(...)`
位置：`src/fdr/pbe_engine.c`

职责：

1. 以 4 个位置为一批处理输入
2. 批量构造位置上下文
3. 查 bitmap / L1
4. 回放 exact 和 wildcard 路径

为什么要有 Batch4：

1. 单位置逐个处理太碎，前端准备开销很大。
2. `Batch4` 可以先把前端组织起来：
   - `window64`
   - key
   - bitmap
   - L1 encoded value

但要注意：

1. 当前 `Batch4` 主要是**组织优化**
2. 还不是最终意义上的全链路 SIMD 批处理

### 6.5 `PBEPositionContext`
位置：`src/fdr/pbe_engine.c`

职责：

1. 表示某一个扫描位置的运行期上下文
2. 避免 exact / wildcard / 多个 L2 entry 重复准备同样的输入视图

这是当前运行期很关键的一个降开销结构。

### 6.6 `pbeBuildPositionContext(...)`
位置：`src/fdr/pbe_engine.c`

职责：

1. 构造当前 `endPos` 对应的 `window64`
2. 计算当前 `key`
3. 计算 `primaryIdx`
4. 构造 `laneWindow32`
5. 计算 `validMask32`

为什么要提前构造 context：

1. 同一位置会经过：
   - exact bucket
   - wildcard bucket
   - 多个 L2 entry
2. 如果每一步都重新建窗口，成本会很高。

### 6.7 `window64` 与 key 提取

#### 6.7.1 `pbeLoadWindow64Normalized(...)`
位置：`src/fdr/pbe_engine.c`

职责：

1. 以当前 `endPos` 为结尾，向前取最多 8 个字节
2. 拼成一个 64 位窗口
3. 同时支持：
   - 当前 `buf`
   - `buf_history`

为什么要有 `window64`：

1. selector 只会从尾部最多 8 字节中取位
2. 所以运行期没必要逐 selector 去反复回读输入
3. 先得到 `window64`，后面无论 scalar 还是 bext 都可以直接在这个 `u64` 上取位

#### 6.7.2 `pbeExtractKeyFromWindow(...)`
位置：`src/fdr/pbe_engine.c`

职责：

1. 根据 header 里的 `extractMode`
2. 选择：
   - `pbeExtractKeyScalarFromWindow(...)`
   - `pbeExtractKeyBext(...)`

#### 6.7.3 `pbeExtractKeyScalarFromWindow(...)`
职责：

1. 按 selector 列表逐位取值
2. 生成 `key`

#### 6.7.4 `pbeExtractKeyBext(...)`
职责：

1. 根据 `bextMask` 压缩抽取 window 中的指定 bit
2. 由于编译期已经把 key 位顺序对齐到 packed 顺序，压缩结果可直接作为最终 key

#### 6.7.5 `pbeExtractPackedBitsSveBitPerm(...)`
位置：`src/fdr/pbe_extract_sve2_bitperm.c`

职责：

1. 在 `SVEBITPERM` 可用时，调用硬件 bit-permute 指令做 packed bit extract

为什么还需要 fallback：

1. 不是所有机器都支持 `SVEBITPERM`
2. 所以当前 `bext` 逻辑是：
   - 能走硬件则走硬件
   - 否则走 `pbeExtractPackedBitsFallback(...)`

### 6.8 L1 bitmap 与 L1 主表

#### 6.8.1 `pbePrimaryBitmapHasValue(...)`
职责：

1. 先检查某个 L1 key 是否对应非空表项

为什么先查 bitmap：

1. L1 很大而且稀疏
2. 先查位图，可以跳过大量空 key 上的主表访问

#### 6.8.2 `pbeDecodePrimaryValue(...)`
职责：

1. 把 L1 的 `u32 encoded` 解码为：
   - `offset`
   - `count`

其中：

1. 低 18 位：L2 起始偏移
2. 高位：连续 L2 项数

### 6.9 L2 预筛

#### 6.9.1 `pbeBuildLaneWindow32FromWindow(...)`
职责：

1. 把当前 8 字节 suffix 视图扩成 32B
2. 形成和 L2 `ruleVector[32]` 对齐的输入布局

当前布局是：

1. 一个 8 字节窗口复制 4 份
2. 对应 L2 每项的 4 个 slot

#### 6.9.2 `pbeEntryLaneMaskFromByteMatches(...)`
职责：

1. 把字节级 compare 结果归约成 slot 级别的 `laneMask`

#### 6.9.3 `pbeEntryMatchMaskFromContextScalar(...)`
职责：

1. 标量版本的 L2 预筛
2. 用于参考实现和测试对照

#### 6.9.4 `pbeEntrySingleSlotMatchMaskFromContext(...)`
职责：

1. 针对 `ruleCount == 1` 的单槽 L2 entry 做轻量快路径
2. 优先检查 `tailOnlyMask`
3. 再检查 `headMask`

为什么单独做这个快路径：

1. 当前很多 L2 entry 实际只有 1 条规则
2. 没必要让单槽 entry 也走完整 32B 向量预筛

#### 6.9.5 `pbeEntryMatchMaskFromContextVector(...)`
职责：

1. 多槽 entry 的向量化预筛
2. 对 `ruleVector` 与 `laneWindow32` 做 32B compare
3. 产出 `laneMask`

为什么引入 `laneMask`：

1. 以前是 entry 级别布尔判断
2. 现在直接知道 entry 里哪几个 slot 可能命中
3. 这样后面只处理命中的 slot，不用扫完整个 entry

### 6.10 `pbeProcessEncodedRange(...)`
位置：`src/fdr/pbe_engine.c`

职责：

1. 根据某个 L1 encoded value 解码出一段连续 L2 range
2. 对每个 L2 entry 做预筛
3. 对命中的 slot 再做：
   - `keyValue/keyMask`
   - groups
   - exact confirm
4. 最终调用 callback

为什么这一步存在：

1. L1 只负责路由
2. 真正的候选规则处理都在 L2 和 ruleMeta 上完成

### 6.11 `pbeRuleExactMatch(...)`
位置：`src/fdr/pbe_engine.c`

职责：

1. 对通过预筛的规则做最终精确确认

当前支持：

1. 完整规则字节比较
2. `nocase`
3. `mask/cmp`
4. history 跨边界读取

这是当前运行期最重的慢路径之一。

### 6.12 `PbeEngineExecNaiveForTest(...)` 与 `PbeRuntimeEntryMatchMaskForTest(...)`
职责：

1. 为测试提供对照路径
2. 保证：
   - Batch4 与 Naive 一致
   - vector prefilter 与 scalar prefilter 一致

---

## 7. 编译期核心结构体说明

### 7.1 `PBEBitSelector`

| 字段 | 含义 | 为什么这样定义 |
|---|---|---|
| `byteOffset` | 距离当前结尾位置向后的第几个字节 | selector 只针对尾部窗口 |
| `bitOffset` | 该字节内的 bit 下标 | 用于定位具体 bit |

### 7.2 `PBEPrimaryHashTable`

| 字段 | 含义 | 为什么这样定义 |
|---|---|---|
| `offsets` | L1 主表，`offsets[key] = encodedValue` | 运行期直接按 key 随机访问 |

### 7.3 `PBEPrimaryHashBitmap`

| 字段 | 含义 | 为什么这样定义 |
|---|---|---|
| `bits` | L1 是否非空的压缩位图 | 先判空再查 L1 主表 |

### 7.4 `PBESecondaryHashEntry`

| 字段 | 含义 | 为什么这样定义 |
|---|---|---|
| `ruleVector[32]` | 4 个 slot 的后缀字节视图，8B/slot | 便于固定布局预筛 |
| `tableControl[32]` | 每个 byte lane 是否有效 | 短规则需要 don't-care |
| `ruleIndex[4]` | slot 对应的 ruleMeta 下标 | 运行期通过它回到元信息 |
| `keyValue[4]` | 规则的 key 值 | 运行期二次过滤 |
| `keyMask[4]` | 规则关心哪些 key bit | 处理 nocase / 短规则 / partial key |
| `headMask` | 头部关键字节位图 | 快速预筛 |
| `tailMask` | 尾部关键字节位图 | 快速预筛 |
| `ruleCount` | 当前 entry 的有效 slot 数 | 末尾 entry 可能不足 4 条 |
| `reserved` | 预留/对齐 | 便于后续扩展 |

### 7.5 `PBERuleMeta`

| 字段 | 含义 | 为什么这样定义 |
|---|---|---|
| `id` | 最终回调 ID | 运行期输出 |
| `groups` | group mask | 运行期 group 过滤 |
| `len` | 规则长度 | confirm 必需 |
| `flags` | `NOCASE/NORUNS/HAS_MASK` | 运行期语义开关 |
| `maskLen` | 掩码有效长度 | confirm 必需 |
| `litOffset` | 规则字节在 literalBlob 中的偏移 | 支持任意长度规则 |
| `lit[8]` | 短规则/调试缓存字段 | 保留短规则局部载荷 |
| `msk[8]` | 掩码字节 | 运行期 mask 校验 |
| `cmp[8]` | 目标比较字节 | 运行期 mask 校验 |

### 7.6 `PBECompileArtifacts`

| 字段 | 含义 | 为什么这样定义 |
|---|---|---|
| `keyBits` | 当前 key 位数 | 当前主线固定为 22 |
| `flags` | 构建期标志 | 记录 partial coverage 等状态 |
| `extractMode` | 提取模式 | scalar / bext |
| `windowBytes` | key 提取窗口大小 | 当前固定 8 |
| `bextMask` | bext 位提取掩码 | 运行期直接使用 |
| `bitSelectors` | selector 列表 | 编译期/inspect 使用 |
| `primaryHashTable` | L1 主表 | 路由结构 |
| `primaryHashBitmap` | L1 位图 | 判空辅助结构 |
| `secondaryHashTable` | L2 表 | 候选规则容器 |
| `ruleMeta` | 规则元信息 | confirm 与 callback 载荷 |
| `literalBlob` | 完整规则字节池 | 支持长规则 |

### 7.7 `PBEFeasibilityReason`
表示 PBE 编译不可行的原因。

当前主要包括：

1. `GREY_DISABLED`
2. `ARCH_UNSUPPORTED`
3. `TOO_FEW_LITERALS`
4. `TOO_MANY_LITERALS`
5. `NO_SELECTORS`
6. `PARTIAL_SECONDARY_CAPACITY`
7. `PARTIAL_ENTRY_OVERFLOW`
8. `PARTIAL_OTHER`
9. `ARTIFACT_BUILD_FAILED`

### 7.8 `PBEFeasibilityResult`

| 字段 | 含义 |
|---|---|
| `canBuild` | 是否允许进入 PBE |
| `flags` | 构建期标志 |
| `reason` | 失败或成功原因 |

---

## 8. 运行期核心结构体说明

### 8.1 `PBERuntimeHeader`

| 字段 | 含义 | 作用 |
|---|---|---|
| `magic` | blob 魔数 | 校验是否为 PBE blob |
| `version` | 布局版本 | 校验 runtime 布局兼容性 |
| `flags` | runtime 标志 | 例如 partial coverage |
| `keyBits` | key 位数 | 运行期读取配置 |
| `selectorCount` | selector 数量 | 运行期读取 selector |
| `primaryCount` | L1 项数 | L1 边界检查 |
| `primaryBitmapSize` | L1 位图大小 | 读取位图 |
| `secondaryCount` | L2 entry 数量 | L2 边界检查 |
| `ruleMetaCount` | ruleMeta 数量 | 规则元信息边界检查 |
| `literalBlobSize` | literal blob 大小 | 规则字节边界检查 |
| `extractMode` | key 提取模式 | scalar / bext 分派 |
| `windowBytes` | 提取窗口大小 | 当前为 8 |
| `bextMask` | 硬件/软件 packed extract 描述 | bext 提取关键参数 |
| `selectorsOffset` | selectors 段偏移 | blob 内定位 |
| `primaryBitmapOffset` | bitmap 段偏移 | blob 内定位 |
| `primaryOffset` | L1 主表偏移 | blob 内定位 |
| `secondaryOffset` | L2 偏移 | blob 内定位 |
| `ruleMetaOffset` | ruleMeta 偏移 | blob 内定位 |
| `literalBlobOffset` | literal blob 偏移 | blob 内定位 |

### 8.2 `PBERuntimeBitSelector`
与编译期 `PBEBitSelector` 对应，用于 runtime/inspect。

### 8.3 `PBERuntimeSecondaryHashEntry`
与编译期 `PBESecondaryHashEntry` 一一对应，是 runtime 真正使用的 L2 entry。

### 8.4 `PBERuntimeRuleMeta`
与编译期 `PBERuleMeta` 一一对应，是 runtime 确认阶段的数据来源。

### 8.5 `PBEPositionContext`
位置：`src/fdr/pbe_engine.c`

这是运行期内部结构，不序列化到 blob。

| 字段 | 含义 | 作用 |
|---|---|---|
| `endPos` | 当前扫描结束位置 | callback 与 confirm 使用 |
| `key` | 当前 key | L2 二次过滤使用 |
| `primaryIdx` | L1 索引 | L1 路由使用 |
| `exactEncoded` | exact bucket 对应 L1 编码值 | exact 路径复用 |
| `wildcardEncoded` | wildcard bucket 对应 L1 编码值 | wildcard 路径复用 |
| `window64` | 当前 8B 输入窗口 | key 提取基础 |
| `laneWindow32[32]` | 扩展后的 32B 输入视图 | L2 预筛复用 |
| `validMask32` | 当前输入有效字节位图 | history/边界处理 |

为什么要定义这个结构：

1. 同一位置上很多数据是 exact / wildcard / 多 entry 共用的。
2. 把它们收敛到一个位置级 context 里，可以减少重复准备开销。

---

## 9. 函数职责总表

## 9.1 编译期接口函数

### `analyzePBEFeasibility(...)`
1. 输入：
   - `target`
   - `lits`
   - `grey`
   - `result`
   - `artifacts`
2. 输出：
   - `bool`
   - 可选填充 `result/artifacts`
3. 作用：
   - 统一判断规则集是否允许进入 PBE

### `pbeFeasibilityReasonName(...)`
1. 输入：`PBEFeasibilityReason`
2. 输出：原因字符串
3. 作用：日志与调试输出

### `pbeHasSveBitPermPrereq(...)`
1. 输入：`target`
2. 输出：`bool`
3. 作用：
   - 判断未来 bext 快路径的前置条件是否成立

### `pbeCanUseBextFastPath(...)`
1. 输入：`target`
2. 输出：`bool`
3. 作用：
   - 编译期决定 runtime header 中是否标记 bext 模式

### `canBuildPBE(...)`
1. 输入：
   - `target`
   - `lits`
   - `grey`
2. 输出：`bool`
3. 作用：
   - 对外提供简单的 PBE 可行性判断入口

### `buildPBEArtifacts(...)`
1. 输入：
   - `lits`
   - `artifacts`
   - `enableDump`
2. 输出：`bool`
3. 作用：
   - 构建完整编译期产物

### `buildPBEBlob(...)`
1. 输入：`artifacts`
2. 输出：`bytecode_ptr<u8>`
3. 作用：
   - 序列化 runtime blob

## 9.2 编译期关键内部函数

### 规则建模相关
1. `normalizeMaskCmp(...)`
   - 统一 `msk/cmp` 语义
2. `getBitState(...)`
   - 获取某规则在某 bit 位置上的状态
3. `computeKeyValueMaskForLiteral(...)`
   - 计算单规则的 `keyValue/keyMask`

### selector 选择相关
1. `signatureOfStates(...)`
   - 为 bit 状态模式生成签名
2. `selectBitSelectors(...)`
   - 选择最终 selector

### 提取描述相关
1. `buildExtractDescriptor(...)`
   - 生成 `bextMask/extractMode`

### 哈希表构建相关
1. `encodePrimaryValue(...)`
   - 编码 L1 的 `(offset,count)`
2. `buildHashTables(...)`
   - 构建 L1/L2
3. `pbePrimaryBitmapBytes(...)`
   - 计算 bitmap 字节数
4. `pbePrimaryBitmapSet(...)`
   - 设置 bitmap 位
5. `buildPrimaryBitmap(...)`
   - 基于 L1 构建独立 bitmap

### RuleMeta 相关
1. `buildRuleMeta(...)`
   - 生成 `ruleMeta + literalBlob`

### Dump/Inspect 相关
1. `byteToBits(...)`
2. `keyToBits(...)`
3. `maskToBits(...)`
4. `dumpRuleBits(...)`
5. `dumpSelectors(...)`
6. `dumpExtractDescriptor(...)`
7. `dumpRuleKeys(...)`
8. `dumpHashTables(...)`
9. `dumpPBEArtifactsVerbose(...)`

这些函数不参与最终匹配语义，但对调试与单测 inspect 很重要。

## 9.3 运行期接口函数

### `PbeEngineExec(...)`
PBE runtime 主入口。

### `PbeEngineExecNaiveForTest(...)`
测试专用 Naive 入口。

### `PbeRuntimeEntryMatchMaskForTest(...)`
测试专用 entry prefilter 对照入口。

### `pbeExtractPackedBitsSveBitPerm(...)`
硬件 `SVEBITPERM` packed bit extract helper。

## 9.4 运行期关键内部函数

### 基础读取/校验
1. `pbeGetByteAt(...)`
2. `pbeValidateLayout(...)`
3. `pbePrimaryBitmapHasValue(...)`
4. `pbeDecodePrimaryValue(...)`

### key 提取
1. `pbeLoadWindow64Normalized(...)`
2. `pbeExtractKeyScalarFromWindow(...)`
3. `pbeExtractPackedBitsFallback(...)`
4. `pbeRemapPackedBits(...)`
5. `pbeExtractKeyBext(...)`
6. `pbeExtractKeyFromWindow(...)`
7. `pbeRuntimeCanUseBextFastPath(...)`

### 位置上下文
1. `pbeComputeValidMask8(...)`
2. `pbeBuildLaneWindow32FromWindow(...)`
3. `pbeBuildPositionContext(...)`

### Batch4 前端
1. `pbeBuildBatch4Contexts(...)`
2. `pbeBatchBitmapMask(...)`
3. `pbeCompactPrimaryLanes(...)`
4. `pbeBatchLoadPrimaryValues(...)`
5. `pbeFillBatch4EncodedValues(...)`
6. `pbeRunBatch4(...)`

### L2 预筛
1. `pbeEntryLaneMaskFromByteMatches(...)`
2. `pbeEntryMatchMaskFromContextScalar(...)`
3. `pbeEntrySingleSlotMatchMaskFromContext(...)`
4. `pbeEntryMatchMaskFromContextVector(...)`

### 确认与回调
1. `pbeRuleExactMatch(...)`
2. `pbeProcessEncodedRange(...)`
3. `pbeRunNaive(...)`
4. `pbeExecWithPath(...)`

---

## 10. 当前编译期流程图

```text
lits
  -> analyzePBEFeasibility(...)
      -> buildPBEArtifacts(...)
          -> selectBitSelectors(...)
          -> buildExtractDescriptor(...)
          -> buildHashTables(...)
          -> buildPrimaryBitmap(...)
          -> buildRuleMeta(...)
  -> 若可行，proto 选择 PBE
  -> fdrBuildTableInternal(...)
      -> 再次 buildPBEArtifacts(...)
      -> buildPBEBlob(...)
      -> FDRCompiler 写入 fdr->pbeOffset / pbeSize
```

---

## 11. 当前运行期流程图

```text
fdrExec / fdrExecStreaming
  -> PbeEngineExec(...)
      -> pbeExecWithPath(...)
          -> pbeValidateLayout(...)
          -> pbeRunBatch4(...)
              -> pbeBuildBatch4Contexts(...)
                  -> window64
                  -> key
                  -> laneWindow32
              -> pbeFillBatch4EncodedValues(...)
                  -> bitmap
                  -> L1
              -> 对每个 lane：
                  -> exact encoded
                  -> wildcard encoded
                  -> pbeProcessEncodedRange(...)
                      -> L2 entry
                      -> laneMask 预筛
                      -> keyValue/keyMask
                      -> groups
                      -> pbeRuleExactMatch(...)
                      -> callback
```

---

## 12. 为什么当前性能仍然不理想

虽然当前正确性已经比较完整，但性能仍然较差，原因主要在以下几点：

1. **L1 固定 22 位，结构很大**
   - `2^22` 项的 L1 对小规则集非常稀疏
2. **`mode=bext` 不等于已经获得完整硬件加速**
   - 现在优化的只是 key 提取这一个局部步骤
3. **Batch4 仍然主要是前端组织优化**
   - 后端 exact / wildcard / confirm 仍然以逐 lane 回放为主
4. **L2 预筛已经接入，但还不是最终的高收益版本**
   - 当前主要是 laneMask 级别过滤
5. **`pbeRuleExactMatch(...)` 仍然是慢路径**
   - 当前高频候选仍要逐字节确认

也就是说，当前 PBE 的实现状态更准确地说是：

1. **框架已经打通**
2. **正确性已经较稳定**
3. **性能优化只做到了中前段**
4. **真正的大热点已经逐渐集中到 exact confirm**

---

## 13. 当前实现状态总结

### 13.1 已经完成的部分
1. PBE 编译期准入判断
2. selector 选择
3. `bextMask` / `bextToKeyBit`
4. L1 主表 + L1 bitmap
5. L2 固定 4 规则分块
6. ruleMeta + literalBlob
7. runtime blob 序列化与布局校验
8. `window64`
9. scalar / bext 双 key 提取路径
10. `Batch4`
11. L2 laneMask 预筛
12. 单槽快路径
13. groups / nocase / mask / history / callback

### 13.2 仍未完成或仍较弱的部分
1. exact confirm 快路径
2. confirm 的更深层向量化
3. 针对极短规则集的专门轻量路径
4. 当前固定 22-bit L1 对小规则集的结构成本问题

### 13.3 当前最重要的认识
当前 PBE 已经不是“不能用”，而是：

1. **能用**
2. **结果基本对**
3. **但性能优化还没有完成最后几层**

因此后续优化方向应该更聚焦在：

1. confirm 快路径
2. 极短规则集专项路径
3. 进一步降低固定 22-bit L1 对小规则集的结构开销

