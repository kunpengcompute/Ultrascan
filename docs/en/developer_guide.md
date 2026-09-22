# Developer Guide

<!-- md-trans-meta sourceCommit=fd215461e79615cfea45605856a99c32a14a7e4b translatedAt=2026-08-27T01:39:54.891Z pushedAt=2026-09-09T07:59:00.983Z -->

## Document Purpose

This document is intended for developers who need to understand or maintain the internal implementation of Ultrascan, and mainly describes the internal APIs of KHSEL.

The function definitions, parameters, return values, lifecycles, and minimal usage examples of the newly added public APIs in Ultrascan are detailed in [API Reference](./api_reference.md).

This developer guide does not maintain the item-by-item definitions of these public APIs; the public header files and the API reference prevail.

## KHSEL Internal Function Description

>![](public_sys-resources/icon-notice.gif) **NOTICE:**
>The functions in KHSEL (`KHSEL_xxx`) are internal Ultrascan APIs, and applications do not need to call them explicitly.

The optimized functions in KHSEL are as follows:

|Function|Description|
|--|--|
| `KHSEL_BuildLily` | Compilation function for short-rule (single byte) matching. |
| `KHSEL_LilyRunExec` | Execution function for short-rule (single byte) matching. |
| `KHSEL_BuildLilyForTeddy` | Compilation function for short-rule (2–4 bytes) matching. |
| `KHSEL_LilyForTeddyRunExec` | Execution function for short-rule (2–4 bytes) matching. |

The KHSEL source code has been integrated into the `src/kunpeng-enhanced` directory of the Ultrascan repository. Therefore, you do not need to install the KHSEL software package separately.

### `KHSEL_BuildLily`

Function description: Compiles single-byte rules in the rule set and outputs a mask after compilation.

```c++
std::vector<u8> KHSEL_BuildLily(
    std::map<char, lilyReport> &lily,
    std::vector<u32> &reportVec,
    std::vector<u32> &ekeyVec,
    u8 &flagsQuiet);
```

| Parameter | Description | Input/Output |
| --- | --- | --- |
| `lily` | Single-byte rule set. | Input |
| `reportVec` | Report IDs corresponding to the single-byte rules. | Output |
| `ekeyVec` | ekeys corresponding to the single-byte rules. | Output |
| `flagsQuiet` | Quiet-mode bitmask of the single-byte rules, with each bit corresponding to one rule. | Output |

A mask is returned after rule compilation.

### `KHSEL_LilyRunExec`

Function description: Matches the runtime input data based on the mask output from compilation.

```c
hs_error_t KHSEL_LilyRunExec(const struct RoseEngine *rose,
                             hs_scratch_t *scratch);
```

| Parameter | Description | Input/Output |
| --- | --- | --- |
| `rose` | RoseEngine object that stores the compile-time output. It must be non-empty. | Input |
| `scratch` | Temporary memory space used for the current scan. It must be non-empty. | Input |

Return value description:

| Return Value | Description |
| --- | --- |
| `0` (`KHSEL_MATCHING_SUCCESS`) | Matching is completed normally. |
| `1` (`KHSEL_MATCHING_TERMINATED`) | Matching is terminated prematurely, for example, when the user callback function requests to stop matching. |

### `KHSEL_BuildLilyForTeddy`

Function description: Compiles 2-to-4-byte rules in the rule set and outputs the lilyTeddy bytecode after compilation.

```c++
ue2::bytecode_ptr<lilyTeddy> KHSEL_BuildLilyForTeddy(
    std::map<std::string, lilyReport> &lilyForTeddy,
    std::priority_queue<LilyForTeddyPair,
                        std::vector<LilyForTeddyPair>,
                        CompareStringLength> &lilyForTeddyPQ,
    std::vector<u32> &reportVec,
    std::vector<u32> &ekeyVec,
    std::vector<u32> &lenVec);
```

| Parameter | Description | Input/Output |
| --- | --- | --- |
| `lilyForTeddy` | 2-to-4-byte rule set. | Input |
| `lilyForTeddyPQ` | Priority queue sorted by rule length. | Input |
| `reportVec` | Report IDs corresponding to the rules. | Output |
| `ekeyVec` | ekeys corresponding to the rules. | Output |
| `lenVec` | Lengths corresponding to the rules. | Output |

The lilyTeddy bytecode after rule compilation is returned.

### `KHSEL_LilyForTeddyRunExec`

Function description: Matches the runtime input data based on the mask output from compilation.

```c
hs_error_t KHSEL_LilyForTeddyRunExec(const struct RoseEngine *rose,
                                     hs_scratch_t *scratch);
```

| Parameter | Description | Input/Output |
| --- | --- | --- |
| `rose` | RoseEngine object that stores the compile-time output. It must be non-empty. | Input |
| `scratch` | Temporary memory space used for the current scan. It must be non-empty. | Input |

Return value description:

| Return Value | Description |
| --- | --- |
| `0` (`KHSEL_MATCHING_SUCCESS`) | Matching is completed normally. |
| `1` (`KHSEL_MATCHING_TERMINATED`) | Matching is terminated prematurely, for example, when the user callback function requests to stop matching. |

## Change History

|Version|Date|Description|
|--|--|--|
| 03 | 2026-09-30 | This is the third official release (V5.8.0): Added the mcsheng algorithm performance optimization and feedback-driven optimization for regular expression matching. |
| 02 | 2026-06-30 | This is the second official release: Added the universal bytecode function. |
| 01 | 2026-03-30 | This is the first official release: Optimized the Ultrascan short-byte (2–4 bytes) rule matching algorithm, and added `KHSEL_BuildLilyForTeddy` and `KHSEL_LilyForTeddyRunExec`. |
