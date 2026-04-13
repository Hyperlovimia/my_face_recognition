#!/bin/bash
# Build and package K230 example programs (incremental build)
# Function: Use CMake and cross-compile toolchain to build face_recognition example and collect ELF and utility files

set -e  # Exit immediately if a command fails
# set -x  # Enable debug mode

# =======================
# Script directory and SDK paths
# =======================
SCRIPT=$(realpath -s "$0")
SCRIPTPATH=$(dirname "$SCRIPT")

export SDK_SRC_ROOT_DIR=$(realpath "${SCRIPTPATH}/../../../../../")
export SDK_RTSMART_SRC_DIR="${SDK_SRC_ROOT_DIR}/src/rtsmart/"
export MPP_SRC_DIR="${SDK_RTSMART_SRC_DIR}/mpp/"
export NNCASE_SRC_DIR="${SDK_RTSMART_SRC_DIR}/libs/nncase/"
export OPENCV_SRC_DIR="${SDK_RTSMART_SRC_DIR}/libs/opencv/"

# Set cross-compile toolchain path
export PATH=$PATH:~/.kendryte/k230_toolchains/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin

# =======================
# Output directories
# =======================
BUILD_DIR="${SCRIPTPATH}/build"
K230_BIN_DIR="${SCRIPTPATH}/k230_bin"
mkdir -p "${BUILD_DIR}"
mkdir -p "${K230_BIN_DIR}"

# =======================
# Build function
# =======================
build_project() {
    pushd "${BUILD_DIR}"

    # Incremental build: only run CMake if CMakeCache.txt does not exist
    if [ ! -f "CMakeCache.txt" ]; then
        echo "[INFO] Running initial CMake configuration..."
        cmake -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_INSTALL_PREFIX="$(pwd)" \
              -DCMAKE_TOOLCHAIN_FILE=../cmake/Riscv64.cmake \
              ..
    else
        echo "[INFO] Using existing CMake configuration for incremental build..."
    fi

    echo "[INFO] Starting build..."
    make -j --no-print-directory

    echo "[INFO] Installing build artifacts..."
    make install --no-print-directory

    popd
}

# =======================
# 仅收集交叉编译生成的 ELF 到 k230_bin（板端部署目录）
# utils/ 为仓库内可选脚本（如三进程启动示例），不参与打包拷贝，避免与 k230_bin 混淆
# =======================
collect_outputs() {
    local elves=(
        "${BUILD_DIR}/bin/face_recognition.elf"
        "${BUILD_DIR}/bin/face_video.elf"
        "${BUILD_DIR}/bin/face_ai.elf"
        "${BUILD_DIR}/bin/face_event.elf"
        "${BUILD_DIR}/bin/fas_test.elf"
    )
    local found=0
    for f in "${elves[@]}"; do
        if [ -f "${f}" ]; then
            found=1
            break
        fi
    done
    if [ "${found}" -eq 1 ]; then
        echo "[INFO] Collecting ELF files to ${K230_BIN_DIR}..."
        for f in "${elves[@]}"; do
            [ -f "${f}" ] && cp -u "${f}" "${K230_BIN_DIR}/"
        done
    else
        echo "[WARN] No ELF files found under ${BUILD_DIR}/bin/"
    fi
}

# =======================
# Main process
# =======================
build_project
collect_outputs

echo "[INFO] Build finished. Output directory: ${K230_BIN_DIR}"
