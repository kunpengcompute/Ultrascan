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
   - 仅在检测到 `SVEBITPERM` 可用时，才允许启用 `bext` 快路径
   - 当前阶段先补 `SVE/SVE2/SVEBITPERM` 能力探测与门控
   - 后续再使用 `bext` 指令实现并行位提取
   - 配合位图命中结果做 `compact`
   - 对非零项做 `LD1/GATHER` 访问一级哈希表

### SVE2/SVEBITPERM 门控补充

1. `bext` 快路径不是默认开启功能。
2. 当前设计要求同时满足以下条件时，未来才允许进入 `bext` 位提取路径：
   - 工程构建系统检测到具备 `SVEBITPERM` 构建能力
   - 运行目标平台声明支持 `SVEBITPERM`
3. 若任一条件不满足，则必须回退到当前标量位提取实现。
4. 本阶段只补充能力位、平台探测和门控框架，不改变现有 PBE 运行语义。
5. 需要区分三层概念：
   - `HAVE_SVE / HAVE_SVE2 / HAVE_SVEBITPERM`：当前翻译单元是否按对应 ISA 编译
   - `HS_BUILD_HAVE_SVE / HS_BUILD_HAVE_SVE2 / HS_BUILD_HAVE_SVEBITPERM`：当前工程是否具备构建对应专用实现的能力
   - `target.has_sve() / target.has_sve2() / target.has_sve_bitperm()`：运行目标平台是否声明支持对应能力
6. 当前阶段新增：
   - `pbeHasSveBitPermPrereq(...)`：表示未来进入 `bext` 快路径的前置条件是否成立
   - `pbeCanUseBextFastPath(...)`：当前根据 `SVEBITPERM` 构建能力和目标能力决定是否允许快路径

### 结构约束补充

1. 一级哈希压缩位图必须独立于一级哈希主表放置。
2. 位图不能作为 `PBEPrimaryHashTable` 的内嵌成员使用。
3. 编译期、blob、运行期都应将两者视为两块独立区域：
   - 一级哈希主表：保存 `u32` 编码值
   - 一级哈希压缩位图：保存表项是否非空

### SVEBITPERM 与 bext 提取补充

1. `bext` 快路径的真实前置条件已经从“仅检测 `SVE2`”修正为“检测 `SVEBITPERM`”。
2. 现在需要区分四层语义：
   - `HAVE_SVE / HAVE_SVE2 / HAVE_SVEBITPERM`：当前翻译单元是否按对应 ISA 编译
   - `HS_BUILD_HAVE_SVE / HS_BUILD_HAVE_SVE2 / HS_BUILD_HAVE_SVEBITPERM`：当前工程是否具备构建对应专用实现的能力
   - `HS_CPU_FEATURES_SVE / HS_CPU_FEATURES_SVE2 / HS_CPU_FEATURES_SVEBITPERM`：运行目标 CPU 能力
   - `target.has_sve() / has_sve2() / has_sve_bitperm()`：编译期目标抽象
3. `pbeHasSveBitPermPrereq(...)` 用来表达“工程已具备 bitperm 构建能力，且目标平台声明支持 bitperm”这一前置条件。
4. `pbeCanUseBextFastPath(...)` 现在不再固定返回 `false`，而是跟随 `SVEBITPERM` 前置条件决定未来快路径是否允许启用。
5. PBE blob 头部新增了 `bext` 提取描述：
   - `extractMode`
   - `windowBytes`
   - `bextMask`
   - `bextToKeyBit[32]`
6. 编译期会把当前 `22` 个 selector 转换成：
   - 一个 `64-bit bextMask`
   - 一个“压缩后 bit 位 -> 原 key bit 位”的重排表
7. 运行期提取流程现在改成：
   - 先从当前位置装载 `8-byte window`
   - 若 `extractMode=scalar`，则按 selector 标量提取
   - 若 `extractMode=bext`，则先执行 packed bit extract，再按 `bextToKeyBit` 重排成最终 `22-bit key`
8. 当前保留两条实现路径：
   - `SVEBITPERM` 可用时，调用专用 helper
   - 否则回退到软件 `bext` 打包实现
9. 这里的 `window64` 采用“原始字节”拼接，不做统一大写归一化；大小写不敏感规则仍通过 `keyMask` 中的“不关心位”来消解差异。

### `window64 -> bextMask -> key` 详细流程图

下面给出当前 PBE 位提取路径的详细数据流。这里的核心目标是：把“当前候选结束位置附近最多 `8` 个字节”先压成一个 `64-bit window64`，再基于 selector 定义提取出最终的 `22-bit key`。

#### 1. 运行期输入视图

```text
history(可选) + current buffer

... h[-3]  h[-2]  h[-1] | b[0]  b[1]  b[2]  b[3]  b[4] ...
                         ^
                    当前扫描位置

假设当前候选结束位置为 endPos
PBE 总是以 endPos 为“后缀末尾”来观察最多 8 个字节
```

#### 2. 构造 `window64`

`window64` 的构造规则是：

1. 以 `endPos` 对应字节作为最低字节 `byte0`
2. `endPos - 1` 对应 `byte1`
3. `endPos - 2` 对应 `byte2`
4. 依次向前，最多取 `8` 个字节
5. 若超出 `history + current` 的可访问范围，则对应字节补 `0`

对应位布局如下：

```text
window64 = [ byte7 | byte6 | byte5 | byte4 | byte3 | byte2 | byte1 | byte0 ]

其中：
byte0 = endPos
byte1 = endPos - 1
byte2 = endPos - 2
...
byte7 = endPos - 7
```

位编号方式：

```text
byte0 的 bit0..bit7  -> window64 的 bit0..bit7
byte1 的 bit0..bit7  -> window64 的 bit8..bit15
byte2 的 bit0..bit7  -> window64 的 bit16..bit23
...
byte7 的 bit0..bit7  -> window64 的 bit56..bit63
```

也就是说：

```text
windowBitIndex = byteOffset * 8 + bitOffset
```

这正好与当前 selector 的定义一致。

#### 3. selector 与 `window64` 的关系

编译期 selector 形式如下：

```text
selector[i] = { byteOffset, bitOffset }
```

含义：

```text
byteOffset = 0  -> 取 endPos 这个字节
byteOffset = 1  -> 取 endPos-1 这个字节
...

bitOffset = 0..7 -> 取该字节中的具体 bit
```

因此一个 selector 实际上就是 `window64` 上的一个固定 bit 位置：

```text
selector[i] 对应的源 bit 位置
= selector[i].byteOffset * 8 + selector[i].bitOffset
```

#### 4. 编译期生成 `bextMask`

编译期会把全部 selector 变成一个 `64-bit bextMask`。

流程如下：

```text
selector 列表（按当前 key bit 顺序）
    |
    | 取出每个 selector 对应的 windowBitIndex
    v
[(windowBitIndex0, keyBit0),
 (windowBitIndex1, keyBit1),
 ...]
    |
    | 按 windowBitIndex 从小到大排序
    v
[(srcBitA, keyBitX),
 (srcBitB, keyBitY),
 ...]
    |
    | 生成两份结果
    v
1. bextMask：在 srcBitA/srcBitB/... 这些位置上置 1
2. bextToKeyBit：记录“压缩后第 j 位”应该放回哪个 keyBit
```

图示如下：

```text
原 selector 顺序（决定最终 key 的 bit 位顺序）

  selector[0] -> srcBit = 11
  selector[1] -> srcBit = 12
  selector[2] -> srcBit = 16
  selector[3] -> srcBit = 26
  ...

按 srcBit 升序排序后：

  packedBit[0] <- srcBit 11 -> keyBit 0
  packedBit[1] <- srcBit 12 -> keyBit 1
  packedBit[2] <- srcBit 16 -> keyBit 2
  packedBit[3] <- srcBit 26 -> keyBit 3
  ...

于是：

  bextMask     : 在 bit11/12/16/26/... 上置 1
  bextToKeyBit : [0, 1, 2, 3, ...]
```

注意：如果 selector 原始顺序和源 bit 升序不一致，那么 `bextToKeyBit` 就会显式记录这种“压缩顺序”和“最终 key 顺序”之间的差异。

#### 5. 运行期提取总流程

完整流程如下：

```text
                 +----------------------+
                 | 当前候选结束位置 endPos |
                 +----------+-----------+
                            |
                            v
                 +----------------------+
                 | 从 history+buf 装载   |
                 | 最多 8 字节 -> window64|
                 +----------+-----------+
                            |
                            v
                 +----------------------+
                 | 检查 extractMode      |
                 +----+-------------+---+
                      |             |
         scalar       |             | bext
                      |             |
                      v             v
      +----------------------+   +----------------------+
      | 按 selector[i]       |   | packed = bext(window,|
      | 逐个读取 window bit  |   |              bextMask)|
      +----------+-----------+   +----------+-----------+
                 |                          |
                 |                          v
                 |               +----------------------+
                 |               | packed bit 按       |
                 |               | bextToKeyBit 重排    |
                 |               +----------+-----------+
                 |                          |
                 +-------------+------------+
                               |
                               v
                    +-----------------------+
                    | 最终得到 22-bit key   |
                    +-----------+-----------+
                                |
                                v
                    +-----------------------+
                    | 先查 L1 bitmap        |
                    +-----------+-----------+
                                |
                                v
                    +-----------------------+
                    | 再查 L1 value         |
                    | 解码出 offset/count   |
                    +-----------+-----------+
                                |
                                v
                    +-----------------------+
                    | 扫描对应 L2 项        |
                    +-----------------------+
```

#### 6. `bext` 路径与 scalar 路径为什么要保持两套

当前保留两套路径的原因有三个：

1. 正确性对照
   - `bext` 路径必须和现有 scalar 提取逐位置完全一致
2. 平台回退
   - 没有 `SVEBITPERM` 时，仍可使用软件 packed-extract 回退
3. 调试可观测
   - UT 可以直接比较：
     - `window64`
     - `scalar key`
     - `bext packed`
     - `重排后 key`

#### 7. 具体小例子

假设当前末尾 4 字节为：

```text
endPos-3   endPos-2   endPos-1   endPos
   'A'        'B'        'C'       'D'
```

那么：

```text
window64 的低 4 个字节为：

byte0 = 'D'
byte1 = 'C'
byte2 = 'B'
byte3 = 'A'
byte4..byte7 = 0
```

如果 selector 为：

```text
s0 = {byteOffset=0, bitOffset=0}
s1 = {byteOffset=1, bitOffset=2}
s2 = {byteOffset=3, bitOffset=6}
```

则对应源 bit 为：

```text
s0 -> srcBit 0
s1 -> srcBit 10
s2 -> srcBit 30
```

编译期会得到：

```text
bextMask = (1<<0) | (1<<10) | (1<<30)
bextToKeyBit = [0, 1, 2]
```

运行期：

```text
packed = bext(window64, bextMask)
key    = remap(packed, bextToKeyBit)
```

如果将来 selector 的逻辑顺序与 `srcBit` 升序不同，例如：

```text
selector 顺序: [srcBit30, srcBit0, srcBit10]
```

那么：

```text
bext 后 packed 顺序仍然是 [srcBit0, srcBit10, srcBit30]

此时：
bextToKeyBit = [1, 2, 0]
```

运行期就需要额外重排一次，才能恢复成当前 PBE 定义下的最终 key 顺序。

#### 8. 当前语义注意点

1. `window64` 现在使用的是原始字节，不做统一大写。
2. 原因是当前编译期 `keyValue/keyMask` 的大小写不敏感语义是通过“不关心位”表达的。
3. 如果运行期提前把字节统一转大写，会破坏当前 `key/keyMask` 兼容关系。
4. 因此当前正确口径是：
   - `window64` 保留原始 bit
   - `nocase` 由 `keyMask` 和后续 exact confirm 共同处理

### Batch4 前端化阶段

在完成 `window64 + bext` 提取描述之后，运行期前端继续推进到 `Batch4` 批量化阶段。

#### 1. 当前批量化范围

本阶段只处理前端：

1. 批量 key 提取
2. 批量 bitmap 判空
3. `compact`
4. 批量 L1 value 访问

本阶段**不修改**以下语义：

1. L2 扫描方式
2. L2 suffix 预检查方式
3. exact confirm 方式

#### 2. `Batch4` 数据流

```text
for 每 4 个相邻 endPos:

    [pos0, pos1, pos2, pos3]
             |
             v
    构造 4 个 window64
             |
             v
    提取 4 个 key
             |
             v
    先查 4 个 bitmap 位
             |
             v
    生成 activeMask
             |
             v
    compact 非空 lane
             |
             v
    仅对 compact 后的 lane 访问 L1 / L2
```

#### 3. 当前实现特点

1. 目前 `Batch4` 仍然是“批量组织 + 标量执行”的第一阶段版本。
2. 也就是说：
   - 已经按 4 个位置一组组织运行期前端
   - 但尚未把 `window64` 构造本身做成真正的向量装载
   - 也尚未把 L1 访问做成真正的硬件 gather
3. 当前 `compact` 的作用主要是：
   - 跳过 bitmap 判空后的空 lane
   - 减少无意义的 L1 / L2 访问
4. 这一步的目标是先把“前端批量数据流”固定下来，为下一阶段真正的向量化做准备。

#### 4. 与后续阶段的边界

当前状态：

1. `window64` 已经建立
2. `bextMask` / `bextToKeyBit` 已经建立
3. `Batch4 + bitmap + compact` 已经建立

尚未完成：

1. 多位置并行 `window64` 装载
2. 多位置真正硬件 `bext` 向量化提取
3. `compact` 后的 packed load / gather 优化
4. L2 `ruleVector/tableControl` 向量化预筛
5. confirm 向量化

### Batch4 第二阶段：compact 后的 packed L1 load

在 `Batch4 + bitmap + compact` 的第一阶段基础上，运行期前端继续推进到第二阶段。当前这一步的目标不是修改匹配语义，而是把 `Batch4` 主循环内部的数据流进一步拆清楚，便于后续继续接 L2 向量化预筛。

#### 1. 本阶段新增的内部步骤

当前 `pbeRunBatch4(...)` 已经拆成以下几步：

1. `pbeBuildBatch4(...)`
   - 构造一批最多 `4` 个相邻 `endPos`
   - 为每个位置提取 `key`
   - 计算对应的一级哈希索引 `primaryIdx`
2. `pbeBatchBitmapMask(...)`
   - 针对这一批 `primaryIdx`
   - 先查询一级哈希压缩位图
   - 生成 `activeMask`
3. `pbeCompactPrimaryLanes(...)`
   - 只保留位图命中的 lane
   - 把对应 `primaryIdx` 压缩成连续数组
4. `pbeBatchLoadPrimaryValues(...)`
   - 对 compact 后的 `primaryIdx` 做 software gather
   - 把一级哈希值装入连续数组
   - 同时再按原始 lane 顺序回填到 `encodedByLane`

#### 2. 当前 packed L1 load 的实现口径

这里的 “packed L1 load” 目前采用的是：

1. 先 `compact`
2. 再对非空 lane 做 software gather
3. 把 gather 结果组织成连续的小数组

当前还没有使用真正的硬件 gather 指令。这样做的原因是：

1. 可以先稳定 `Batch4` 的前端组织方式
2. 可以明显减少空 lane 的一级哈希访问
3. 不会破坏当前已经通过的回调顺序与匹配语义

#### 3. 为什么仍然要按原始 lane 顺序回放

虽然一级哈希访问已经变成 “compact 后集中处理”，但真正进入 `L2` 和回调时，仍然必须恢复为原始 lane 顺序，并保持：

1. 同一位置 `exact` 先于 `wildcard`
2. 全局 `offset` 单调不下降

这是为了满足 Rose 运行期关于回调顺序的约束，避免出现：

1. 较晚位置的 `exact`
2. 早于较早位置的 `wildcard`

从而触发最小匹配偏移的断言问题。

#### 4. 本阶段完成后，前端已经具备的能力

当前前端已经具备：

1. `window64`
2. `bextMask / bextToKeyBit`
3. `Batch4`
4. 一级哈希位图判空
5. `compact`
6. compact 后的 packed L1 load
7. 保序的 exact/wildcard 回放

#### 5. 仍未完成的部分

本阶段之后，尚未完成的是：

1. 多位置真正向量化 `window64` 装载
2. 多位置真正硬件 `bext` 并行提取
3. `L2 ruleVector/tableControl` 向量化预筛
4. exact confirm 向量化
