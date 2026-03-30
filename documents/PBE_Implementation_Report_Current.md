# PBE 现阶段实现说明报告

## 1. 目标与范围
本文档描述当前代码基线下 PBE 的真实实现状态，覆盖：
1. PBE 进入条件与选择链路。
2. 编译期与运行期数据结构。
3. 核心函数定义、入参出参与功能。
4. 编译期/运行期流程图（文字版）。
5. 当前语义边界与下一步完善方向。

说明：本文档对应当前仓库实现，不包含已回退方案（如二级哈希拉链化）。

---

## 2. PBE 进入条件

### 2.1 选择顺序
在 `fdrBuildProtoInternal(...)` 中，选择顺序为：
1. Teddy
2. PBE
3. NeoFdr
4. Fdr

即 Teddy 仍然优先；PBE 在 Teddy 之后尝试。

### 2.2 PBE 构建前置条件
`canBuildPBE(...)` 需满足：
1. `grey.allowPbe == true`
2. 平台为 `__aarch64__`
3. `lits.size() >= 4`
4. `lits.size() <= u16::max`

### 2.3 PBE-only 条件收口
当前契约改为：
1. `canBuildPBE(...)` 负责完整可行性判定（包含一次完整 artifacts 预构建）。
2. 若预构建结果包含 `PBE_ARTIFACT_FLAG_PARTIAL_COVERAGE`，`canBuildPBE(...)` 返回 false。
3. 一旦 `canBuildPBE(...)` 返回 true，后续 proto 选择阶段直接进入 PBE 分支，不再由 `buildPBEArtifacts(...)` 决定是否回退。

这意味着当前定义下，以下场景视为“不符合 PBE 条件”：
1. 二级哈希单槽（32 条）装不下某 key 的全部规则。
2. 位提取 key 展开被截断（X 位展开上限触发）。

### 2.4 表构建阶段硬校验
`fdrBuildTableInternal(...)` 对 `engineID == 2`（PBE）增加硬校验：
1. 若 `pbeBlob` 未成功生成，则直接构建失败（返回 `nullptr`）。
2. 若 table 阶段再次构建 artifacts 时出现 `PARTIAL_COVERAGE`，视为契约违例并失败（理论上不应发生）。
3. 避免出现“engine 是 PBE，但对象里无有效 PBE blob”的异常状态。

---

## 3. 结构体定义与字段说明

## 3.1 FDR 头部中的 PBE 字段
`struct FDR` 中新增并保留：
1. `u32 pbeOffset`：PBE blob 相对 FDR 基址偏移。
2. `u32 pbeSize`：PBE blob 字节大小。

用途：运行期通过这两个字段定位 PBE runtime header 与各段数据。

## 3.2 编译期结构（`pbe_compile.h`）

### `PBEBitSelector`
1. `byteOffset`：相对当前扫描位置向后取第几个字节。
2. `bitOffset`：该字节中取哪一位。

### `PBEPrimaryHashTable`
1. `offsets`：一级哈希表，值为二级表偏移。
2. `0` 表示空项（无候选）。

### `PBESecondaryHashEntry`
固定 32 槽：
1. `ruleVector[32]`：候选规则末字节（nocase 规则已做大写归一）。
2. `tableControl[32]`：候选规则长度（压缩到 `u8`）。
3. `ruleIndex[32]`：映射到 `ruleMeta` 的索引。
4. `headMask/tailMask`：位图信息（当前朴素路径主要使用 ruleIndex/tableControl）。
5. `ruleBase/ruleCount`：该 entry 对应规则起始与数量（最多 32）。

### `PBERuleMeta`
1. `id`：匹配回调 id。
2. `groups`：规则分组位图。
3. `len`：规则长度。
4. `flags`：规则标志位（`NOCASE/NORUNS/HAS_MASK`）。
5. `maskLen`：`msk/cmp` 有效长度。
6. `litOffset`：规则字节在 `literalBlob` 中偏移。
7. `lit[8]`：短规则缓存（兼容字段）。
8. `msk[8]/cmp[8]`：尾部掩码比较信息。

### `PBECompileArtifacts`
编译期总产物：
1. `keyBits`
2. `flags`
3. `bitSelectors`
4. `primaryHashTable`
5. `secondaryHashTable`
6. `ruleMeta`
7. `literalBlob`

## 3.3 运行期结构（`pbe_runtime.h`）

### `PBERuntimeHeader`
1. `magic/version`：布局识别（当前 version=1）。
2. `flags`：运行期标志（含 `PARTIAL_COVERAGE`）。
3. `keyBits/selectorCount/primaryCount/secondaryCount/ruleMetaCount/literalBlobSize`
4. 各段 offset：`selectorsOffset/primaryOffset/secondaryOffset/ruleMetaOffset/literalBlobOffset`

### `PBERuntimeBitSelector`
1. `byteOffset`
2. `bitOffset`
3. `reserved`

### `PBERuntimeSecondaryHashEntry`
与编译期结构对齐（单 entry 32 规则）。

### `PBERuntimeRuleMeta`
与编译期 ruleMeta 对齐，用于精确校验与回调。

---

## 4. 核心函数说明

## 4.1 对外核心函数

### `bool canBuildPBE(const target_t &, const vector<hwlmLiteral> &, const Grey &)`
1. 入参：
   - `target`：目标 CPU 能力
   - `lits`：规则集
   - `grey`：灰度配置
2. 出参：`bool`
3. 功能：做 PBE 预筛，不构建数据结构。

### `bool buildPBEArtifacts(const vector<hwlmLiteral> &, PBECompileArtifacts *)`
1. 入参：
   - `lits`
   - `artifacts`（输出）
2. 出参：`bool`
3. 功能：
   - 选择位提取码
   - 构建 L1/L2
   - 构建 ruleMeta/literalBlob
   - 校验 `PARTIAL_COVERAGE`（若有则失败）

### `bytecode_ptr<u8> buildPBEBlob(const PBECompileArtifacts &)`
1. 入参：`artifacts`
2. 出参：序列化后的 PBE blob
3. 功能：按 runtime layout 写入 header 与所有数据段。

### `hwlm_error_t PbeEngineExec(const FDR *, const FDR_Runtime_Args *, hwlm_group_t)`
1. 入参：
   - `fdr`：引擎对象
   - `a`：运行参数（buf/history/callback/scratch 等）
   - `control`：组掩码
2. 出参：`hwlm_error_t`
3. 功能：
   - 校验 PBE blob 布局
   - 执行朴素匹配流程
   - 当前不再调用 Neo 运行期兜底

## 4.2 编译期内部函数（`pbe_compile.cpp`）
1. `normalizeMaskCmp(...)`：统一 msk/cmp 语义。
2. `getBitState(...)`：获取某规则某 bit 的 0/1/不关心状态。
3. `buildBitCandidates(...)`：按统计特征生成候选 bit。
4. `selectBitSelectors(...)`：选最终 selectors（默认 16 位目标）。
5. `enumerateHashKeysForLiteral(...)`：单规则展开所有 key。
6. `buildHashTables(...)`：构建 L1/L2，记录 partial 覆盖风险。
7. `buildRuleMeta(...)`：构建规则元信息与 literalBlob。

## 4.3 运行期内部函数（`pbe_engine.c`）
1. `pbeValidateLayout(...)`：校验 blob 各段边界。
2. `pbeExtractKey(...)`：根据 selectors 提取当前 key。
3. `pbeEntryMayMatchAtPos(...)`：轻量候选预筛（长度+末字节）。
4. `pbeRuleExactMatch(...)`：执行完整规则比较（含 history/nocase/mask）。
5. `pbeRunNaive(...)`：主循环（L1->L2->group->精确比较->回调）。
6. `pbeGetByteAt(...)`：统一读取 current/history 字节。

---

## 5. 编译期流程图（文字版）
1. 进入 `fdrBuildProtoInternal(...)`。
2. Teddy 尝试，若成功直接返回 Teddy proto。
3. 调用 `canBuildPBE(...)` 判断是否允许尝试 PBE。
4. 调用 `buildPBEArtifacts(...)`：
   - 选位提取码
   - 构建 L1/L2
   - 构建 ruleMeta/literalBlob
   - 若 `PARTIAL_COVERAGE`，返回失败
5. 若 artifacts 成功，选择 `engineID=2`（PBE）并返回 proto。
6. 后续进入 `fdrBuildTableInternal(...)`：
   - 再次构建 artifacts + `buildPBEBlob(...)`
   - 若 `pbeBlob` 为空，直接失败
   - 将 blob 写入 FDR 对象，记录 `pbeOffset/pbeSize`

---

## 6. 运行期流程图（文字版）
1. `fdrExec/fdrExecStreaming` 根据 `fdr->engineID` 分派到 `PbeEngineExec`。
2. `PbeEngineExec`：
   - 校验 `pbeOffset/pbeSize`
   - 校验 runtime header 与段边界
   - 若 header 标了 `PARTIAL_COVERAGE`，当前返回 `HWLM_SUCCESS`
3. 进入 `pbeRunNaive(...)`，对每个扫描位置：
   - `pbeExtractKey` 取 key
   - 用 key 查 L1 得到二级偏移
   - `pbeEntryMayMatchAtPos` 做快速预筛
   - 命中候选后遍历 `ruleIndex[]`
   - group 过滤
   - `pbeRuleExactMatch` 执行精确校验
   - 回调 `a->cb(end, id, scratch)`
   - 若回调终止，返回 `HWLM_TERMINATED`
4. 扫描完成返回 `HWLM_SUCCESS`。

---

## 7. 当前语义支持情况
已支持：
1. block/streaming（含 history 跨边界读取）
2. groups 过滤
3. nocase
4. mask/cmp（归一化后语义）
5. 长度 > 8 规则（literalBlob）

当前策略说明：
1. `noruns` 标记已保留在 ruleMeta flags 中。
2. 为与 Neo 对齐，当前 PBE 朴素路径未启用全局 `last-id` 抑制模型。

---

## 8. 单测覆盖（现有）
`unit/internal/pbe_vs_neo.cpp`：
1. `BlockGroupsConsistency`
2. `BlockNorunsConsistency`
3. `StreamingMaskConsistency`
4. `BlockMaskAndNoCaseConsistency`
5. `PartialCoverageRejectedByPbeBuild`

测试目标：
1. 在可构建场景下，PBE 与 Neo 结果对齐。
2. 在 partial 覆盖场景下，PBE 构建拒绝（满足当前 PBE-only 条件定义）。

---

## 9. 下一步完善建议（不引入拉链）
1. 降低 `PARTIAL_COVERAGE` 触发概率（不改为拉链）：
   - 优化 bit selector 选择，降低单 key 聚集。
   - 控制 key 展开策略，减少截断。
2. 明确 `PbeEngineExec` 异常条件返回策略：
   - 当前异常返回 `HWLM_SUCCESS`，建议补充可观测日志或调试断言开关。
3. 补充更细颗粒回归：
   - `groups + streaming + mask` 组合。
   - 长规则 + history + nocase 组合。

---

## 10. 位提取到哈希构建的流程图与位宽说明（现实现）

## 10.1 总流程（从规则到哈希表）
```text
规则集 lits
   |
   | 1) 候选 bit 统计（后缀 8 字节内，共 64 个 bit 位置）
   |    - 跳过无信息位
   |    - 优先高区分度位
   |    - 去除特征完全相同位
   v
selectors（位提取选择器，目标位数 22）
   |
   | 2) 每条规则按 selectors 取值（0/1/X）
   |    - X 位做 key 展开
   v
规则 -> key 集合
   |
   | 3) 聚合 key -> 规则索引列表
   v
构建 L2（二级 entry，单 entry 最多 32 规则）
   |
   | 4) 回填 L1：L1[key] = L2 偏移
   v
得到 PBE 哈希结构（L1 + L2 + RuleMeta + literalBlob）
```

## 10.2 位宽定义（当前参数）
1. 位提取默认位数：`22`（`PBE_DEFAULT_KEY_BITS=22`）。
2. L1 键位：`keyBits` 位（当前目标 22 位，实际不超过候选位数）。
3. L2 键位约束：`18` 位（容量约束体现为最多 `2^18=262144` 个二级 entry）。

说明：
1. L1 逻辑键空间由 `keyBits` 决定（理论 `2^keyBits`）。
2. L2 不是再次按 7 位重哈希；当前代码中 7 位主要用于二级 entry 总量上限控制。

## 10.3 规则如何映射为 key（bit 级）
对第 `i` 个 selector：
1. selector 形式：`(byteOffset, bitOffset)`。
2. 从规则尾部第 `byteOffset` 字节取 `bitOffset` 位。
3. 该位状态为：
   - `0`：key 的第 i 位固定 0
   - `1`：key 的第 i 位固定 1
   - `X`：key 的第 i 位可 0/1（展开）

因此每条规则得到一个 key 集合：
1. 无 X 位：仅 1 个 key。
2. 有 k 个 X 位：最多 `2^k` 个 key（受展开上限限制）。

## 10.4 L1 / L2 结构形态

### L1（一级哈希）
逻辑形式：
```text
L1[key] -> secondary_offset
```
1. `key`：`keyBits` 位。
2. 值 `secondary_offset`：
   - `0` 表示空
   - 非 0 表示指向 L2 对应 entry。

### L2（二级哈希）
每个 entry 包含最多 32 条候选规则：
1. `ruleIndex[32]`：映射到 RuleMeta 的规则索引。
2. `tableControl[32]`：规则长度。
3. `ruleVector[32]`：规则尾字节（预筛）。
4. `ruleCount`：有效槽数量。
5. `headMask/tailMask`：构建期写入的位图信息。

## 10.5 运行期从 L1 到 L2 的计算
对于扫描位置 `cur`：
1. `key = pbeExtractKey(selectors, ...)`。
2. `idx = key % primaryCount`。
3. `secOff = L1[idx]`。
4. 若 `secOff != 0`，访问 `L2[secOff]` 并做候选规则校验。

可写成：
```text
cur -> key(22bit) -> idx -> L1[idx] -> secOff -> L2[secOff]
```

## 10.6 哈希表示意（ASCII）
```text
L1 (primary)
  key(bin)                 value
  000...0010101010101010 -> 7
  000...0010101010110011 -> 12
  ...                      ...

L2 (secondary)
  entry[7]:
    ruleCount=3
    slot0: ruleIndex=5  len=6  ruleVector='A'
    slot1: ruleIndex=9  len=4  ruleVector='t'
    slot2: ruleIndex=2  len=8  ruleVector='Z'
```

## 10.7 当前打印与观测
当前在 `buildPBEArtifacts(...)` 集成了构建期打印，输出包括：
1. 每条规则的字节与二进制 bit 表示。
2. selectors 对应的 bit 位。
3. 规则的 `keyValue/keyMask`（dec/hex/bin）。
4. L1 的 key/value。
5. L2 各 entry 与 slot 映射。

---

## 11. 关键变更：取消 X 展开，改为单 key + 掩码匹配

当前实现已从“规则映射多 key（X 展开）”切换为“规则映射单 key（默认 X=0）+ `keyMask`”。

## 11.1 编译期
1. 对每条规则计算：
   - `keyValue`：关心位上的取值拼接
   - `keyMask`：关心位为 1，不关心位为 0
2. 每条规则只放入一个 L1 key 桶（`keyValue` 对应桶）。
3. `PBESecondaryHashEntry` 新增：
   - `u32 keyValue[32]`
   - `u32 keyMask[32]`

## 11.2 运行期
对每个候选槽位新增判定：
```text
((inputKey ^ keyValue) & keyMask) == 0
```
仅当该条件成立时才进入后续 groups / exact / mask-cmp 校验。

## 11.3 参数约束保持
1. 位提取目标位数：22 位。
2. 二级约束：18 位容量（最多 262144 个二级 entry）。

---

## 12. 编译期管线重构（可行性分析与构建解耦）

为保证“`canBuildPBE=true` 后续稳定进入 PBE”并减少重复构建，现已做以下重构：

1. 新增可行性分析接口 `analyzePBEFeasibility(...)`：
   - 负责统一判断是否可走 PBE。
   - 返回 `PBEFeasibilityResult`（含 `reason`、`flags`）。
   - 可选产出 `PBECompileArtifacts`（供后续复用）。

2. `buildPBEArtifacts(...)` 回归纯构建职责：
   - 不再承担策略回退判断。
   - 新增 `enableDump` 参数，支持探测阶段静默构建。

3. 失败原因可观测化：
   - 引入 `PBEFeasibilityReason` 与 `pbeFeasibilityReasonName(...)`。
   - 典型原因包括：灰度关闭、架构不支持、literal 数量越界、无 selectors、二级容量/entry 溢出等。

4. 避免重复构建：
   - 在 `fdrBuildProtoInternal(...)` 中执行一次可行性分析并生成 artifacts。
   - 将 artifacts 缓存到 `HWLMProto::pbeArtifacts`。
   - `fdrBuildTableInternal(...)` 优先复用缓存 artifacts 直接序列化，避免二次构建。

---

## 13. 增量更新（历史阶段：PBE Runtime v3 与 128 槽 L2）

这一节保留为历史记录，用于说明曾经尝试过的 `128` 槽 L2 方案。

### 13.1 Runtime 版本与兼容性
1. 当时 `PBE_RUNTIME_VERSION` 从 `2` 升级到 `3`。
2. 当时运行期仅接受 v3 PBE blob。
3. 当前实现已经不再以这套 `128` 槽方案为主线。

### 13.2 历史方案的 L2 结构
1. 当时 L2 单项容量从 `32` 扩到 `128`。
2. 当时以下数组都扩到 `128` 槽：
   - `ruleVector[]`
   - `tableControl[]`
   - `ruleIndex[]`
   - `keyValue[]`
   - `keyMask[]`
3. 当时 `headMask/tailMask` 也扩成了 `u32[4]`。
4. 当前实现已回到“单项 4 规则”的方案，这一节仅作为历史留档。

---

## 14. 增量更新（历史阶段：动态 keyBits）

这一节同样保留为历史记录。

### 14.1 历史目标
1. 曾尝试根据规则规模动态调整 `keyBits`。
2. 目标是减少小规则集上的 L1 体积膨胀。
3. 该方向当前已经暂停，现阶段重新固定为 `22` 位。

### 14.2 历史说明
1. 当时保留了 selector 排序逻辑。
2. 当时会对不同 `k` 做评分，动态选择最终 `keyBits`。
3. 当前实现不再采用这套策略，这一节仅保留背景说明。

---

## 15. 增量更新（当前主线：固定 22 位 L1 + 4 规则 L2 分块）

这一节描述当前生效的实现方案。

### 15.1 当前策略
1. `keyBits` 固定为 `22`（`PBE_KEY_BITS=22`）。
2. L1 大小固定为 `2^22`。
3. L1 key 是位提取结果拼出的 22 位数值。
4. L1 value 是一个打包后的 `u32`：
   - 低 `18` 位：L2 起始偏移
   - 高位：该 L1 key 对应的连续 L2 项数

### 15.2 L2 表项结构
每个 L2 项最多表示 `4` 条冲突规则：
1. `32B ruleVector`
   - 由 `4` 个 lane 组成
   - 每个 lane 保存一条规则最后最多 `8` 字节的归一化后缀
2. `32B tableControl`
   - 同样由 `4` 个 lane 组成
   - 每个 lane 的 `8` 个控制字节表示该位置是否有效
3. `32b headMask`
4. `32b tailMask`

### 15.3 多项 L2 冲突处理
1. 如果一个 L1 key 只有 `<= 4` 条规则，则只分配一个 L2 项。
2. 如果超过 `4` 条规则，则顺序分配多个连续 L2 项。
3. 所需 L2 项数编码到 L1 value 的高位中。
4. 运行期会从 L1 解码出 `(offset, count)`，然后顺序扫描对应范围。

### 15.4 一级哈希压缩位图
1. 当前在 L1 之外新增了一级哈希压缩位图：
   - 每个 L1 表项对应 1 bit
   - bit 为 `1` 表示该 L1 key 存在非空 value
   - bit 为 `0` 表示该 L1 key 一定为空
2. 运行期在读取 L1 value 之前，先查询独立压缩位图。
3. 如果位图判定为空，则直接跳过该 key，不再访问 L1 主表。
4. 当前这一步先落成标量预检查路径；后续可在此基础上继续做：
   - 位图命中结果的 `compact`
   - 非零单元的 `LD1/GATHER`
   - 向量化一级哈希访问

### 15.5 当前运行期行为
1. 运行期先提取固定 22 位 key。
2. 先查一级哈希压缩位图，判断该 key 是否可能非空。
3. 如果位图命中，再读取 L1 value，并解码出：
   - `secondaryOffset`
   - `entryCount`
4. 顺序扫描 `[secondaryOffset, secondaryOffset + entryCount)` 范围内的 L2 项。
5. 每个 L2 项内部先做 `32B ruleVector + 32B tableControl` 的 suffix 预检查。
6. 预检查通过后，再结合 `keyValue/keyMask` 与 `ruleMeta` 做精确确认。

### 15.6 当前回归覆盖
当前单测覆盖分成三组：

1. `PBEvsNeo.*`
   - block 一致性
   - streaming 一致性
   - `groups / noruns / mask / nocase`
   - multi-entry exact bucket
   - multi-entry wildcard bucket
   - exact + wildcard 混合路径
   - L1 `count|offset` 编码校验
2. `PBECompile.*`
   - feasibility reason 名称映射
   - grey 关闭时的拒绝路径
   - 规则数过少时的拒绝路径
   - blob header 与 artifacts 的序列化一致性
3. `PBERuntime.*`
   - `magic` 非法
   - `version` 非法
   - layout offset 非法
   - 要求全部能安静回退，不产生误匹配，也不崩溃

### 15.7 Inspect 统计项
`PBEInspect` 现在除了完整的 selector/L1/L2 dump 之外，还会额外输出一组构建统计摘要：

1. `nonEmptyL1`
2. `bitmapBytes`
3. `exactBucketCount`
4. `multiEntryBucketCount`
5. `maxL2EntriesPerKey`
6. `wildcardL2Entries`
7. `wildcardRules`
8. `totalL2Entries`
9. `totalRulesInL2`

这些统计项主要用于真实规则集分析，帮助我们在进入性能优化前判断：
1. L1 实际使用率如何；
2. wildcard 桶压力是否过大；
3. 多项 L2 是否过多；
4. 当前结构分布是否合理。

### 15.8 下一步规划
下一阶段会继续沿当前框架推进两项工作：

1. 一级哈希访问优化
   - 利用压缩位图先判空
   - 再对命中的 key 做 `compact`
   - 对非零项做 `LD1/GATHER`
2. 位提取码优化
   - 下一步位提取实现计划采用 `bext` 指令
   - 目标是实现并行向量化位提取
   - 在不改变当前语义的前提下，把“提取 22 位 key”从标量路径升级为并行路径

### 15.9 一级哈希位图独立化
1. 当前实现中，一级哈希压缩位图已经从一级哈希主表中拆分出来。
2. 编译期 artifacts 现在分别保存：
   - `primaryHashTable.offsets`
   - `primaryHashBitmap.bits`
3. blob 中也分别使用独立区域描述：
   - `primaryBitmapOffset / primaryBitmapSize`
   - `primaryOffset / primaryCount`
4. 运行期访问顺序保持为：
   - 先查位图
   - 再查一级哈希主表
5. 这样做的目的，是为后续的 `compact + gather + bext` 优化预留独立的数据布局空间。

### 15.10 SVE2 门控准备
1. 当前已经补充 `SVE` / `SVE2` CPU feature 定义。
2. `target_t` 已新增：
   - `has_sve()`
   - `has_sve2()`
3. 运行时平台探测已在 AArch64/Linux 路径上补充：
   - `SVE`
   - `SVE2`
4. 当前新增了 `pbeCanUseBextFastPath(...)` 作为未来 `bext` 快路径的统一门控入口。
5. 该门控目前遵循以下规则：
   - 当前新增 `pbeHasSveBitPermPrereq(...)` 用于表达“构建能力 + 目标能力”前置条件
   - `pbeCanUseBextFastPath(...)` 目前仍固定返回 false
6. 构建层与运行层现在已拆分为三种语义：
   - `HAVE_SVE / HAVE_SVE2`：当前翻译单元的 ISA 宏
   - `HS_BUILD_HAVE_SVE / HS_BUILD_HAVE_SVE2 / HS_BUILD_HAVE_SVEBITPERM`：工程构建能力
   - `HS_CPU_FEATURES_SVE / HS_CPU_FEATURES_SVE2 / HS_CPU_FEATURES_SVEBITPERM`：运行目标 CPU 能力
7. 本阶段尚未接入真正的 `bext` 位提取执行逻辑，只完成了探测与门控准备。

### 15.11 SVEBITPERM 与 bext 提取实现进展
1. 现在已经新增 `HS_CPU_FEATURES_SVEBITPERM`，用于表达 Arm `SVE2 BitPerm` 运行目标能力。
2. 构建系统新增了 `HS_BUILD_HAVE_SVEBITPERM` 探测，并会尝试：
   - `-march=armv9-a+sve2-bitperm`
   - `-march=armv8.6-a+sve2-bitperm`
3. 运行时 `cpuid_flags()` 也会在 AArch64/Linux 上额外探测 `HWCAP2_SVEBITPERM`。
4. `target_t` 现在新增：
   - `has_sve_bitperm()`
5. PBE 的 bitperm 前置判断已经从原先的 `pbeHasSve2Prereq(...)` 修正为：
   - `pbeHasSveBitPermPrereq(...)`
6. PBE blob 头部新增 `bext` 提取描述字段：
   - `extractMode`
   - `windowBytes`
   - `bextMask`
   - `bextToKeyBit[32]`
7. 编译期现在会把 selector 列表转换成：
   - 一个按源 bit 升序排列的 `bextMask`
   - 一个 packed bit 到原 key bit 的重排表
8. 运行期提取逻辑已经改为双路径：
   - `scalar`：从 `window64` 按 selector 逐位取值
   - `bext`：先执行 packed bit extract，再按重排表恢复到当前 key bit 顺序
9. 当前 `window64` 采用原始字节拼接，不做统一大写归一化；这一步是为了保持与当前编译期 `keyValue/keyMask` 语义一致。
10. 当 `SVEBITPERM` 真正可用时，运行期会调用独立的 `pbe_extract_sve2_bitperm.c` helper；否则回退到软件 packed-extract 实现。

### 15.12 Batch4 前端化进展
1. 当前运行期主循环已经从逐位置前端推进到 `Batch4` 组织方式。
2. 每一轮会先收集最多 `4` 个相邻 `endPos`：
   - 分别提取 `key`
   - 分别做 bitmap 判空
   - 对非空 lane 做 `compact`
   - 然后只对 compact 后的 lane 继续做 L1/L2 访问
3. 本阶段仍然保留原有的 L2 / exact confirm 语义，不改变匹配结果。
4. 这意味着当前已经完成的是：
   - `window64`
   - `bextMask`
   - `Batch4`
   - `bitmap`
   - `compact`
5. 但当前仍未完成的是：
   - 多位置真正向量化 `window64` 装载
   - packed L1 load / gather
   - L2 向量化预筛
   - confirm 向量化
6. 因此这一阶段的性能收益预期是“前端组织优化”，还不是最终的高性能版本。

### 15.13 Batch4 第二阶段：compact 后的 packed L1 load

1. 当前 `pbeRunBatch4(...)` 已经从“在主循环里边提 key 边查表”的写法，继续收敛成四个独立步骤：
   - `pbeBuildBatch4(...)`
   - `pbeBatchBitmapMask(...)`
   - `pbeCompactPrimaryLanes(...)`
   - `pbeBatchLoadPrimaryValues(...)`
2. 这意味着当前运行期前端已经显式区分：
   - 批量构造 `endPos/key/primaryIdx`
   - 一级哈希位图批量判空
   - 对非空 lane 做 `compact`
   - 对 compact 后的一级哈希索引做 packed L1 load
3. 当前 packed L1 load 的具体实现方式是：
   - 先 software gather
   - 再把结果组织到连续小数组
   - 最后按原始 lane 顺序回填成 `encodedByLane`
4. 这一步的主要收益不是“已经实现真正 gather 指令”，而是：
   - 跳过空 lane
   - 让一级哈希访问数据流更规整
   - 为下一步 L2 向量化预筛准备稳定输入
5. 为了保证 Rose 运行期的偏移单调约束，当前实现继续保持：
   - 同一位置 `exact` 先于 `wildcard`
   - 不因为 compact 改变跨 lane 的原始回调顺序
6. 本阶段新增了 3 组直接对照单测：
   - `PBERuntime.Batch4MatchesNaiveDirect`
   - `PBERuntime.Batch4SparseBitmapSkipsEmptyLanes`
   - `PBERuntime.Batch4OrderStableWithWildcard`
7. 这 3 组测试的目的，是把“当前默认 Batch4 路径”和“保留的 Naive 参考路径”直接放在同一个 PBE blob 上逐条对比，确保：
   - 匹配结果一致
   - 原始回调顺序一致
   - wildcard/exact 混合场景下也不回归
