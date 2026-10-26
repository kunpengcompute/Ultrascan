#!/bin/bash
###############################################################################
# build_ci.sh — Ultrascan PR 流水线构建脚本
#
# 镜像: swr.cn-north-4.myhuaweicloud.com/cloud_boostkit/openeuler22.03_lts_sp4:Ultrascan_26.3.0.01
# 架构: aarch64 (鲲鹏)
# 依赖: Tier1 预构建镜像已预装 GCC 10.3.1 / CMake 3.22.0 / Ragel 6.10 /
#       Boost 1.87.0 / PCRE 8.45(/opt/pcre-8.45) / SQLite 3.37.2，无需在线安装
#
# 源码依据:
#   - docs/zh/installation_guide.md  构建命令 cmake .. && make -j
#   - CMakeLists.txt:39-45           默认 CMAKE_BUILD_TYPE=RELWITHDEBINFO
#   - CMakeLists.txt:576-584         BUILD_CHIMERA 依赖 PCRE 源码就位
#   - cmake/pcre.cmake               优先从 ${REPO_ROOT}/pcre 构建
#   - examples/CMakeLists.txt        simplegrep 总是构建
#
# 用法（工作目录为源码根目录，平台 checkout 后默认如此；基分支脚本回退
#      执行时工作目录同样保持为预合入代码根目录）:
#   sh .gitcode/scripts/build_ci.sh
# 环境变量可覆盖: BUILD_JOBS / CMAKE_BUILD_TYPE / REPO_ROOT / BUILD_DIR
#
# 退出码: 0=成功, 非0=失败
###############################################################################

set -euo pipefail

# ---- 可配置参数 ----
REPO_ROOT="${REPO_ROOT:-$PWD}"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RELWITHDEBINFO}"

echo "============================================"
echo "  Ultrascan PR Pipeline - Build"
echo "============================================"
echo "  Repo root:    ${REPO_ROOT}"
echo "  Build dir:    ${BUILD_DIR}"
echo "  Build jobs:   ${BUILD_JOBS}"
echo "  Build type:   ${CMAKE_BUILD_TYPE}"
echo "  Date:         $(date '+%Y-%m-%d %H:%M:%S')"
echo "============================================"
echo ""

# ---- Step 1: 依赖检查（Tier1 镜像预装，缺失即报错退出）----
echo "[1/5] 检查构建依赖..."
echo ""

DEP_FAIL=0

echo "  GCC:      $(gcc --version 2>/dev/null | head -1 || echo 'NOT FOUND')"
command -v gcc >/dev/null 2>&1 || DEP_FAIL=1

echo "  CMake:    $(cmake --version 2>/dev/null | head -1 || echo 'NOT FOUND')"
command -v cmake >/dev/null 2>&1 || DEP_FAIL=1

echo "  Make:     $(make --version 2>/dev/null | head -1 || echo 'NOT FOUND')"
command -v make >/dev/null 2>&1 || DEP_FAIL=1

echo "  Ragel:    $(ragel -v 2>/dev/null || echo 'NOT FOUND')"
if ! command -v ragel >/dev/null 2>&1; then
    echo "           ERROR: Ragel 是编译期必需 (CMakeLists.txt find_program(RAGEL))"
    DEP_FAIL=1
fi

echo "  Python:   $(python3 --version 2>/dev/null || echo 'NOT FOUND')"
if ! command -v python3 >/dev/null 2>&1; then
    echo "           ERROR: Python 是 CMake 构建系统依赖 (find_package(PythonInterp))"
    DEP_FAIL=1
fi

# Boost: 系统包或源码目录 (cmake/boost.cmake 优先 ${REPO_ROOT}/include, 回退 find_package)
if [ -f "${REPO_ROOT}/include/boost/version.hpp" ]; then
    echo "  Boost:    ${REPO_ROOT}/include/boost (source symlink)"
elif [ -f "/usr/include/boost/version.hpp" ]; then
    echo "  Boost:    $(grep 'BOOST_LIB_VERSION' /usr/include/boost/version.hpp 2>/dev/null | sed 's/.*"\(.*\)".*/\1/' || echo 'system boost-devel')"
else
    echo "  Boost:    NOT FOUND"
    echo "           ERROR: Boost 1.57+ 必选 (cmake/boost.cmake)"
    DEP_FAIL=1
fi

# SQLite (可选, hsbench 依赖)
if pkg-config --exists sqlite3 2>/dev/null; then
    echo "  SQLite:   $(pkg-config --modversion sqlite3) (system, hsbench 依赖)"
else
    echo "  SQLite:   NOT FOUND (hsbench 将不可用, 非阻断)"
fi

# libpcap (可选, pcapscan/patbench 示例依赖)
if [ -f "/usr/lib64/libpcap.so" ] || [ -f "/usr/lib/libpcap.so" ] || pkg-config --exists libpcap 2>/dev/null; then
    echo "  libpcap:  found (pcapscan/patbench 示例可用)"
else
    echo "  libpcap:  NOT FOUND (pcapscan/patbench 示例不构建, 非阻断)"
fi

echo ""

if [ ${DEP_FAIL} -ne 0 ]; then
    echo "============================================"
    echo "  BUILD FAILED: 必需依赖缺失"
    echo "============================================"
    exit 1
fi

# ---- Step 2: 准备 PCRE 源码（决定 BUILD_CHIMERA 及 chimera 系产物/测试）----
# 如果 PCRE 未就位: BUILD_CHIMERA=FALSE → unit-chimera/hscollider/hscheck/collide_* 均不构建
echo "[2/5] 准备 PCRE 源码..."
if [ -d "${REPO_ROOT}/pcre" ]; then
    echo "  PCRE 源码已就位: ${REPO_ROOT}/pcre"
elif [ -d "/opt/pcre-8.45" ]; then
    cp -rf /opt/pcre-8.45 "${REPO_ROOT}/pcre"
    echo "  已从 /opt/pcre-8.45 拷贝到 ${REPO_ROOT}/pcre (Tier1 镜像预置)"
else
    echo "  WARNING: PCRE 源码未找到 (${REPO_ROOT}/pcre 或 /opt/pcre-8.45)"
    echo "           → BUILD_CHIMERA=FALSE, 以下将不构建:"
    echo "             unit-chimera, libchimera.a, hscheck, hscollider, collide_quick_test"
    echo "           → 请参照 docs/zh/installation_guide.md 下载 PCRE 8.41+ 源码"
fi
echo ""

# ---- Step 3: 清理并创建构建目录 ----
echo "[3/5] 准备构建目录..."
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
echo "  已创建: ${BUILD_DIR}"
echo ""

# ---- Step 4: CMake 配置 ----
echo "[4/5] 执行 CMake 配置..."
echo "  cd ${BUILD_DIR} && cmake -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} .."
echo ""

cd "${BUILD_DIR}"
cmake -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" ..
CMAKE_RC=$?

if [ ${CMAKE_RC} -ne 0 ]; then
    echo ""
    echo "============================================"
    echo "  BUILD FAILED: CMake 配置失败 (exit=${CMAKE_RC})"
    echo "============================================"
    exit ${CMAKE_RC}
fi

echo "  CMake 配置成功"
echo ""

# ---- Step 5: make 编译 ----
echo "[5/5] 执行 make -j${BUILD_JOBS} 编译..."
echo ""

make -j"${BUILD_JOBS}"
MAKE_RC=$?

if [ ${MAKE_RC} -ne 0 ]; then
    echo ""
    echo "============================================"
    echo "  BUILD FAILED: make 编译失败 (exit=${MAKE_RC})"
    echo "============================================"
    exit ${MAKE_RC}
fi

echo ""

# ---- 验证构建产物 ----
# 产物分类:
#   [必须]  任何情况都应构建 (不依赖 PCRE/SQLite/Debug)
#   [PCRE]  依赖 BUILD_CHIMERA (PCRE 源码就位)
#   [SQL]   依赖 SQLite
#   [DBG]   依赖 Debug 模式 (DUMP_SUPPORT=ON)
echo "============================================"
echo "  构建产物验证"
echo "============================================"

BUILD_TYPE_UPPER=$(echo "${CMAKE_BUILD_TYPE}" | tr '[:lower:]' '[:upper:]')
HAS_PCRE=0
[ -d "${REPO_ROOT}/pcre" ] && HAS_PCRE=1

# 必须产物 (libhs.a, libhs_runtime.a, unit-hyperscan, simplegrep)
MANDATORY_ARTIFACTS=(
    "lib/libhs.a"
    "lib/libhs_runtime.a"
    "bin/unit-hyperscan"
    "bin/simplegrep"
)

# PCRE 条件产物 (libpcre.a, libchimera.a, unit-chimera, hscheck, hscollider)
PCRE_ARTIFACTS=(
    "lib/libpcre.a"
    "lib/libchimera.a"
    "bin/unit-chimera"
    "bin/hscheck"
    "bin/hscollider"
)

# SQLite 条件产物
SQLITE_ARTIFACTS=(
    "bin/hsbench"
)

# Debug 条件产物 (DUMP_SUPPORT=ON 时构建)
DEBUG_ARTIFACTS=(
    "bin/hsdump"
)

MANDATORY_OK=true
CONDITIONAL_MISSING=0

echo "--- 必须产物 ---"
for art in "${MANDATORY_ARTIFACTS[@]}"; do
    path="${BUILD_DIR}/${art}"
    if [ -f "${path}" ]; then
        sz=$(du -h "${path}" | cut -f1)
        printf "  [OK]      %-30s %s\n" "${art}" "${sz}"
    else
        printf "  [MISSING] %-30s [失败]\n" "${art}"
        MANDATORY_OK=false
    fi
done

echo "--- PCRE 条件产物 (BUILD_CHIMERA=${HAS_PCRE}) ---"
if [ ${HAS_PCRE} -eq 1 ]; then
    for art in "${PCRE_ARTIFACTS[@]}"; do
        path="${BUILD_DIR}/${art}"
        if [ -f "${path}" ]; then
            sz=$(du -h "${path}" | cut -f1)
            printf "  [OK]      %-30s %s\n" "${art}" "${sz}"
        else
            printf "  [MISSING] %-30s [应构建, 检查 PCRE 配置]\n" "${art}"
            MANDATORY_OK=false
        fi
    done
else
    echo "  (跳过: PCRE 源码未就位, BUILD_CHIMERA=FALSE)"
    for art in "${PCRE_ARTIFACTS[@]}"; do
        printf "  [SKIP]     %-30s (PCRE 缺失)\n" "${art}"
    done
fi

echo "--- SQLite 条件产物 ---"
for art in "${SQLITE_ARTIFACTS[@]}"; do
    path="${BUILD_DIR}/${art}"
    if [ -f "${path}" ]; then
        sz=$(du -h "${path}" | cut -f1)
        printf "  [OK]      %-30s %s\n" "${art}" "${sz}"
    else
        printf "  [SKIP]     %-30s (SQLite 缺失, 非阻断)\n" "${art}"
        CONDITIONAL_MISSING=$((CONDITIONAL_MISSING+1))
    fi
done

echo "--- Debug 条件产物 (当前: ${BUILD_TYPE_UPPER}) ---"
if [[ "${BUILD_TYPE_UPPER}" == *"DEBUG"* ]]; then
    for art in "${DEBUG_ARTIFACTS[@]}"; do
        path="${BUILD_DIR}/${art}"
        if [ -f "${path}" ]; then
            sz=$(du -h "${path}" | cut -f1)
            printf "  [OK]      %-30s %s\n" "${art}" "${sz}"
        else
            printf "  [MISSING] %-30s [Debug 模式应构建, 检查 DUMP_SUPPORT]\n" "${art}"
            MANDATORY_OK=false
        fi
    done
else
    echo "  (跳过: 非 Debug 模式, DUMP_SUPPORT=OFF, hsdump 不构建)"
    printf "  [SKIP]     %-30s (release 模式)\n" "bin/hsdump"
fi

echo ""
echo "============================================"
if [ "${MANDATORY_OK}" = "true" ]; then
    echo "  BUILD SUCCESS"
    echo "  构建类型: ${BUILD_TYPE_UPPER}, 并发: ${BUILD_JOBS}"
    echo "  产物目录: ${BUILD_DIR}"
    if [ ${CONDITIONAL_MISSING} -gt 0 ]; then
        echo "  告警: ${CONDITIONAL_MISSING} 个可选产物缺失 (非阻断)"
    fi
else
    echo "  BUILD FAILED: 必须产物缺失"
fi
echo "============================================"

[ "${MANDATORY_OK}" = "true" ] && exit 0 || exit 1
