# Ultrascan介绍

## 最新消息

\[2026-06-30\]: 原Hyperscan正式更名为Ultrascan，同时发布Ultrascan 5.7.0。新增通用字节码功能，支持规则集字节码跨平台部署。

\[2026-03-30\]: 发布Hyperscan 2.6.0。新增基于鲲鹏920新型号处理器优化Hyperscan 2~4字节短规则匹配算法。

\[2025-12-30\]: 发布Hyperscan KHSEL 2.5.3。优化Hyperscan多模匹配算法。优化Rose解释器后端长字符串校验。增加短规则旁路算法开关。

## 项目介绍

Ultrascan是一款高性能的开源正则表达式匹配库，在支持PCRE的大部分语法的前提下，增加了特定的语法和工作模式来保证其在真实网络场景下的实用性。Ultrascan针对不同使用场景设计了短规则旁路、假阳性阻断等高效匹配算法，以及结合SIMD指令，实现了正则表达式的高性能匹配。Ultrascan适用于部署在诸如DPI/IPS/IDS/FW等场景中。在鲲鹏平台上，基于NEON指令集对Ultrascan进行了改造，以适配Aarch64架构，同时针对算法进行了优化。

## 特性介绍

|特性名称|特性介绍
|--|--|
| 短规则旁路技术 | 短规则旁路技术特性包括单字节短规则算法和2~4字节短规则算法，通过将导致性能瓶颈的短规则从常规规则中分离，用旁路规则算法消除冗余操作，从而大幅提升整体匹配性能。 |
| 假阳性阻断技术 | 假阳性阻断技术特性能够减少解释器大量无用的调用，进而大幅提高Ultrascan匹配性能。  |
| 通用字节码技术 | 通用字节码技术特性能够将Ultrascan的正则表达式编译为跨平台支持格式，该格式支持一套规则集字节码能够在x86和鲲鹏计算平台上运行，无需重新编译。 |

## 目录结构

```text
├── chimera                                                    # Chimera接口目录，提供PCRE兼容的正则表达式功能
│   ├── ch.h                                                  # Chimera公共API头文件
│   ├── ch_compile.cpp                                        # Chimera编译时功能实现
│   ├── ch_runtime.c                                          # Chimera运行时功能实现
│   └── ...                                                   # 其他Chimera相关文件
├── cmake                                                      # CMake构建系统配置目录
├── doc                                                        # 开发参考文档目录
│   └── dev-reference                                         # RST文件和Doxygen配置，生成API文档和开发指南
├── docs                                                       # 项目文档目录
│   └── zh                                                    # 中文文档目录
│       ├── figures                                           # 中文文档图片资源目录
│       ├── quick_start.md                                    # 快速入门
│       ├── release_notes.md                                  # 版本说明书
│       ├── installation_guide.md                              # 安装指南
│       ├── developer_guide.md                                # 开发指南
│       ├── user_guide.md                                     # 用户指南
├── examples                                                   # 示例代码目录
│   ├── CMakeLists.txt                                        # 示例代码构建配置
│   ├── README.md                                             # 示例代码说明文档
│   ├── simplegrep.c                                          # 简单的grep实现示例
│   ├── pcapscan.cc                                           # 网络数据包扫描示例
│   └── patbench.cc                                           # 模式匹配性能测试示例
├── include                                                    # 公共头文件目录
│   └── boost-patched/                                        # Boost库补丁版本
├── src                                                        # 核心源代码目录
│   ├── compiler/                                             # 编译器模块，负责将正则表达式编译为内部表示
│   ├── fdr/                                                  # FDR引擎
│   ├── hwlm/                                                 # HWLM引擎
│   ├── kunpeng-enhanced/                                     # 鲲鹏平台增强实现，包含Lily引擎
│   ├── nfa/                                                  # NFA引擎
│   ├── nfagraph/                                             # NFA图构建和优化模块
│   ├── rose/                                                 # ROSE引擎相关模块
│   ├── som/                                                  # SOM（Start of Match）相关实现
│   └── util/                                                 # 内部工具函数
├── tools                                                      # 工具目录
│   ├── fuzz/                                                 # fuzz测试工具
│   ├── hsbench/                                              # 性能基准测试工具
│   │   ├── CMakeLists.txt                                    # 构建配置
│   │   ├── README.md                                         # 性能基准测试工具说明文档
│   │   ├── scripts/                                          # 辅助脚本
│   │   └── ...                                               # 其他源文件
│   ├── hscheck/                                              # Ultrascan检查工具
│   ├── hscollider/                                           # 对比pcre的正确性测试工具
│   └── hsdump/                                               # Ultrascan转储工具
├── unit                                                       # 单元测试目录
│   ├── chimera/                                              # Chimera接口单元测试
│   ├── gtest/                                                # Google Test框架
│   ├── Ultrascan/                                            # Ultrascan核心功能单元测试
│   ├── internal/                                             # 内部模块单元测试
│   └── CMakeLists.txt                                        # 单元测试工具构建配置
├── util                                                       # 实用工具目录
│   ├── CMakeLists.txt                                        # 实用工具构建配置
│   ├── ExpressionParser.rl                                   # 正则表达式解析器（ragel）
│   ├── cross_compile.cpp                                     # 交叉编译支持
│   ├── database_util.cpp                                     # 数据库工具
│   ├── ng_corpus_editor.cpp                                  # 语料库编辑器
│   ├── ng_corpus_generator.cpp                               # 语料库生成器
│   └── ...                                                   # 其他实用工具
├── README.md                                                  # 项目说明文档
├── config.txt                                                 # 配置文件
└── ...                                                        # 其他根级文件
```

## 版本说明

每个版本的特性变更详细信息，具体请参见《[版本说明书](docs/zh/release_notes.md)》。

## 约束与限制

鲲鹏计算平台上，Ultrascan使用lily引擎对单字节规则、2-4字节短规则匹配场景进行性能增强，该优化特性存在如下约束：

lily单字节、2-4字节短规则匹配引擎各自仅能处理至多8条单字节规则，超出部分将采用原有引擎处理。
在单条语料中，lily引擎所负责匹配的单字节规则命中次数合计应不大于4096，否则将停止匹配，并返回HS_SCAN_TERMINATED错误码。
在单条语料中，lily引擎所负责匹配的2-4字节短规则命中次数合计应不大于4096，否则将停止匹配，并返回HS_SCAN_TERMINATED错误码。

## 环境部署

介绍Ultrascan的环境依赖及安装方式和编译方法，具体请参见《[安装指南](./docs/zh/installation_guide.md)》。

## 快速入门

介绍基于Ultrascan官方提供的性能Benchmark工具hsbench的快速入门，具体请参见《[快速入门](./docs/zh/quick_start.md)》。

## 学习文档

|名称|简介|
|--|--|
|[版本说明书](docs/zh/release_notes.md)|提供Ultrascan每个发布版本的基础信息和特性更新信息。|
|[安装指南](./docs/zh/installation_guide.md)|指导用户如何安装部署及编译软件。|
|[快速入门](./docs/zh/quick_start.md)|提供快速上手验证指导。|
|[用户指南](./docs/zh/user_guide.md)|提供Ultrascan特性使用指导。|
|[开发指南](./docs/zh/developer_guide.md)|提供Ultrascan特性相关接口说明及定义等。|

## 贡献声明

欢迎大家为社区做贡献，如果使用过程中有任何问题/建议，或者需要反馈特性需求和bug报告，可以提交[Issues](https://gitcode.com/boostkit/Ultrascan/issues)联系我们，具体贡献方法可参考[这里](https://gitcode.com/boostkit/community/blob/master/docs/contributor/contributing.md)。同时也欢迎大家在[讨论专区](https://gitcode.com/boostkit/community/discussions)展开讨论交流。感谢您的支持。

## 免责声明

此代码仓计划参与Ultrascan软件开源，仅作Ultrascan性能提升，编码风格遵照原生开源软件，继承原生开源软件安全设计，不破坏原生开源软件设计及编码风格和方式，软件的任何漏洞与安全问题，均由相应的上游社区根据其漏洞和安全响应机制解决。请密切关注上游社区发布的通知和版本更新。鲲鹏计算社区对软件的漏洞及安全问题不承担任何责任。

## License

本项目采用BSD许可证。详见[LICENSE](LICENSE)文件。

本项目的文档适用CC-BY 4.0许可证，具体请参见[LICENSE](docs/LICENSE)文件。
