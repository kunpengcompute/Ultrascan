# Quick Start<a name="EN-US_TOPIC_0000002550013877"></a>

hsbench is a performance benchmark tool provided by Hyperscan. You can compare the performance before and after KHSEL is used based on hsbench test results.

1. Go to the  **build**  directory.

    ```
    cd build
    ```

2. Obtain  [hsbench rule sets](https://cdrdv2.intel.com/v1/dl/getContent/739375)  and input data, and decompress them to the  **build/hsbench-samples**  directory.
3. Run hsbench.

    ```
    ./bin/hsbench -e ./hsbench-samples/pcre/snort_literals -c ./hsbench-samples/corpora/gutenberg.db -N -n1
    ```

    Test results:

    ![](figures/en-us_image_0000002550013885.png)

    Parameters in the test results:

    - **Time spent scanning**: time taken to scan the target database using the target rule set
    - **Matches per iteration**: number of matches in each iteration using the rule set
    - **Mean throughput \(overall\)**: average throughput \(Mbit/s\)
    - **Max throughput \(per core\)**: maximum throughput among each CPU core \(Mbit/s\)

