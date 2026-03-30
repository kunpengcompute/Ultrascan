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

### 4.5 增量补充（2026-03-25）
1. 放宽 `msk/cmp` 朴素路径约束：支持 `maskLen > literalLen` 的 overhang 场景。
2. 编译期不再因 `msk.size() > literal.size()` 打 `NEEDS_NEO_FALLBACK`。
3. 运行期 `msk/cmp` 校验仍锚定在匹配结束位置的尾窗口，允许历史区参与比较。

### 4.6 增量补充（2026-03-25）
1. 增加 `msk/cmp` 编译期归一化（normalize）规则：
   - 支持 msk-only（cmp 默认 0）
   - 支持 msk/cmp 等长
   - 支持 msk 长于 cmp（缺失 cmp 按 0 填充）
2. 仍保守回退的场景：
   - cmp-only
   - cmp 长于 msk
3. 归一化后，位提取建模与 RuleMeta 载荷使用同一语义，避免编译期/运行期约束不一致。

### 4.7 增量补充（2026-03-25）
1. 扩展 `msk/cmp` 归一化：
   - 新增支持 cmp-only（msk 缺省按 0xff）
   - 新增支持 cmp 长于 msk（缺失 msk 按 0xff）
2. 归一化后统一落到 `(byte & msk) == cmp` 语义执行。
3. 该改动进一步缩小 `NEEDS_NEO_FALLBACK` 触发面。

### 4.8 增量补充（2026-03-25）
1. 移除 `NEEDS_NEO_FALLBACK` 主流程依赖：
   - 编译期不再额外打该标记
   - 运行期回退条件收敛为 `PARTIAL_COVERAGE`（及布局校验失败）
2. 当前可覆盖语义（朴素路径）包括：
   - history 跨边界读取
   - nocase
   - noruns
   - groups
   - msk/cmp（含归一化场景）
   - 长度大于 8 的规则（literal blob）

### 4.9 增量补充（2026-03-25）
1. 进入 P0 一致性封口，新增最小回归集：`PBE vs Neo`。
2. 用例文件：
   - `unit/internal/pbe_vs_neo.cpp`
3. 覆盖维度（最小闭环）：
   - block + groups：验证 group mask 对候选结果的一致性过滤
   - block + noruns：验证 run 抑制语义一致
   - streaming + mask：验证跨 history 的匹配及 msk/cmp 一致性
   - block + mask+nocase：验证掩码与大小写无关组合语义一致
4. 构建策略：
   - 显式关闭 Teddy（`fdrAllowTeddy=0`）
   - 打开 `allowNeoFdr=1` 与 `allowPbe=1`
   - 分别用 hint=1（Neo）和 hint=2（PBE）构建并对比输出
5. 平台策略：
   - 非 Arm64 或当前环境无法构建 PBE 时，该组用例自动跳过（不影响现有主线）。

### 4.10 增量补充（2026-03-26）
1. 新增 `PARTIAL_COVERAGE` 回退一致性用例：
   - 在 `unit/internal/pbe_vs_neo.cpp` 增加 `PartialCoverageFallbackConsistency`。
2. 用例构造方式：
   - 构造 40 条同 key 规则，触发二级表 `ruleCount` 截断。
   - 编译期应写入 `PBE_RUNTIME_FLAG_PARTIAL_COVERAGE`。
3. 断言点：
   - 运行期读取 PBE header，确认 `PARTIAL_COVERAGE` 标记存在。
   - 同一输入下，`PBE` 与 `Neo` 输出完全一致（证明回退路径生效）。

### 4.11 增量补充（2026-03-26）
1. 修复 `BlockNorunsConsistency` 暴露的语义偏差：
   - 原 PBE 朴素路径使用全局 `lastMatchId` 做 `noruns` 抑制，会误抑制相邻位置合法匹配。
2. 当前策略（P0 一致性优先）：
   - 暂时移除 PBE 运行期 `noruns` 抑制逻辑，先保证 `PBE vs Neo` 结果对齐。
3. 后续计划：
   - 在 P1 阶段按与 Neo 一致的规则重建 `noruns` 语义（而非全局 last-id 近似）。

### 4.12 增量补充（2026-03-26）
1. 目标对齐调整：当命中 PBE 条件后，运行期不再以 Neo 作为兜底执行路径。
2. 编译期策略变更：
   - `buildPBEArtifacts` 在出现 `PBE_ARTIFACT_FLAG_PARTIAL_COVERAGE` 时直接返回失败。
   - 含 partial 覆盖风险的规则集不再进入 PBE 引擎分支（即“不符合 PBE 条件”）。
   - `fdrBuildTableInternal` 对 `engineID=2` 增加硬校验：若未生成有效 `pbeBlob` 则直接构建失败，避免生成“空 PBE”运行时对象。
3. 运行期策略变更：
   - `PbeEngineExec` 移除到 `KHSEL_NeoFdrEngineExec` 的回退调用，保持纯 PBE 执行路径。
4. 回归用例调整：
   - 将 `PartialCoverageFallbackConsistency` 改为 `PartialCoverageRejectedByPbeBuild`。
   - 校验 partial 覆盖场景下 Neo 可构建而 PBE 构建失败（符合新的 PBE 条件定义）。
## 增量更新：固定 22 位 L1 与 4 规则 L2 项

当前设计目标已经调整为以下固定策略：

1. 位提取位数固定为 `22` 位，由宏 `PBE_KEY_BITS` 控制。
2. 一级哈希表 L1 的项数固定为 `2^22`。
3. L1 的 key 为位提取结果拼出的 22 位新数值。
4. L1 的 value 为一个 `u32` 编码值：
   - 低 18 位：二级哈希表 L2 的起始偏移
   - 高位：该 L1 key 对应的连续 L2 项数
5. 二级哈希表 L2 每一项固定承载 `4` 条冲突规则，结构固定为：
   - `32B ruleVector`
   - `32B TBL control`
   - `32b headMask`
   - `32b tailMask`
6. 如果某个 L1 key 的冲突规则数超过 4 条，则顺序分配多个连续 L2 项，
   并把项数编码回 L1 value 的高位。

### 编译期实现

1. 选择最多 22 个位提取选择器，最终 `keyBits` 固定为 22。
2. 根据规则的 `keyValue/keyMask` 进行 L1 分桶。
3. 每个桶内按 `4` 条规则一组切分为多个连续 L2 项。
4. 同时构建一级哈希压缩位图：
   - 每个 L1 表项对应 1 bit
   - bit=1 表示该 L1 key 非空
   - bit=0 表示该 L1 key 为空
5. 当前编译期会把压缩位图与 L1 主表作为两块独立区域分别写入 PBE blob。
6. 每个 L2 项内：
   - `ruleVector` 保存每条规则最后最多 8 字节的归一化后缀
   - `tableControl` 标记该 8 字节窗口中哪些字节有效
   - `headMask/tailMask` 以 32 bit 表示整个 32B lane 的有效位分布
7. L1 value 通过 `(count << 18) | offset` 编码。

### 运行期实现

1. 运行期先提取 22 位 key。
2. 在读取 L1 主表之前，先查询一级哈希压缩位图。
3. 如果位图显示该 key 为空，则直接跳过该次 L1 访问。
4. 如果位图命中，再从 L1 读取编码值，并解码出：
   - `secondaryOffset`
   - `entryCount`
5. 依次遍历 `[secondaryOffset, secondaryOffset + entryCount)` 范围内的 L2 项。
6. 每个 L2 项内部先利用 `ruleVector + tableControl` 做 suffix 预检查。
7. 预检查通过后，再结合 `keyValue/keyMask` 与 `ruleMeta` 做精确确认。

### 当前状态说明

1. 当前实现目标是先把固定 22 位 L1、4 规则 L2、多项连续扫描这条主链路打通。
2. 当前已经补充了一级哈希压缩位图的编译期与运行期框架。
3. 运行期仍然是朴素 C 版本，后续再继续做向量化与性能优化。
4. 下一步位提取码优化计划：
   - 使用 `bext` 指令实现并行位提取
   - 配合位图命中结果做 `compact`
   - 对非零项做 `LD1/GATHER` 访问一级哈希表

### 结构约束补充

1. 一级哈希压缩位图必须独立于一级哈希主表放置。
2. 位图不能作为 `PBEPrimaryHashTable` 的内嵌成员使用。
3. 编译期、blob、运行期都应将两者视为两块独立区域：
   - 一级哈希主表：保存 `u32` 编码值
   - 一级哈希压缩位图：保存表项是否非空
