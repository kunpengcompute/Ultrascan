# Hyperscan

Hyperscan是一款高性能的开源正则表达式匹配库，在支持PCRE的大部分语法的前提下，Hyperscan增加了特定的语法和工作模式来保证其在真实网络场景下的实用性。Hyperscan针对不同使用场景设计了多种高效匹配算法，以及结合SIMD指令，实现了正则表达式的高性能匹配。Hyperscan适用于部署在诸如DPI/IPS/IDS/FW等场景中。在鲲鹏平台上，我们将Hyperscan进行了基于neon指令集改造，以适配aarch64架构，同时进行了算法上的进一步优化。

# 使用说明

使用说明及环境要求请参考鲲鹏官网https://www.hikunpeng.com/document/detail/zh/kunpengaccel/system-lib/cg-hyperscan/kunpengaccel_hyperscan_02_0001.html

# 许可证

Hyperscan 采用 BSD 许可证授权。许可证文件可在项目代码仓库中找到。

# 版本管理

`master` 分支 (Github/kunpengcompute):该分支始终包含 Intel Hyperscan 的最新版本。

`aarch64` 分支 (Github/kunpengcompute):该分支始终包含支持 AArch64 架构的最新版本。AArch64 分支是基于 Intel Hyperscan 5.4.2 版本开发的。发布到 aarch64 分支的每个版本在发布前都经过质量保证（QA）和测试。建议： 如果您是 AArch64 平台的用户（而非开发者），应使用此分支的版本。

`khsel` 分支该分支主要包含针对 aarch64 平台的增强功能。 更多信息请访问： https://www.hikunpeng.com/developer/boostkit/library/detail?subtab=Hyperscan

# 移植工作
执行平台特定的操作，包括：
编译适配
检测特定的头文件
SIMD 指令集判断
以及其他相关操作。

# 性能优化
通过以下技术在鲲鹏平台上进行性能提升：
使用 NEON 指令集
内联汇编优化
数据对齐
指令对齐
内存数据预取
静态分支预测
代码结构优化等等。

# 参与社区
Hyperscan 的官方网站是 www.hyperscan.io。

`aarch64` 分支: 如有疑问或建议，我们鼓励您在 Github 上创建 issue。

若您希望直接联系华为团队，可以发送邮件至 kunpengcompute@huawei.com。
