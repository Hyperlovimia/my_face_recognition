#include "ui_runtime.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <limits.h>
#include <unistd.h>

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
    bool have_shared_info = false;
    bool logged_waiting_for_surface = false;
    UiSessionState session = UiSessionState::idle;
    std::string status_text = "Tap signup to add face";
    std::string active_request_id;
    uint64_t last_activity_ms = 0;
    uint64_t request_seq = 0;
    int preview_timeout_ms = 30000;
    bridge_ui_shared_info_t shared_info{};
};

UiSharedState g_ui;
std::thread g_ui_thread;

lv_obj_t *g_main_screen = nullptr;
lv_obj_t *g_edit_screen = nullptr;
lv_obj_t *g_main_status = nullptr;
lv_obj_t *g_edit_status = nullptr;
lv_obj_t *g_signup_btn = nullptr;
lv_obj_t *g_import_btn = nullptr;
lv_obj_t *g_delete_btn = nullptr;
lv_obj_t *g_cancel_btn = nullptr;
lv_obj_t *g_name_ta = nullptr;
lv_obj_t *g_keyboard = nullptr;

std::string g_last_bound_request_id;
UiSessionState g_last_session = UiSessionState::idle;

namespace fs = std::filesystem;

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

std::string executable_dir()
{
    char exe_path[PATH_MAX] = {0};
    const ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (n <= 0)
        return ".";
    exe_path[n] = '\0';
    return fs::path(exe_path).parent_path().string();
}

std::string ui_asset_path(const char *file_name)
{
    const fs::path exe_dir = executable_dir();
    const fs::path local_data = exe_dir / "data" / "img" / file_name;
    if (fs::exists(local_data))
        return local_data.string();

    const fs::path source_data = exe_dir.parent_path() / "ui" / "data" / "img" / file_name;
    if (fs::exists(source_data))
        return source_data.string();

    return local_data.string();
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

void on_delete_click(lv_event_t *)
{
    bool can_submit = false;
    {
        std::lock_guard<std::mutex> lk(g_ui.mu);
        if (g_ui.session == UiSessionState::idle && g_ui.cfg.submit_command)
        {
            g_ui.status_text = "Clearing database...";
            g_ui.last_activity_ms = monotonic_ms();
            g_ui.dirty = true;
            can_submit = true;
        }
    }
    if (can_submit)
        submit_local_command(IPC_BRIDGE_CMD_DB_RESET, "");
}

void on_import_click(lv_event_t *)
{
    std::lock_guard<std::mutex> lk(g_ui.mu);
    if (g_ui.session != UiSessionState::idle)
        return;
    g_ui.status_text = "Import is not available in this project";
    g_ui.last_activity_ms = monotonic_ms();
    g_ui.dirty = true;
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

lv_obj_t *create_icon_action(lv_obj_t *parent, const char *image_name, const char *fallback_text, lv_coord_t x_ofs,
                             lv_event_cb_t cb)
{
    const std::string asset_path = ui_asset_path(image_name);
    lv_obj_t *obj = nullptr;
    if (fs::exists(asset_path))
    {
        obj = lv_img_create(parent);
        lv_img_set_src(obj, asset_path.c_str());
    }
    else
    {
        obj = lv_btn_create(parent);
        lv_obj_set_size(obj, 148, 84);
        lv_obj_set_style_radius(obj, 22, LV_PART_MAIN);
        lv_obj_set_style_bg_color(obj, lv_color_hex(0x1d2d3f), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(obj, LV_OPA_90, LV_PART_MAIN);
        lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
        lv_obj_t *label = lv_label_create(obj);
        lv_label_set_text(label, fallback_text);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_center(label);
    }

    lv_obj_set_align(obj, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_pos(obj, x_ofs, -90);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, nullptr);
    return obj;
}

void create_main_screen()
{
    g_main_screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(g_main_screen);
    lv_obj_set_style_bg_opa(g_main_screen, LV_OPA_TRANSP, LV_PART_MAIN);

    g_main_status = lv_label_create(g_main_screen);
    lv_label_set_text(g_main_status, "Tap signup to add face");
    lv_label_set_long_mode(g_main_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_main_status, lv_pct(88));
    lv_obj_set_style_text_align(g_main_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_main_status, lv_color_hex(0xf5f7fa), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_main_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(g_main_status, LV_ALIGN_TOP_MID, 0, 72);

    g_import_btn = create_icon_action(g_main_screen, "import.png", "Import", -160, on_import_click);
    g_signup_btn = create_icon_action(g_main_screen, "signup.png", "Signup", 0, on_register_click);
    g_delete_btn = create_icon_action(g_main_screen, "delete.png", "Delete", 160, on_delete_click);
}

void create_edit_screen()
{
    g_edit_screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(g_edit_screen);
    lv_obj_set_style_bg_opa(g_edit_screen, LV_OPA_TRANSP, LV_PART_MAIN);

    g_edit_status = lv_label_create(g_edit_screen);
    lv_label_set_text(g_edit_status, "Preview ready. Type a name.");
    lv_label_set_long_mode(g_edit_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_edit_status, lv_pct(86));
    lv_obj_set_style_text_align(g_edit_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_edit_status, lv_color_hex(0xf5f7fa), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_edit_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(g_edit_status, LV_ALIGN_TOP_MID, 0, 72);

    g_cancel_btn = lv_btn_create(g_edit_screen);
    lv_obj_set_size(g_cancel_btn, 130, 52);
    lv_obj_align(g_cancel_btn, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_set_style_radius(g_cancel_btn, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_cancel_btn, lv_color_hex(0x1d2d3f), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_cancel_btn, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_cancel_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(g_cancel_btn, on_cancel_click, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cancel_label = lv_label_create(g_cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(cancel_label);

    g_name_ta = lv_textarea_create(g_edit_screen);
    lv_obj_set_size(g_name_ta, lv_pct(100), LV_SIZE_CONTENT);
    lv_textarea_set_text(g_name_ta, "");
    lv_textarea_set_one_line(g_name_ta, true);
    lv_textarea_set_max_length(g_name_ta, 32);
    lv_textarea_set_align(g_name_ta, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_font(g_name_ta, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_radius(g_name_ta, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_name_ta, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_name_ta, LV_OPA_TRANSP, LV_PART_MAIN);

    g_keyboard = lv_keyboard_create(g_edit_screen);
    lv_obj_set_style_text_font(g_keyboard, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_keyboard_set_mode(g_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(g_keyboard, g_name_ta);
    lv_obj_set_size(g_keyboard, lv_pct(100), 580);
    lv_obj_align(g_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(g_keyboard, on_keyboard_event, LV_EVENT_ALL, nullptr);
    lv_obj_align_to(g_name_ta, g_keyboard, LV_ALIGN_OUT_TOP_MID, 0, -10);
}

void apply_snapshot_to_ui(UiSessionState session, const std::string &status, const std::string &request_id)
{
    if (session == UiSessionState::idle)
    {
        if (lv_scr_act() != g_main_screen)
            lv_scr_load(g_main_screen);
        lv_label_set_text(g_main_status, status.c_str());
        lv_obj_clear_state(g_signup_btn, LV_STATE_DISABLED);
        lv_obj_clear_state(g_import_btn, LV_STATE_DISABLED);
        lv_obj_clear_state(g_delete_btn, LV_STATE_DISABLED);
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
    cfg->touch_device = g_ui.cfg.touch_device.empty() ? "/dev/input/event0" : g_ui.cfg.touch_device.c_str();
    cfg->logical_width = static_cast<int>(UI_OVERLAY_SHARED_WIDTH);
    cfg->logical_height = static_cast<int>(UI_OVERLAY_SHARED_HEIGHT);
    cfg->screen_width = 1080;
    cfg->screen_height = 1920;
    cfg->flip_x = false;
    cfg->flip_y = false;
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

    bool lvgl_inited = false;
    bool port_ready = false;
    bool screens_created = false;
    uint32_t active_generation = 0;
    bool surface_change_logged = false;

    while (!g_ui.stop.load())
    {
        UiSessionState session = UiSessionState::idle;
        std::string status;
        std::string request_id;
        bool have_shared_info = false;
        bridge_ui_shared_info_t shared_info{};
        {
            std::lock_guard<std::mutex> lk(g_ui.mu);
            session = g_ui.session;
            status = g_ui.status_text;
            request_id = g_ui.active_request_id;
            g_ui.dirty = false;
            have_shared_info = g_ui.have_shared_info;
            if (have_shared_info)
                shared_info = g_ui.shared_info;
            if (!have_shared_info && !g_ui.logged_waiting_for_surface)
            {
                ui_log("face_netd_ui: waiting for RT shared overlay info");
                g_ui.logged_waiting_for_surface = true;
            }
        }

        if (!port_ready)
        {
            if (!have_shared_info)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            port_cfg.shared_info = shared_info;
            if (!lvgl_inited)
            {
                lv_init();
                lvgl_inited = true;
            }
            if (!k230_ui_port_init(&port_cfg))
            {
                ui_log("face_netd_ui: k230_ui_port_init failed");
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            if (!screens_created)
            {
                create_main_screen();
                create_edit_screen();
                screens_created = true;
            }
            lv_scr_load(g_main_screen);
            active_generation = shared_info.ui_generation;
            port_ready = true;
            ui_log("face_netd_ui: shared overlay ready generation=" + std::to_string(active_generation));
        }

        if (have_shared_info && shared_info.ui_generation != active_generation)
        {
            if (!surface_change_logged)
            {
                ui_log("face_netd_ui: shared overlay generation changed, keeping current surface until restart");
                surface_change_logged = true;
            }
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

    if (port_ready)
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
        g_ui.status_text = "Tap signup to add face";
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
    else if (cmd == IPC_BRIDGE_CMD_DB_RESET)
    {
        set_idle_status_locked(result.message[0] ? result.message
                                                 : (result.ok ? "Database cleared" : "Database clear failed"));
    }
}

void ui_on_bridge_event(const bridge_event_t &ev)
{
    std::lock_guard<std::mutex> lk(g_ui.mu);
    if (g_ui.session != UiSessionState::idle)
        return;
    if (ev.evt_kind == IPC_EVT_KIND_STRANGER)
        g_ui.status_text = "Stranger detected. Tap signup.";
    else if (ev.evt_kind == IPC_EVT_KIND_LIVENESS_FAIL)
        g_ui.status_text = "Liveness failed. Align face and retry.";
    else
        g_ui.status_text = "Tap signup to add face";
    g_ui.dirty = true;
}

void ui_on_shared_info(const bridge_ui_shared_info_t &info)
{
    if (info.magic != IPC_MAGIC)
        return;

    std::lock_guard<std::mutex> lk(g_ui.mu);
    if (g_ui.have_shared_info && std::memcmp(&g_ui.shared_info, &info, sizeof(info)) == 0)
        return;

    g_ui.shared_info = info;
    g_ui.have_shared_info = true;
    g_ui.logged_waiting_for_surface = false;
    g_ui.dirty = true;
}

bool ui_is_session_active()
{
    if (!g_ui.running.load())
        return false;
    std::lock_guard<std::mutex> lk(g_ui.mu);
    return g_ui.session != UiSessionState::idle || !g_ui.pending_cmds.empty();
}
