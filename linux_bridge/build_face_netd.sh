#!/bin/bash
set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(realpath "${SCRIPT_DIR}/..")
SDK_ROOT=$(realpath "${PROJECT_ROOT}/../../../../")
TOOLCHAIN_DIR="${SDK_ROOT}/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin"

export PATH="${TOOLCHAIN_DIR}:$PATH"

make -C "${SCRIPT_DIR}" clean
make -C "${SCRIPT_DIR}" -j"$(nproc)"

echo "[INFO] face_netd built at ${SCRIPT_DIR}/out/face_netd"
