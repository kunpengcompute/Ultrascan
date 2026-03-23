# PBE 多模匹配方案设计（编译期与运行期）

## 1. 目标与范围

本文档定义 Hyperscan 中 PBE 引擎的阶段化实现方案，目标如下：

1. 以前置哈希替代原 FDR 前端低效 shift-or 分组粗筛。
2. 以后端向量化批校验替代链式访问确认模型。
3. 更好处理长度不一致、模糊匹配与大小写不敏感规则。
4. 先交付可编译的主干框架（Phase-0），再逐步替换核心算法。

说明：本方案统一使用 `PBE` 命名，不使用 `PBEFDR` 命名。

## 2. 集成位置

PBE 的编译期入口挂接在：

- `fdrBuildProtoInternal(...)`
- `fdrBuildTableInternal(...)`

当前链路为：`teddy -> PBE -> NeoFDR/FDR`（按条件与回退策略选择）。

运行期通过 `fdr->engineID` 分派：

- `engineID=0`: `KHSEL_FdrEngineExec`
- `engineID=1`: `KHSEL_NeoFdrEngineExec`
- `engineID=2`: `KHSEL_PbeEngineExec`

## 3. 编译期设计

### 3.1 位提取码选择（并行位提取）

依据三条原则选择位提取码：

1. 尽量避开不关心位（通配/模糊造成的 X 位），防止哈希污染扩散。
2. 优先选择高辨识度位，使规则分布更均衡。
3. 特征完全等价的位只保留一位，减少冗余。

产出 `bitSelectors`，用于运行期并行位提取。

### 3.2 前置哈希表构建

不使用 `L1/L2` 命名，统一为：

1. 一级哈希表：`PrimaryHashTable`
2. 二级哈希表：`SecondaryHashTable`

一级表项存放二级表偏移；二级表项承载规则向量化编码，建议字段：

- `ruleVector[32]`
- `tableControl[32]`
- `headMask`
- `tailMask`
- `ruleBase`
- `ruleCount`

### 3.3 规则向量化编码

针对候选冲突组进行批量编码：

1. 长度不一致：通过头尾掩码与长度元信息过滤。
2. 大小写不敏感：编码阶段清除大小写差异位。
3. 模糊匹配：通过掩码控制不关心位。

## 4. 运行期设计

### 4.1 主流程

每次迭代执行：

1. 按 `bitSelectors` 并行位提取得到哈希键。
2. 查询 `PrimaryHashTable` 得到二级偏移。
3. 读取 `SecondaryHashTable` 候选条目。
4. 对候选规则执行向量化批校验。
5. 头尾掩码与规则元信息二次过滤后回调上报。

### 4.2 与现有链路兼容

1. 保留 zone/flood/streaming 外围框架。
2. 对边界或未覆盖场景保留 confirm 回退路径。
3. 先保证正确性，再逐步放大 PBE 主路径覆盖。

## 5. 当前落地状态（Phase-0 + Phase-1）

已完成框架化改造：

1. 新增 PBE 编译期骨架：
   - `src/fdr/pbe_compile.h`
   - `src/fdr/pbe_compile.cpp`
2. 在 `fdrBuildProtoInternal` 中加入 PBE 选择挂点。
3. 新增运行期入口 `KHSEL_PbeEngineExec`，并完成 `engineID=2` 分派。
4. CMake 已加入 PBE 编译期源码。

当前行为说明：

1. PBE 编译期构建函数为占位实现，接口与数据结构已固定。
2. `KHSEL_PbeEngineExec` 先复用 Neo 路径，保证可编译、可运行、可回退。

## 6. Phase-1 已实现内容（2026-03-23）

已在 `src/fdr/pbe_compile.cpp` 实现以下逻辑：

1. 修复 `canBuildPBE(...)` 架构判断问题：
   - 去除不存在的 `target.has_neon()` 调用。
   - 改为 `__aarch64__` 条件启用，非 Arm64 平台直接返回 `false`。
   - 补充 `#include "util/verify_types.h"`，修复 `verify_u32/verify_u8` 编译缺失头文件问题。
2. 位提取码选择（编译期）：
   - 候选位空间：末尾 `8` 字节共 `64` bit。
   - 结合 `nocase` 与 `msk/cmp` 语义计算 bit 的 `0/1/X` 状态。
   - 使用“关心位比例 + 熵”评分，落实原则 1/2。
   - 使用列特征签名去重，落实原则 3。
   - 默认选择最多 `12` 位作为哈希键。
3. 一级/二级哈希表生成：
   - 一级哈希表 `PrimaryHashTable.offsets`：大小 `2^keyBits`。
   - 二级哈希表 `SecondaryHashTable`：第 `0` 项保留为空项。
   - 每个 key 聚合候选规则，写入 `ruleVector/tableControl/headMask/tailMask/ruleBase/ruleCount`。
4. 不完全指定位（X）规则处理增强：
   - 从“直接回退”升级为“多 key 展开（最多 64 个 key/规则）后入一级哈希表”。
   - 降低纯回退规则比例，为后续运行期向量化校验提供更高覆盖率输入。

当前 Phase-1 限制：

1. X 位数量过多时会触发展开上限裁剪（64 个 key/规则），剩余语义由回退路径兜底。
2. 运行期 `KHSEL_PbeEngineExec` 仍复用 Neo 路径，尚未消费 PBE 表。
3. 二级表中的 `ruleVector/tableControl` 目前为可运行的过渡编码，后续会替换为最终向量校验编码格式。

## 7. 下一阶段实现计划（Phase-2）

1. 将 Phase-1 产出的一级/二级哈希表布局写入最终 bytecode，并在运行期可直接访问。
2. 为不完全指定位（X）规则加入多 key 展开或专用回退桶，减少回退比例。
3. 实现冲突组向量化批校验，替换链式确认主路径。
4. 进行 ARM 平台性能调优（冲突率、访存局部性、向量指令展开）。
