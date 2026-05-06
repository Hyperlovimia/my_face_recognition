#ifndef MY_FACE_ATTENDANCE_LOG_H
#define MY_FACE_ATTENDANCE_LOG_H

#include <cstdint>
#include <string>

#include "ipc_proto.h"

/**
 * 解析考勤根目录：
 * argv[1] > getenv("FACE_ATT_LOG_BASE") > getenv("FACE_ATT_LOG") > /sd/face_logs
 * （约定为根目录；每日单文件 YYYY-MM-DD.jsonl）
 */
std::string attendance_log_resolve_base_dir(int argc, char **argv);

/** JSONL：wall_time 为本地时分秒字符串 HH:MM:SS（文件名仍为当日 YYYY-MM-DD.jsonl）；大核时钟未校准时可能不准，TF 以 face_netd 为准。 */
bool attendance_log_append_ipc_evt(const std::string &base_dir, const ipc_evt_t *ev, std::string *used_file_out);

/** 进程启停等元数据，evt_kind 固定用 "meta" */
bool attendance_log_append_meta(const std::string &base_dir, const char *note, std::string *used_file_out);

#endif
