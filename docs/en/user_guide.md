# User Guide

<!-- md-trans-meta sourceCommit=3d692a8b20476bbb17920b3af025a57bbac9b0bc translatedAt=2026-08-27T01:42:40.763Z pushedAt=2026-09-09T07:42:18.218Z -->

## Prerequisites

You have installed and compiled Ultrascan by following instructions in [Installation Guide](./installation_guide.md). If feedback-driven optimization for regular expression matching is required, the corresponding build option must be explicitly enabled on the AArch64 platform.

## Grey Compilation Parameter Settings

Ultrascan no longer reads `config.txt` from the repository root directory, the executable file directory, or its parent directory. When you need to adjust Grey compilation parameters, call `hs_set_grey_overrides()` before compiling the database; after compilation is complete, you can call `hs_reset_grey_overrides()` to restore the default values.

The commonly used configuration items are as follows:

- `allowLily`: Enables or disables the short-byte optimization feature. The value can be `1` (enable) or `0` (disable).
- `allowNeoFdr`: Enables or disables the false-positive blocking feature. The value can be `1` (enable) or `0` (disable). This configuration item is not used for enabling feedback-driven optimization for regular expression matching introduced in V5.8.0.

Usage example:

```c
hs_error_t err = hs_set_grey_overrides(
    "allowLily:1;allowNeoFdr:1;");
if (err != HS_SUCCESS) {
    /* The override string is invalid, and the original configuration remains unchanged. */
}

/* Call hs_compile_*() to compile the database. */

hs_reset_grey_overrides();
```

The Grey override string format is `key:value;key:value;...`. Passing `NULL` or an empty string also restores the default values. The configuration is shared at the process level. It is recommended that the settings be complete before compilation, and that concurrent modification be avoided during compilation.

For the complete configuration format, error handling, example program parameters, and API definitions, see [Grey Configuration APIs](./api_reference.md#3-grey-configuration-apis).

## Feedback-driven Optimization for Regular Expression Matching

Feedback-driven optimization for regular expression matching is a new feature introduced in V5.8.0. It is applicable to service corpora where a small number of front-end fragments are triggered frequently but rule reporting is rarely produced. Its usage flow is as follows:

1. Use a normal compilation API to generate a baseline database.
2. Use the scanning API with a collector to collect service corpora.
3. Filter high-waste fragments by threshold and generate feedback.
4. Recompile the same rule set using the feedback.
5. After verifying the matching results, switch to the new database and continue observing performance.

Note the following when using it:

- Currently, only AArch64 is supported, and `-DHS_ENABLE_FP_FEEDBACK=ON` must be explicitly set at build time.
- A collector is bound to a specific database object that creates it and inherits that object's lifetime.
- A collector does not support concurrent writes from multiple threads. Each scanning thread should use an independent collector, and the collectors are merged after writing stops.
- Baseline compilation and feedback-based compilation must use the same expressions, IDs, flags, extended parameters, modes, and key Grey configurations.
- Before and after feedback-based compilation, the final matching results should be verified with service corpora; performance sampling must not be used as a substitute for correctness verification.
- As the public stream close/reset APIs do not carry a collector, the corresponding EOD stage is not within the sampling scope.
- Builds with this capability disabled still export the relevant public symbols, but functional calls return `HS_ARCH_ERROR`; common service paths should continue to call the original compilation and scanning APIs.

For a first-time experience, it is recommended that you follow the [hspgo tool workflow](./quick_start.md#hspgo-feedback-driven-optimization-tool-for-regular-expression-matching). For application integration, refer to [Feedback-driven Optimization APIs for Regular Expression Matching](./api_reference.md#4-feedback-driven-optimization-apis-for-regular-expression-matching).

## Universal Bytecode

When you need to deploy the same rule set bytecode to both x86 and Kunpeng computing platforms, you can use `hsdump` to generate universal bytecode, or call the `fat_hs_*` public APIs to complete compilation, serialization, and deserialization.

For tool operations, see [Quick Start](./quick_start.md#hsdump-universal-bytecode-generation-tool). For function definitions, see [Universal Bytecode APIs](./api_reference.md#2-universal-bytecode-apis).