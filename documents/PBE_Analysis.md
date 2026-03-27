**结论**

现在 `allowPbe` 打开后性能差这么大，核心原因不是“PBE 思想本身慢”，而是**当前代码还是一个功能打通版的朴素实现**，还没有把你目标里的“前端并行位提取粗筛 + 后端向量化冲突校验”真正落下来。  
从你这组数据看：

- PBE：`17,312,841` bytes DB，`42.86 Mbit/sec`
- NeoFdr：`83,977` bytes DB，`1,319.93 Mbit/sec`

这已经很说明问题了：**当前 PBE 的编译期把结构做得很大，运行期却还是按字节、按候选、按规则线性扫。**

---

**编译期全流程**

当前 PBE 编译期主链路在 [fdr_compile.cpp](d:\work\2026\Hyperscan\code\hyperscan\src\fdr\fdr_compile.cpp) 和 [pbe_compile.cpp](d:\work\2026\Hyperscan\code\hyperscan\src\fdr\pbe_compile.cpp)。

1. 引擎选择  
在 [fdr_compile.cpp:876](d:\work\2026\Hyperscan\code\hyperscan\src\fdr\fdr_compile.cpp:876) 之后，PBE 会先于 Neo 做一次可行性分析。  
现在这一步已经放到 `addIncludedInfo(...)` 之后了，所以判定口径比之前一致。

2. 位提取选择  
在 [pbe_compile.cpp:357](d:\work\2026\Hyperscan\code\hyperscan\src\fdr\pbe_compile.cpp:357) 的 `selectBitSelectors(...)` 里：
- 候选位来自后缀 8 字节，共 64 bit。
- 默认目标位数是 [pbe_compile.cpp:29](d:\work\2026\Hyperscan\code\hyperscan\src\fdr\pbe_compile.cpp:29) 的 `PBE_DEFAULT_KEY_BITS = 22`。
- 按 care ratio + entropy 打分，选最多 22 位。

这里第一个大问题是：**22 位是硬编码默认值，没有结合规则数、冲突率、L1 大小做成本约束。**

3. L1/L2 构建  
在 [pbe_compile.cpp:408](d:\work\2026\Hyperscan\code\hyperscan\src\fdr\pbe_compile.cpp:408) 的 `buildHashTables(...)`：
- L1 大小直接是 `1 << keyBits`，见 [pbe_compile.cpp:421](d:\work\2026\Hyperscan\code\hyperscan\src\fdr\pbe_compile.cpp:421)。
- 如果 `keyBits=22`，那就是 `4,194,304` 个槽。
- 每个槽一个 `u32 offset`，**L1 单独就约 16 MB**。

这基本就解释了为什么你的 PBE 数据库从 `84 KB` 直接膨胀到 `17.3 MB`。

4. 单 key / wildcard 策略  
在 [pbe_compile.cpp:441](d:\work\2026\Hyperscan\code\hyperscan\src\fdr\pbe_compile.cpp:441)：
- `keyMask == fullMask` 的规则进精确桶。
- 否则一律进 `bucketKey=0` 的 wildcard 桶。

这一步虽然避免了 “X 位展开污染哈希表”，但副作用是：**模糊/短规则/带不关心位规则容易集中到一个桶，运行期候选数会上升。**

5. L2 entry 结构很重  
当前 L2 entry 在 [pbe_runtime.h:40](d:\work\2026\Hyperscan\code\hyperscan\src\fdr\pbe_runtime.h:40) 开始：
- 128 槽固定数组
- `ruleVector[128]`
- `tableControl[128]`
- `ruleIndex[128]`
- `keyValue[128]`
- `keyMask[128]`
- `headMask[4]`
- `tailMask[4]`

单个 entry 大约 **1.5 KB 级别**。这对功能是方便的，但对 cache 不友好。

---

**运行期全流程**

当前运行期在 [pbe_engine.c](d:\work\2026\Hyperscan\code\hyperscan\src\fdr\pbe_engine.c)。

1. 入口  
[PbeEngineExec](d:\work\2026\Hyperscan\code\hyperscan\src\fdr\pbe_engine.c:272) 只做 layout 校验，然后直接进 [pbeRunNaive](d:\work\2026\Hyperscan\code\hyperscan\src\fdr\pbe_engine.c:170)。

2. 每个字节都扫描  
`pbeRunNaive(...)` 的主循环在 [pbe_engine.c:193](d:\work\2026\Hyperscan\code\hyperscan\src\fdr\pbe_engine.c:193)：
- 对 `buf` 的每个位置 `i`
- 先算一次 key
- 查一次 exact 桶
- 再查一次 wildcard 桶

这是**逐字节、逐位置**的标量扫描，不是 Teddy/Neo 那种批量并行前端。

3. key 提取也是标量  
[pbeExtractKey](d:\work\2026\Hyperscan\code\hyperscan\src\fdr\pbe_engine.c:251)：
- 对每个 selector 循环
- 逐位读历史/当前字节
- 逐位拼 key

这和你目标里的“并行位提取”不是一回事，当前只是**软件标量版 bit gather**。

4. 候选筛选很弱  
[pbeEntryMayMatchAtPos](d:\work\2026\Hyperscan\code\hyperscan\src\fdr\pbe_engine.c:142) 只是：
- 线性扫 `entry->ruleCount`
- 比较尾字节 `ruleVector[i]`

也就是说，当前所谓前端筛选，本质还是**线性尾字节比较**，没有向量化、没有真正使用 `headMask/tailMask`。

5. 后端校验完全朴素  
在 [pbe_engine.c:203](d:\work\2026\Hyperscan\code\hyperscan\src\fdr\pbe_engine.c:203) 的 `PBE_RUN_ENTRY` 宏里：
- 再线性扫一遍候选规则
- 做 `((key ^ kv) & km) != 0` 过滤
- 再做 groups 判断
- 最后进 [pbeRuleExactMatch](d:\work\2026\Hyperscan\code\hyperscan\src\fdr\pbe_engine.c:47)

而 `pbeRuleExactMatch(...)` 是：
- 按字节读历史/主缓冲
- 逐字节比较
- mask/cmp 逐字节再比一次

这就是最直接的 O(候选数 × 规则长度) 朴素确认。

---

**为什么会慢这么多**

结合你这组结果，主要是这 6 个原因叠加：

1. **L1 过大，数据库膨胀严重**  
`keyBits=22` 直接带来约 16MB L1。  
这不仅拉高编译期 heap，也让运行期 L1 访问更容易 cache miss。

2. **运行期是逐字节标量扫描**  
Neo/FDR 的强项是 SIMD 前端粗筛；当前 PBE 没有这个前端，只有 `for (i=...)` 的朴素循环。

3. **每个位置可能做两次桶探测**  
exact 桶 + wildcard 桶，见 [pbe_engine.c:243](d:\work\2026\Hyperscan\code\hyperscan\src\fdr\pbe_engine.c:243)。

4. **候选校验是两层线性循环**  
先 `pbeEntryMayMatchAtPos` 线性扫一次，再 `PBE_RUN_ENTRY` 里线性扫一次。

5. **最终确认完全没向量化**  
`pbeRuleExactMatch(...)` 是逐字节确认，这和你想要的“后端 hash 冲突向量化校验”差距很大。

6. **headMask/tailMask 现在几乎没发挥性能作用**  
虽然编译期写了 `headMask/tailMask`，但运行期主路径并没有用它们做真正的批量筛选。  
所以当前结构上像 PBE，执行上更像“带哈希桶的朴素确认器”。

---

**和你最初目标的差距**

你最初目标里最关键的 4 件事，当前只有第 1 件做了壳子：

1. 前端位提取 hash  
现状：有了，但还是标量位提取，且 `keyBits=22` 没成本模型。

2. 前端替代 shift-or 粗筛  
现状：没有真正做到，当前前端还是逐位置扫描 + 简单尾字节比较。

3. 后端 hash 冲突向量化校验  
现状：没有，当前后端还是线性候选遍历 + 逐字节 exact compare。

4. 对长度不一致 / 模糊 / nocase 的统一高效处理  
现状：语义上能处理，性能上还没体现优势。

---

**下一步最该做的优化顺序**

1. **先把 `keyBits` 从固定 22 改成带成本约束的选择**
- 目标：避免 372 条规则也建 4M 槽 L1。
- 这是你当前数据库从 `84KB` 变 `17MB` 的头号原因。

2. **把运行期从 `pbeRunNaive` 改成块式/向量式前端**
- 至少要先把 `pbeEntryMayMatchAtPos` 从线性尾字节比较改成 NEON 批量比较。
- 这一步会直接决定吞吐能不能接近 Neo。

3. **让 `headMask/tailMask` 真正参与候选筛选**
- 否则这两个字段现在基本只是“存了但没用到性能上”。

4. **后端把候选校验改成向量化**
- 当前最贵的是“每个位置 × 每条候选规则”的二次线性扫描。

如果你愿意，我下一步可以直接基于当前代码，先做一版“为什么 `22-bit L1` 对这个数据集极不合理”的编译期成本模型改造，把 PBE 数据库大小先从 17MB 往下压。