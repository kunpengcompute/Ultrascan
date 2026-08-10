# Hyperscan Fuzz 测试

`unit/fuzz` 是一个基于 Google Test 的 fuzz 测试程序。它会调用
`tools/fuzz` 下的 Python 生成器产生正则表达式，再用随机数据驱动
Hyperscan 的编译、扫描、stream、scratch、fat compile 等接口。当前还覆盖
生成器 record 解析、QUIET-only 断言问题的确定性回归，以及假阳性反馈 collector、
带 collector 扫描、feedback 生成和反馈重编译等公开 API。

注意：这个 fuzz 不是 libFuzzer 入口形式，不需要 `LLVMFuzzerTestOneInput`。

## 1. 目录结构

```text
unit/fuzz/
├── data/                  # 扫描数据生成器
├── generator/             # 调用 tools/fuzz/*.py 生成 pattern
├── runner/                # Hyperscan API 调用封装
├── CMakeLists.txt         # fuzz 可执行程序构建脚本
├── fuzz_test.cpp          # Google Test 主测试逻辑
├── fuzz_test.h            # Generator/Runner 接口定义
└── main.cpp               # gtest main 和超时监控
```

## 2. 依赖

ARM 机器上需要：

- C/C++ 编译器
- CMake
- Python。默认命令为 `python`；其他命令或路径可通过 `HS_FUZZ_PYTHON` 指定

- lcov/genhtml（可选），用于覆盖率报告

## 3. 构建和运行

下面是推荐的长时间 fuzz 构建。假阳性反馈路径需要在 AArch64 构建项目本体时开启
`HS_ENABLE_FP_FEEDBACK`：

```bash
cd /path/to/Ultrascan
mkdir -p build_cov
cd build_cov
cmake .. -DHS_ENABLE_FP_FEEDBACK=ON 
make -j$(nproc)
```

再构建 fuzz。标准目录布局会自动使用项目根目录下的 `build_cov`：

```bash
cd ../unit/fuzz
mkdir -p build
cd build
cmake .. 
make -j$(nproc)
```

运行：

```bash
./hyperscan_fuzz_test
```

正常构建和运行不需要设置任何环境变量。若本体没有开启反馈功能，程序会探测到
`HS_ARCH_ERROR`，继续执行普通 fuzz，并自动跳过反馈路径。

与 fuzz 直接相关的编译选项如下。项目本体和 fuzz 是两个独立 CMake 工程，所以
`CMAKE_BUILD_TYPE` 应分别配置；fuzz 的编译类型不会改变已经链接的 `libhs`。

| 配置位置 | 选项 | 默认值 | 作用和用法 |
| --- | --- | --- | --- |
| 项目本体 | `CMAKE_BUILD_TYPE` | `RelWithDebInfo` | `RelWithDebInfo` 适合长时间 fuzz；`Debug` 适合定位断言和崩溃；`Release` 用于无调试符号的性能运行；`MinSizeRel` 优化体积，不建议用于 fuzz。 |
| 项目本体 | `HS_ENABLE_FP_FEEDBACK` | `OFF` | AArch64 上设为 `ON` 才会执行假阳性反馈 fuzz；x86 不支持设为 `ON`。 |
| 项目本体 | `HS_ARM_MARCH` | `AUTO` | AArch64 指令集目标；本机 fuzz 通常使用 `AUTO`，跨机器运行使用 `PORTABLE`，也可指定 `native` 或具体架构值（如 `armv8.2-a+crc+sve`）。 |
| 项目本体 | `OPTIMISE` | `ON` | 控制项目自身的 `-O2/-O3`。仅设置 `CMAKE_BUILD_TYPE=Debug` 不会关闭它；需要低优化调试时同时设置 `-DOPTIMISE=OFF`。 |
| 项目本体 | `DEBUG_OUTPUT` | `OFF` | 开启内部调试输出并自动关闭 `OPTIMISE`，日志非常多，只用于定向排障。 |
| 项目本体 | `ENABLE_COVERAGE` | `OFF` | 为本体添加 gcov 覆盖率参数；应与 `-DOPTIMISE=OFF` 一起使用。 |
| 项目本体 | `BUILD_SHARED_LIBS` | `OFF` | `ON` 时构建共享 `libhs`；默认静态库即可运行 fuzz。 |
| 项目本体 | `BUILD_STATIC_AND_SHARED` | `OFF` | `ON` 时同时构建静态库和共享库，普通 fuzz 不需要。 |
| fuzz | `CMAKE_BUILD_TYPE` | 未设置 | 建议与本体一致；`Debug` 便于调试 fuzz 驱动，`RelWithDebInfo` 适合长时间运行。 |
| fuzz | `HS_BUILD_DIR` | `../../build` | 指定要测试的 Hyperscan 构建目录。只有本体不在标准 `build` 目录时才需要设置。 |

`unit/fuzz/CMakeLists.txt` 当前始终为 fuzz 可执行文件加入 gcov 参数；顶层
`ENABLE_COVERAGE` 只决定 Hyperscan 本体是否生成覆盖率数据，与 `Debug` 是否开启
没有直接关系。

需要关闭优化定位崩溃时，分别这样配置：

```bash
# 项目本体
cmake .. -DHS_ENABLE_FP_FEEDBACK=ON \
  -DCMAKE_BUILD_TYPE=Debug -DOPTIMISE=OFF

# unit/fuzz/build
cmake .. -DCMAKE_BUILD_TYPE=Debug
```

当前 fuzz 支持的运行环境变量如下，全部都是可选项：

| 环境变量 | 默认值 | 作用 |
| --- | --- | --- |
| `HS_FUZZ_COUNT` | `10000000` | 每个 Python 生成器产生的 pattern 数；共有 3 个生成器。必须是正整数，`0` 或非法值会回退到默认值。 |
| `HS_FUZZ_THREADS` | `1` | 单 pattern 用例的工作线程数；大于 `1` 时启用生产者/消费者并行执行。 |
| `HS_FUZZ_QUEUE_SIZE` | `4096` | 并行模式下的待执行队列容量；单线程时无效，`0` 回退到默认值。 |
| `HS_FUZZ_MULTI_LIMIT` | `1024` | 送入 multi 编译接口的 pattern 上限；`0` 禁用 multi 接口 fuzz。 |
| `HS_FUZZ_FP_LIMIT` | `256` | 每个生成器执行完整 collector/feedback/recompile 流程的用例上限；`0` 禁用逐用例反馈流程。 |
| `HS_FUZZ_PYTHON` | `python` | Python 命令或可执行文件路径，例如 `python3` 或 `/usr/bin/python3`。 |
| `HS_FUZZ_VERBOSE` | `0` | 设为非 `0` 值时输出逐 API 详细日志；默认只输出进度、错误和汇总。 |
| `HS_FUZZ_MAX_UNIQUE_ERRORS` | `30` | 汇总中每个 API 最多展示的不同错误信息数；只限制输出，不限制执行。 |
| `HS_FUZZ_TRACE_CASE` | `0` | 设为非 `0` 值时，在标准输出打印每个用例的 begin/end、ID、flags 和 pattern。 |
| `HS_FUZZ_TRACE_DIR` | 未设置 | 将各 worker 当前执行的用例和阶段写入 `<目录>/worker_N.current`；目录需预先创建，崩溃后可用残留文件定位。 |

常用运行示例：

```bash
# 快速验证：每个生成器执行 100 个 pattern
HS_FUZZ_COUNT=100 ./hyperscan_fuzz_test

# 并行执行，并限制高开销的 multi 和反馈路径
HS_FUZZ_THREADS=8 HS_FUZZ_MULTI_LIMIT=512 HS_FUZZ_FP_LIMIT=128 \
  ./hyperscan_fuzz_test

# 为崩溃定位保留每个 worker 的当前用例和执行阶段
mkdir -p /tmp/hs-fuzz-trace
HS_FUZZ_TRACE_DIR=/tmp/hs-fuzz-trace ./hyperscan_fuzz_test
```

## 4. 覆盖率编译

如果要看 `src/hs.cpp`、`src/fat_database.c` 等源码覆盖率，必须让 Hyperscan
本体也带 coverage 参数编译。顶层 `CMakeLists.txt` 中使用
`ENABLE_COVERAGE` 开关。

推荐的 coverage block 如下，必须包含 `CMAKE_C_FLAGS`，否则 C 文件
`src/fat_database.c` 不会生成 `.gcno/.gcda`：

```cmake
if(ENABLE_COVERAGE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -g -fprofile-arcs -ftest-coverage")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -g -fprofile-arcs -ftest-coverage")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fprofile-arcs -ftest-coverage")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fprofile-arcs -ftest-coverage")
    message(STATUS "Coverage compilation options enabled")
else()
    message(STATUS "Coverage compilation options disabled")
endif()
```

重新构建 Hyperscan 本体：

```bash
export HS_ROOT=/path/to/hyperscan
cd "$HS_ROOT"
rm -rf build_cov
mkdir build_cov
cd build_cov

cmake .. -DHS_ENABLE_FP_FEEDBACK=ON -DENABLE_COVERAGE=ON \
  -DCMAKE_BUILD_TYPE=Debug -DOPTIMISE=OFF
make -j$(nproc)
```

然后重新构建并运行 fuzz：

```bash
cd "$HS_ROOT/unit/fuzz"
rm -rf build
mkdir build
cd build

cmake ..
make -j$(nproc)
./hyperscan_fuzz_test
```

运行后确认已产生 `.gcda`：

```bash
cd "$HS_ROOT"
find build_cov -name '*.gcda' | head
find build_cov -name '*.gcda' | grep fat_database
```

## 5. 生成全量覆盖率报告

在项目根目录执行：

```bash
cd "$HS_ROOT"
rm -f coverage.info coverage.info.cleaned

lcov --capture \
  --directory build_cov \
  --directory unit/fuzz/build \
  --ignore-errors inconsistent,inconsistent \
  --output-file coverage.info
```

过滤 gtest 和系统头文件：

```bash
lcov --remove coverage.info \
  '*/gtest/*' \
  '/usr/*' \
  --ignore-errors unused,mismatch,inconsistent \
  --output-file coverage.info.cleaned
```

生成全量 HTML：

```bash
genhtml coverage.info.cleaned \
  --ignore-errors inconsistent,inconsistent \
  --output-directory coverage_report
```

报告入口：

```text
$HS_ROOT/coverage_report/index.html
```

## 6. 常见问题

### 6.1 `lcov: ERROR: (empty) no .gcda files found in build`

原因通常是 Hyperscan 本体没有带 coverage 编译，或者 fuzz 没有正常跑完。

检查：

```bash
find build_cov -name '*.gcno' | head
find build_cov -name '*.gcda' | head
```

如果没有 `.gcno`，重新使用：

```bash
cmake .. -DHS_ENABLE_FP_FEEDBACK=ON -DENABLE_COVERAGE=ON \
  -DCMAKE_BUILD_TYPE=Debug -DOPTIMISE=OFF
```

如果有 `.gcno` 但没有 `.gcda`，先运行 fuzz：

```bash
cd "$HS_ROOT/unit/fuzz/build"
./hyperscan_fuzz_test
```

### 6.2 `coverage.info` 不能读取

说明上一步 `lcov --capture` 没有成功生成 `coverage.info`。先重新 capture，
不要直接执行 remove/extract：

```bash
lcov --capture \
  --directory build_cov \
  --directory unit/fuzz/build \
  --ignore-errors inconsistent,inconsistent \
  --output-file coverage.info
```

### 6.3 `fat_database.c` 不在 HTML 里

先确认 tracefile 里有没有记录：

```bash
grep -n 'SF:.*fat_database.c' coverage.info coverage.info.cleaned 2>/dev/null
```

再确认编译和运行数据：

```bash
find build_cov -name '*.gcno' | grep fat_database
find build_cov -name '*.gcda' | grep fat_database
```

如果 `.gcno` 没有，说明顶层 `CMakeLists.txt` 没给 C 文件加 coverage 参数，
需要检查 `CMAKE_C_FLAGS`。

### 6.4 lcov 报 `inconsistent`

Google Test 宏和新版 lcov/gcov 组合可能出现行号不一致，例如：

```text
lcov: ERROR: (inconsistent) mismatched end line ...
```

使用：

```bash
--ignore-errors inconsistent,inconsistent
```

### 6.5 fuzz 链接了错误的 libhs

标准目录布局会自动使用项目根目录下的 `build_cov`，CMake 配置阶段会打印实际选中的
Hyperscan 库路径。只有根库位于其他目录时才需要显式指定，例如：

```bash
cmake .. -DHS_BUILD_DIR=/path/to/nonstandard-hyperscan-build
```

如果配置输出仍指向旧库，删除独立 fuzz 构建目录或清理其中的 CMake cache
后再重新配置。

## 7. 当前覆盖接口概览

编译接口：

- `hs_compile`
- `hs_compile_multi`
- `hs_compile_ext_multi`
- `hs_compile_lit`
- `hs_compile_lit_multi`
- `fat_hs_compile`
- `fat_hs_compile_multi`
- `fat_hs_compile_ext_multi`
- `fat_hs_compile_lit`
- `fat_hs_compile_lit_multi`

运行时接口：

- `hs_scan`
- `hs_scan_stream`
- `hs_scan_vector`
- `hs_scan_with_collector`
- `hs_scan_stream_with_collector`
- `hs_scan_vector_with_collector`

假阳性反馈接口：

- `hs_fp_collector_create`
- `hs_fp_collector_reset`
- `hs_fp_collector_merge`
- `hs_fp_collector_free`
- `hs_fp_collector_to_feedback`
- `hs_fp_collector_to_feedback_with_dump`
- `hs_fp_feedback_free`
- `hs_compile_multi_with_feedback`
- `hs_compile_ext_multi_with_feedback`

定向回归：

- Python 生成器 `id:/pattern/flags` record 解析，包含 pattern 正文中的 `/`
- QUIET 非组合表达式在 block、stream、vectored 模式下编译和扫描成功，且不产生 callback
- 普通扫描、带 collector 扫描和反馈重编译扫描的完整 callback 多重集保持一致

stream 操作：

- `hs_reset_stream`
- `hs_copy_stream`
- `hs_reset_and_copy_stream`
- `hs_compress_stream`
- `hs_expand_stream`
- `hs_reset_and_expand_stream`

工具接口：

- `hs_expression_info`
- `hs_expression_ext_info`
- `hs_populate_platform`
- `hs_clone_scratch`
- `hs_scratch_size`

fat database 相关接口：

- `fat_hs_database_size`
- `fat_hs_database_info`
- `fat_hs_serialize_database`
- `fat_hs_serialized_database_size`
- `fat_hs_deserialize_database`
- `fat_hs_deserialize_database_at`
- `fat_hs_free_database`
