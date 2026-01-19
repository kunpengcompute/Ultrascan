cmake_minimum_required(VERSION 3.10)
project(replaceAll_acc CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PROJECT_NAME "replaceAll_acc")

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(BUILD_TESTS "Build tests" OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

message("-- Building type: ${CMAKE_BUILD_TYPE}")

find_package(OpenMP)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS} -Wall -fopenmp -Wextra -D_FORTIFY_SOURCE=2 -O3 -DPERF_ARMv8_ASM -fstack-protector-all -Wl,-z,relro,-z,now,-z,noexecstack -fPIC -s -fcommon")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "4.8.5" OR CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "4.8.5")
            message("-- Insufficient gcc version:${CMAKE_CXX_COMPILER_VERSION}")
            set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=armv8-a+crc+crypto+fp+simd -mtune=cortex-a53 -fno-strict-aliasing -std=gnu11")
        else()
            set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=armv8-a+crc")
        endif()
    endif()
    message("-- Building type: Release")
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS} -Wall -fopenmp -Wextra -g -O0 -DPERF_ARMv8_ASM -fstack-protector-all -fPIC -fcommon")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=armv8-a+crc")
    endif()
    message("-- Building type: Debug")
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_C_FLAGS} -std=c++11")

include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/test/common/include)

set(SOURCES
    src/replaceAll_acc.cpp
)

set(HEADERS
    include/replaceAll_acc.h
    include/simd_types.h
    include/khsel_ops.h
)

add_library(${PROJECT_NAME} ${SOURCES} ${HEADERS})

target_compile_definitions(${PROJECT_NAME} PRIVATE HS_OPTIMIZE)

if(BUILD_SHARED_LIBS)
    message("-- Building shared library")
else()
    message("-- Building static library")
endif()

install(TARGETS ${PROJECT_NAME}
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES ${HEADERS} DESTINATION include/${PROJECT_NAME})
