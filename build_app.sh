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

export SDK_SRC_ROOT_DIR=$(realpath "${SCRIPTPATH}/../../../../")
export MPP_SRC_DIR="${SDK_SRC_ROOT_DIR}/src/big/mpp/"
export NNCASE_SRC_DIR="${SDK_SRC_ROOT_DIR}/src/big/nncase/"
export OPENCV_SRC_DIR="${SDK_SRC_ROOT_DIR}/src/big/utils/lib/opencv/"

K230_RISCV_BIN="${SDK_SRC_ROOT_DIR}/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin"
K230_DOCKER_RISCV_BIN="/opt/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin"

setup_toolchain_path() {
    if [ -d "${K230_RISCV_BIN}" ]; then
        export PATH="${K230_RISCV_BIN}:${PATH}"
    elif [ -d "${K230_DOCKER_RISCV_BIN}" ]; then
        export PATH="${K230_DOCKER_RISCV_BIN}:${PATH}"
    fi
}

ensure_toolchain_ready() {
    if command -v riscv64-unknown-linux-musl-gcc >/dev/null 2>&1; then
        return 0
    fi

    echo "[ERROR] riscv64-unknown-linux-musl-gcc is not in PATH."
    echo "  Expected one of these toolchain locations:"
    echo "    ${K230_RISCV_BIN}"
    echo "    ${K230_DOCKER_RISCV_BIN}"
    echo "  Please run: cd ${SDK_SRC_ROOT_DIR} && make prepare_toolchain"
    exit 1
}

path_not_writable() {
    local path="$1"
    [ -e "${path}" ] && [ ! -w "${path}" ]
}

ensure_workspace_writable() {
    if path_not_writable "${BUILD_DIR}" || path_not_writable "${K230_BIN_DIR}"; then
        echo "[ERROR] Existing build/output directories are not writable:"
        path_not_writable "${BUILD_DIR}" && echo "  ${BUILD_DIR}"
        path_not_writable "${K230_BIN_DIR}" && echo "  ${K230_BIN_DIR}"
        echo "  Please fix ownership first, for example:"
        echo "    sudo chown -R $(id -u):$(id -g) ${BUILD_DIR} ${K230_BIN_DIR}"
        exit 1
    fi
}

# =======================
# Output directories
# =======================
BUILD_DIR="${SCRIPTPATH}/build"
K230_BIN_DIR="${SCRIPTPATH}/k230_bin"
mkdir -p "${BUILD_DIR}"
mkdir -p "${K230_BIN_DIR}"

setup_toolchain_path
ensure_toolchain_ready
ensure_workspace_writable

# =======================
# Build function
# =======================
build_project() {
    pushd "${BUILD_DIR}"

    # 迁移目录后旧的 CMakeCache.txt 会记录原始源码路径，直接复用会配置失败。
    rm -rf CMakeCache.txt CMakeFiles

    echo "[INFO] Running CMake configuration..."
    cmake -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX="$(pwd)" \
          -DCMAKE_TOOLCHAIN_FILE=../cmake/Riscv64.cmake \
          ..

    echo "[INFO] Starting build..."
    make -j --no-print-directory

    echo "[INFO] Installing build artifacts..."
    make install --no-print-directory

    popd
}

# =======================
# 收集板端部署需要的 ELF、模型和脚本到 k230_bin
# =======================
collect_outputs() {
    local elves=(
        "${BUILD_DIR}/bin/face_recognition.elf"
        "${BUILD_DIR}/bin/face_recognition_scrfd.elf"
        "${BUILD_DIR}/bin/face_video.elf"
        "${BUILD_DIR}/bin/face_ai.elf"
        "${BUILD_DIR}/bin/face_event.elf"
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
        cp -u utils/* "${K230_BIN_DIR}/" 2>/dev/null || true
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
