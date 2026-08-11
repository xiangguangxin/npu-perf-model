#!/usr/bin/env bash
#
# 从仓库内自带的 SystemC 源码(third_party/systemc-src，含 TLM2.0)本地编译，
# 安装到 third_party/systemc。源码已入库，本脚本不联网、不下载。
# CMakeLists.txt 默认就从 third_party/systemc 找 SystemC，跑完即可 build 主工程。
#
# 用法:
#   scripts/setup_systemc.sh            # 已装则跳过
#   scripts/setup_systemc.sh --force    # 强制重编
#   CXX_STD=17 scripts/setup_systemc.sh # 覆盖 C++ 标准(需与主工程一致)
#
# 依赖: cmake(>=3.16)、C++17 编译器、make
set -euo pipefail

CXX_STD="${CXX_STD:-17}"
FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SRC_DIR="${PROJECT_ROOT}/third_party/systemc-src"   # 入库的源码
PREFIX="${PROJECT_ROOT}/third_party/systemc"         # 生成的 install prefix(gitignored)
BUILD_DIR="${PROJECT_ROOT}/third_party/.build/systemc"

log() { printf '\033[1;34m[setup-systemc]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[setup-systemc] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# ---- 源码存在性 ----
[[ -f "${SRC_DIR}/CMakeLists.txt" ]] || \
    die "未找到 SystemC 源码: ${SRC_DIR}（应随仓库一同拉取）"

# ---- 已装则跳过(除非 --force) ----
if [[ -f "${PREFIX}/include/systemc" && -f "${PREFIX}/lib/libsystemc.so" && ${FORCE} -eq 0 ]]; then
    log "SystemC 已存在于 ${PREFIX}，跳过。（--force 可强制重编）"
    exit 0
fi

command -v cmake >/dev/null || die "未找到 cmake"
command -v make  >/dev/null || die "未找到 make"

JOBS="$( (command -v nproc >/dev/null && nproc) || echo 4)"
log "源码    : ${SRC_DIR}"
log "安装到  : ${PREFIX}   (C++${CXX_STD}, -j${JOBS})"

# ---- 强制重编时清干净 ----
if [[ ${FORCE} -eq 1 ]]; then
    rm -rf "${BUILD_DIR}" "${PREFIX}"
fi

log "cmake 配置 ..."
cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_CXX_STANDARD="${CXX_STD}" \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_BUILD_TYPE=Release

log "编译 ..."
cmake --build "${BUILD_DIR}" -j"${JOBS}"

log "安装 ..."
cmake --install "${BUILD_DIR}"

log "完成 ✅  SystemC 已装到 ${PREFIX}"
log "现在可构建主工程:"
log "    cmake -S \"${PROJECT_ROOT}\" -B \"${PROJECT_ROOT}/build\" && cmake --build \"${PROJECT_ROOT}/build\" -j"
