# Hyperscan Fuzz 测试框架

## 1. 项目介绍

Hyperscan Fuzz 测试框架是一个专门为测试 Hyperscan 正则表达式引擎而设计的模糊测试系统。它通过生成大量随机的正则表达式模式和测试数据，全面测试 Hyperscan 的各种接口和功能，以发现潜在的问题和边界情况。

### 1.1 测试目标

- 验证 Hyperscan 各种接口的稳定性和正确性
- 发现潜在的崩溃、内存泄漏和性能问题
- 测试边界情况和异常输入
- 确保不同平台上的一致性行为

## 2. 测试框架结构

```
fuzz/
├── data/            # 测试数据生成器
│   ├── data_generator.cpp
│   └── data_generator.h
├── generator/       # 测试用例生成器
│   └── python_generator.cpp
├── runner/          # 测试运行器
│   └── hs_runner.cpp
├── CMakeLists.txt   # 构建配置
├── fuzz_test.cpp    # 测试主逻辑
├── fuzz_test.h      # 测试接口定义
└── main.cpp         # 测试入口
```

### 2.1 核心组件

- **测试用例生成器**：负责生成各种正则表达式测试用例
- **数据生成器**：负责生成各种类型的测试数据
- **测试运行器**：负责执行测试并验证结果
- **测试框架**：基于 Google Test 框架组织测试流程

## 3. 安装和依赖

### 3.1 系统要求

- C++11 或更高版本的编译器
- CMake 3.10 或更高版本
- Python 3.x（用于生成测试用例）
- Google Test 框架（已包含在测试目录中）

### 3.2 构建步骤

1. 确保已构建 Hyperscan 库
2. 进入 fuzz 测试目录
3. 创建并进入构建目录
4. 运行 CMake 配置
5. 编译测试

```bash
cd unit/fuzz
mkdir build && cd build
cmake ..
make
```

## 4. 使用方法

### 4.1 运行测试

```bash
# 运行所有测试
./fuzz_test

# 运行特定类型的测试
./fuzz_test --gtest_filter=HyperscanFuzzTest.AllInterfaces
```

### 4.2 测试参数配置

测试框架使用预定义的测试参数配置，位于 `fuzz_test.cpp` 文件中：

```cpp
static const FuzzTestParams testParams[] = {
    {"aristocrats", 10, 10000000, false},
    {"completocrats", 10, 10000000, false},
    {"heuristocrats", 10, 10000000, false}
};
```

参数说明：
- **generatorType**：生成器类型（aristocrats, completocrats, heuristocrats）
- **depth**：生成深度，控制正则表达式的复杂度
- **count**：测试用例数量
- **fullCharset**：是否使用完整字符集

## 5. 测试用例生成

### 5.1 生成器类型

- **aristocrats**：生成更复杂的正则表达式
- **completocrats**：生成更全面的正则表达式覆盖
- **heuristocrats**：基于启发式方法生成正则表达式

### 5.2 测试用例格式

测试用例格式为：`ID:/pattern/flags`

- **ID**：测试用例唯一标识符
- **pattern**：正则表达式模式
- **flags**：编译标志，如 `i`（大小写不敏感）、`m`（多行模式）等

## 6. 测试数据生成

测试框架生成四种类型的测试数据：

- **随机文本**：包含字母和数字
- **二进制数据**：0-255的随机字节
- **特殊字符**：各种标点符号和特殊符号
- **边界数据**：包含空字符、换行符、回车符等边界情况

每种数据类型的长度在 0-1024 之间随机生成。

## 7. 测试接口覆盖

### 7.1 编译接口

- `hs_compile`：编译单个正则表达式
- `hs_compile_multi`：编译多个正则表达式
- `hs_compile_ext_multi`：支持扩展参数的编译
- `hs_compile_lit`：编译纯字面量表达式
- `hs_compile_lit_multi`：编译多个纯字面量表达式

### 7.2 运行时接口

- `hs_scan`：在数据块上执行匹配
- `hs_scan_stream`：在流上执行匹配
- `hs_scan_vector`：在分散的数据上执行匹配

### 7.3 流操作接口

- `hs_reset_stream`：重置流状态
- `hs_copy_stream`：复制流状态
- `hs_reset_and_copy_stream`：重置并复制流状态
- `hs_compress_stream`：压缩流状态
- `hs_expand_stream`：扩展流状态
- `hs_reset_and_expand_stream`：重置并扩展流状态

### 7.4 工具接口

- `hs_expression_info`：获取表达式信息
- `hs_expression_ext_info`：获取带扩展参数的表达式信息
- `hs_populate_platform`：获取平台信息
- `hs_clone_scratch`：克隆临时内存
- `hs_scratch_size`：获取临时内存大小

## 8. 测试结果分析

### 8.1 测试输出

测试运行时会输出详细的测试信息，包括：
- 生成的测试用例数量
- 生成的测试数据数量
- 每个测试用例的执行情况
- 每个接口的测试状态

### 8.2 错误处理

测试框架会捕获并报告以下类型的错误：
- 编译错误：正则表达式编译失败
- 运行时错误：扫描过程中的错误
- 内存错误：内存分配和释放问题
- 崩溃：程序异常终止

## 9. 示例输出

```
Generated 1000 test cases
Generated 10 test data items

=== 测试用例 1 ===
测试 hs_compile...
测试 hs_scan...
测试 hs_scan_stream...
测试 hs_compile_lit...
测试 hs_expression_info...
测试 hs_expression_ext_info...
测试 hs_reset_stream...
测试 hs_copy_stream...
测试 hs_reset_and_copy_stream...
测试 hs_compress_stream...
测试 hs_expand_stream...
测试 hs_reset_and_expand_stream...
测试 hs_scan_vector...
测试 hs_clone_scratch...
测试 hs_scratch_size...

=== 测试多模式接口 ===
测试 hs_compile_multi...
测试 hs_compile_ext_multi...
测试 hs_compile_lit_multi...

=== 测试平台接口 ===
测试 hs_populate_platform...
```

## 9. 查看覆盖率

```bash
    #  在执行fuzz测试后的当前路径下执行
    lcov --capture --directory . --output-file coverage.info

    lcov --remove coverage.info '*/gtest/*' '/usr/*' --output-file coverage.info.cleaned
```