#!/bin/sh
# 三进程启动顺序：事件 → AI → 视频（与 RT-Smart IPC channel 创建/打开顺序一致）
# 交互命令（i/d/n/q 等）在 face_event 终端输入；face_video 仅采集与显示。
# 写入 PID 文件供 watchdog_face3.sh 使用（不依赖 ps）。
#
# 环境变量：
#   PID_DIR           PID 目录，默认 /tmp/face3_pids
#   FACE_METRICS=1    face_video / face_ai 周期性输出指标到 stderr
#   RUN_FACE3_RESTART=1  任一子进程退出后：先杀其余子进程，再整组重新拉起（循环监督）
#   FAS_KMODEL=路径   静默活体 kmodel；文件不存在则不带第 9 参数（不启用活体）
#   FACE_FAS_REAL_THRESH 活体 REAL 概率阈值，默认 0.5
#
# 默认：任一子进程退出 → 杀光其余进程 → 退出码 1（便于 systemd/openrc 重启策略）

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BIN_DIR="${SCRIPT_DIR}"

PID_DIR="${PID_DIR:-/tmp/face3_pids}"
mkdir -p "${PID_DIR}" || {
    echo "run_face3: cannot mkdir ${PID_DIR}" >&2
    exit 1
}

DET_KMODEL="${DET_KMODEL:-${BIN_DIR}/face_detection_320.kmodel}"
REC_KMODEL="${REC_KMODEL:-${BIN_DIR}/face_recognition_mobilefacenet.kmodel}"
DB_DIR="${DB_DIR:-${BIN_DIR}/face_db}"
LOG_FILE="${LOG_FILE:-/tmp/attendance.log}"
DEBUG_VIDEO="${DEBUG_VIDEO:-0}"
DET_THRES="${DET_THRES:-0.5}"
NMS_THRES="${NMS_THRES:-0.2}"
REC_THRES="${REC_THRES:-70}"
DEBUG_AI="${DEBUG_AI:-0}"
FAS_KMODEL="${FAS_KMODEL:-${BIN_DIR}/face_antispoof.kmodel}"

kill_from_pidfile() {
    _f="$1"
    [ -f "$_f" ] || return 0
    _pid=$(cat "$_f" 2>/dev/null)
    [ -n "$_pid" ] || return 0
    kill "$_pid" 2>/dev/null || true
}

kill_all_children() {
    echo "run_face3: killing remaining children..."
    kill_from_pidfile "${PID_DIR}/face_video.pid"
    kill_from_pidfile "${PID_DIR}/face_ai.pid"
    kill_from_pidfile "${PID_DIR}/face_event.pid"
    killall face_video.elf 2>/dev/null || true
    killall face_ai.elf 2>/dev/null || true
    killall face_event.elf 2>/dev/null || true
    rm -f "${PID_DIR}/face_event.pid" "${PID_DIR}/face_ai.pid" "${PID_DIR}/face_video.pid" 2>/dev/null || true
}

# 三个 PID 均存在且仍存活
all_children_alive() {
    for _n in face_event face_ai face_video; do
        _f="${PID_DIR}/${_n}.pid"
        [ -f "$_f" ] || return 1
        _pid=$(cat "$_f" 2>/dev/null)
        [ -n "$_pid" ] || return 1
        if [ -d "/proc/${_pid}" ]; then
            continue
        fi
        kill -0 "$_pid" 2>/dev/null || return 1
    done
    return 0
}

cleanup_on_signal() {
    echo "run_face3: caught signal, stopping children..." >&2
    kill_all_children
    exit 130
}
trap cleanup_on_signal INT TERM

start_three() {
    echo "[1/3] face_event.elf -> ${LOG_FILE} pid -> ${PID_DIR}/face_event.pid"
    "${BIN_DIR}/face_event.elf" "${LOG_FILE}" &
    echo $! > "${PID_DIR}/face_event.pid"
    sleep 0.5

    echo "[2/3] face_ai.elf pid -> ${PID_DIR}/face_ai.pid"
    if [ -f "${FAS_KMODEL}" ]; then
        echo "run_face3: FaceAntiSpoof kmodel: ${FAS_KMODEL}"
        "${BIN_DIR}/face_ai.elf" "${DET_KMODEL}" "${DET_THRES}" "${NMS_THRES}" "${REC_KMODEL}" "${REC_THRES}" \
            "${DB_DIR}" "${DEBUG_AI}" "${FAS_KMODEL}" &
    else
        echo "run_face3: no ${FAS_KMODEL}, face_ai without liveness"
        "${BIN_DIR}/face_ai.elf" "${DET_KMODEL}" "${DET_THRES}" "${NMS_THRES}" "${REC_KMODEL}" "${REC_THRES}" \
            "${DB_DIR}" "${DEBUG_AI}" &
    fi
    echo $! > "${PID_DIR}/face_ai.pid"
    sleep 1

    echo "[3/3] face_video.elf (debug=${DEBUG_VIDEO}) pid -> ${PID_DIR}/face_video.pid"
    "${BIN_DIR}/face_video.elf" "${DEBUG_VIDEO}" &
    echo $! > "${PID_DIR}/face_video.pid"

    echo "run_face3: PIDs in ${PID_DIR}; supervisor polling every 2s (any child exit -> kill others)"
}

# 一轮监督：启动三进程，直到任一退出则清理并返回 1
run_one_round() {
    start_three
    while true; do
        sleep 2
        if all_children_alive; then
            continue
        fi
        echo "run_face3: one or more children exited, cleaning up..." >&2
        kill_all_children
        return 1
    done
}

# 主循环：默认单轮后退出；RUN_FACE3_RESTART=1 时子进程死后自动再拉起
while true; do
    run_one_round
    _rc=$?
    if [ "${RUN_FACE3_RESTART:-0}" = "1" ]; then
        echo "run_face3: RUN_FACE3_RESTART=1, restarting in 2s..." >&2
        sleep 2
        continue
    fi
    exit "${_rc:-1}"
done
