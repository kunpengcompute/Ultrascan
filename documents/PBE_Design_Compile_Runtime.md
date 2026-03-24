# PBE 设计与实现记录（编译期/运行期）

## 1. 目标
1. 先打通 PBE 全链路（编译期产物 -> 运行期执行），优先正确性。
2. 在不影响原有语义的前提下逐步替换 Neo/FDR 的部分路径。
3. 性能优化放在框架打通之后。

## 2. 当前选择链路
1. Teddy 优先。
2. Teddy 未选中后，判断是否走 PBE。
3. 之后再尝试 NeoFdr / Fdr。

灰度开关：
1. `allowPbe=0`（默认）：不尝试 PBE。
2. `allowPbe=1`：在 Teddy 后尝试 PBE。

## 3. 阶段A（已完成）
1. 编译期生成并序列化 PBE blob：
   - bit selectors
   - primary hash table
   - secondary hash table
   - rule meta
2. 运行期可读取并校验 blob 布局。
3. `struct FDR` 兼容性已修复：`pbeOffset/pbeSize` 放在结构体尾部，避免破坏既有 NeoFdr 字段偏移。

## 4. 阶段B（进行中）

### 4.1 已实现
1. 运行期 `PbeEngineExec` 支持朴素匹配路径（不再仅仅探测）。
2. secondary entry 新增 `ruleIndex[32]`，候选槽位可稳定映射到 `ruleMeta`。
3. `ruleMeta` 扩展为可执行载荷：
   - `lit[8] + litOffset`
   - `maskLen/msk/cmp`
   - `id/groups/len/flags`
4. 新增 `literalBlob`：
   - 编译期存放归一化规则字节
   - 运行期按 `litOffset` 读取完整规则（支持 `len > 8`）
4. 运行期已接入：
   - groups 过滤
   - nocase 精确比较
   - noruns 抑制
   - callback 调用与终止返回
5. 保守回退策略：
   - `PARTIAL_COVERAGE` 回退 Neo
   - `NEEDS_NEO_FALLBACK` 回退 Neo（当前用于后续未覆盖语义保守兜底）
6. history 语义：
   - 运行期按 `buf_history/len_history` 执行跨边界字节读取
   - streaming/history 场景可走 PBE 朴素路径（在非回退标记下）

### 4.2 本次调整
1. 删除了先前实现的 PBE 运行期命中统计开关与相关计数逻辑，避免性能影响。
2. 当前 `PbeEngineExec` 保持纯执行路径，不再包含统计打印分支。
3. `msk/cmp` 已并入朴素精确匹配，不再作为默认回退条件。
4. `len > 8` 已通过 `literalBlob` 支持，不再作为默认回退条件。

## 5. 下一步（阶段B续）
1. 梳理并收敛 `NEEDS_NEO_FALLBACK` 的触发来源，缩小回退范围。
2. 建立 PBE vs Neo 一致性回归集（block + streaming + groups + noruns + mask/cmp）。
3. 在一致性稳定后再进入向量化替换，不提前做性能优化。

## 6. 增量更新约定
后续所有设计变更继续增量补充到本文档，不再另起新文档。

### 4.3 增量补充（2026-03-24）
1. 朴素路径支持 `len > 8` 规则：
   - 新增 runtime literal blob，RuleMeta 持有 `litOffset`。
   - 运行期按偏移读取完整规则字节做精确比较。
2. RuleMeta 与 runtime 布局新增：
   - `literalBlobOffset/literalBlobSize`
   - `litOffset`
3. `msk/cmp` 语义仍由朴素路径执行：
   - 在精确比较后追加 `(byte & msk) == cmp` 校验。
4. 当前保守回退条件继续保留：
   - `PARTIAL_COVERAGE` 或 `NEEDS_NEO_FALLBACK` 仍回退 Neo。

## 7. 从 7ebbd253 开始的实现梳理（详细）
基线提交：`7ebbd2533fa1007b1bc4c9d74ea5f45679f50395`（搭建 PBE 框架）。

### 7.1 提交时间线与核心变化
1. `0a82a44`（实现位提取码及哈希表构建）
   - 完成位提取码选择（默认 16 bit）。
   - 完成一级/二级哈希表编译期构建。
   - 支持 X 位多 key 展开（上限 64）并标记 partial 覆盖。
2. `dc6d4d3`（将构建出的哈希表写入 bytecode 布局）
   - 新增 PBE runtime blob 序列化与运行期读取。
   - `engineID=2` 分派到 `PbeEngineExec`。
   - PBE 运行时代码从 `kunpeng-enhanced` 迁移到 `src/fdr/pbe_engine.c`（纯 C）。
3. `959c8e3`（增加运行期前置筛选）
   - 增加布局校验、key 提取、二级候选前置判断。
4. `3199544`（增加 Pbe 开关，修复匹配数不一致）
   - 新增 `allowPbe` 灰度开关。
   - 修复 `struct FDR` 布局兼容（`pbeOffset/pbeSize` 放尾部）。
5. `9d62985`（增加规则元信息）
   - 引入 `ruleMeta` 编译期保存与运行期读取。
6. `605b2c9`（新增运行期朴素匹配）
   - 打通 PBE 朴素匹配闭环：候选 -> 精确比较 -> 回调。
   - 接入 history、nocase、noruns、groups、msk/cmp。
   - 仍保留回退 Neo 的保守分支。
7. `605b2c9` 之后的增量（当前工作区）
   - 删除运行期统计开关（避免性能噪音）。
   - 增加 `literalBlob + litOffset`，支持 `len > 8` 全量字节比较。
   - 继续完善文档与阶段化计划。

### 7.2 关键问题与修复
1. 强制 Neo 时匹配数不一致：
   - 原因：`struct FDR` 头部布局变更破坏了 Neo 运行时旧偏移假设。
   - 修复：`pbeOffset/pbeSize` 移到结构体尾部，恢复兼容。
2. 运行期头文件/类型不一致：
   - 通过统一声明依赖与运行时代码迁移到 `src/fdr` 解决。
3. 前置早退导致风险：
   - 早退逻辑已关闭，避免测试阶段误差。

## 8. 与最初目标的差距评估
最初目标：前端前置 hash + 后端冲突向量化校验 + 完整语义覆盖（长度差异、模糊匹配、大小写不敏感）。

当前达成度（粗略）：
1. 前端（编译期构建）约 75%
   - 位提取与双级哈希构建已完成。
   - 覆盖标记与回退机制已建立。
2. 运行期功能打通约 65%
   - 朴素匹配闭环已可执行（含 history、nocase、noruns、groups、msk/cmp）。
   - 仍有保守回退路径未完全收敛。
3. 向量化目标约 10%
   - 目前仍为朴素路径，尚未替换为向量化冲突校验主路径。
4. 工程化稳定性约 70%
   - 结构体兼容问题已修复。
   - 需要系统化一致性回归来锁定行为边界。

## 9. 下一步实现规划（重排）
### 9.1 P0：正确性封口（优先）
1. 明确 `NEEDS_NEO_FALLBACK` 的触发条件清单并最小化。
2. 建立一致性矩阵：
   - block/streaming
   - nocase/case
   - groups 切换
   - noruns
   - msk/cmp
   - 长短规则混合
3. 目标：PBE 与 Neo 在允许覆盖范围内 100% 对齐。

### 9.2 P1：回退范围收敛
1. 将目前保守回退条件按条目拆分，逐项转为 PBE 原生处理。
2. 保留最后兜底回退，但尽量不在热路径触发。

### 9.3 P2：后端向量化替换
1. 在不改接口前提下引入向量化候选校验实现（先并行存在）。
2. 通过灰度开关切换朴素/向量化路径做 A/B 正确性验证。
3. 验证通过后再默认启用向量化路径。

### 9.4 P3：性能与工程收尾
1. 再做 profile 驱动优化（访存局部性、预取、分支裁剪）。
2. 清理临时兼容逻辑与冗余回退分支。
3. 完成阶段验收与回归基线固化。

### 4.4 增量补充（2026-03-24）
1. 将 `NEEDS_NEO_FALLBACK` 触发条件显式化并在编译期统一打标：
   - `msk/cmp` 仅一侧存在
   - `msk.size != cmp.size`
   - `msk.size > literal.size`
2. 运行期保守回退条件与编译期标记严格对齐，便于后续逐项收敛回退范围。
