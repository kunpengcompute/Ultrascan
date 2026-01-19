# replaceAll_acc算子使用说明

## 项目简介

replaceAll_acc是一个基于ARM NEON SIMD指令集优化的字符串替换算子，专门用于高性能字符串处理场景。该算子使用Shufti算法进行字符匹配，在ARMv8架构服务器上提供优异的性能表现。

## 编译环境要求

- 操作系统：Linux (ARM64架构)
- 编译器：GCC 4.8.5或更高版本
- CMake版本：3.10或更高版本
- 依赖库：Google Test (用于测试)

## 源码编译方法

### 1. 编译主库

```bash
# 创建构建目录
mkdir build && cd build

# 配置CMake（默认Release模式，静态库）
cmake ..

# 编译
make

# 安装（可选）
make install
```

### 2. 编译选项说明

#### RELEASE/DEBUG模式切换

```bash
# Release模式（默认，优化编译）
cmake -DCMAKE_BUILD_TYPE=Release ..
make

# Debug模式（包含调试信息）
cmake -DCMAKE_BUILD_TYPE=Debug ..
make
```

#### 动态库/静态库切换

```bash
# 编译静态库（默认）
cmake -DBUILD_SHARED_LIBS=OFF ..
make

# 编译动态库
cmake -DBUILD_SHARED_LIBS=ON ..
make
```

### 3. 输出二进制位置

编译完成后，生成的文件位于：

| 文件类型 | Release模式 | Debug模式 |
|---------|------------|----------|
| 静态库 | `build/libreplaceAll_acc.a` | `build/libreplaceAll_acc.a` |
| 动态库 | `build/libreplaceAll_acc.so` | `build/libreplaceAll_acc.so` |

## 测试编译方法

### 1. 编译测试

```bash
# 进入测试构建目录
cd test/build

# 编译所有测试（默认使用32个并行任务）
make

# 或者分别编译
make func    # 编译功能测试
make perf    # 编译性能测试

# 编译Debug版本
make DEBUG=1
```

### 2. 测试输出位置

| 测试类型 | 输出位置 |
|---------|---------|
| 功能测试 | `test/build/functest/functest` |
| 性能测试 | `test/build/perftest/perftest` |

## 测试运行方法

### 1. 运行所有测试

```bash
cd test/build

# 运行所有功能测试
make rfunc

# 运行所有性能测试
make rperf
```

### 2. 运行特定测试

```bash
cd test/build

# 运行指定的功能测试
make rfunc func=ReplaceAllAcc.ExecMatch1

# 运行指定的性能测试
make rperf perf=ReplaceAllAcc.BASIC_PERF001
```

### 3. 清理测试构建产物

```bash
cd test/build
make clean
```

## 完整编译示例

### 示例1：编译Release版本静态库并运行所有测试

```bash
# 编译主库
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF ..
make

# 编译并运行测试
cd ../test/build
make
make rfunc
make rperf
```

### 示例2：编译Debug版本动态库并运行特定测试

```bash
# 编译主库
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug -DBUILD_SHARED_LIBS=ON ..
make

# 编译并运行特定测试
cd ../test/build
make DEBUG=1
make rfunc func=ReplaceAllAcc.ExecMatch1
```

### 示例3：快速编译和测试

```bash
# 一键编译主库和所有测试
mkdir build && cd build
cmake ..
make
cd ../test/build
make
make rfunc
make rperf
```

## 测试说明

### 功能测试 (functest)

功能测试用于验证replaceAll_acc算子的正确性，测试内容包括：
- 字符串替换功能的正确性验证
- 边界条件测试
- 特殊字符处理测试

### 性能测试 (perftest)

性能测试用于评估replaceAll_acc算子的性能表现，测试内容包括：
- 不同数据规模下的性能测试
- 与标准库实现的性能对比
- 吞吐量和延迟测试

## Makefile命令参考

在`test/build`目录下可用的命令：

| 命令 | 说明 |
|-----|------|
| `make` | 编译所有测试（func和perf） |
| `make func` | 编译功能测试 |
| `make perf` | 编译性能测试 |
| `make rfunc` | 运行所有功能测试 |
| `make rperf` | 运行所有性能测试 |
| `make rfunc func=TEST_NAME` | 运行指定的功能测试 |
| `make rperf perf=TEST_NAME` | 运行指定的性能测试 |
| `make clean` | 清理构建产物 |
| `make DEBUG=1` | 编译调试版本 |
| `make help` | 显示帮助信息 |

## 注意事项

1. **编译顺序**：必须先编译主库，再编译测试
2. **依赖库**：确保系统已安装Google Test库
3. **ARM架构**：本项目仅支持ARM64架构
4. **并行编译**：测试编译默认使用32个并行任务，可根据机器配置调整Makefile中的`-j32`参数
5. **库路径**：运行测试时，如果使用动态库，需要设置`LD_LIBRARY_PATH`环境变量

## 故障排除

### 问题1：找不到Google Test库

```bash
# Ubuntu/Debian
sudo apt-get install libgtest-dev

# CentOS/RHEL
sudo yum install gtest-devel
```

### 问题2：动态库运行时找不到

```bash
# 设置库路径
export LD_LIBRARY_PATH=/path/to/build:$LD_LIBRARY_PATH
```

### 问题3：编译失败，提示ARM指令不支持

确保在ARM64架构服务器上编译，或者检查编译器是否支持ARM NEON指令集。

## 性能优化建议

1. 使用Release模式编译以获得最佳性能
2. 根据CPU核心数调整并行编译任务数
3. 在生产环境中使用静态库以减少依赖
4. 对于大规模数据处理，建议使用性能测试评估实际表现

## 许可证

本项目采用BSD许可证，详见源码文件中的版权声明。
