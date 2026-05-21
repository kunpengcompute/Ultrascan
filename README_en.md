# Hyperscan Introduction<a name="EN-US_TOPIC_0000002518252292"></a>

## What's New

\[2026-03-30\]: Released Hyperscan 2.6.0. Optimized the Hyperscan short-byte (2–4 bytes) rule matching algorithm based on the new Kunpeng 920 processor model.

\[2025-12-30\]: Released Hyperscan KHSEL 2.5.3. Optimized the Hyperscan multi-pattern matching algorithm and the backend validation of long strings for the Rose interpreter. Added a configuration item for toggling the short-rule bypass algorithm.

## Project Overview<a name="EN-US_TOPIC_0000002549772085"></a>

Hyperscan  is a high-performance regular expression matching library. It is developed based on Perl Compatible Regular Expressions \(PCRE\) and is open-source under the Berkeley Software Distribution \(BSD\) license. It follows the regular expression syntax of the commonly used libpcre library but has its own C interfaces. Based on the official Hyperscan release and Kunpeng microarchitecture, the implementation mechanism of core interfaces is redesigned, the development and performance are optimized, and the software package suitable for the Kunpeng platform is released. Users of the Kunpeng platform can download this software package based on their service requirements to improve the stability and performance of services on the Kunpeng platform.

The following functions are added to the Hyperscan version dedicated for the Kunpeng platform:

- The Kunpeng platform branch fully compatible with Armv8-A is added. In addition, its use on the x86 platform is not affected.
- NEON instructions, inline assembly, data alignment, instruction alignment, memory prefetch, static branch prediction, and adjusted code structure are used to improve the performance on the Kunpeng platform.
- The Kunpeng Hyperscan Enhanced Library \(KHSEL\) is released. It includes the KHSEL\_ops and KHSEL\_core sub-libraries, a hybrid model with a short-rule bypass, and a false-positive blocking model.
    - KHSEL\_ops provides the ReplaceAllAcc function, which accelerates the ReplaceAll function of the C++ standard library in a fixed rule. The optimization effect can be obtained on Kunpeng 920 series processors.
    - KHSEL\_core optimizes the large-pattern matching algorithm FDR, small-pattern quick matching algorithm Shufti, and long-rule verification, and enhances the scan performance of Hyperscan for processing datasets such as snort\_literal and snort\_pcre.
    - The hybrid model with a short-rule bypass significantly improves the matching performance for rule sets containing short rules.
    - The false-positive blocking model greatly improves the matching performance for rule sets that contain bad string fragments. "Bad strings" refer to a small number of rules with special fragments, causing excessive false positives in multi-pattern matching. This triggers a large number of interpreter calls and inefficient long-rule verification, but yields zero true matches. These unnecessary interpreter calls become computing hotspots, undermining the pre-filtering capability of multi-pattern matching.

For more information about Hyperscan, visit the  [Kunpeng repository on GitCode](https://gitcode.com/boostkit/hyperscan).

## Directory Structure<a name="EN-US_TOPIC_0000002549772081"></a>

```text
├── chimera                                                    # Chimera API directory
│   ├── ch.h                                                  # Chimera public API header file
│   ├── ch_compile.cpp                                        # Chimera compile-time function implementation
│   ├── ch_runtime.c                                          # Chimera runtime function implementation
│   └── ...                                                   # Other Chimera-related files
├── cmake                                                      # CMake build configuration directory
├── doc                                                        # Development reference document directory
│   └── dev-reference                                         # RST files and Doxygen configuration for generating the API document and developer guide
├── docs                                                       # Project document directory
│   └── en                                                    # English document directory
│       ├── figures                                           # Directory of images in Chinese documents
│       ├── quick_start.md                                    # Quick Start
│       ├── release_notes.md                                  # Hyperscan Release Notes
│       ├── compilation_guide.md                              # Hyperscan Compilation Guide
│       ├── developer_guide.md                                # Hyperscan Developer Guide
│       ├── user_guide.md                                     # Hyperscan User Guide
├── examples                                                   # Sample code directory
│   ├── CMakeLists.txt                                        # Build configuration for sample code
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
│   ├── hscheck/                                              # Hyperscan check tool
│   ├── hscollider/                                           # PCRE-based correctness test tool
│   └── hsdump/                                               # Hyperscan dump tool
├── unit                                                       # Unit test directory
│   ├── chimera/                                              # Chimera API unit test
│   ├── gtest/                                                # GoogleTest framework
│   ├── hyperscan/                                            # Unit test for core Hyperscan functions
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

For details about feature changes in each version, see [Release Notes](./docs/en/release\_notes.md).

## Environment Deployment

For details about the environment dependencies, installation methods, and compilation methods of Hyperscan, see [Installation Guide](./docs/en/installation_guide.md).

## Quick Start

The Hyperscan quick start guide uses the performance benchmark tool hsbench provided by Hyperscan. For details, see \[quick\_start.md\]\(./docs/en/quick\_start.md\).

## Helpful Links

|Name|Overview|
|--|--|
|[Quick Start](./docs/en/quick_start.md)|Provides guidance for a quick start and verification.|
|[Release Notes](./docs/en/release_notes.md)|Provides basic information and feature updates of each Hyperscan version.|
|[Installation Guide](./docs/en/installation_guide.md)|Describes how to install, deploy, and compile the software.|
|[User Guide](./docs/en/user_guide.md)|Provides guidance on how to use the Hyperscan feature.|
|[Developer Guide](./docs/en/developer_guide.md)|Provides descriptions and definitions of APIs related to the Hyperscan feature.|

## Contribution Statement

You are welcome to contribute to the community. If you have any questions/suggestions or want to provide feedback on feature requirements and bug reports, you can submit  [issues](https://gitcode.com/boostkit/hyperscan). For details, see the  [contribution guideline](https://gitcode.com/boostkit/community/blob/master/docs/contributor/contributing.md). You are also welcome to share insights in the  [discussion zone](https://gitcode.com/boostkit/community/discussions). Thank you for your support.

## Disclaimer

This code repository contributes to the Hyperscan open-source project solely for performance optimization. It strictly adheres to the coding style and methods, as well as security design of the native open-source software. Any vulnerability and security issues of the software shall be resolved by the corresponding upstream communities according to their response mechanisms. Please pay attention to the notifications and version updates released by the upstream communities. The Kunpeng computing community does not assume any responsibility for the vulnerabilities and security issues of the software.

## License

This project uses the BSD license. For details, see [LICENSE](LICENSE).
