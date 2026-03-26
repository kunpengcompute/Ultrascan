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
`buildPBEArtifacts(...)` 内部若出现 `PBE_ARTIFACT_FLAG_PARTIAL_COVERAGE`，直接返回失败。

这意味着当前定义下，以下场景视为“不符合 PBE 条件”：
1. 二级哈希单槽（32 条）装不下某 key 的全部规则。
2. 位提取 key 展开被截断（X 位展开上限触发）。

### 2.4 表构建阶段硬校验
`fdrBuildTableInternal(...)` 对 `engineID == 2`（PBE）增加硬校验：
1. 若 `pbeBlob` 未成功生成，则直接构建失败（返回 `nullptr`）。
2. 避免出现“engine 是 PBE，但对象里无有效 PBE blob”的异常状态。

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

