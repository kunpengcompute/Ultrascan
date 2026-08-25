# Introduction to Ultrascan

English | [简体中文](./README.md)

## Latest Updates

[2026-06-30]: Renamed Hyperscan to Ultrascan, and released Ultrascan 5.7.0. Added the universal bytecode function to support cross-platform deployment of rule-set bytecode.

[2026-03-30]: Released Hyperscan 2.6.0. Added the Hyperscan short-byte (2–4 bytes) rule matching algorithm based on the new Kunpeng 920 processor model.

[2025-12-30]: Released Hyperscan KHSEL 2.5.3. Optimized the Hyperscan multi-pattern matching algorithm. Optimized the long string validation on the Rose interpreter backend. Added a configuration item for toggling the short-rule bypass algorithm.

## Project Introduction

Ultrascan is a high-performance, open-source regular expression matching library. It supports most Perl Compatible Regular Expressions (PCRE) syntax and has specific syntax and working modes to ensure its practicability in real network scenarios. Ultrascan has designed efficient matching algorithms such as short-rule bypass and false-positive blocking for different application scenarios. It also uses SIMD instructions to implement high-performance matching of regular expressions. Ultrascan is suited for scenarios such as data distribution, intrusion prevention system (IPS), intrusion detection system (IDS), and firewall. On the Kunpeng platform, Ultrascan has been reconstructed based on the NEON instruction set to adapt to the AArch64 architecture, and the algorithms have been optimized.

## Feature Description

|Feature|Description
|--|--|
| Short-rule bypass technology| The short-rule bypass technology includes algorithms for single-byte and 2-to-4-byte rules. It separates short rules that cause performance bottlenecks from common rules and uses the bypass algorithm to eliminate redundant operations, thereby significantly improving the overall matching performance.|
| False-positive blocking technology| The false-positive blocking technology reduces a large number of unnecessary interpreter calls, significantly improving the matching performance of Ultrascan. |
| Universal bytecode technology| The universal bytecode technology can compile Ultrascan regular expressions into a format supported across platforms. This format allows rule-set bytecode to run on both x86 and Kunpeng computing platforms without the need for recompilation.|

## Directory Structure

```text
├── chimera                                                    # Chimera API directory, which provides PCRE-compatible regular expression functions
│   ├── ch.h                                                  # Chimera public API header file
│   ├── ch_compile.cpp                                        # Chimera compile-time function implementation
│   ├── ch_runtime.c                                          # Chimera runtime function implementation
│   └── ...                                                   # Other Chimera-related files
├── cmake                                                      # CMake build configuration directory
├── doc                                                        # Development reference document directory
│   └── dev-reference                                         # RST files and Doxygen configuration for generating the API document and developer guide
├── docs                                                       # Project document directory
│   └── en                                                    # English document directory
│       ├── figures                                           # Directory of images in documents
│       ├── quick_start.md                                    # Quick Start
│       ├── release_notes.md                                  # Release Notes
│       ├── installation_guide.md                              # Installation Guide
│       ├── developer_guide.md                                # Developer Guide
│       ├── user_guide.md                                     # User Guide
├── examples                                                   # Sample code directory
│   ├── CMakeLists.txt                                        # Sample code build configuration
│   ├── README.md                                             # Sample code description document
│   ├── simplegrep.c                                          # Simple grep implementation example
│   ├── pcapscan.cc                                           # Network data packet scanning example
│   └── patbench.cc                                           # Pattern matching performance test example
├── include                                                    # Common header file directory
│   └── boost-patched/                                        # Boost library patch version
├── src                                                        # Core source code directory
│   ├── compiler/                                             # Compiler module, which compiles regular expressions into internal representations
│   ├── fdr/                                                  # FDR engine
│   ├── hwlm/                                                 # HWLM engine
│   ├── kunpeng-enhanced/                                     # Kunpeng platform enhancement implementation, which includes the Lily engine
│   ├── nfa/                                                  # NFA engine
│   ├── nfagraph/                                             # NFA graph build and optimization module
│   ├── rose/                                                 # ROSE engine
│   ├── som/                                                  # Start of Match (SOM) implementation
│   └── util/                                                 # Internal utility functions
├── tools                                                      # Tool directory
│   ├── fuzz/                                                 # Fuzz testing tool
│   ├── hsbench/                                              # Performance benchmark tool
│   │   ├── CMakeLists.txt                                    # Build configuration
│   │   ├── README.md                                         # Performance benchmark tool description document
│   │   ├── scripts/                                          # Auxiliary scripts
│   │   └── ...                                               # Other source files
│   ├── hscheck/                                              # Ultrascan check tool
│   ├── hscollider/                                           # PCRE-based correctness comparison test tool
│   └── hsdump/                                               # Ultrascan dump tool
├── unit                                                       # Unit test directory
│   ├── chimera/                                              # Chimera API unit test
│   ├── gtest/                                                # GoogleTest framework
│   ├── Ultrascan/                                            # Unit test for core Ultrascan functions
│   ├── internal/                                             # Unit test for internal modules
│   └── CMakeLists.txt                                        # Build configuration for unit test tools
├── util                                                       # Utility directory
│   ├── CMakeLists.txt                                        # Build configuration for utilities
│   ├── ExpressionParser.rl                                   # Regular expression parser (Ragel)
│   ├── cross_compile.cpp                                     # Cross-compilation support
│   ├── database_util.cpp                                     # Database utility
│   ├── ng_corpus_editor.cpp                                  # Corpus editor
│   ├── ng_corpus_generator.cpp                               # Corpus generator
│   └── ...                                                   # Other utilities
├── README.md                                                  # Project description document
├── config.txt                                                 # Configuration file
└── ...                                                        # Other root-level files
```

## Release Notes

For details about feature changes in each version, see [Release Notes](docs/en/release_notes.md).

## Constraints

On the Kunpeng computing platform, Ultrascan uses the Lily engine to enhance the performance of single-byte and 2-to-4-byte rule matching. This optimization feature has the following restrictions:

The Lily single-byte and 2-to-4-byte rule matching engines can each process a maximum of eight rules. Any additional rules will be processed by the original engine.
For a data record to be matched with single-byte rules by the Lily engine, the total number of hits must be less than or equal to 4096. Otherwise, the matching stops and the error code `HS_SCAN_TERMINATED` is returned.
For a data record to be matched with 2-to-4-byte rules by the Lily engine, the total number of hits must be less than or equal to 4096. Otherwise, the matching stops and the error code `HS_SCAN_TERMINATED` is returned.

## Environment Deployment

For details about the environment dependencies, installation methods, and compilation methods of Ultrascan, see [Installation Guide](./docs/en/installation_guide.md).

## Quick Start

For details about how to quickly get started with the performance benchmark tool hsbench provided by Ultrascan, see [Quick Start](./docs/en/quick_start.md).

## Documents

|Name|Description|
|--|--|
|[Release Notes](./docs/en/release_notes.md)|Provides basic information and feature updates of each Ultrascan version.|
|[Installation Guide](./docs/en/installation_guide.md)|Describes how to install, deploy, and compile the software.|
|[Quick Start](./docs/en/quick_start.md)|Provides guidance for a quick start and verification.|
|[User Guide](./docs/en/user_guide.md)|Provides guidance on how to use the Ultrascan feature.|
|[Developer Guide](./docs/en/developer_guide.md)|Provides descriptions and definitions of APIs related to the Ultrascan feature.|

## Contribution Statement

We welcome your contributions to the community. If you have any questions/suggestions or want to provide feedback on feature requirements and bug reports, you can submit [issues](https://gitcode.com/boostkit/Ultrascan/issues). You are also welcome to share insights in [Discussions](https://gitcode.com/boostkit/community/discussions). Thank you for your support.

## Disclaimer

This code repository contributes to the Ultrascan open-source project solely for performance optimization. It strictly adheres to the coding style and methods, as well as security design of the native open-source software. Any vulnerability and security issues of the software shall be resolved by the corresponding upstream communities according to their response mechanisms. Please pay attention to the notifications and version updates released by the upstream communities. The Kunpeng computing community does not assume any responsibility for software vulnerabilities and security issues.

## License

This project uses the BSD license. For details, see [LICENSE](LICENSE).

The documents of this project are licensed under CC-BY 4.0. For details, see [LICENSE](docs/LICENSE).
