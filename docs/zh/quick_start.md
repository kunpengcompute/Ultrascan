# 快速入门<a name="ZH-CN_TOPIC_0000002550013877"></a>

hsbench是Hyperscan官方提供的性能Benchmark工具，通过hsbench的测试结果能够对比使用KHSEL库前后的性能差异。

1. 进入创建好的`build`目录。

    ```bash
    cd build
    ```

2. 获取[hsbench规则集](https://cdrdv2.intel.com/v1/dl/getContent/739375)并输入数据，并解压到`build/hsbench-samples`目录。
3. 运行hsbench。

    ```bash
    ./bin/hsbench -e ./hsbench-samples/pcre/snort_literals -c ./hsbench-samples/corpora/gutenberg.db -N -n1
    ```

    运行结果：

    ![](figures/zh-cn_image_0000002550013885.png)

    运行结果参数说明如下：

    - Time spent scanning：使用目标规则集扫描目标数据库，扫描所用的时间。
    - Matches per iteration：每次迭代，按规则集匹配命中的数量。
    - Mean throughput \(overall\)：平均吞吐量（Mbit每秒）。
    - Max throughput \(per core\)：所有CPU核中的最大吞吐量（Mbit每秒）。
