# Hyperscan

Hyperscan是一款高性能的开源正则表达式匹配库，在支持PCRE的大部分语法的前提下，Hyperscan增加了特定的语法和工作模式来保证其在真实网络场景下的实用性。Hyperscan针对不同使用场景设计了多种高效匹配算法，以及结合SIMD指令，实现了正则表达式的高性能匹配。Hyperscan适用于部署在诸如DPI/IPS/IDS/FW等场景中。在鲲鹏平台上，我们将Hyperscan进行了基于neon指令集改造，以适配aarch64架构，同时进行了算法上的进一步优化。

# 使用说明

使用说明及环境要求请参考鲲鹏官网https://www.hikunpeng.com/document/detail/zh/kunpengaccel/system-lib/cg-hyperscan/kunpengaccel_hyperscan_02_0001.html

# License

Hyperscan is licensed under the BSD License. See the LICENSE file in the
project repository.

# Versioning

The `master` branch on Github/kunpengcompute will always contain the most recent 
release of Intel Hyperscan. 

The `aarch64` branch on Github/kunpengcompute will always contain the most recent 
release that supports the AArch64 architecture. The AArch64 branch was developed
based on Intel Hyperscan 5.4.2. Each version released to `aarch64` branch goes through
QA and testing before it is released; if you're a user of AArch64, rather than a developer,
this is the version you should be using.

The `khsel` branch on kunpengcompute contains the enhancement mainly on aarch64 platform. Visiting the kunpeng website to get more information. https://www.hikunpeng.com/developer/boostkit/library/detail?subtab=Hyperscan

# Porting
Perform platform-specific operations, including compilation, 
detecting specific header files, SIMD instruction judgment, and so on.

# Optimization
Improve the Kunpeng platform by using the NEON instructions, inline assembly, 
data alignment, instruction alignment, memory data prefetching, static branch 
prediction, code structure optimization, etc.

# Get Involved

The official homepage for Hyperscan is at [www.hyperscan.io](https://www.hyperscan.io).

`master` branch

If you have questions or comments, we encourage you to [join the mailing
list](https://lists.01.org/mailman/listinfo/hyperscan). Bugs can be filed by
sending email to the list, or by creating an issue on Github.

If you wish to contact the Hyperscan team at Intel directly, without posting
publicly to the mailing list, send email to
[hyperscan@intel.com](mailto:hyperscan@intel.com).

`aarch64` branch

If you have questions or comments, we encourage you to create an issue on Github.

If you wish to contact the Huawei team directly, you can send an email to 
kunpengcompute@huawei.com.
