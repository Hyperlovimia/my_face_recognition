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

# Cross-compile toolchain: riscv64 musl. Prefer SDK tree, then /opt (Docker with correct -v)
TOOLCHAIN_DIR_NAME="riscv64-linux-musleabi_for_x86_64-pc-linux-gnu"
TOOLCHAIN_GCC="riscv64-unknown-linux-musl-gcc"
K230_TOOLCHAIN_ROOT=""
for _root in \
  "${SDK_SRC_ROOT_DIR}/toolchain/${TOOLCHAIN_DIR_NAME}" \
  "${SCRIPTPATH}/toolchain/${TOOLCHAIN_DIR_NAME}" \
  "/opt/toolchain/${TOOLCHAIN_DIR_NAME}"; do
  if [ -x "${_root}/bin/${TOOLCHAIN_GCC}" ]; then
    K230_TOOLCHAIN_ROOT="${_root}"
    break
  fi
done
if [ -z "${K230_TOOLCHAIN_ROOT}" ]; then
  echo "[ERROR] RISC-V cross compiler (${TOOLCHAIN_GCC}) not found."
  echo "  Searched under:"
  echo "    ${SDK_SRC_ROOT_DIR}/toolchain/${TOOLCHAIN_DIR_NAME}"
  echo "    ${SCRIPTPATH}/toolchain/${TOOLCHAIN_DIR_NAME}"
  echo "    /opt/toolchain/${TOOLCHAIN_DIR_NAME}"
  echo "  - On the host, ensure the k230 SDK toolchain is present (often k230_sdk/toolchain/...)."
  echo "  - In Docker: run from the k230_sdk root and use -v \$(pwd)/toolchain:/opt/toolchain (SDK toolchain),"
  echo "    or remove -v \$(pwd)/toolchain:... if my_face_recognition/toolchain is empty (empty mount hides /opt/toolchain)."
  exit 1
fi
export PATH="${K230_TOOLCHAIN_ROOT}/bin:${PATH}"

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
        "${BUILD_DIR}/bin/face_video.elf"
        "${BUILD_DIR}/bin/face_ai.elf"
        "${BUILD_DIR}/bin/face_event.elf"
        "${BUILD_DIR}/bin/face_ctrl.elf"
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
