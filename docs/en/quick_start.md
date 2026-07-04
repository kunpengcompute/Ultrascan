# Quick Start

## Prerequisites

You have installed and compiled Ultrascan by following instructions in [Installation Guide](./installation_guide.md).

## Universal Bytecode Generation Tool hsdump

You can use hsdump to compile rule-set bytecode that can be deployed on both the Kunpeng and x86 computing platforms. hsdump is a debugging tool provided by Ultrascan, used to dump internal information during the pattern compilation process. By using hsdump, bytecode that supports cross-platform deployment can be compiled.

1. Go to the created `build` directory.

    ```bash
    cd build
    ```

2. Create a file that contains regular expressions, for example, `patterns.txt`. The format is as follows:

    ```text
    1:/hatstand.*teakettle/s
    2:/(hatstand|teakettle)/iH
    ```

3. Run hsdump to generate the debugging information for the universal bytecode.

    ```bash
    ./bin/hsdump -e patterns.txt -o ./dump_output -U -X -N
    ```

    Parameter description:

    - `-e PATH`: specifies the path to the rule file.
    - `-o PATH`: specifies the output directory. The default value is the current directory.
    - `-U`/`--dump_db`: dumps the final database (in the universal bytecode format).
    - `-N`/`--block`: uses the block mode for compilation. (The stream mode is used by default.)
    - `-X`/`--no_intermediate`: does not dump intermediate data.

4. Check the output result.

    hsdump generates a universal bytecode file `db.raw` in the specified output directory.

## Universal Bytecode Performance Test Tool hsbench

hsbench is a performance benchmark test tool provided by Ultrascan. You can compare the performance before and after KHSEL is used based on the hsbench test results.

1. Go to the created `build` directory.

    ```bash
    cd build
    ```

2. Obtain the [hsbench rule sets](https://cdrdv2.intel.com/v1/dl/getContent/739375), and decompress them to the `build/hsbench-samples` directory.

3. Run hsbench.

    Not using the universal bytecode:

    ```bash
    ./bin/hsbench -e ./hsbench-samples/pcre/snort_literals -c ./hsbench-samples/corpora/gutenberg.db -N -n1
    ```

    Using the universal bytecode:

    ```bash
    ./bin/hsbench -U ./dump_output/db.raw -c ./hsbench-samples/corpora/gutenberg.db -N -n1
    ```
        
    Parameter description:

    - `-e PATH`: specifies the path to the rule file.
    - `-U`/`--dump_db`: uses the database in the universal bytecode format.
    - `-c`/`--corpus`: specifies the path to the test database.
    - `-N`/`--block`: uses the block mode for compilation. (The stream mode is used by default.)
    - `-n`/`--num_iterations`: specifies the number of test iterations (default value: `1`).

    Running result (using the rule sets):

    <img src="figures/en-us_image_0000002550013885.png" style="width: 60%; height: auto;" />

    Running result (using the universal bytecode):
    ![](figures/5.png)

    Parameters in the test results:

    - `Time spent scanning`: time taken to scan the target database using the target rule set
    - `Matches per iteration`: number of matches in each iteration using the rule set
    - `Mean throughput (overall)`: average throughput (Mbit/s)
    - `Max throughput (per core)`: maximum throughput among all CPU cores (Mbit/s)
