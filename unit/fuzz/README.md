# Hyperscan Fuzz 测试

`unit/fuzz` 是一个基于 Google Test 的 fuzz 测试程序。它会调用
`tools/fuzz` 下的 Python 生成器产生正则表达式，再用随机数据驱动
Hyperscan 的编译、扫描、stream、scratch、fat compile 等接口。

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
- Python，最好保证 `python` 命令可用；如果系统只有 `python3`，需要把
  `unit/fuzz/generator/python_generator.cpp` 中的 `python` 改为 `python3`
- lcov/genhtml，用于覆盖率报告

## 3. 普通编译和运行

先在项目根目录构建 Hyperscan 本体：

```bash
export HS_ROOT=/path/to/hyperscan
cd "$HS_ROOT"
rm -rf build
mkdir build
cd build

cmake ..
make -j$(nproc)
```

再构建 fuzz：

```bash
cd "$HS_ROOT/unit/fuzz"
rm -rf build
mkdir build
cd build

cmake ..
make -j$(nproc)
```

运行 fuzz：

```bash
./hyperscan_fuzz_test
```

默认只输出每组生成器的汇总信息，不再打印每个 pattern、每次 scan、
每个 compile error 的详细日志。

如果需要排查具体失败路径，可以打开详细日志：

```bash
HS_FUZZ_VERBOSE=1 ./hyperscan_fuzz_test
```

汇总里的 `unique errors` 默认最多展示 30 类错误。需要查看更多时：

```bash
HS_FUZZ_MAX_UNIQUE_ERRORS=100 ./hyperscan_fuzz_test
```

只运行主测试：

```bash
./hyperscan_fuzz_test --gtest_filter='FuzzTests/HyperscanFuzzTest.AllInterfaces/*'
```

调试时建议先把 `unit/fuzz/fuzz_test.cpp` 里的 `count` 调小，例如 `100`。
`count` 表示每个生成器产生多少条正则表达式，不是匹配次数，也不是性能循环次数。

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
rm -rf build
mkdir build
cd build

cmake .. -DENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
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
find build -name '*.gcda' | head
find build -name '*.gcda' | grep fat_database
```

## 5. 生成全量覆盖率报告

在项目根目录执行：

```bash
cd "$HS_ROOT"
rm -f coverage.info coverage.info.cleaned

lcov --capture \
  --directory build \
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
find build -name '*.gcno' | head
find build -name '*.gcda' | head
```

如果没有 `.gcno`，重新使用：

```bash
cmake .. -DENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
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
  --directory build \
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
find build -name '*.gcno' | grep fat_database
find build -name '*.gcda' | grep fat_database
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

确认 fuzz 链接的是当前项目的库：

```bash
cd "$HS_ROOT/unit/fuzz/build"
ldd ./hyperscan_fuzz_test | grep hs
```

应该指向：

```text
$HS_ROOT/build/lib/libhs.so
```

如果指向 `/usr/lib` 或 `/usr/local/lib`，覆盖率不会进当前项目的 `build`。

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
