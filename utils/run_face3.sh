#!/bin/sh
# 三进程启动顺序：事件 → AI → 视频（与 RT-Smart IPC channel 创建/打开顺序一致）
# 交互命令（i/d/n/q 等）在 face_event 终端输入；face_video 仅采集与显示。
# 请按板端实际路径修改模型与数据库目录。

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BIN_DIR="${SCRIPT_DIR}"

DET_KMODEL="${DET_KMODEL:-${BIN_DIR}/face_detection_320.kmodel}"
REC_KMODEL="${REC_KMODEL:-${BIN_DIR}/face_recognition_mobilefacenet.kmodel}"
DB_DIR="${DB_DIR:-${BIN_DIR}/face_db}"
LOG_FILE="${LOG_FILE:-/tmp/attendance.log}"
DEBUG_VIDEO="${DEBUG_VIDEO:-0}"
DET_THRES="${DET_THRES:-0.5}"
NMS_THRES="${NMS_THRES:-0.2}"
REC_THRES="${REC_THRES:-70}"
DEBUG_AI="${DEBUG_AI:-0}"

echo "[1/3] face_event.elf -> ${LOG_FILE}"
"${BIN_DIR}/face_event.elf" "${LOG_FILE}" &
sleep 0.5

echo "[2/3] face_ai.elf"
"${BIN_DIR}/face_ai.elf" "${DET_KMODEL}" "${DET_THRES}" "${NMS_THRES}" "${REC_KMODEL}" "${REC_THRES}" "${DB_DIR}" "${DEBUG_AI}" &
sleep 1

echo "[3/3] face_video.elf (debug=${DEBUG_VIDEO})"
exec "${BIN_DIR}/face_video.elf" "${DEBUG_VIDEO}"
