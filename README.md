# 项目介绍

Hyperscan是一款高性能的开源正则表达式匹配库，在支持PCRE的大部分语法的前提下，增加了特定的语法和工作模式来保证其在真实网络场景下的实用性。Hyperscan针对不同使用场景设计了多种高效匹配算法，以及结合SIMD指令，实现了正则表达式的高性能匹配。Hyperscan适用于部署在诸如DPI/IPS/IDS/FW等场景中。在鲲鹏平台上，华为基于NEON指令集对Hyperscan进行了改造，以适配AAarch64架构，同时针对算法进行了优化。

# 版本说明

**表 1**  版本说明

<table><thead align="left"><tr id="row16841427102415"><th class="cellrowborder" valign="top" width="33.33333333333333%" id="mcps1.2.4.1.1"><p id="p884142792414"><a name="p884142792414"></a><a name="p884142792414"></a>Kunpeng Hyperscan</p>
</th>
<th class="cellrowborder" valign="top" width="33.33333333333333%" id="mcps1.2.4.1.2"><p id="p38418273243"><a name="p38418273243"></a><a name="p38418273243"></a>开源Hyperscan</p>
</th>
<th class="cellrowborder" valign="top" width="33.33333333333333%" id="mcps1.2.4.1.3"><p id="p18841162714243"><a name="p18841162714243"></a><a name="p18841162714243"></a>特性</p>
</th>
</tr>
</thead>
<tbody><tr id="row88410276244"><td class="cellrowborder" valign="top" width="33.33333333333333%" headers="mcps1.2.4.1.1 "><p id="p2841827172419"><a name="p2841827172419"></a><a name="p2841827172419"></a>2.5.1</p>
</td>
<td class="cellrowborder" valign="top" width="33.33333333333333%" headers="mcps1.2.4.1.2 "><p id="p1084192762411"><a name="p1084192762411"></a><a name="p1084192762411"></a>5.4.2</p>
</td>
<td class="cellrowborder" valign="top" width="33.33333333333333%" headers="mcps1.2.4.1.3 "><p id="p42261134102514"><a name="p42261134102514"></a><a name="p42261134102514"></a>正则表达式匹配与匹配函数。</p>
</td>
</tr>
</tbody>
</table>

# 约束与限制
Arm平台上，Hyperscan使用lily引擎对单字节规则匹配场景进行性能增强，该优化特性存在如下约束：
1. lily引擎仅能处理至多8条单字节规则，超出部分将采用原有引擎处理。
2. 在单条语料中，lily引擎所负责匹配的单字节规则命中次数合计应不大于4096，否则将停止匹配，并返回HS_SCAN_TERMINATED信号。

# 环境部署

Hyperscan当前适配的处理器和操作系统为鲲鹏920系列处理器，openEuler 22.03操作系统，若您在使用过程中遇到问题，请先检查使用的环境是否在已验证的环境范围内。

**表 1**  Hyperscan已验证环境

<table><thead align="left"><tr id="row49014226274"><th class="cellrowborder" valign="top" width="50%" id="mcps1.2.3.1.1"><p id="p19628134422716"><a name="p19628134422716"></a><a name="p19628134422716"></a>操作系统</p>
</th>
<th class="cellrowborder" valign="top" width="50%" id="mcps1.2.3.1.2"><p id="p7628104492718"><a name="p7628104492718"></a><a name="p7628104492718"></a>CPU类型</p>
</th>
</tr>
</thead>
<tbody><tr id="row179062222716"><td class="cellrowborder" valign="top" width="50%" headers="mcps1.2.3.1.1 "><p id="p1862834462712"><a name="p1862834462712"></a><a name="p1862834462712"></a>openEuler 22.03 LTS SP3</p>
</td>
<td class="cellrowborder" valign="top" width="50%" headers="mcps1.2.3.1.2 "><p id="p12628184482717"><a name="p12628184482717"></a><a name="p12628184482717"></a>鲲鹏920系列处理器</p>
</td>
</tr>
</tbody>
</table>

具体编译方法可以参考《[Hyperscan编译指南](https://www.hikunpeng.com/document/detail/zh/kunpengaccel/system-lib/cg-hyperscan/kunpengaccel_hyperscan_02_0001.html)》。

# 快速上手

hsbench是Hyperscan官方提供的性能Benchmark工具，通过hsbench的测试结果能够对比使用开源Hyperscan和Kunpeng Hyperscan的性能差异。

1.  进入创建好的“build”目录。

    ```
    cd build
    ```

2.  获取hsbench规则集和输入数据，并解压到“build/hsbench-samples”目录。

    [获取链接](https://cdrdv2.intel.com/v1/dl/getContent/739375)

3.  运行hsbench。

    ```
    ./bin/hsbench -e ./hsbench-samples/pcre/snort_literals -c ./hsbench-samples/corpora/gutenberg.db -N -n1
    ```

# 安装后验证

运行结果：

```
Signatures:        ./hsbench-samples/pcre/snort_literals
Hyperscan info:    Version: 5.4.2 Features:  Mode: BLOCK
Expression count:  3,116
Bytecode size:     923,384 bytes
Database CRC:      0xc4b8895e
Scratch size:      5,183 bytes
Compile time:      0.137 seconds
Peak heap usage:   23,830,528 bytes

Time spent scanning:       0.025 seconds
Corpus size:               6,701,044 bytes (3,280 blocks)
Matches per iteration:     4,302 (0.657 matches/kilobyte)
Overall block rate:        132,895.00 blocks/sec
Mean throughput (overall): 2,172.04 Mbit/sec
Max throughput (per core): 2,172.10 Mbit/sec

```

运行结果参数说明如下：

-   Time spent scanning：使用目标规则集扫描目标数据库，扫描所用的时间。
-   Matches per iteration：每次迭代，按规则集匹配命中的数量。
-   Mean throughput \(overall\)：平均吞吐量（Mbit每秒）。
-   Max throughput \(per core\)：所有CPU核中的最大吞吐量（Mbit每秒）。

# 贡献指南

如果使用过程中有任何问题，或者需要反馈特性需求和bug报告，可以提交issues联系我们，具体贡献方法可参考[这里](https://gitcode.com/boostkit/community/blob/master/docs/contributor/contributing.md)。

# 免责声明

此代码仓计划参与Hyperscan软件开源，仅作Hyperscan性能提升，编码风格遵照原生开源软件，继承原生开源软件安全设计，不破坏原生开源软件设计及编码风格和方式，软件的任何漏洞与安全问题，均由相应的上游社区根据其漏洞和安全响应机制解决。请密切关注上游社区发布的通知和版本更新。鲲鹏计算社区对软件的漏洞及安全问题不承担任何责任。

