#!/bin/bash
set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(realpath "${SCRIPT_DIR}/..")
SDK_ROOT=$(realpath "${PROJECT_ROOT}/../../../../")
TOOLCHAIN_DIR="${SDK_ROOT}/toolchain/Xuantie-900-gcc-linux-5.10.4-glibc-x86_64-V2.6.0/bin"
OUT_DIR="${SCRIPT_DIR}/out"
K230_BIN_DIR="${PROJECT_ROOT}/k230_bin"
FACE_BRIDGE_DIR="${K230_BIN_DIR}/face_bridge"
TARGET_INI="${SCRIPT_DIR}/face_netd.ini"
UI_DATA_DIR="${SCRIPT_DIR}/ui/data"

export PATH="${TOOLCHAIN_DIR}:$PATH"

: "${STATIC:=0}"
: "${DEBUG:=0}"
: "${DO_STRIP:=1}"

path_not_writable() {
    local path="$1"
    if [ ! -e "${path}" ]; then
        return 1
    fi

    if [ -d "${path}" ]; then
        local probe="${path}/.write_probe.$$"
        if touch "${probe}" 2>/dev/null; then
            rm -f "${probe}"
            return 1
        fi
        return 0
    fi

    [ ! -w "${path}" ]
}

ensure_workspace_writable() {
    if path_not_writable "${OUT_DIR}" || path_not_writable "${K230_BIN_DIR}" || path_not_writable "${FACE_BRIDGE_DIR}"; then
        echo "[ERROR] Existing output directories are not writable:"
        path_not_writable "${OUT_DIR}" && echo "  ${OUT_DIR}"
        path_not_writable "${K230_BIN_DIR}" && echo "  ${K230_BIN_DIR}"
        path_not_writable "${FACE_BRIDGE_DIR}" && echo "  ${FACE_BRIDGE_DIR}"
        echo "  Please fix ownership first, for example:"
        echo "    sudo chown -R $(id -u):$(id -g) ${OUT_DIR} ${K230_BIN_DIR}"
        exit 1
    fi
}

collect_outputs() {
    mkdir -p "${K230_BIN_DIR}"
    mkdir -p "${FACE_BRIDGE_DIR}"

    if [ ! -f "${OUT_DIR}/face_netd" ]; then
        echo "[WARN] ${OUT_DIR}/face_netd not found, skip collecting outputs."
        return 0
    fi

    echo "[INFO] Collecting linux_bridge outputs to ${FACE_BRIDGE_DIR}..."
    cp -ru "${OUT_DIR}/." "${FACE_BRIDGE_DIR}/"

    if [ -f "${TARGET_INI}" ]; then
        cp -u "${TARGET_INI}" "${FACE_BRIDGE_DIR}/"
    else
        echo "[WARN] ${TARGET_INI} not found, skip copying config."
    fi

    if [ -d "${UI_DATA_DIR}" ]; then
        mkdir -p "${OUT_DIR}/data" "${FACE_BRIDGE_DIR}/data"
        cp -ru "${UI_DATA_DIR}/." "${OUT_DIR}/data/"
        cp -ru "${UI_DATA_DIR}/." "${FACE_BRIDGE_DIR}/data/"
    else
        echo "[WARN] ${UI_DATA_DIR} not found, skip copying UI assets."
    fi
}

prune_intermediate_artifacts() {
    local dir="$1"
    [ -d "${dir}" ] || return 0

    find "${dir}" -type f -name '*.o' -delete
    find "${dir}" -depth -type d -empty -delete
}

ensure_workspace_writable

make -C "${SCRIPT_DIR}" clean
make -C "${SCRIPT_DIR}" -j"$(nproc)" STATIC="${STATIC}" DEBUG="${DEBUG}" DO_STRIP="${DO_STRIP}"

collect_outputs
prune_intermediate_artifacts "${OUT_DIR}"
prune_intermediate_artifacts "${FACE_BRIDGE_DIR}"

echo "[INFO] face_netd built at ${OUT_DIR}/face_netd (STATIC=${STATIC}, DEBUG=${DEBUG}, DO_STRIP=${DO_STRIP})"
echo "[INFO] linux_bridge outputs are available in ${FACE_BRIDGE_DIR}"
