#!/bin/sh
# 守护三进程：通过 PID 文件 + /proc 或 kill -0 检测存活（不依赖 ps/grep）。
# 停止旧进程时用 PID 文件 kill，避免依赖 killall（裁剪系统可能没有）。
# 前置：使用 run_face3.sh 启动以生成 ${PID_DIR}/*.pid
# 环境变量：PID_DIR（默认 /tmp/face3_pids）、RUN_SCRIPT、BIN_DIR

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BIN_DIR="${BIN_DIR:-${SCRIPT_DIR}}"
RUN_SCRIPT="${RUN_SCRIPT:-${SCRIPT_DIR}/run_face3.sh}"
PID_DIR="${PID_DIR:-/tmp/face3_pids}"

# 优先 /proc（裁剪系统常保留）；否则尝试 kill -0
proc_alive() {
    _f="$1"
    [ -f "$_f" ] || return 1
    _pid=$(cat "$_f" 2>/dev/null)
    [ -n "$_pid" ] || return 1
    if [ -d "/proc/${_pid}" ]; then
        return 0
    fi
    kill -0 "$_pid" 2>/dev/null
}

alive_all() {
    proc_alive "${PID_DIR}/face_event.pid" &&
        proc_alive "${PID_DIR}/face_ai.pid" &&
        proc_alive "${PID_DIR}/face_video.pid"
}

# 按与启动相反的顺序结束（先停采集/显示，再 AI，再事件）
kill_from_pidfile() {
    _f="$1"
    [ -f "$_f" ] || return 0
    _pid=$(cat "$_f" 2>/dev/null)
    [ -n "$_pid" ] || return 0
    kill "$_pid" 2>/dev/null || true
}

kill_all_three() {
    kill_from_pidfile "${PID_DIR}/face_video.pid"
    kill_from_pidfile "${PID_DIR}/face_ai.pid"
    kill_from_pidfile "${PID_DIR}/face_event.pid"
    # 兜底：若仍有同名进程（无 pid 文件场景）
    killall face_video.elf 2>/dev/null || true
    killall face_ai.elf 2>/dev/null || true
    killall face_event.elf 2>/dev/null || true
}

echo "watchdog_face3: PID_DIR=${PID_DIR}, restart=${RUN_SCRIPT}"

while true; do
    sleep 3
    if alive_all; then
        continue
    fi
    echo "watchdog_face3: one or more processes missing, restarting..."
    kill_all_three
    sleep 1
    rm -f "${PID_DIR}/face_event.pid" "${PID_DIR}/face_ai.pid" "${PID_DIR}/face_video.pid" 2>/dev/null || true
    (cd "${BIN_DIR}" && exec "${RUN_SCRIPT}") &
done
