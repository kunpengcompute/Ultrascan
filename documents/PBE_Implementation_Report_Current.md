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

## 13. Incremental Update (PBE Runtime v3, L2 128 Slots)

This section records the latest implementation change set.

### 13.1 Runtime Version and Compatibility
1. `PBE_RUNTIME_VERSION` is upgraded from `2` to `3`.
2. Current runtime only accepts v3 PBE blobs (`PbeEngineExec` layout validation checks v3).
3. v2 blob compatibility is not provided in this stage.

### 13.2 L2 Entry Capacity and Mask Width
1. L2 per-entry slot capacity is upgraded from `32` to `128`.
2. The following arrays are now `128` slots in both compile/runtime structures:
   - `ruleVector[]`
   - `tableControl[]`
   - `ruleIndex[]`
   - `keyValue[]`
   - `keyMask[]`
3. `headMask/tailMask` are upgraded from single `u32` to `u32[4]` (128-bit total coverage).
4. `PARTIAL_ENTRY_OVERFLOW` threshold changes from `>32` to `>128` rules per L2 bucket.

### 13.3 Compile-Time and Runtime Path Alignment
1. Build-time overflow detection now uses `PBE_RULE_SLOTS_PER_ENTRY=128`.
2. Blob serialization/deserialization is aligned with the new v3 L2 layout.
3. Runtime candidate loops now scan up to `entry->ruleCount` with upper bound `128`.

### 13.4 Observability / Dump Changes
1. Build artifact dump now prints `entry_capacity=128`.
2. L2 mask dump now prints:
   - `headMaskWords={w0,w1,w2,w3}`
   - `tailMaskWords={w0,w1,w2,w3}`
   - full 128-bit binary string for both masks.
3. Inspect unit test output is updated with the same 128-bit mask presentation.

### 13.5 Unit Test Updates
1. Existing PBE-vs-Neo consistency tests remain.
2. Overflow rejection case now uses `>128` colliding literals (previously `>32`).
3. Added acceptance test for dense collision bucket within capacity (`<=128`) to ensure PBE build still succeeds.

---

## 14. Incremental Update (Dynamic keyBits Selection)

To reduce oversized PBE databases on small and medium literal sets, `keyBits`
is no longer treated as a fixed `22`-bit width. The compile path now keeps the
existing selector-ranking logic, but chooses the final active prefix of
selectors dynamically.

### 14.1 Goal
1. Keep L1 size under control for small/medium rule sets.
2. Reduce collision hot spots, especially the wildcard bucket.
3. Preserve current PBE blob/runtime format without changing runtime logic.

### 14.2 Current Selection Strategy
1. First, build a ranked selector candidate list with the existing rules:
   - prefer cared bits,
   - prefer discriminative bits,
   - deduplicate identical state columns.
2. Then evaluate `k = 1..limit` using the first `k` selectors.
3. Choose the `k` with the best score and truncate the selector list to that
   final `keyBits`.

### 14.3 Evaluation Inputs
For each candidate `k`, the compiler computes:
1. `l1Bytes = (1 << k) * sizeof(u32)`
2. `nonEmptyBuckets`
3. `maxBucketSize`
4. `wildcardBucketSize`
5. `collisionCost = sum(bucketSize^2)`

### 14.4 Score Function
The current scoring model is:

```text
score =
    1    * l1Bytes +
    256  * collisionCost +
    4096 * maxBucketSize +
    8192 * wildcardBucketSize
```

This intentionally penalizes wildcard concentration more heavily because the
current runtime probes both the exact bucket and the wildcard bucket.

### 14.5 Upper Bounds
`keyBits` is additionally capped by:
1. selector count,
2. runtime key-width limit (`<= 32`),
3. default upper bound (`<= 22`),
4. literal-count heuristic:
   - `<= 64 -> 12`
   - `<= 256 -> 14`
   - `<= 1024 -> 16`
   - `> 1024 -> 18`
5. L1 size budget:
   - `PBE_KEY_BITS_L1_MAX_BYTES = 256 KB`

With `u32` L1 entries, the current L1 budget corresponds to an effective
bit-width upper bound of `16`.

### 14.6 Observability
Compile-time dump now prints a `KeyBits-Eval` table showing:
1. each evaluated `k`,
2. `l1Bytes`,
3. `nonEmptyBuckets`,
4. `maxBucketSize`,
5. `wildcardBucketSize`,
6. `collisionCost`,
7. `score`,
8. the selected `keyBits`.

---

## 15. Incremental Update (Fixed 22-bit L1 + 4-Rule L2 Chunks)

This section supersedes the previous dynamic-keyBits direction for the current
implementation baseline.

### 15.1 Current Strategy
1. `keyBits` is now fixed to `22` (`PBE_KEY_BITS=22`).
2. L1 table size is fixed to `2^22`.
3. L1 key is the value composed from the extracted selector bits.
4. L1 value is a packed `u32`:
   - low `18` bits: L2 base offset
   - high bits: number of consecutive L2 entries for this L1 key

### 15.2 L2 Entry Shape
Each L2 entry now represents up to `4` colliding rules:
1. `32B ruleVector`
   - `4` lanes
   - each lane stores the normalized trailing `8`-byte window of one rule
2. `32B tableControl`
   - `4` lanes
   - each lane uses `8` control bytes to mark which bytes in the ruleVector
     lane are valid
3. `32b headMask`
4. `32b tailMask`

### 15.3 Multi-Entry Collision Handling
1. If one L1 key has `<= 4` rules, it uses one L2 entry.
2. If one L1 key has `> 4` rules, the compiler allocates multiple consecutive
   L2 entries.
3. The number of required L2 entries is encoded into the high bits of the L1
   value.
4. Runtime decodes `(offset, count)` from L1 and scans that L2 range in order.

### 15.4 Current Runtime Behavior
1. Runtime first extracts the fixed-width 22-bit key.
2. It decodes the L1 value into:
   - `secondaryOffset`
   - `entryCount`
3. It scans each consecutive L2 entry in that range.
4. Within each L2 entry, it performs a suffix-window precheck using the
   `32B ruleVector + 32B tableControl`.
5. Exact confirm is still done by `PBERuntimeRuleMeta + literalBlob`.

### 15.5 Notes
1. The older “128 slots per L2 entry” layout is no longer the active target.
2. The previous dynamic `keyBits` section is retained as history only and is
   not the current implementation policy.

### 15.6 Current Regression Coverage
The current unit coverage is organized into three groups:

1. `PBEvsNeo.*`
   - block consistency
   - streaming consistency
   - `groups / noruns / mask / nocase`
   - multi-entry exact bucket
   - multi-entry wildcard bucket
   - mixed exact + wildcard
   - L1 `count|offset` encoding checks
2. `PBECompile.*`
   - feasibility reason name mapping
   - grey-disabled feasibility rejection
   - too-few-literals rejection
   - blob header / artifact serialization consistency
3. `PBERuntime.*`
   - invalid `magic`
   - invalid `version`
   - invalid layout offset
   - all required to fall back cleanly without matches or crashes

### 15.7 Inspect Statistics
`PBEInspect` now prints a compact build-statistics summary in addition to the
full selector/L1/L2 dump. The summary includes:

1. `nonEmptyL1`
2. `exactBucketCount`
3. `multiEntryBucketCount`
4. `maxL2EntriesPerKey`
5. `wildcardL2Entries`
6. `wildcardRules`
7. `totalL2Entries`
8. `totalRulesInL2`

This is intended to make real-rule-set distribution analysis easier before
starting performance optimization work.
