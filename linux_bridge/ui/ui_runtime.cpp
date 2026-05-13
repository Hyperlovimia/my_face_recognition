#include "ui_runtime.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

extern "C" {
#include "lvgl.h"
}

#include "k230_port.h"

namespace {

enum class UiSessionState {
    idle = 0,
    preview_requested,
    editing_name,
    commit_pending,
};

struct UiSharedState {
    runtime_config cfg;
    std::mutex mu;
    std::map<std::string, int> pending_cmds;
    std::atomic<bool> running{false};
    std::atomic<bool> stop{false};
    bool dirty = true;
    bool active = false;
    UiSessionState session = UiSessionState::idle;
    std::string status_text = "Tap Register to add face";
    std::string active_request_id;
    uint64_t last_activity_ms = 0;
    uint64_t request_seq = 0;
    int preview_timeout_ms = 30000;
};

UiSharedState g_ui;
std::thread g_ui_thread;

lv_obj_t *g_main_screen = nullptr;
lv_obj_t *g_edit_screen = nullptr;
lv_obj_t *g_main_status = nullptr;
lv_obj_t *g_edit_status = nullptr;
lv_obj_t *g_register_btn = nullptr;
lv_obj_t *g_name_ta = nullptr;
lv_obj_t *g_keyboard = nullptr;
lv_obj_t *g_cancel_btn = nullptr;

std::string g_last_bound_request_id;
UiSessionState g_last_session = UiSessionState::idle;

uint64_t monotonic_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void ui_log(const std::string &msg)
{
    if (g_ui.cfg.log_message)
        g_ui.cfg.log_message(msg);
}

std::string next_ui_request_id_locked()
{
    ++g_ui.request_seq;
    return "ui_" + std::to_string(monotonic_ms()) + "_" + std::to_string(g_ui.request_seq);
}

void submit_local_command(int cmd, const std::string &name)
{
    std::function<void(const std::string &, int, const std::string &)> submit;
    std::string request_id;
    {
        std::lock_guard<std::mutex> lk(g_ui.mu);
        if (!g_ui.cfg.submit_command)
            return;
        submit = g_ui.cfg.submit_command;
        request_id = next_ui_request_id_locked();
        g_ui.pending_cmds[request_id] = cmd;
        g_ui.active_request_id = request_id;
        g_ui.last_activity_ms = monotonic_ms();
        g_ui.dirty = true;
        g_ui.active = true;
    }
    submit(request_id, cmd, name);
}

void set_idle_status_locked(const std::string &text)
{
    g_ui.session = UiSessionState::idle;
    g_ui.active = !g_ui.pending_cmds.empty();
    g_ui.active_request_id.clear();
    g_ui.status_text = text;
    g_ui.last_activity_ms = monotonic_ms();
    g_ui.dirty = true;
}

void on_register_click(lv_event_t *)
{
    bool can_submit = false;
    {
        std::lock_guard<std::mutex> lk(g_ui.mu);
        if (g_ui.session == UiSessionState::idle && g_ui.cfg.submit_command)
        {
            g_ui.session = UiSessionState::preview_requested;
            g_ui.status_text = "Capturing preview...";
            g_ui.last_activity_ms = monotonic_ms();
            g_ui.dirty = true;
            can_submit = true;
        }
    }
    if (can_submit)
        submit_local_command(IPC_BRIDGE_CMD_REGISTER_PREVIEW, "");
}

void on_cancel_click(lv_event_t *)
{
    bool should_cancel = false;
    {
        std::lock_guard<std::mutex> lk(g_ui.mu);
        if (g_ui.session == UiSessionState::editing_name || g_ui.session == UiSessionState::preview_requested)
        {
            g_ui.pending_cmds.clear();
            g_ui.status_text = "Registration cancelled";
            g_ui.last_activity_ms = monotonic_ms();
            g_ui.dirty = true;
            should_cancel = true;
        }
    }
    if (should_cancel)
    {
        submit_local_command(IPC_BRIDGE_CMD_REGISTER_CANCEL, "");
        std::lock_guard<std::mutex> lk(g_ui.mu);
        set_idle_status_locked("Registration cancelled");
    }
}

void on_keyboard_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED)
    {
        std::lock_guard<std::mutex> lk(g_ui.mu);
        g_ui.last_activity_ms = monotonic_ms();
        return;
    }
    if (code == LV_EVENT_CANCEL)
    {
        on_cancel_click(e);
        return;
    }
    if (code != LV_EVENT_READY)
        return;

    const char *name = lv_textarea_get_text(g_name_ta);
    const std::string trimmed = name ? name : "";
    if (trimmed.empty())
    {
        std::lock_guard<std::mutex> lk(g_ui.mu);
        g_ui.status_text = "Please enter a name first";
        g_ui.dirty = true;
        return;
    }

    bool can_submit = false;
    {
        std::lock_guard<std::mutex> lk(g_ui.mu);
        if (g_ui.session == UiSessionState::editing_name)
        {
            g_ui.session = UiSessionState::commit_pending;
            g_ui.status_text = "Registering...";
            g_ui.last_activity_ms = monotonic_ms();
            g_ui.dirty = true;
            can_submit = true;
        }
    }
    if (can_submit)
        submit_local_command(IPC_BRIDGE_CMD_REGISTER_COMMIT, trimmed);
}

void create_main_screen()
{
    g_main_screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(g_main_screen);
    lv_obj_set_style_bg_opa(g_main_screen, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t *panel = lv_obj_create(g_main_screen);
    lv_obj_set_size(panel, lv_pct(96), 220);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x162231), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 18, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Board Registration");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    g_main_status = lv_label_create(panel);
    lv_label_set_text(g_main_status, "Tap Register to add face");
    lv_label_set_long_mode(g_main_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_main_status, lv_pct(86));
    lv_obj_set_style_text_align(g_main_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(g_main_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(g_main_status, LV_ALIGN_TOP_MID, 0, 82);

    g_register_btn = lv_btn_create(panel);
    lv_obj_set_size(g_register_btn, 280, 72);
    lv_obj_align(g_register_btn, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_add_event_cb(g_register_btn, on_register_click, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *btn_label = lv_label_create(g_register_btn);
    lv_label_set_text(btn_label, "Register");
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_center(btn_label);
}

void create_edit_screen()
{
    g_edit_screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(g_edit_screen);
    lv_obj_set_style_bg_opa(g_edit_screen, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t *panel = lv_obj_create(g_edit_screen);
    lv_obj_set_size(panel, lv_pct(98), lv_pct(98));
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 18, LV_PART_MAIN);

    g_cancel_btn = lv_btn_create(panel);
    lv_obj_set_size(g_cancel_btn, 150, 54);
    lv_obj_align(g_cancel_btn, LV_ALIGN_TOP_LEFT, 18, 18);
    lv_obj_add_event_cb(g_cancel_btn, on_cancel_click, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cancel_label = lv_label_create(g_cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Enter Name");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    g_edit_status = lv_label_create(panel);
    lv_label_set_text(g_edit_status, "Preview ready. Type a name.");
    lv_label_set_long_mode(g_edit_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_edit_status, lv_pct(86));
    lv_obj_set_style_text_align(g_edit_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(g_edit_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(g_edit_status, LV_ALIGN_TOP_MID, 0, 92);

    g_name_ta = lv_textarea_create(panel);
    lv_obj_set_size(g_name_ta, lv_pct(90), LV_SIZE_CONTENT);
    lv_textarea_set_one_line(g_name_ta, true);
    lv_textarea_set_max_length(g_name_ta, 32);
    lv_obj_set_style_text_font(g_name_ta, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_align(g_name_ta, LV_ALIGN_TOP_MID, 0, 138);

    g_keyboard = lv_keyboard_create(panel);
    lv_keyboard_set_mode(g_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(g_keyboard, g_name_ta);
    lv_obj_set_size(g_keyboard, lv_pct(96), 430);
    lv_obj_align(g_keyboard, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_add_event_cb(g_keyboard, on_keyboard_event, LV_EVENT_ALL, nullptr);
}

void apply_snapshot_to_ui(UiSessionState session, const std::string &status, const std::string &request_id)
{
    if (session == UiSessionState::idle)
    {
        if (lv_scr_act() != g_main_screen)
            lv_scr_load(g_main_screen);
        lv_label_set_text(g_main_status, status.c_str());
        lv_obj_clear_state(g_register_btn, LV_STATE_DISABLED);
        return;
    }

    if (lv_scr_act() != g_edit_screen)
        lv_scr_load(g_edit_screen);

    if (g_last_bound_request_id != request_id && session == UiSessionState::editing_name)
    {
        lv_textarea_set_text(g_name_ta, "");
        g_last_bound_request_id = request_id;
    }

    lv_label_set_text(g_edit_status, status.c_str());

    const bool editable = session == UiSessionState::editing_name;
    if (editable)
    {
        lv_obj_clear_state(g_name_ta, LV_STATE_DISABLED);
        lv_obj_clear_state(g_cancel_btn, LV_STATE_DISABLED);
        lv_obj_clear_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_state(g_name_ta, LV_STATE_DISABLED);
        if (session == UiSessionState::commit_pending)
            lv_obj_add_state(g_cancel_btn, LV_STATE_DISABLED);
        else
            lv_obj_clear_state(g_cancel_btn, LV_STATE_DISABLED);
        if (session == UiSessionState::preview_requested)
            lv_obj_add_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

void maybe_timeout_session()
{
    std::function<void(const std::string &, int, const std::string &)> submit;
    bool fire_cancel = false;
    {
        std::lock_guard<std::mutex> lk(g_ui.mu);
        if (g_ui.preview_timeout_ms <= 0)
            return;
        if (g_ui.session != UiSessionState::preview_requested && g_ui.session != UiSessionState::editing_name)
            return;
        if ((monotonic_ms() - g_ui.last_activity_ms) < static_cast<uint64_t>(g_ui.preview_timeout_ms))
            return;
        submit = g_ui.cfg.submit_command;
        g_ui.pending_cmds.clear();
        g_ui.status_text = "Registration timed out";
        set_idle_status_locked("Registration timed out");
        fire_cancel = static_cast<bool>(submit);
    }
    if (fire_cancel)
        submit_local_command(IPC_BRIDGE_CMD_REGISTER_CANCEL, "");
}

bool resolve_profile(const std::string &profile, k230_ui_port_config *cfg)
{
    if (!cfg)
        return false;
    if (profile != "dongshanpi_nt35516")
        return false;
    cfg->drm_device = "/dev/dri/card0";
    cfg->touch_device = g_ui.cfg.touch_device.empty() ? "/dev/input/event0" : g_ui.cfg.touch_device.c_str();
    cfg->overlay_width = 1080;
    cfg->overlay_height = 720;
    cfg->offset_x = 0;
    cfg->offset_y = 0;
    cfg->align_bottom = true;
    cfg->flip_x = true;
    cfg->flip_y = true;
    return true;
}

void ui_thread_main()
{
    k230_ui_port_config port_cfg{};
    if (!resolve_profile(g_ui.cfg.overlay_profile, &port_cfg))
    {
        ui_log("face_netd_ui: unsupported overlay profile: " + g_ui.cfg.overlay_profile);
        g_ui.running.store(false);
        return;
    }

    lv_init();
    if (!k230_ui_port_init(&port_cfg))
    {
        ui_log("face_netd_ui: k230_ui_port_init failed");
        g_ui.running.store(false);
        return;
    }

    create_main_screen();
    create_edit_screen();
    lv_scr_load(g_main_screen);

    while (!g_ui.stop.load())
    {
        UiSessionState session = UiSessionState::idle;
        std::string status;
        std::string request_id;
        {
            std::lock_guard<std::mutex> lk(g_ui.mu);
            session = g_ui.session;
            status = g_ui.status_text;
            request_id = g_ui.active_request_id;
            g_ui.dirty = false;
        }

        if (!status.empty() || g_last_session != session)
        {
            apply_snapshot_to_ui(session, status, request_id);
            g_last_session = session;
        }

        maybe_timeout_session();
        lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    k230_ui_port_deinit();
    g_ui.running.store(false);
}

}  // namespace

bool ui_runtime_start(const runtime_config &cfg)
{
    if (!cfg.enabled)
        return true;

    {
        std::lock_guard<std::mutex> lk(g_ui.mu);
        g_ui.cfg = cfg;
        g_ui.preview_timeout_ms = cfg.preview_timeout_ms;
        g_ui.status_text = "Tap Register to add face";
        g_ui.dirty = true;
        g_ui.stop.store(false);
        g_ui.running.store(true);
    }

    g_ui_thread = std::thread(ui_thread_main);
    return true;
}

void ui_runtime_stop()
{
    if (!g_ui.running.load())
        return;
    g_ui.stop.store(true);
    if (g_ui_thread.joinable())
        g_ui_thread.join();
}

void ui_on_bridge_result(const bridge_cmd_result_t &result)
{
    std::lock_guard<std::mutex> lk(g_ui.mu);
    auto it = g_ui.pending_cmds.find(result.request_id);
    if (it == g_ui.pending_cmds.end())
        return;

    const int cmd = it->second;
    g_ui.pending_cmds.erase(it);
    g_ui.active = !g_ui.pending_cmds.empty() || g_ui.session != UiSessionState::idle;

    if (cmd == IPC_BRIDGE_CMD_REGISTER_PREVIEW)
    {
        if (result.ok)
        {
            g_ui.session = UiSessionState::editing_name;
            g_ui.status_text = "Preview ready. Type a name.";
            g_ui.active_request_id = result.request_id;
        }
        else
        {
            set_idle_status_locked(result.message[0] ? result.message : "Preview failed");
        }
    }
    else if (cmd == IPC_BRIDGE_CMD_REGISTER_COMMIT)
    {
        set_idle_status_locked(result.message[0] ? result.message
                                                 : (result.ok ? "Registration complete" : "Registration failed"));
    }
    else if (cmd == IPC_BRIDGE_CMD_REGISTER_CANCEL)
    {
        set_idle_status_locked(result.message[0] ? result.message : "Registration cancelled");
    }
    else if (cmd == IPC_BRIDGE_CMD_DB_RESET && !result.ok)
    {
        g_ui.status_text = result.message;
        g_ui.dirty = true;
    }
}

void ui_on_bridge_event(const bridge_event_t &ev)
{
    std::lock_guard<std::mutex> lk(g_ui.mu);
    if (g_ui.session != UiSessionState::idle)
        return;
    if (ev.evt_kind == IPC_EVT_KIND_STRANGER)
        g_ui.status_text = "Stranger detected. Tap Register.";
    else if (ev.evt_kind == IPC_EVT_KIND_LIVENESS_FAIL)
        g_ui.status_text = "Liveness failed. Align face and retry.";
    else
        g_ui.status_text = "Tap Register to add face";
    g_ui.dirty = true;
}

bool ui_is_session_active()
{
    if (!g_ui.running.load())
        return false;
    std::lock_guard<std::mutex> lk(g_ui.mu);
    return g_ui.session != UiSessionState::idle || !g_ui.pending_cmds.empty();
}
