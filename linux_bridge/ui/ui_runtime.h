#ifndef MY_FACE_UI_RUNTIME_H
#define MY_FACE_UI_RUNTIME_H

#include <functional>
#include <string>

#include "../../src/ipc_proto.h"

struct runtime_config {
    bool enabled = false;
    std::string touch_device;
    int preview_timeout_ms = 30000;
    std::string overlay_profile;
    std::function<void(const std::string &, int, const std::string &)> submit_command;
    std::function<void(const std::string &)> log_message;
};

bool ui_runtime_start(const runtime_config &cfg);
void ui_runtime_stop();
void ui_on_bridge_result(const bridge_cmd_result_t &result);
void ui_on_bridge_event(const bridge_event_t &ev);
void ui_on_shared_info(const bridge_ui_shared_info_t &info);
void ui_on_local_import_progress(const std::string &request_id, int current, int total);
bool ui_is_session_active();

#endif
