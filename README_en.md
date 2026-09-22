# Ultrascan Introduction

<!-- md-trans-meta sourceCommit=fd215461e79615cfea45605856a99c32a14a7e4b translatedAt=2026-08-27T01:43:08.264Z pushedAt=2026-09-09T07:42:24.657Z -->

[Chinese](./README.md) | English

## Latest Updates

[2026-09-30]: Released Ultrascan 5.8.0. Added the mcsheng algorithm performance optimization and feedback-driven optimization for regular expression matching. Optimized the Grey configuration method to support setting and resetting process-level Grey configuration through public APIs.

[2026-06-30]: Renamed Hyperscan to Ultrascan, and released Ultrascan 5.7.0. Added the universal bytecode function to support cross-platform deployment of rule-set bytecode.

[2026-03-30]: Released Hyperscan 2.6.0. Added the Hyperscan short-byte (2–4 bytes) rule matching algorithm based on the new Kunpeng 920 processor model.

[2025-12-30]: Released Hyperscan KHSEL 2.5.3. Optimized the Hyperscan multi-pattern matching algorithm. Optimized the long string validation on the Rose interpreter backend. Added a configuration item for toggling the short-rule bypass algorithm.

## Project Introduction

Ultrascan is a high-performance, open-source regular expression matching library. It supports most Perl Compatible Regular Expressions (PCRE) syntax and adds specific syntax and working modes to ensure its practicability in real network scenarios. Ultrascan provides efficient matching algorithms such as short-rule bypass and false-positive blocking for different application scenarios. It also uses SIMD instructions to implement high-performance matching of regular expressions, and provides universal bytecode and feedback-driven optimization for regular expression matching. Ultrascan is suited for scenarios such as data distribution, intrusion prevention system (IPS), intrusion detection system (IDS), and firewall. On the Kunpeng platform, Ultrascan has been reconstructed based on the NEON instruction set to adapt to the AArch64 architecture, and the algorithms have been optimized.

## Feature Description

|Feature|Description
|--|--|
| Short-rule bypass technology | The short-rule bypass technology includes algorithms for single-byte and 2-to-4-byte rules. It separates short rules that cause performance bottlenecks from common rules and uses the bypass algorithm to eliminate redundant operations, thereby significantly improving the overall matching performance. |
| False-positive blocking technology | The false-positive blocking technology reduces a large number of unnecessary interpreter calls, significantly improving the matching performance of Ultrascan.  |
| Universal bytecode technology | The universal bytecode technology can compile Ultrascan regular expressions into a format supported across platforms. This format allows rule-set bytecode to run on both x86 and Kunpeng computing platforms without the need for recompilation. |
| Feedback-driven optimization for regular expression matching | This technology collects feedback on inefficient fragments based on real scanning corpora, and uses the feedback to recompile the rule database, forming an optimization closed loop oriented to service workloads. |

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
│       ├── api_reference.md                                  # API Reference
│       ├── developer_guide.md                                # Developer Guide
│       ├── user_guide.md                                     # User Guide
├── examples                                                   # Example code directory
│   ├── CMakeLists.txt                                        # Example code build configuration
│   ├── README.md                                             # Example code description document
│   ├── simplegrep.c                                          # Example grep implementation
│   ├── pcapscan.cc                                           # Network packet scanning example
│   └── patbench.cc                                           # Pattern matching performance test example
├── include                                                    # Public header file directory
│   └── boost-patched/                                        # Boost library patch version
├── src                                                        # Core source code directory
│   ├── compiler/                                             # Compiler module, which compiles regular expressions into internal representations
│   ├── fdr/                                                  # FDR engine
│   ├── hwlm/                                                 # HWLM engine
│   ├── kunpeng-enhanced/                                     # Kunpeng platform enhancement implementation, including the Lily engine
│   ├── nfa/                                                  # NFA engine
│   ├── nfagraph/                                             # NFA graph build and optimization module
│   ├── parser/                                               # Regular expression parser
│   ├── rose/                                                 # ROSE engine
│   ├── smallwrite/                                           # smallwrite engine
│   ├── som/                                                  # Start of Match (SOM) implementation
│   └── util/                                                 # Internal utility functions
├── tools                                                      # Tool directory
│   ├── fuzz/                                                 # Fuzz testing tool
│   ├── hsbench/                                              # Performance benchmark tool
│   │   ├── CMakeLists.txt                                    # Build configuration
│   │   ├── README.md                                         # Performance benchmark tool description
│   │   ├── scripts/                                          # Auxiliary scripts
│   │   └── ...                                               # Other source files
│   ├── hscheck/                                              # Ultrascan check tool
│   ├── hscollider/                                           # PCRE-based correctness comparison test tool
│   ├── hspgo/                                                # Closed-loop tool for feedback-driven optimization for regular expression matching
│   └── hsdump/                                               # Ultrascan dump tool
├── unit                                                       # Unit test directory
│   ├── chimera/                                              # Chimera API unit test
│   ├── gtest/                                                # GoogleTest framework
│   ├── hyperscan/                                            # Core function unit test
│   ├── internal/                                             # Internal module unit test
│   └── CMakeLists.txt                                        # Unit test tool build configuration
├── util                                                       # Utility directory
│   ├── CMakeLists.txt                                        # Utility build configuration
│   ├── ExpressionParser.rl                                   # Regular expression parser (Ragel)
│   ├── cross_compile.cpp                                     # Cross-compilation support
│   ├── database_util.cpp                                     # Database utility
│   ├── ng_corpus_editor.cpp                                  # Corpus editor
│   ├── ng_corpus_generator.cpp                               # Corpus generator
│   └── ...                                                   # Other utilities
├── CHANGELOG.md                                              # Change log document
├── CMakeLists.txt                                            # Project root-level build configuration
├── COPYING                                                   # License copy declaration file
├── LICENSE                                                   # Project license file
├── README.md                                                 # Project description document (Chinese)
├── README_en.md                                              # Project description document (English)
├── ThirdPartyNotice.md                                       # Third-party notice document
├── hs.def                                                    # Exported symbol definitions
├── hs_runtime.def                                            # Runtime exported symbol definitions
└── ...                                                       # Other root-level files
```

## Release Notes

For details about feature changes in each version, see [Release Notes](./docs/en/release_notes.md).

## Constraints

On the Kunpeng computing platform, Ultrascan uses the Lily engine to enhance the performance of single-byte and 2-to-4-byte rule matching. This optimization feature has the following restrictions:

The Lily single-byte and 2-to-4-byte rule matching engines can each process a maximum of eight rules. Any additional rules will be processed by the original engine.
For a data record to be matched with single-byte rules by the Lily engine, the total number of hits must be less than or equal to 4096. Otherwise, the matching stops and the error code `HS_SCAN_TERMINATED` is returned.
For a data record to be matched with 2-to-4-byte rules by the Lily engine, the total number of hits must be less than or equal to 4096. Otherwise, the matching stops and the error code `HS_SCAN_TERMINATED` is returned.

Feedback-driven optimization for regular expression matching currently supports only AArch64 and is disabled by default. It must be explicitly enabled at build time by setting `-DHS_ENABLE_FP_FEEDBACK=ON`.

For AArch64 targets, the instruction set baseline can be selected via `HS_ARM_MARCH` (the CMake target processor must be `aarch64` or `AARCH64`): By default, `AUTO` uses `-march=native -mtune=native` for native builds, and falls back to `PORTABLE` during cross-compilation or when the compiler does not support this flag combination; `PORTABLE` always uses `-march=armv8-a+crc` and can only be deployed on machines that support Armv8-A and the CRC32 extensions, making it suitable for distribution across machines that meet this baseline and for performance comparison; an explicit architecture value (such as `armv8.2-a+crc+sve` or `armv9-a+sve2`) can also be passed. The outputs built with `AUTO`, `native`, or explicit architecture values can only be deployed on machines that support all of their instruction set extensions. For details, see [Installation Guide](./docs/en/installation_guide.md).

## Environment Deployment

For details about the environment dependencies, installation methods, and compilation methods of Ultrascan, see [Installation Guide](./docs/en/installation_guide.md).

## Quick Start

For details about how to quickly get started with the universal bytecode tool, the performance benchmark tool, and feedback-driven optimization tool for regular expression matching, see [Quick Start](./docs/en/quick_start.md).

## Documents

|Name|Description|
|--|--|
|[Release Notes](./docs/en/release_notes.md)|Provides basic information and feature update information about each Ultrascan release.|
|[Installation Guide](./docs/en/installation_guide.md)|Describes how to install, deploy, and compile the software.|
|[Quick Start](./docs/en/quick_start.md)|Provides guidance for a quick start and verification.|
|[User Guide](./docs/en/user_guide.md)|Provides guidance on how to use the Ultrascan features.|
|[Developer Guide](./docs/en/developer_guide.md)|Provides guidance on how to develop the internal APIs and features in Ultrascan.|
|[API Reference](./docs/en/api_reference.md)|Summarizes the definitions and usage instructions of the new public APIs added in Ultrascan.|

## Contribution Statement

We welcome your contributions to the community. If you have any questions/suggestions or want to provide feedback on feature requirements and bug reports, you can submit [issues](https://gitcode.com/boostkit/Ultrascan/issues). For details, see the [contribution guideline](https://gitcode.com/boostkit/community/blob/master/docs/contributor/contributing.md). You are also welcome to share insights in [Discussions](https://gitcode.com/boostkit/community/discussions). Thank you for your support.

## Disclaimer

This code repository contributes to the Ultrascan open-source project solely for performance optimization. It strictly adheres to the coding style and methods, as well as security design of the native open-source software. Any vulnerability and security issues of the software shall be resolved by the corresponding upstream communities according to their response mechanisms. Please pay attention to the notifications and version updates released by the upstream communities. The Kunpeng computing community does not assume any responsibility for software vulnerabilities and security issues.

## License

This project uses the BSD license. For details, see [LICENSE](LICENSE).

The documents of this project are licensed under CC-BY 4.0. For details, see [LICENSE](./docs/LICENSE).
