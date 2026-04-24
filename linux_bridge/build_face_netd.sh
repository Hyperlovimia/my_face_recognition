#!/bin/bash
set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(realpath "${SCRIPT_DIR}/..")
SDK_ROOT=$(realpath "${PROJECT_ROOT}/../../../../")
TOOLCHAIN_DIR="${SDK_ROOT}/toolchain/Xuantie-900-gcc-linux-5.10.4-glibc-x86_64-V2.6.0/bin"

export PATH="${TOOLCHAIN_DIR}:$PATH"

: "${STATIC:=0}"
: "${DEBUG:=0}"
: "${DO_STRIP:=1}"

make -C "${SCRIPT_DIR}" clean
make -C "${SCRIPT_DIR}" -j"$(nproc)" STATIC="${STATIC}" DEBUG="${DEBUG}" DO_STRIP="${DO_STRIP}"

echo "[INFO] face_netd built at ${SCRIPT_DIR}/out/face_netd (STATIC=${STATIC}, DEBUG=${DEBUG}, DO_STRIP=${DO_STRIP})"
