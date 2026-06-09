cmake_minimum_required(VERSION 3.10)

project(hyperscan_fuzz_test)

# 设置C++标准
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(HS_BUILD_DIR "${CMAKE_SOURCE_DIR}/../../build" CACHE PATH "Hyperscan build directory")
find_package(Threads REQUIRED)

# 包含头文件目录
include_directories(
    ${CMAKE_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/../../src
    ${HS_BUILD_DIR}
)

# 启用覆盖率编译选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fprofile-arcs -ftest-coverage")

# 源文件
set(SOURCES
    main.cpp
    fuzz_test.cpp
    generator/python_generator.cpp
    internal/dverm_search_aligned_fuzz.cpp
    runner/hs_runner.cpp
    ../gtest/gtest-all.cc
    data/data_generator.cpp
)

# 链接库
find_library(HS_LIBRARY hs PATHS ${HS_BUILD_DIR}/lib)

# 创建可执行文件
add_executable(hyperscan_fuzz_test ${SOURCES})

# 链接库
target_link_libraries(hyperscan_fuzz_test
    ${HS_LIBRARY}
    Threads::Threads
)

# 复制Python生成器脚本到构建目录
configure_file(${CMAKE_SOURCE_DIR}/../../tools/fuzz/aristocrats.py ${CMAKE_BINARY_DIR}/aristocrats.py COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/../../tools/fuzz/heuristocrats.py ${CMAKE_BINARY_DIR}/heuristocrats.py COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/../../tools/fuzz/completocrats.py ${CMAKE_BINARY_DIR}/completocrats.py COPYONLY)

# 添加覆盖率报告目标
add_custom_target(coverage
    COMMAND ./hyperscan_fuzz_test
    COMMAND ${LCOV_EXECUTABLE} --capture --directory . --output-file coverage.info
    COMMAND ${LCOV_EXECUTABLE} --remove coverage.info '*/gtest/*' '/usr/*' --output-file coverage.info.cleaned
    COMMAND ${GENHTML_EXECUTABLE} coverage.info.cleaned --output-directory coverage_report
    COMMAND echo "Coverage report generated in coverage_report/index.html"
    DEPENDS hyperscan_fuzz_test
)
