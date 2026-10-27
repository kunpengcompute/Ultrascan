# Quick Start

<!-- md-trans-meta sourceCommit=3d692a8b20476bbb17920b3af025a57bbac9b0bc translatedAt=2026-08-27T01:39:47.359Z pushedAt=2026-09-09T02:47:21.115Z -->

## Prerequisites

You have installed and compiled Ultrascan by following instructions in [Installation Guide](./installation_guide.md).

## hsdump—Universal Bytecode Generation Tool<a id="hsdump-universal-bytecode-generation-tool"></a>

You can use hsdump to compile rule-set bytecode that can be deployed on both the Kunpeng and x86 computing platforms. hsdump is a debugging tool provided by Ultrascan, used to dump internal information during the pattern compilation process. By using hsdump, bytecode that supports cross-platform deployment can be compiled.

1. Prepare the rule file. Write regular expressions in `/opt/Ultrascan/build-debug/patterns.txt` in the following format:

    ```text
    1:/hatstand.*teakettle/s
    2:/(hatstand|teakettle)/iH
    ```

2. Run hsdump to generate the debugging information for the universal bytecode.

    ```bash
    cd /opt/Ultrascan/build-debug
    ./bin/hsdump -e ./patterns.txt -o ./dump_output -U -X -N
    ```

    Parameter description:

    - `-e PATH`: specifies the path to the rule file.
    - `-o PATH`: specifies the output directory. The default value is the current directory.
    - `-U`/`--dump_db`: dumps the final database (in the universal bytecode format).
    - `-N`/`--block`: uses the block mode for compilation. (The stream mode is used by default.)
    - `-X`/`--no_intermediate`: does not dump intermediate data.
    - `-G OVERRIDES`: sets the Grey compilation parameter for the current process, for example `-G "allowLily:1;"`.

3. Check the output result.

    hsdump generates a universal bytecode file `/opt/Ultrascan/build-debug/dump_output/db.raw`.

## hsbench—Universal Bytecode Performance Test Tool<a id="hsbench-universal-bytecode-performance-test-tool"></a>

hsbench is a benchmark performance test tool provided by Ultrascan. It can be used to observe the matching throughput, hit count, and database resource overhead on a specified rule set and corpus.

1. Obtain the [hsbench rule set](https://cdrdv2.intel.com/v1/dl/getContent/739375) and extract it to the `/opt/Ultrascan/hsbench-samples` directory. This directory is prepared by the user and is not part of the Ultrascan build outputs.

2. Run hsbench.

    Not using the universal bytecode:

    ```bash
    cd /opt/Ultrascan/build
    ./bin/hsbench \
        -e ../hsbench-samples/pcre/snort_literals \
        -c ../hsbench-samples/corpora/gutenberg.db -N -n 1
    ```

    Using the universal bytecode:

    ```bash
    ./bin/hsbench \
        -U /opt/Ultrascan/build-debug/dump_output/db.raw \
        -c ../hsbench-samples/corpora/gutenberg.db \
        -N -n 1
    ```

    Parameter description:

    - `-e PATH`: specifies the path to the rule file.
    - `-U`/`--dump_db`: uses the database in the universal bytecode format.
    - `-c`/`--corpus`: specifies the path to the test database.
    - `-N`/`--block`: uses the block mode for compilation. (The stream mode is used by default.)
    - `-n`/`--num_iterations`: specifies the number of test iterations (default value: `1`).
    - `-G OVERRIDES`: sets the Grey parameter for rule compilation. When loading a generated database, ensure that its compilation configuration meets expectations.

    Running result (using the rule set):

    <img src="figures/en-us_image_0000002550013885.png" style="width: 60%; height: auto;" />

    Running result (using the universal bytecode):
    ![](figures/5.png)

    Parameters in the test results:

    - `Time spent scanning`: time taken to scan the target database using the target rule set
    - `Matches per iteration`: number of matches in each iteration using the rule set
    - `Mean throughput (overall)`: average throughput (Mbit/s)
    - `Max throughput (per core)`: maximum throughput among all CPU cores (Mbit/s)

## hspgo—Feedback-driven Optimization Tool for Regular Expression Matching<a id="hspgo-feedback-driven-optimization-tool-for-regular-expression-matching"></a>

`hspgo` is used to demonstrate and evaluate the complete closed loop of feedback-driven optimization for regular expression matching, including baseline database compilation, corpus collection, filtering to obtain feedback, database recompilation, and throughput testing after the new database takes effect.

### Prerequisites

- Ultrascan is built with `-DHS_ENABLE_FP_FEEDBACK=ON` on the AArch64 platform.
- SQLite 3 has been installed in the build environment; otherwise, `hspgo` will not be generated.
- Prepare a rule file or directory in hsbench format, as well as an SQLite corpus.

For specific build commands, see the "Compiling Ultrascan" step in [Installation Guide](./installation_guide.md).

### Collection and Feedback-based Compilation

The following commands use the block mode and explicitly specify `-b 1 -n 5` (both default to `20`, consistent with the default value of hsbench `-n`). They first run one round of baseline measurement for comparison, then perform the tool's built-in feedback collection, and finally run five rounds of measurement after switching to the database compiled based on the feedback. The example lowers the filtering threshold to 1 to facilitate workflow observation on a small corpus. In production environments, use the default threshold initially, and then fine-tune it based on the dump results.

```bash
cd /opt/Ultrascan/build-feedback
./bin/hspgo \
    -e /opt/Ultrascan/hsbench-samples/pcre/teakettle_2500 \
    -c /opt/Ultrascan/hsbench-samples/corpora/gutenberg.db \
    -N -b 1 -n 5 \
    -m 1 -p 1 -q 0 -s 0 -k 3 \
    -o ./hspgo-output \
    -O ./hspgo-feedback \
    -G "allowLily:1;allowNeoFdr:0"
```

Main parameters:

| Parameter | Description |
| --- | --- |
| `-e PATH` | Rule file or directory. |
| `-c FILE` | hsbench SQLite corpus. |
| `-N`/`-V` | Block/vectored mode. Streaming mode is used when this parameter is not specified. |
| `-b N` | Number of baseline scan rounds. Defaults to `20` (consistent with the default value of hsbench `-n`). |
| `-n N` | Number of measurement rounds after switching to the database compiled based on feedback. Defaults to `20` (consistent with the default value of hsbench `-n`). |
| `-m N` | Minimum number of triggers. Defaults to `1000`. |
| `-p N` | Minimum number of false-positive triggers. Defaults to `1000`. |
| `-q RATIO` | Minimum false-positive ratio. The value ranges from 0 to 1, and defaults to `0.99`. |
| `-s RATIO` | Minimum global waste ratio. The value ranges from 0 to 1, and defaults to `0.05`. |
| `-k N` | Maximum number of bad fragments to keep. The value is unlimited by default. |
| `-v` | Outputs the summary, diagnosis information, and top-ranked fragments. |
| `-o DIR` | Outputs the report and feedback CSV file. |
| `-O DIR` | Outputs the reusable feedback binary file. |
| `-G OVERRIDES` | Sets the Grey parameter shared by this round's baseline and feedback-based compilation; `allowNeoFdr` is forcibly set to `0` with a warning output if a non-zero value is passed to it. |

The tool only counts the optimized throughput after the database switch; baseline rounds are not mixed into the optimization measurement results. The following shows an execution example.

```text
[root@localhost build-feedback]# ./bin/hspgo \
    -e /opt/Ultrascan/hsbench-samples/pcre/teakettle_2500 \
    -c /opt/Ultrascan/hsbench-samples/corpora/gutenberg.db \
    -N -b 1 -n 5 \
    -m 1 -p 1 -q 0 -s 0 -k 3 \
    -o ./hspgo-output \
    -O ./hspgo-feedback \
    -G "allowLily:1;allowNeoFdr:0"
hspgo feedback demo

HSPGO feedback configuration:
Scan mode:                  block
Threads:                    1
Baseline rounds:            1
Measurement rounds:         5
Source fingerprint:         0xb0834f6dac8a4b93
Feedback source:            collector
Feedback thresholds:        trigger>=1; false>=1; fp_rate>=0; fp_share>=0; topk=3
CSV output dir:             ./hspgo-output
Feedback output dir:        ./hspgo-feedback
Grey overrides:             allowLily:1;allowNeoFdr:0

Baseline database:
Signatures:                 /opt/Ultrascan/hsbench-samples/pcre/teakettle_2500
Ultrascan info:             Version: 5.8.0 Mode: BLOCK
Expression count:           2,500
Bytecode size:              3,256,880 bytes
Database CRC:               0x5f920441
Scratch size:               387,084 bytes
Compile time:               1.304 seconds
Peak heap usage:            35,565,568 bytes

Baseline scan:
Time spent scanning:        0.017 seconds
Corpus size:                6,701,044 bytes (3,280 blocks)
Matches per iteration:      3,771 (0.576 matches/kilobyte)
Overall block rate:         189,390.77 blocks/sec
Mean throughput (overall):  3,095.40 Mbit/sec
Max throughput (per core):  3,130.23 Mbit/sec

Feedback database:
Signatures:                 /opt/Ultrascan/hsbench-samples/pcre/teakettle_2500
Ultrascan info:             Version: 5.8.0 Mode: BLOCK
Expression count:           2,500
Bytecode size:              3,269,424 bytes
Database CRC:               0xca27a5d9
Scratch size:               387,807 bytes
Compile time:               1.376 seconds
Peak heap usage:            42,967,040 bytes

Hot switch: active database replaced by feedback-compiled DB.

Optimized measurement:
Time spent scanning:        0.082 seconds
Corpus size:                6,701,044 bytes (3,280 blocks)
Matches per iteration:      3,771 (0.576 matches/kilobyte)
Overall block rate:         201,131.02 blocks/sec
Mean throughput (overall):  3,287.29 Mbit/sec
Max throughput (per core):  3,291.47 Mbit/sec
Optimized/Baseline:         106.20%
```

### Feedback Reuse

Use `-I` to import feedback previously exported by the same version of `hspgo`, and skip re-collection.

```bash
cd /opt/Ultrascan/build-feedback
./bin/hspgo \
    -e /opt/Ultrascan/hsbench-samples/pcre/snort_literals \
    -c /opt/Ultrascan/hsbench-samples/corpora/gutenberg.db \
    -N -I ./hspgo-feedback -n 5
```

During import, the scanning mode, rule set, and Grey override string must be consistent with those used during export. `hspgo` verifies the tool-layer fingerprint; the public `hs_fp_feedback_t` itself does not carry this fingerprint. Therefore, users must ensure configuration consistency when integrating services directly with the APIs.

To integrate the closed-loop process into applications, see [Feedback-driven Optimization APIs for Regular Expression Matching](./api_reference.md#4-feedback-driven-optimization-apis-for-regular-expression-matching).
