#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <ctime>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

extern "C" {
#include "k_ipcmsg.h"
}

#include "../src/ipc_proto.h"
#include "ui/ui_runtime.h"
#include "third_party/mongoose/mongoose.h"

namespace fs = std::filesystem;

/* Must match face_event.elf / ipc_proto.h on K230; do not renumber without redeploying all sides. */
static_assert(sizeof(bridge_cmd_req_t) == 136, "bridge_cmd_req_t ABI");
static_assert(IPC_BRIDGE_CMD_REGISTER_PREVIEW == 5, "wire cmd for register_preview");

namespace {

constexpr const char *kSchema = "k230.face.bridge.v1";
constexpr int kIpcRemoteIdLinux = 1;
constexpr int kMqttPollMs = 200;
constexpr int kMqttConnectTimeoutMs = 10000;
constexpr int kMqttRetryInitialMs = 1000;
constexpr int kMqttRetryMaxMs = 8000;
constexpr size_t kMaxQueueDepth = 512;
/** Min / max for heartbeat_interval_ms after load (avoid MQTT flood or accidental hang). */
constexpr int kHeartbeatMinMs = 200;
constexpr int kHeartbeatMaxMs = 120000;
constexpr int kHeartbeatDefaultMs = 1000;
constexpr const char *kImportSourceDir = "/mnt/tf/faces_import";
constexpr size_t kImportErrorDetailLimit = 20;
constexpr int kImportChildResultTimeoutMs = 120000;

struct Config {
    std::string device_id = "k230-dev-01";
    std::string mqtt_url = "mqtt://192.168.1.10:1883";
    std::string mqtt_client_id = "face-netd-k230-dev-01";
    std::string mqtt_username;
    std::string mqtt_password;
    /** Linux 小核可见的人脸库目录（与 RT face_ai 的 db_dir 一致，常为 /sharefs/face_db） */
    std::string face_db_dir = "/sharefs/face_db";
    /** TF 考勤根目录；每日单文件：<base>/<YYYY-MM-DD>.jsonl */
    std::string attendance_log_base = "/mnt/tf/face_logs";
    /** 考勤日志 wall_time / 文件名日期：相对 UTC 向东偏移的分钟数（中国常为 480）。MQTT/HTTP 带回 pc_utc_offset_minutes 后自动覆盖。 */
    int attendance_tz_offset_minutes = 480;
    /** server_pc HTTP 根 URL（如 http://192.168.1.10:8000）；空则从 mqtt_url 主机推导 :8000；disabled 则关闭校时拉取 */
    std::string server_http_base;
    int heartbeat_interval_ms = kHeartbeatDefaultMs;
    int ipc_connect_retry_ms = 500;
    int ipc_sync_timeout_ms = 3000;
    int mqtt_keepalive_s = 15;
    bool ui_enabled = false;
    std::string ui_touch_device = "/dev/input/event0";
    int ui_preview_timeout_ms = 30000;
    std::string ui_overlay_profile = "dongshanpi_nt35516";
};

struct PublishItem {
    std::string topic;
    std::string payload;
    uint8_t qos = 1;
    bool retain = false;
};

enum class PendingCmdSource {
    remote = 0,
    ui = 1,
};

struct PendingCmd {
    std::string request_id;
    int cmd = IPC_BRIDGE_CMD_NONE;
    std::string name;
    PendingCmdSource source = PendingCmdSource::remote;
};

struct Runtime {
    Config cfg;
    std::atomic<bool> stop{false};
    std::atomic<bool> mqtt_connected{false};
    std::atomic<bool> rt_connected{false};
    std::atomic<bool> status_dirty{true};
    std::atomic<bool> shutdown_after_flush{false};
    std::atomic<bool> runner_alive{false};
    std::atomic<int> ipc_id{-1};
    std::atomic<int> db_count{-1};
    std::atomic<uint64_t> last_rt_seen_ms{0};
    std::atomic<uint64_t> mqtt_dial_start_ms{0};
    std::atomic<uint64_t> mqtt_last_progress_ms{0};
    std::atomic<uint64_t> mqtt_last_pending_log_ms{0};
    std::atomic<uint64_t> mqtt_next_dial_ms{0};
    std::atomic<uint32_t> mqtt_connect_attempts{0};
    std::atomic<uint32_t> mqtt_retry_failures{0};

    std::mutex out_mu;
    std::deque<PublishItem> outbox;

    std::mutex cmd_mu;
    std::condition_variable cmd_cv;
    std::deque<PendingCmd> cmd_queue;

    std::mutex status_mu;
    uint64_t last_status_publish_ms = 0;

    /** serialize TF attendance JSONL writes from ipc callback thread */
    std::mutex attendance_sd_log_mu;
    std::atomic<bool> import_active{false};

    std::mutex internal_reply_mu;
    std::condition_variable internal_reply_cv;
    std::set<std::string> internal_requests;
    std::map<std::string, bridge_cmd_result_t> internal_results;
    std::atomic<uint64_t> internal_request_seq{0};
};

Runtime g_rt;

/** 与 server_pc main.py _MIN_SANE_DEVICE_EPOCH_MS 一致：小于此值的 Linux epoch ms 视为未校时 */
constexpr uint64_t kMinSaneLinuxEpochMs = 1000000000000ULL;

std::mutex g_att_wall_cal_mu;
uint64_t g_att_wall_ref_server_ms = 0;
uint64_t g_att_wall_ref_linux_ms = 0;
std::atomic<uint64_t> g_att_wall_last_http_warn_linux_ms{0};
/** PC 下发的本地时区相对 UTC 偏移（东为正分）；INT32_MIN 表示尚未收到 MQTT/HTTP。 */
std::atomic<int32_t> g_pc_utc_offset_minutes{INT32_MIN};

mg_mgr g_mgr;
mg_connection *g_mqtt = nullptr;

uint64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void sleep_ms(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string trim(const std::string &s)
{
    const char *ws = " \t\r\n";
    const size_t begin = s.find_first_not_of(ws);
    if (begin == std::string::npos)
        return "";
    const size_t end = s.find_last_not_of(ws);
    return s.substr(begin, end - begin + 1);
}

static bool is_ymd_calendar(const std::string &s)
{
    if (s.size() != 10 || s[4] != '-' || s[7] != '-')
        return false;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (i == 4 || i == 7)
            continue;
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    return true;
}

static uint64_t wall_calendar_ms_for_attendance_folder();

/** Civil date/time for fixed offset east of UTC (minutes). Uses gmtime_r(utc + offset) trick; no historical DST tables. */
static bool utc_ms_to_civil_tm_fixed_offset(uint64_t utc_ms, long offset_minutes_east, struct tm *out_tm)
{
    if (!out_tm)
        return false;
    const std::time_t utc_sec = static_cast<std::time_t>(utc_ms / 1000ULL);
    const std::time_t adj = utc_sec + offset_minutes_east * 60;
    return gmtime_r(&adj, out_tm) != nullptr;
}

static long effective_attendance_tz_offset_minutes()
{
    const int32_t synced = g_pc_utc_offset_minutes.load(std::memory_order_relaxed);
    if (synced == INT32_MIN)
        return static_cast<long>(g_rt.cfg.attendance_tz_offset_minutes);
    /* server_pc 在 Docker 里常为 UTC：mqtt 里 utc_offset_minutes=0 会与本地办公桌时区不一致。
     * 此时若 face_netd.ini 配置了非 0 区（默认 480），用工卡 ini；要严格按 UTC 记考勤请写 attendance_tz_offset_minutes=0。 */
    if (synced == 0 && g_rt.cfg.attendance_tz_offset_minutes != 0)
        return static_cast<long>(g_rt.cfg.attendance_tz_offset_minutes);
    return static_cast<long>(synced);
}

static std::string today_local_ymd_linux()
{
    const uint64_t ms = wall_calendar_ms_for_attendance_folder();
    struct tm tm_local {};
    if (!utc_ms_to_civil_tm_fixed_offset(ms, effective_attendance_tz_offset_minutes(), &tm_local))
        return {};
    char buf[16]{};
    if (strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_local) <= 0)
        return {};
    return std::string(buf);
}

bool parse_int(const std::string &s, int *out)
{
    if (!out)
        return false;
    char *end = nullptr;
    long v = strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0')
        return false;
    *out = static_cast<int>(v);
    return true;
}

bool parse_bool(const std::string &s, bool *out)
{
    if (!out)
        return false;
    std::string v;
    v.reserve(s.size());
    for (unsigned char ch : s)
        v.push_back(static_cast<char>(std::tolower(ch)));
    if (v == "1" || v == "true" || v == "yes" || v == "on")
    {
        *out = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off")
    {
        *out = false;
        return true;
    }
    return false;
}

std::string json_escape(const std::string &input)
{
    std::ostringstream oss;
    for (unsigned char ch : input)
    {
        switch (ch)
        {
        case '\\':
            oss << "\\\\";
            break;
        case '"':
            oss << "\\\"";
            break;
        case '\b':
            oss << "\\b";
            break;
        case '\f':
            oss << "\\f";
            break;
        case '\n':
            oss << "\\n";
            break;
        case '\r':
            oss << "\\r";
            break;
        case '\t':
            oss << "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch) << std::dec;
            }
            else
            {
                oss << ch;
            }
        }
    }
    return oss.str();
}

bool load_config(const std::string &path, Config *cfg)
{
    if (!cfg)
        return false;

    std::ifstream ifs(path);
    if (!ifs.is_open())
    {
        std::cerr << "face_netd: cannot open config " << path << std::endl;
        return false;
    }

    std::string line;
    int line_no = 0;
    while (std::getline(ifs, line))
    {
        ++line_no;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;

        const size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            std::cerr << "face_netd: ignore malformed config line " << line_no << std::endl;
            continue;
        }

        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));

        if (key == "device_id")
            cfg->device_id = value;
        else if (key == "mqtt_url")
            cfg->mqtt_url = value;
        else if (key == "mqtt_client_id")
            cfg->mqtt_client_id = value;
        else if (key == "mqtt_username")
            cfg->mqtt_username = value;
        else if (key == "mqtt_password")
            cfg->mqtt_password = value;
        else if (key == "heartbeat_interval_ms")
            parse_int(value, &cfg->heartbeat_interval_ms);
        else if (key == "ipc_connect_retry_ms")
            parse_int(value, &cfg->ipc_connect_retry_ms);
        else if (key == "ipc_sync_timeout_ms")
            parse_int(value, &cfg->ipc_sync_timeout_ms);
        else if (key == "mqtt_keepalive_s")
            parse_int(value, &cfg->mqtt_keepalive_s);
        else if (key == "ui_enabled")
            parse_bool(value, &cfg->ui_enabled);
        else if (key == "ui_touch_device")
            cfg->ui_touch_device = value;
        else if (key == "ui_preview_timeout_ms")
            parse_int(value, &cfg->ui_preview_timeout_ms);
        else if (key == "ui_overlay_profile")
            cfg->ui_overlay_profile = value;
        else if (key == "face_db_dir")
            cfg->face_db_dir = value;
        else if (key == "attendance_log_base")
            cfg->attendance_log_base = value;
        else if (key == "attendance_tz_offset_minutes")
            parse_int(value, &cfg->attendance_tz_offset_minutes);
        else if (key == "server_http_base")
            cfg->server_http_base = value;
        else if (key == "attendance_log_path")
        {
            /* 兼容旧配置：若写的是 …/face_logs/xxx.log，去掉文件名当作根目录 */
            std::string v = value;
            while (!v.empty() && (v.back() == '/' || v.back() == '\\'))
                v.pop_back();
            const size_t slash = v.find_last_of('/');
            if (slash != std::string::npos)
                cfg->attendance_log_base = v.substr(0, slash);
            else
                cfg->attendance_log_base = v;
        }
        else
            std::cerr << "face_netd: ignore unknown config key: " << key << std::endl;
    }

    if (cfg->device_id.empty())
        cfg->device_id = "k230-dev-01";
    if (cfg->mqtt_client_id.empty())
        cfg->mqtt_client_id = "face-netd-" + cfg->device_id;
    if (cfg->mqtt_url.empty())
        cfg->mqtt_url = "mqtt://192.168.1.10:1883";
    if (cfg->heartbeat_interval_ms <= 0)
        cfg->heartbeat_interval_ms = kHeartbeatDefaultMs;
    if (cfg->heartbeat_interval_ms < kHeartbeatMinMs)
        cfg->heartbeat_interval_ms = kHeartbeatMinMs;
    if (cfg->heartbeat_interval_ms > kHeartbeatMaxMs)
        cfg->heartbeat_interval_ms = kHeartbeatMaxMs;
    if (cfg->ipc_connect_retry_ms <= 0)
        cfg->ipc_connect_retry_ms = 500;
    if (cfg->ipc_sync_timeout_ms <= 0)
        cfg->ipc_sync_timeout_ms = 3000;
    if (cfg->mqtt_keepalive_s <= 0)
        cfg->mqtt_keepalive_s = 15;
    if (cfg->ui_touch_device.empty())
        cfg->ui_touch_device = "/dev/input/event0";
    if (cfg->ui_preview_timeout_ms <= 0)
        cfg->ui_preview_timeout_ms = 30000;
    if (cfg->ui_overlay_profile.empty())
        cfg->ui_overlay_profile = "dongshanpi_nt35516";
    if (cfg->face_db_dir.empty())
        cfg->face_db_dir = "/sharefs/face_db";
    if (cfg->attendance_log_base.empty())
        cfg->attendance_log_base = "/mnt/tf/face_logs";
    if (cfg->attendance_tz_offset_minutes < -840 || cfg->attendance_tz_offset_minutes > 840)
        cfg->attendance_tz_offset_minutes = 480;

    return true;
}

std::string topic_up_event()
{
    return "k230/" + g_rt.cfg.device_id + "/up/event";
}

std::string topic_up_reply()
{
    return "k230/" + g_rt.cfg.device_id + "/up/reply";
}

std::string topic_up_status()
{
    return "k230/" + g_rt.cfg.device_id + "/up/status";
}

std::string topic_down_cmd()
{
    return "k230/" + g_rt.cfg.device_id + "/down/cmd";
}

void enqueue_publish(std::string topic, std::string payload, uint8_t qos, bool retain)
{
    std::lock_guard<std::mutex> lk(g_rt.out_mu);
    if (g_rt.outbox.size() >= kMaxQueueDepth)
    {
        g_rt.outbox.pop_front();
        std::cerr << "face_netd: publish queue overflow, oldest item dropped" << std::endl;
    }
    g_rt.outbox.push_back(PublishItem{std::move(topic), std::move(payload), qos, retain});
}

std::string base64_encode_bytes(const unsigned char *data, size_t len)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3)
    {
        const unsigned int remaining = static_cast<unsigned int>(len - i);
        const unsigned int b0 = data[i];
        const unsigned int b1 = remaining > 1 ? data[i + 1] : 0;
        const unsigned int b2 = remaining > 2 ? data[i + 2] : 0;
        const unsigned int triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(tbl[(triple >> 18) & 63]);
        out.push_back(tbl[(triple >> 12) & 63]);
        out.push_back(remaining > 1 ? tbl[(triple >> 6) & 63] : '=');
        out.push_back(remaining > 2 ? tbl[triple & 63] : '=');
    }
    return out;
}

void publish_reply_mqtt(const std::string &request_id, const std::string &cmd_label, bool ok, int count,
                        const std::string &message, const std::string &extra_tail)
{
    std::ostringstream oss;
    oss << "{\"schema\":\"" << kSchema << "\""
        << ",\"device_id\":\"" << json_escape(g_rt.cfg.device_id) << "\""
        << ",\"request_id\":\"" << json_escape(request_id) << "\""
        << ",\"cmd\":\"" << json_escape(cmd_label) << "\""
        << ",\"ok\":" << (ok ? "true" : "false")
        << ",\"count\":" << count << ",\"message\":\"" << json_escape(message) << "\"" << extra_tail << "}";
    enqueue_publish(topic_up_reply(), oss.str(), 1, false);
}

const char *bridge_cmd_name(int cmd);

struct ImportErrorDetail {
    std::string file;
    std::string name;
    std::string message;
};

std::string json_escape_bounded(const std::string &input, size_t max_items)
{
    if (input.size() <= max_items)
        return json_escape(input);
    return json_escape(input.substr(0, max_items));
}

std::string import_name_from_path(const fs::path &path)
{
    std::string name = trim(path.stem().string());
    if (name.size() >= IPC_NAME_MAX)
        name.resize(IPC_NAME_MAX - 1);
    return name;
}

bool is_supported_import_extension(const fs::path &path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
}

std::string import_stage_file_path(const std::string &request_id)
{
    return std::string(IPC_IMPORT_STAGE_DIR) + "/" + request_id + ".img";
}

std::string next_internal_import_request_id()
{
    const uint64_t seq = g_rt.internal_request_seq.fetch_add(1) + 1;
    return "imp_" + std::to_string(now_ms()) + "_" + std::to_string(seq);
}

void publish_local_result(const PendingCmd &cmd, bool ok, int count, const std::string &message,
                          const std::string &extra_tail = "")
{
    if (cmd.source == PendingCmdSource::ui)
    {
        bridge_cmd_result_t result{};
        result.magic = IPC_MAGIC;
        result.cmd = cmd.cmd;
        result.count = count;
        result.ok = ok ? 1 : 0;
        strncpy(result.request_id, cmd.request_id.c_str(), sizeof(result.request_id) - 1);
        strncpy(result.message, message.c_str(), sizeof(result.message) - 1);
        ui_on_bridge_result(result);
        return;
    }
    publish_reply_mqtt(cmd.request_id, bridge_cmd_name(cmd.cmd), ok, count, message, extra_tail);
}

bool is_internal_request_id(const std::string &request_id)
{
    std::lock_guard<std::mutex> lk(g_rt.internal_reply_mu);
    return g_rt.internal_requests.find(request_id) != g_rt.internal_requests.end();
}

void register_internal_request(const std::string &request_id)
{
    std::lock_guard<std::mutex> lk(g_rt.internal_reply_mu);
    g_rt.internal_requests.insert(request_id);
    g_rt.internal_results.erase(request_id);
}

bool take_internal_result(const std::string &request_id, bridge_cmd_result_t *out, int timeout_ms)
{
    if (!out)
        return false;
    std::unique_lock<std::mutex> lk(g_rt.internal_reply_mu);
    const auto pred = [&] { return g_rt.internal_results.find(request_id) != g_rt.internal_results.end() || g_rt.stop.load(); };
    if (!g_rt.internal_reply_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), pred))
        return false;
    auto it = g_rt.internal_results.find(request_id);
    if (it == g_rt.internal_results.end())
        return false;
    *out = it->second;
    g_rt.internal_results.erase(it);
    g_rt.internal_requests.erase(request_id);
    return true;
}

void stash_internal_result(const bridge_cmd_result_t &result)
{
    std::lock_guard<std::mutex> lk(g_rt.internal_reply_mu);
    if (g_rt.internal_requests.find(result.request_id) == g_rt.internal_requests.end())
        return;
    g_rt.internal_results[result.request_id] = result;
    g_rt.internal_reply_cv.notify_all();
}

std::string format_import_summary(int success_count, int total_count, int failed_count)
{
    std::ostringstream oss;
    oss << "Imported " << success_count << "/" << total_count << " faces";
    if (failed_count > 0)
        oss << " (" << failed_count << " failed)";
    return oss.str();
}

std::string build_import_reply_extra(int total_count, int failed_count, int db_count_after,
                                     const std::vector<ImportErrorDetail> &errors)
{
    std::ostringstream oss;
    oss << ",\"total\":" << total_count << ",\"failed\":" << failed_count << ",\"db_count_after\":" << db_count_after
        << ",\"errors\":[";
    for (size_t i = 0; i < errors.size(); ++i)
    {
        if (i != 0)
            oss << ",";
        oss << "{\"file\":\"" << json_escape(errors[i].file) << "\",\"name\":\"" << json_escape(errors[i].name)
            << "\",\"message\":\"" << json_escape(errors[i].message) << "\"}";
    }
    oss << "]";
    return oss.str();
}

struct FaceGalleryEntry {
    int slot = 0;
    std::string name;
    bool has_image = false;
};

bool read_file_trim(const std::string &path, std::string *out)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
        return false;
    std::ostringstream oss;
    oss << ifs.rdbuf();
    *out = trim(oss.str());
    return true;
}

std::vector<FaceGalleryEntry> scan_face_db_dir(const std::string &dir)
{
    std::vector<FaceGalleryEntry> entries;
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec))
        return entries;

    std::vector<int> slots;
    for (const auto &de : fs::directory_iterator(dir, ec))
    {
        if (ec)
            break;
        if (!de.is_regular_file(ec))
            continue;
        const std::string fname = de.path().filename().string();
        constexpr const char *k_suffix = ".name";
        constexpr size_t k_suf_len = 5;
        if (fname.size() <= k_suf_len)
            continue;
        if (fname.compare(fname.size() - k_suf_len, k_suf_len, k_suffix) != 0)
            continue;
        const std::string stem = fname.substr(0, fname.size() - k_suf_len);
        char *end = nullptr;
        const long sl = strtol(stem.c_str(), &end, 10);
        if (end == stem.c_str() || *end != '\0' || sl <= 0 || sl > 4096)
            continue;
        slots.push_back(static_cast<int>(sl));
    }
    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());

    for (int sl : slots)
    {
        const std::string name_path = dir + "/" + std::to_string(sl) + ".name";
        const std::string jpg_path = dir + "/" + std::to_string(sl) + ".jpg";
        FaceGalleryEntry e{};
        e.slot = sl;
        if (!read_file_trim(name_path, &e.name))
            continue;
        std::error_code jec;
        e.has_image = fs::exists(jpg_path, jec) && fs::is_regular_file(jpg_path, jec);
        entries.push_back(std::move(e));
    }
    return entries;
}

void handle_db_face_list_local(const std::string &request_id)
{
    const auto vec = scan_face_db_dir(g_rt.cfg.face_db_dir);
    std::ostringstream arr;
    arr << "[";
    bool first = true;
    for (const auto &e : vec)
    {
        if (!first)
            arr << ",";
        first = false;
        arr << "{\"slot\":" << e.slot << ",\"name\":\"" << json_escape(e.name) << "\""
            << ",\"has_image\":" << (e.has_image ? "true" : "false") << "}";
    }
    arr << "]";
    std::ostringstream extra;
    extra << ",\"entries\":" << arr.str();
    publish_reply_mqtt(request_id, "db_face_list", true, static_cast<int>(vec.size()), "ok", extra.str());
}

void handle_db_face_image_local(const std::string &request_id, long slot)
{
    constexpr size_t k_max_bytes = 512 * 1024;
    if (slot < 1 || slot > 4096)
    {
        publish_reply_mqtt(request_id, "db_face_image", false, 0, "invalid slot", "");
        return;
    }
    const std::string path = g_rt.cfg.face_db_dir + "/" + std::to_string(slot) + ".jpg";
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
    {
        publish_reply_mqtt(request_id, "db_face_image", false, 0, "photo not found", "");
        return;
    }
    std::ostringstream buf;
    buf << ifs.rdbuf();
    std::string raw = buf.str();
    if (raw.empty())
    {
        publish_reply_mqtt(request_id, "db_face_image", false, 0, "empty photo file", "");
        return;
    }
    if (raw.size() > k_max_bytes)
    {
        publish_reply_mqtt(request_id, "db_face_image", false, 0, "photo too large", "");
        return;
    }
    const std::string b64 =
        base64_encode_bytes(reinterpret_cast<const unsigned char *>(raw.data()), raw.size());
    std::ostringstream extra;
    extra << ",\"slot\":" << slot << ",\"mime\":\"image/jpeg\""
          << ",\"image_b64\":\"" << json_escape(b64) << "\"";
    publish_reply_mqtt(request_id, "db_face_image", true, 1, "ok", extra.str());
}

void handle_attendance_log_fetch_local(const std::string &request_id, long max_bytes_in, long tail_lines_in,
                                       const std::string &date_in)
{
    constexpr size_t k_default_max = 256 * 1024;
    constexpr size_t k_hard_max = 1024 * 1024;
    size_t cap = k_default_max;
    if (max_bytes_in > 0 && static_cast<size_t>(max_bytes_in) <= k_hard_max)
        cap = static_cast<size_t>(max_bytes_in);

    size_t tail_lines = 500;
    if (tail_lines_in > 0 && tail_lines_in <= 50000)
        tail_lines = static_cast<size_t>(tail_lines_in);

    std::string ymd = trim(date_in);
    if (ymd.empty())
        ymd = today_local_ymd_linux();
    if (ymd.empty() || !is_ymd_calendar(ymd))
    {
        publish_reply_mqtt(request_id, "attendance_log_fetch", false, 0, "invalid date (use YYYY-MM-DD)", "");
        return;
    }

    const std::string &base = g_rt.cfg.attendance_log_base;
    const std::string path_flat = base + "/" + ymd + ".jsonl";
    const std::string path_legacy = base + "/" + ymd + "/events.jsonl";

    std::string path;
    std::error_code ec;
    if (fs::exists(path_flat, ec) && fs::is_regular_file(path_flat, ec))
        path = path_flat;
    else if (fs::exists(path_legacy, ec) && fs::is_regular_file(path_legacy, ec))
        path = path_legacy;
    else
    {
        publish_reply_mqtt(request_id, "attendance_log_fetch", false, 0,
                            "no log file for date (expect <base>/<YYYY-MM-DD>.jsonl or legacy .../<date>/events.jsonl)", "");
        return;
    }

    if (!fs::is_regular_file(path, ec))
    {
        publish_reply_mqtt(request_id, "attendance_log_fetch", false, 0, "path is not a regular file", "");
        return;
    }

    const auto sz_raw = fs::file_size(path, ec);
    if (ec)
    {
        publish_reply_mqtt(request_id, "attendance_log_fetch", false, 0, "cannot stat log file", "");
        return;
    }
    const size_t fsize = static_cast<size_t>(sz_raw);

    bool truncated = false;
    std::string chunk;
    {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs)
        {
            publish_reply_mqtt(request_id, "attendance_log_fetch", false, 0, "cannot open log file", "");
            return;
        }
        size_t start = 0;
        if (fsize > cap)
        {
            start = fsize - cap;
            truncated = true;
        }
        ifs.seekg(static_cast<std::streamoff>(start));
        std::ostringstream oss;
        oss << ifs.rdbuf();
        chunk = oss.str();
    }

    if (truncated && !chunk.empty())
    {
        const size_t nl = chunk.find('\n');
        if (nl != std::string::npos && nl + 1 < chunk.size())
            chunk.erase(0, nl + 1);
        chunk = std::string("[… omitted earlier bytes]\n") + chunk;
    }

    if (tail_lines > 0 && !chunk.empty())
    {
        size_t cnt = 0;
        for (size_t i = chunk.size(); i > 0;)
        {
            --i;
            if (chunk[i] == '\n')
            {
                ++cnt;
                if (cnt >= tail_lines)
                {
                    chunk.erase(0, i + 1);
                    break;
                }
            }
        }
    }

    const std::string b64 =
        base64_encode_bytes(reinterpret_cast<const unsigned char *>(chunk.data()), chunk.size());

    std::ostringstream extra;
    extra << ",\"date\":\"" << json_escape(ymd) << "\""
          << ",\"path\":\"" << json_escape(path) << "\""
          << ",\"truncated\":" << (truncated ? "true" : "false") << ",\"file_size\":" << fsize
          << ",\"bytes_returned\":" << chunk.size() << ",\"log_b64\":\"" << b64 << "\"";

    publish_reply_mqtt(request_id, "attendance_log_fetch", true, static_cast<int>(chunk.size()),
                        truncated ? "ok (tail bytes truncated)" : "ok", extra.str());
}

bool ensure_import_stage_dir(std::string *error_out)
{
    std::error_code ec;
    if (fs::exists(IPC_IMPORT_STAGE_DIR, ec))
    {
        if (ec || !fs::is_directory(IPC_IMPORT_STAGE_DIR, ec))
        {
            if (error_out)
                *error_out = "import staging path is not a directory";
            return false;
        }
        return true;
    }
    if (fs::create_directories(IPC_IMPORT_STAGE_DIR, ec))
        return true;
    if (!ec && fs::exists(IPC_IMPORT_STAGE_DIR))
        return true;
    if (error_out)
        *error_out = "cannot create import staging dir";
    return false;
}

std::vector<fs::path> scan_import_source_files(std::string *error_out)
{
    std::vector<fs::path> files;
    std::error_code ec;
    if (!fs::exists(kImportSourceDir, ec) || ec)
    {
        if (error_out)
            *error_out = "import source dir not found";
        return files;
    }
    if (!fs::is_directory(kImportSourceDir, ec) || ec)
    {
        if (error_out)
            *error_out = "import source path is not a directory";
        return files;
    }

    for (const auto &de : fs::directory_iterator(kImportSourceDir, ec))
    {
        if (ec)
            break;
        if (!de.is_regular_file(ec) || ec)
            continue;
        if (!is_supported_import_extension(de.path()))
            continue;
        files.push_back(de.path());
    }
    std::sort(files.begin(), files.end(), [](const fs::path &lhs, const fs::path &rhs) {
        return lhs.filename().string() < rhs.filename().string();
    });
    if (files.empty() && error_out)
        *error_out = "no importable images found";
    return files;
}

bool copy_file_to_import_stage(const fs::path &src, const std::string &request_id, std::string *error_out)
{
    std::ifstream ifs(src, std::ios::binary);
    if (!ifs)
    {
        if (error_out)
            *error_out = "cannot open source image";
        return false;
    }
    const std::string dst = import_stage_file_path(request_id);
    std::ofstream ofs(dst, std::ios::binary | std::ios::trunc);
    if (!ofs)
    {
        if (error_out)
            *error_out = "cannot create staged image";
        return false;
    }
    ofs << ifs.rdbuf();
    if (!ofs.good())
    {
        if (error_out)
            *error_out = "cannot write staged image";
        return false;
    }
    return true;
}

bool pop_publish(PublishItem *out)
{
    if (!out)
        return false;
    std::lock_guard<std::mutex> lk(g_rt.out_mu);
    if (g_rt.outbox.empty())
        return false;
    *out = std::move(g_rt.outbox.front());
    g_rt.outbox.pop_front();
    return true;
}

void enqueue_command(PendingCmd cmd)
{
    {
        std::lock_guard<std::mutex> lk(g_rt.cmd_mu);
        if (g_rt.cmd_queue.size() >= kMaxQueueDepth)
        {
            g_rt.cmd_queue.pop_front();
            std::cerr << "face_netd: command queue overflow, oldest command dropped" << std::endl;
        }
        g_rt.cmd_queue.push_back(std::move(cmd));
    }
    g_rt.cmd_cv.notify_one();
}

bool is_library_write_cmd(int cmd);

bool has_pending_library_write_cmd()
{
    std::lock_guard<std::mutex> lk(g_rt.cmd_mu);
    for (const auto &cmd : g_rt.cmd_queue)
    {
        if (is_library_write_cmd(cmd.cmd))
            return true;
    }
    return false;
}

bool is_library_write_cmd(int cmd)
{
    switch (static_cast<ipc_bridge_cmd_t>(cmd))
    {
    case IPC_BRIDGE_CMD_DB_RESET:
    case IPC_BRIDGE_CMD_REGISTER_CURRENT:
    case IPC_BRIDGE_CMD_REGISTER_PREVIEW:
    case IPC_BRIDGE_CMD_REGISTER_COMMIT:
    case IPC_BRIDGE_CMD_REGISTER_CANCEL:
    case IPC_BRIDGE_CMD_IMPORT_FACES:
        return true;
    default:
        return false;
    }
}

bool pop_command_blocking(PendingCmd *out)
{
    if (!out)
        return false;
    std::unique_lock<std::mutex> lk(g_rt.cmd_mu);
    g_rt.cmd_cv.wait(lk, [] { return g_rt.stop.load() || !g_rt.cmd_queue.empty(); });
    if (g_rt.stop.load())
        return false;
    *out = std::move(g_rt.cmd_queue.front());
    g_rt.cmd_queue.pop_front();
    return true;
}

const char *bridge_cmd_name(int cmd)
{
    switch (static_cast<ipc_bridge_cmd_t>(cmd))
    {
    case IPC_BRIDGE_CMD_DB_COUNT:
        return "db_count";
    case IPC_BRIDGE_CMD_DB_RESET:
        return "db_reset";
    case IPC_BRIDGE_CMD_REGISTER_CURRENT:
        return "register_current";
    case IPC_BRIDGE_CMD_REGISTER_PREVIEW:
        return "register_preview";
    case IPC_BRIDGE_CMD_REGISTER_COMMIT:
        return "register_commit";
    case IPC_BRIDGE_CMD_REGISTER_CANCEL:
        return "register_cancel";
    case IPC_BRIDGE_CMD_IMPORT_FACES:
        return "import_faces";
    case IPC_BRIDGE_CMD_SHUTDOWN:
        return "shutdown";
    default:
        return "unknown";
    }
}

bool bridge_cmd_from_string(const std::string &cmd, int *out)
{
    if (!out)
        return false;
    if (cmd == "db_count")
        *out = IPC_BRIDGE_CMD_DB_COUNT;
    else if (cmd == "db_reset")
        *out = IPC_BRIDGE_CMD_DB_RESET;
    else if (cmd == "register_current")
        *out = IPC_BRIDGE_CMD_REGISTER_CURRENT;
    else if (cmd == "register_preview" || cmd == "register-preview")
        *out = IPC_BRIDGE_CMD_REGISTER_PREVIEW;
    else if (cmd == "register_commit" || cmd == "register-commit")
        *out = IPC_BRIDGE_CMD_REGISTER_COMMIT;
    else if (cmd == "register_cancel" || cmd == "register-cancel")
        *out = IPC_BRIDGE_CMD_REGISTER_CANCEL;
    else if (cmd == "import_faces" || cmd == "import-faces")
        *out = IPC_BRIDGE_CMD_IMPORT_FACES;
    else if (cmd == "shutdown")
        *out = IPC_BRIDGE_CMD_SHUTDOWN;
    else
        return false;
    return true;
}

std::string build_status_json(bool online)
{
    std::ostringstream oss;
    oss << "{\"schema\":\"" << kSchema << "\""
        << ",\"device_id\":\"" << json_escape(g_rt.cfg.device_id) << "\""
        << ",\"online\":" << (online ? "true" : "false")
        << ",\"rt_connected\":" << (g_rt.rt_connected.load() ? "true" : "false")
        << ",\"db_count\":" << g_rt.db_count.load()
        << ",\"last_seen_ms\":" << g_rt.last_rt_seen_ms.load() << "}";
    return oss.str();
}

void enqueue_status_publish(bool online, bool force)
{
    {
        std::lock_guard<std::mutex> lk(g_rt.status_mu);
        const uint64_t now = now_ms();
        if (!force && (now - g_rt.last_status_publish_ms) < static_cast<uint64_t>(g_rt.cfg.heartbeat_interval_ms))
            return;
        g_rt.last_status_publish_ms = now;
    }
    enqueue_publish(topic_up_status(), build_status_json(online), 1, true);
    g_rt.status_dirty.store(false);
}

void mark_rt_seen()
{
    g_rt.last_rt_seen_ms.store(now_ms());
    g_rt.status_dirty.store(true);
}

static std::string derive_http_base_from_mqtt_url(const std::string &mqtt_url)
{
    const size_t p = mqtt_url.find("://");
    if (p == std::string::npos)
        return {};
    const size_t i = p + 3;
    const size_t colon = mqtt_url.find(':', i);
    const size_t slash = mqtt_url.find('/', i);
    std::string host;
    if (colon != std::string::npos && (slash == std::string::npos || colon < slash))
        host = mqtt_url.substr(i, colon - i);
    else if (slash != std::string::npos)
        host = mqtt_url.substr(i, slash - i);
    else
        host = mqtt_url.substr(i);
    while (!host.empty() && std::isspace(static_cast<unsigned char>(host.back())))
        host.pop_back();
    if (host.empty())
        return {};
    return "http://" + host + ":8000";
}

static std::string resolved_server_http_base_for_sync()
{
    const std::string &raw = g_rt.cfg.server_http_base;
    if (raw == "-" || raw == "disabled" || raw == "off")
        return {};
    if (!raw.empty())
        return raw;
    return derive_http_base_from_mqtt_url(g_rt.cfg.mqtt_url);
}

static bool parse_http_base_host_port(const std::string &base_raw, std::string *host_out, int *port_out)
{
    std::string s = trim(base_raw);
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    if (s.size() >= 8 && s.compare(0, 8, "https://") == 0)
        return false;
    if (s.size() >= 7 && s.compare(0, 7, "http://") == 0)
        s = s.substr(7);
    if (s.empty())
        return false;
    const size_t colon = s.find(':');
    if (colon == std::string::npos)
    {
        *host_out = s;
        *port_out = 80;
        return true;
    }
    *host_out = s.substr(0, colon);
    const std::string ps = s.substr(colon + 1);
    char *end = nullptr;
    const long p = std::strtol(ps.c_str(), &end, 10);
    if (end == ps.c_str() || p <= 0 || p > 65535)
        return false;
    *port_out = static_cast<int>(p);
    return true;
}

static uint64_t parse_server_ms_json_body(const std::string &body)
{
    const auto pos = body.find("\"server_ms\"");
    if (pos == std::string::npos)
        return 0;
    size_t q = body.find(':', pos);
    if (q == std::string::npos)
        return 0;
    ++q;
    while (q < body.size() && (body[q] == ' ' || body[q] == '\t'))
        ++q;
    char *end = nullptr;
    const unsigned long long v = std::strtoull(body.c_str() + q, &end, 10);
    if (end == body.c_str() + q || v == 0ULL || v < kMinSaneLinuxEpochMs)
        return 0;
    return static_cast<uint64_t>(v);
}

static constexpr long k_utc_off_json_missing = 99999;

/** Parses utc_offset_minutes from JSON body if present (mosquito mg_json); clamps [-840,840]. */
static void apply_pc_utc_offset_minutes_from_json_body(const std::string &body)
{
    const long off =
        mg_json_get_long(mg_str_n(body.data(), body.size()), "$.utc_offset_minutes", k_utc_off_json_missing);
    if (off == k_utc_off_json_missing)
        return;
    if (off < -840 || off > 840)
        return;
    g_pc_utc_offset_minutes.store(static_cast<int32_t>(off), std::memory_order_relaxed);
}

static void apply_wall_clock_from_pc_ms(uint64_t server_ms, const char *via_tag)
{
    if (server_ms < kMinSaneLinuxEpochMs)
        return;
    std::lock_guard<std::mutex> lk(g_att_wall_cal_mu);
    const uint64_t L = now_ms();
    g_att_wall_ref_server_ms = server_ms;
    g_att_wall_ref_linux_ms = L;
    std::cout << "face_netd: attendance folder calendar synced (" << via_tag << ") server_ms=" << server_ms
              << std::endl;
}

static void handle_time_sync_mqtt_message(const mg_str &data)
{
    if (!data.ptr || data.len == 0)
        return;
    std::string body(static_cast<size_t>(data.len), '\0');
    std::memcpy(&body[0], data.ptr, data.len);
    apply_pc_utc_offset_minutes_from_json_body(body);
    const uint64_t sm = parse_server_ms_json_body(body);
    if (sm == 0ULL)
        return;
    apply_wall_clock_from_pc_ms(sm, "MQTT k230/time_sync");
}

static uint64_t http_fetch_server_time_ms_blocking(const std::string &http_base_raw)
{
    std::string host;
    int port = 80;
    if (!parse_http_base_host_port(http_base_raw, &host, &port))
        return 0;

    int fd = -1;
    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &sin.sin_addr) == 1)
    {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0)
        {
            struct timeval tv;
            tv.tv_sec = 5;
            tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (connect(fd, reinterpret_cast<struct sockaddr *>(&sin), sizeof(sin)) != 0)
            {
                close(fd);
                fd = -1;
            }
        }
    }

    if (fd < 0)
    {
        struct addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = nullptr;
        const std::string port_str = std::to_string(port);
        if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res)
            return 0;

        for (struct addrinfo *p = res; p; p = p->ai_next)
        {
            fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (fd < 0)
                continue;
            struct timeval tv;
            tv.tv_sec = 5;
            tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (connect(fd, p->ai_addr, static_cast<socklen_t>(p->ai_addrlen)) == 0)
                break;
            close(fd);
            fd = -1;
        }
        freeaddrinfo(res);
    }

    if (fd < 0)
        return 0;

    std::string host_hdr = host;
    if (port != 80)
        host_hdr += ":" + std::to_string(port);
    const std::string req = "GET /api/server-time HTTP/1.1\r\nHost: " + host_hdr
                            + "\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
    const ssize_t sent = send(fd, req.data(), req.size(), 0);
    if (sent != static_cast<ssize_t>(req.size()))
    {
        close(fd);
        return 0;
    }

    std::string buf;
    buf.reserve(8192);
    char tmp[4096];
    for (;;)
    {
        const ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0)
            break;
        buf.append(tmp, static_cast<size_t>(n));
        if (buf.size() > 65536)
            break;
    }
    close(fd);

    if (buf.size() < 12 || buf.rfind("HTTP/", 0) != 0)
        return 0;

    size_t line_end = buf.find("\r\n");
    if (line_end == std::string::npos)
        line_end = buf.find('\n');
    if (line_end == std::string::npos || line_end < static_cast<size_t>(10))
        return 0;
    const std::string line0 = buf.substr(0, line_end);
    int http_maj = 0, http_min = 0, status_code = 0;
    if (std::sscanf(line0.c_str(), "HTTP/%d.%d %d", &http_maj, &http_min, &status_code) != 3 || status_code != 200)
    {
        static std::atomic<bool> warn_http_status{};
        if (!warn_http_status.exchange(true))
            std::cerr << "face_netd: server-time HTTP non-200 line=\"" << line0 << "\"" << std::endl;
        return 0;
    }

    size_t sep_r = buf.find("\r\n\r\n");
    size_t sep_n = buf.find("\n\n");
    size_t sep = std::string::npos;
    size_t skip = 0;
    if (sep_r != std::string::npos && (sep_n == std::string::npos || sep_r <= sep_n))
    {
        sep = sep_r;
        skip = 4;
    }
    else if (sep_n != std::string::npos)
    {
        sep = sep_n;
        skip = 2;
    }
    else
        return 0;

    const std::string body = buf.substr(sep + skip);
    apply_pc_utc_offset_minutes_from_json_body(body);
    return parse_server_ms_json_body(body);
}

/** Linux epoch 不可信时：优先用已缓存的 PC 时间（MQTT k230/time_sync 或 HTTP）；否则尝试 HTTP，再等 MQTT。 */
static uint64_t wall_calendar_ms_for_attendance_folder()
{
    const uint64_t L = now_ms();
    if (L >= kMinSaneLinuxEpochMs)
        return L;

    auto extrapolate_from_refs = [&]() -> uint64_t {
        std::lock_guard<std::mutex> lk(g_att_wall_cal_mu);
        const uint64_t l_now = now_ms();
        if (g_att_wall_ref_server_ms >= kMinSaneLinuxEpochMs)
        {
            const uint64_t est = g_att_wall_ref_server_ms + (l_now - g_att_wall_ref_linux_ms);
            if (est >= kMinSaneLinuxEpochMs)
                return est;
        }
        return 0;
    };

    if (const uint64_t ex = extrapolate_from_refs(); ex != 0)
        return ex;

    const std::string http_base = resolved_server_http_base_for_sync();
    if (!http_base.empty())
    {
        const uint64_t fetched = http_fetch_server_time_ms_blocking(http_base);
        if (fetched >= kMinSaneLinuxEpochMs)
        {
            apply_wall_clock_from_pc_ms(fetched, "HTTP /api/server-time");
            return fetched;
        }

        const uint64_t noww = now_ms();
        uint64_t prev = g_att_wall_last_http_warn_linux_ms.load(std::memory_order_relaxed);
        if (prev == 0 || noww >= prev + 600000ULL)
        {
            if (g_att_wall_last_http_warn_linux_ms.compare_exchange_weak(prev, noww, std::memory_order_relaxed))
                std::cerr << "face_netd: PC calendar HTTP failed (" << http_base
                          << "/api/server-time). Using MQTT k230/time_sync from server_pc if available..."
                          << std::endl;
        }
    }

    if (const uint64_t ex2 = extrapolate_from_refs(); ex2 != 0)
        return ex2;

    return L;
}

static void attendance_wall_clock_prime_from_pc_async()
{
    std::thread(
        []()
        {
            const uint64_t ms = wall_calendar_ms_for_attendance_folder();
            (void)ms;
        })
        .detach();
}

void cancel_internal_request(const std::string &request_id)
{
    std::lock_guard<std::mutex> lk(g_rt.internal_reply_mu);
    g_rt.internal_requests.erase(request_id);
    g_rt.internal_results.erase(request_id);
}

bool dispatch_bridge_command_ack(const PendingCmd &cmd, std::string *error_out)
{
    const int ipc_id = g_rt.ipc_id.load();
    if (ipc_id < 0 || !g_rt.rt_connected.load() || !kd_ipcmsg_is_connect(ipc_id))
    {
        if (error_out)
            *error_out = "rt bridge disconnected";
        return false;
    }

    bridge_cmd_req_t req{};
    req.magic = IPC_MAGIC;
    req.cmd = static_cast<int32_t>(cmd.cmd);
    strncpy(req.request_id, cmd.request_id.c_str(), sizeof(req.request_id) - 1);
    strncpy(req.name, cmd.name.c_str(), sizeof(req.name) - 1);

    k_ipcmsg_message_t *msg =
        kd_ipcmsg_create_message(IPC_FACE_BRIDGE_MODULE, IPC_BRIDGE_MSG_CMD_REQ, &req, sizeof(req));
    if (!msg)
    {
        if (error_out)
            *error_out = "failed to allocate ipc message";
        return false;
    }

    k_ipcmsg_message_t *resp = nullptr;
    const k_s32 ret = kd_ipcmsg_send_sync(ipc_id, msg, &resp, g_rt.cfg.ipc_sync_timeout_ms);
    kd_ipcmsg_destroy_message(msg);

    if (ret != K_SUCCESS || !resp)
    {
        if (error_out)
            *error_out = (ret == K_IPCMSG_ETIMEOUT) ? "bridge ack timeout" : "bridge ack failed";
        if (resp)
            kd_ipcmsg_destroy_message(resp);
        return false;
    }

    if (resp->u32BodyLen < sizeof(bridge_cmd_ack_t) || !resp->pBody)
    {
        if (error_out)
            *error_out = "invalid bridge ack";
        kd_ipcmsg_destroy_message(resp);
        return false;
    }

    bridge_cmd_ack_t ack{};
    memcpy(&ack, resp->pBody, sizeof(ack));
    kd_ipcmsg_destroy_message(resp);

    if (ack.magic != IPC_MAGIC)
    {
        if (error_out)
            *error_out = "bridge ack magic mismatch";
        return false;
    }

    mark_rt_seen();
    if (!ack.accepted)
    {
        if (error_out)
            *error_out = ack.reason[0] ? ack.reason : "bridge rejected";
        return false;
    }

    std::cout << "face_netd: accepted command " << bridge_cmd_name(cmd.cmd) << " request_id=" << cmd.request_id
              << std::endl;
    return true;
}

void record_import_error(std::vector<ImportErrorDetail> *errors, const fs::path &file_path, const std::string &name,
                         const std::string &message)
{
    if (!errors || errors->size() >= kImportErrorDetailLimit)
        return;
    errors->push_back(ImportErrorDetail{file_path.filename().string(), name, message});
}

void handle_import_faces_local(const PendingCmd &cmd)
{
    if (g_rt.import_active.exchange(true))
    {
        publish_local_result(cmd, false, 0, "Import already in progress");
        return;
    }

    const auto import_done = [] { g_rt.import_active.store(false); };

    std::string stage_error;
    if (!ensure_import_stage_dir(&stage_error))
    {
        import_done();
        publish_local_result(cmd, false, 0, stage_error.empty() ? "cannot prepare import staging dir" : stage_error);
        return;
    }

    std::string scan_error;
    const std::vector<fs::path> files = scan_import_source_files(&scan_error);
    if (files.empty())
    {
        import_done();
        publish_local_result(cmd, false, 0, scan_error.empty() ? "no importable images found" : scan_error,
                             build_import_reply_extra(0, 0, g_rt.db_count.load(), {}));
        return;
    }

    const int total_count = static_cast<int>(files.size());
    int success_count = 0;
    int failed_count = 0;
    std::vector<ImportErrorDetail> errors;

    for (size_t idx = 0; idx < files.size(); ++idx)
    {
        const fs::path &file_path = files[idx];
        const std::string name = import_name_from_path(file_path);
        ui_on_local_import_progress(cmd.request_id, static_cast<int>(idx + 1), total_count);

        if (name.empty())
        {
            ++failed_count;
            record_import_error(&errors, file_path, name, "empty name after trimming filename");
            continue;
        }

        const std::string child_request_id = next_internal_import_request_id();
        register_internal_request(child_request_id);

        std::string copy_error;
        if (!copy_file_to_import_stage(file_path, child_request_id, &copy_error))
        {
            cancel_internal_request(child_request_id);
            ++failed_count;
            record_import_error(&errors, file_path, name, copy_error.empty() ? "cannot stage import file" : copy_error);
            continue;
        }

        PendingCmd child_cmd{};
        child_cmd.request_id = child_request_id;
        child_cmd.cmd = IPC_BRIDGE_CMD_IMPORT_FACES;
        child_cmd.name = name;
        child_cmd.source = PendingCmdSource::remote;

        std::string dispatch_error;
        if (!dispatch_bridge_command_ack(child_cmd, &dispatch_error))
        {
            cancel_internal_request(child_request_id);
            std::error_code rm_ec;
            fs::remove(import_stage_file_path(child_request_id), rm_ec);
            ++failed_count;
            record_import_error(&errors, file_path, name,
                                dispatch_error.empty() ? "cannot dispatch import request" : dispatch_error);
            continue;
        }

        bridge_cmd_result_t child_result{};
        if (!take_internal_result(child_request_id, &child_result, kImportChildResultTimeoutMs))
        {
            ++failed_count;
            record_import_error(&errors, file_path, name, "timed out waiting for import result");
            continue;
        }

        if (child_result.ok)
            ++success_count;
        else
        {
            ++failed_count;
            record_import_error(&errors, file_path, name,
                                child_result.message[0] ? child_result.message : "import failed");
        }
    }

    const int db_count_after = static_cast<int>(scan_face_db_dir(g_rt.cfg.face_db_dir).size());
    g_rt.db_count.store(db_count_after);
    import_done();

    const bool ok = success_count > 0;
    const std::string summary = format_import_summary(success_count, total_count, failed_count);
    publish_local_result(cmd, ok, success_count, summary, build_import_reply_extra(total_count, failed_count, db_count_after, errors));
}

void publish_reply_failure(const std::string &request_id, int cmd, const std::string &message)
{
    publish_reply_mqtt(request_id, bridge_cmd_name(cmd), false, -1, message, "");
}

void publish_reply_failure(const PendingCmd &cmd, const std::string &message)
{
    publish_local_result(cmd, false, -1, message);
}

/** Same JSONL shape as RT face_event; written under attendance_log_base when TF is mounted on Linux.
 *  Mirrors ipc events when big-core /sd is unavailable or returns EINVAL. */
static void append_attendance_jsonl_linux_tf(const bridge_event_t &ev)
{
    const std::string &base = g_rt.cfg.attendance_log_base;
    if (base.empty())
        return;

    /* wall_time：与 PC 本地钟对齐（utc_offset_minutes from MQTT/HTTP，否则 attendance_tz_offset_minutes） */
    const uint64_t folder_clock_ms = wall_calendar_ms_for_attendance_folder();
    struct tm tm_local {};
    if (!utc_ms_to_civil_tm_fixed_offset(folder_clock_ms, effective_attendance_tz_offset_minutes(), &tm_local))
        return;
    char ymd[16]{};
    if (strftime(ymd, sizeof(ymd), "%Y-%m-%d", &tm_local) <= 0)
        return;
    char hms[12]{};
    if (strftime(hms, sizeof(hms), "%H:%M:%S", &tm_local) <= 0)
        return;

    const ipc_evt_kind_t ek = static_cast<ipc_evt_kind_t>(ev.evt_kind);
    const int is_stranger = (ek == IPC_EVT_KIND_STRANGER) ? 1 : 0;
    const int live_ok = (ek == IPC_EVT_KIND_LIVENESS_FAIL) ? 0 : 1;
    const char *evt_str = "unknown";
    switch (ek)
    {
    case IPC_EVT_KIND_RECOGNIZED:
        evt_str = "recognized";
        break;
    case IPC_EVT_KIND_STRANGER:
        evt_str = "stranger";
        break;
    case IPC_EVT_KIND_LIVENESS_FAIL:
        evt_str = "liveness_fail";
        break;
    default:
        break;
    }

    std::string nm(ev.name, strnlen(ev.name, IPC_NAME_MAX));

    std::ostringstream json_line;
    json_line << "{\"wall_time\":\"" << hms << "\",\"evt_kind\":\"" << evt_str << "\""
              << ",\"face_id\":" << ev.face_id << ",\"name\":\"" << json_escape(nm) << "\""
              << ",\"score\":" << ev.score << ",\"is_stranger\":" << is_stranger << ",\"live_ok\":" << live_ok << "}";

    std::error_code ec;
    fs::create_directories(fs::path(base), ec);
    if (ec)
    {
        static std::atomic<bool> warn_mkdir{};
        if (!warn_mkdir.exchange(true))
            std::cerr << "face_netd: attendance create_directories failed " << base << " : " << ec.message()
                      << std::endl;
        return;
    }

    const std::string path = (fs::path(base) / (std::string(ymd) + ".jsonl")).string();
    {
        std::lock_guard<std::mutex> lk(g_rt.attendance_sd_log_mu);
        std::ofstream ofs(path, std::ios::app | std::ios::binary);
        if (!ofs)
        {
            static std::atomic<bool> warn_open{};
            if (!warn_open.exchange(true))
                std::cerr << "face_netd: cannot open attendance log " << path << " (mount TF under attendance_log_base?)"
                          << std::endl;
            return;
        }
        ofs << json_line.str() << '\n';
    }
}

void handle_bridge_event(const bridge_event_t &ev)
{
    append_attendance_jsonl_linux_tf(ev);
    ui_on_bridge_event(ev);

    std::ostringstream oss;
    oss << "{\"schema\":\"" << kSchema << "\""
        << ",\"device_id\":\"" << json_escape(g_rt.cfg.device_id) << "\""
        << ",\"evt_kind\":\"";
    switch (static_cast<ipc_evt_kind_t>(ev.evt_kind))
    {
    case IPC_EVT_KIND_RECOGNIZED:
        oss << "recognized";
        break;
    case IPC_EVT_KIND_STRANGER:
        oss << "stranger";
        break;
    case IPC_EVT_KIND_LIVENESS_FAIL:
        oss << "liveness_fail";
        break;
    default:
        oss << "unknown";
        break;
    }
    oss << "\""
        << ",\"face_id\":" << ev.face_id
        << ",\"name\":\"" << json_escape(ev.name) << "\""
        << ",\"score\":" << ev.score
        << ",\"ts_ms\":" << ev.ts_ms << "}";
    enqueue_publish(topic_up_event(), oss.str(), 1, false);
}

void handle_bridge_result(const bridge_cmd_result_t &result)
{
    if (is_internal_request_id(result.request_id))
    {
        stash_internal_result(result);
        return;
    }

    if (result.ok && result.cmd == IPC_BRIDGE_CMD_DB_COUNT)
        g_rt.db_count.store(result.count);
    if (result.ok && result.cmd == IPC_BRIDGE_CMD_DB_RESET)
        g_rt.db_count.store(0);

    ui_on_bridge_result(result);
    if (std::strncmp(result.request_id, "ui_", 3) != 0)
    {
        publish_reply_mqtt(result.request_id, bridge_cmd_name(result.cmd), static_cast<bool>(result.ok), result.count,
                           std::string(result.message), "");
    }

    mark_rt_seen();

    if (result.cmd == IPC_BRIDGE_CMD_SHUTDOWN && result.ok)
    {
        enqueue_status_publish(false, true);
        g_rt.shutdown_after_flush.store(true);
    }
}

void handle_bridge_ui_shared_info(const bridge_ui_shared_info_t &info)
{
    ui_on_shared_info(info);
    mark_rt_seen();
    std::cout << "face_netd: received UI shared overlay info phys=0x" << std::hex
              << static_cast<unsigned long long>(info.ui_phys_addr) << std::dec << " bytes=" << info.ui_bytes
              << " generation=" << info.ui_generation << " geometry=" << info.ui_width << "x" << info.ui_height
              << " stride=" << info.ui_stride << std::endl;
}

void bridge_ipc_handler(k_s32, k_ipcmsg_message_t *msg)
{
    if (!msg || msg->bIsResp || !msg->pBody)
        return;

    if (msg->u32CMD == IPC_BRIDGE_MSG_EVENT && msg->u32BodyLen >= sizeof(bridge_event_t))
    {
        bridge_event_t ev{};
        memcpy(&ev, msg->pBody, sizeof(ev));
        if (ev.magic != IPC_MAGIC)
            return;
        mark_rt_seen();
        handle_bridge_event(ev);
        return;
    }

    if (msg->u32CMD == IPC_BRIDGE_MSG_CMD_RESULT && msg->u32BodyLen >= sizeof(bridge_cmd_result_t))
    {
        bridge_cmd_result_t result{};
        memcpy(&result, msg->pBody, sizeof(result));
        if (result.magic != IPC_MAGIC)
            return;
        handle_bridge_result(result);
        return;
    }

    if (msg->u32CMD == IPC_BRIDGE_MSG_UI_SHARED_INFO && msg->u32BodyLen >= sizeof(bridge_ui_shared_info_t))
    {
        bridge_ui_shared_info_t info{};
        memcpy(&info, msg->pBody, sizeof(info));
        if (info.magic != IPC_MAGIC)
            return;
        handle_bridge_ui_shared_info(info);
    }
}

void run_thread_body(int ipc_id)
{
    kd_ipcmsg_run(ipc_id);
    g_rt.runner_alive.store(false);
}

void ipc_supervisor_thread()
{
    if (access("/dev/ipcm_user", F_OK) != 0)
    {
        std::cerr << "face_netd: cannot open /dev/ipcm_user (ipcmsg driver not ready?). "
                  << "Expected k_ipcm.ko to be loaded." << std::endl;
        g_rt.stop.store(true);
        g_rt.cmd_cv.notify_all();
        return;
    }

    k_ipcmsg_connect_t attr{};
    attr.u32RemoteId = kIpcRemoteIdLinux;
    attr.u32Port = IPC_FACE_BRIDGE_PORT;
    attr.u32Priority = 0;

    const k_s32 add_ret = kd_ipcmsg_add_service(IPC_FACE_BRIDGE_SERVICE, &attr);
    if (add_ret != K_SUCCESS)
    {
        std::cerr << "face_netd: kd_ipcmsg_add_service(" << IPC_FACE_BRIDGE_SERVICE << ") failed ret=" << add_ret
                  << std::endl;
        g_rt.stop.store(true);
        g_rt.cmd_cv.notify_all();
        return;
    }

    unsigned retry_count = 0;
    while (!g_rt.stop.load())
    {
        k_s32 ipc_id = -1;
        const k_s32 conn_ret = kd_ipcmsg_try_connect(&ipc_id, IPC_FACE_BRIDGE_SERVICE, bridge_ipc_handler);
        if (conn_ret != K_SUCCESS)
        {
            if ((retry_count++ % 10) == 0)
            {
                std::cerr << "face_netd: waiting for RT bridge service " << IPC_FACE_BRIDGE_SERVICE
                          << " port=" << IPC_FACE_BRIDGE_PORT << " ret=" << conn_ret
                          << ". Make sure RT-Smart face_event.elf is already running and prints "
                          << "\"bridge service " << IPC_FACE_BRIDGE_SERVICE << " port=" << IPC_FACE_BRIDGE_PORT
                          << " ready\"" << std::endl;
            }
            sleep_ms(g_rt.cfg.ipc_connect_retry_ms);
            continue;
        }

        retry_count = 0;
        g_rt.ipc_id.store(ipc_id);
        g_rt.rt_connected.store(true);
        mark_rt_seen();
        std::cout << "face_netd: connected to RT bridge service=" << IPC_FACE_BRIDGE_SERVICE << " id=" << ipc_id
                  << std::endl;

        // Mark the runner alive before the thread starts so the supervisor
        // does not observe a false value and tear the connection down
        // immediately due to scheduling order.
        g_rt.runner_alive.store(true);
        std::thread runner(run_thread_body, ipc_id);

        while (!g_rt.stop.load())
        {
            if (!g_rt.runner_alive.load())
                break;
            if (!kd_ipcmsg_is_connect(ipc_id))
                break;
            sleep_ms(200);
        }

        kd_ipcmsg_disconnect(ipc_id);
        if (runner.joinable())
            runner.join();

        g_rt.ipc_id.store(-1);
        if (g_rt.rt_connected.exchange(false))
        {
            g_rt.status_dirty.store(true);
            std::cout << "face_netd: RT bridge disconnected, waiting for reconnect" << std::endl;
        }

        if (!g_rt.stop.load())
            sleep_ms(g_rt.cfg.ipc_connect_retry_ms);
    }

    int ipc_id = g_rt.ipc_id.exchange(-1);
    if (ipc_id >= 0)
        kd_ipcmsg_disconnect(ipc_id);
    g_rt.rt_connected.store(false);
    kd_ipcmsg_del_service(IPC_FACE_BRIDGE_SERVICE);
}

void command_worker_thread()
{
    while (!g_rt.stop.load())
    {
        PendingCmd cmd;
        if (!pop_command_blocking(&cmd))
            break;

        if (cmd.cmd == IPC_BRIDGE_CMD_IMPORT_FACES)
        {
            handle_import_faces_local(cmd);
            continue;
        }

        std::string dispatch_error;
        if (!dispatch_bridge_command_ack(cmd, &dispatch_error))
        {
            publish_reply_failure(cmd, dispatch_error.empty() ? "rt bridge disconnected" : dispatch_error);
            g_rt.status_dirty.store(true);
        }
    }
}

void maybe_connect_mqtt();

void mqtt_publish_now(const PublishItem &item)
{
    if (!g_mqtt || !g_rt.mqtt_connected.load())
        return;
    mg_mqtt_opts opts{};
    opts.topic = mg_str(item.topic.c_str());
    opts.message = mg_str(item.payload.c_str());
    opts.qos = item.qos;
    opts.retain = item.retain;
    mg_mqtt_pub(g_mqtt, &opts);
}

void drain_publish_queue()
{
    if (!g_mqtt || !g_rt.mqtt_connected.load())
        return;

    PublishItem item;
    while (pop_publish(&item))
        mqtt_publish_now(item);
}

void parse_and_enqueue_command(mg_str json)
{
    char *request_id = mg_json_get_str(json, "$.request_id");
    char *cmd = mg_json_get_str(json, "$.cmd");
    char *name = mg_json_get_str(json, "$.name");

    std::string request_id_s = request_id ? trim(request_id) : "";
    std::string cmd_s = cmd ? trim(cmd) : "";
    std::string name_s = name ? trim(name) : "";

    if (request_id)
        free(request_id);
    if (cmd)
        free(cmd);
    if (name)
        free(name);

    if (request_id_s.empty())
    {
        publish_reply_failure("", IPC_BRIDGE_CMD_NONE, "request_id is required");
        return;
    }
    if (cmd_s == "db_face_list")
    {
        handle_db_face_list_local(request_id_s);
        return;
    }
    if (cmd_s == "db_face_image")
    {
        const long slot = mg_json_get_long(json, "$.slot", -1);
        handle_db_face_image_local(request_id_s, slot);
        return;
    }
    if (cmd_s == "attendance_log_fetch")
    {
        const long max_bytes = mg_json_get_long(json, "$.max_bytes", -1);
        const long tail_lines = mg_json_get_long(json, "$.tail_lines", -1);
        char *date_c = mg_json_get_str(json, "$.date");
        std::string date_s = date_c ? trim(date_c) : "";
        if (date_c)
            free(date_c);
        handle_attendance_log_fetch_local(request_id_s, max_bytes, tail_lines, date_s);
        return;
    }

    int bridge_cmd = IPC_BRIDGE_CMD_NONE;
    if (!bridge_cmd_from_string(cmd_s, &bridge_cmd))
    {
        publish_reply_failure(request_id_s, IPC_BRIDGE_CMD_NONE, "unsupported command");
        return;
    }
    if ((ui_is_session_active() || g_rt.import_active.load() || has_pending_library_write_cmd()) &&
        is_library_write_cmd(bridge_cmd))
    {
        publish_reply_failure(request_id_s, bridge_cmd,
                              g_rt.import_active.load() ? "import already in progress" : "library write command busy");
        return;
    }
    if (bridge_cmd == IPC_BRIDGE_CMD_REGISTER_CURRENT && name_s.empty())
    {
        publish_reply_failure(request_id_s, bridge_cmd, "name is required");
        return;
    }
    if (bridge_cmd == IPC_BRIDGE_CMD_REGISTER_COMMIT && name_s.empty())
    {
        publish_reply_failure(request_id_s, bridge_cmd, "name is required");
        return;
    }

    enqueue_command(PendingCmd{request_id_s, bridge_cmd, name_s, PendingCmdSource::remote});
}

void mqtt_send_login(mg_connection *c)
{
    if (!c)
        return;

    mg_mqtt_opts opts{};
    opts.client_id = mg_str(g_rt.cfg.mqtt_client_id.c_str());
    opts.user = mg_str(g_rt.cfg.mqtt_username.c_str());
    opts.pass = mg_str(g_rt.cfg.mqtt_password.c_str());
    opts.qos = 1;
    opts.retain = true;
    opts.keepalive = static_cast<uint16_t>(g_rt.cfg.mqtt_keepalive_s);
    opts.clean = true;

    const std::string will_topic = topic_up_status();
    const std::string will_payload = build_status_json(false);
    opts.topic = mg_str(will_topic.c_str());
    opts.message = mg_str(will_payload.c_str());

    g_rt.mqtt_last_progress_ms.store(now_ms());
    std::cout << "face_netd: mqtt event CONNECT id=" << c->id << " send CONNECT client_id=" << g_rt.cfg.mqtt_client_id
              << " keepalive=" << g_rt.cfg.mqtt_keepalive_s << "s will_topic=" << will_topic << std::endl;
    mg_mqtt_login(c, &opts);
}

uint64_t mqtt_retry_delay_ms(uint32_t failures)
{
    uint64_t delay = kMqttRetryInitialMs;
    while (failures > 1 && delay < static_cast<uint64_t>(kMqttRetryMaxMs))
    {
        delay *= 2;
        if (delay > static_cast<uint64_t>(kMqttRetryMaxMs))
            delay = kMqttRetryMaxMs;
        --failures;
    }
    return delay;
}

void mqtt_reset_retry_backoff()
{
    g_rt.mqtt_retry_failures.store(0);
    g_rt.mqtt_next_dial_ms.store(0);
}

void mqtt_schedule_retry(bool was_connected)
{
    if (g_rt.stop.load())
    {
        g_rt.mqtt_next_dial_ms.store(0);
        return;
    }

    const uint32_t failures = was_connected ? 1 : (g_rt.mqtt_retry_failures.load() + 1);
    const uint64_t delay = mqtt_retry_delay_ms(failures);
    g_rt.mqtt_retry_failures.store(failures);
    g_rt.mqtt_next_dial_ms.store(now_ms() + delay);

    std::cerr << "face_netd: mqtt reconnect scheduled in " << delay << "ms"
              << " (failure_streak=" << failures << ")" << std::endl;
}

void mqtt_handle_read(mg_connection *c)
{
    if (!c)
        return;

    for (;;)
    {
        mg_mqtt_message mm{};
        const int rc = mg_mqtt_parse(c->recv.buf, c->recv.len, 4, &mm);
        if (rc == MQTT_MALFORMED)
        {
            std::cerr << "face_netd: mqtt malformed packet, closing connection" << std::endl;
            c->is_closing = 1;
            break;
        }
        if (rc != MQTT_OK)
            break;

        g_rt.mqtt_last_progress_ms.store(now_ms());
        std::cout << "face_netd: mqtt packet cmd=" << static_cast<int>(mm.cmd) << " len=" << mm.dgram.len << std::endl;

        switch (mm.cmd)
        {
        case MQTT_CMD_CONNACK:
            if (mm.ack != 0)
            {
                std::cerr << "face_netd: mqtt connack failed code=" << static_cast<int>(mm.ack) << std::endl;
                c->is_closing = 1;
                break;
            }

            mqtt_reset_retry_backoff();
            g_rt.mqtt_connected.store(true);
            g_mqtt = c;
            {
                mg_mqtt_opts sub{};
                const std::string down_topic = topic_down_cmd();
                sub.topic = mg_str(down_topic.c_str());
                sub.qos = 1;
                mg_mqtt_sub(c, &sub);
            }
            {
                mg_mqtt_opts sub{};
                sub.topic = mg_str("k230/time_sync");
                sub.qos = 0;
                mg_mqtt_sub(c, &sub);
            }
            enqueue_status_publish(true, true);
            std::cout << "face_netd: mqtt connected " << g_rt.cfg.mqtt_url << std::endl;
            attendance_wall_clock_prime_from_pc_async();
            break;

        case MQTT_CMD_PUBLISH:
            if (mm.qos > 0)
            {
                uint16_t id = mg_ntohs(mm.id);
                mg_mqtt_send_header(c, MQTT_CMD_PUBACK, 0, sizeof(id));
                mg_send(c, &id, sizeof(id));
            }
            if (mg_strcmp(mm.topic, mg_str(topic_down_cmd().c_str())) == 0)
                parse_and_enqueue_command(mm.data);
            else if (mg_strcmp(mm.topic, mg_str("k230/time_sync")) == 0)
                handle_time_sync_mqtt_message(mm.data);
            break;

        case MQTT_CMD_PUBREC:
        {
            uint16_t id = mg_ntohs(mm.id);
            mg_mqtt_send_header(c, MQTT_CMD_PUBREL, 2, sizeof(id));
            mg_send(c, &id, sizeof(id));
            break;
        }

        case MQTT_CMD_PUBREL:
        {
            uint16_t id = mg_ntohs(mm.id);
            mg_mqtt_send_header(c, MQTT_CMD_PUBCOMP, 0, sizeof(id));
            mg_send(c, &id, sizeof(id));
            break;
        }

        default:
            break;
        }

        mg_iobuf_del(&c->recv, 0, mm.dgram.len);
    }
}

void mqtt_event_handler(mg_connection *c, int ev, void *ev_data)
{
    switch (ev)
    {
    case MG_EV_OPEN:
        g_rt.mqtt_last_progress_ms.store(now_ms());
        std::cout << "face_netd: mqtt event OPEN id=" << c->id << " url=" << g_rt.cfg.mqtt_url << std::endl;
        break;
    case MG_EV_RESOLVE:
        g_rt.mqtt_last_progress_ms.store(now_ms());
        std::cout << "face_netd: mqtt event RESOLVE id=" << c->id << std::endl;
        break;
    case MG_EV_CONNECT:
        mqtt_send_login(c);
        break;
    case MG_EV_READ:
    {
        g_rt.mqtt_last_progress_ms.store(now_ms());
        long *n = static_cast<long *>(ev_data);
        std::cout << "face_netd: mqtt event READ id=" << c->id << " bytes=" << (n ? *n : -1)
                  << " recv_len=" << c->recv.len << std::endl;
        mqtt_handle_read(c);
        break;
    }
    case MG_EV_WRITE:
    {
        if (!g_rt.mqtt_connected.load())
        {
            g_rt.mqtt_last_progress_ms.store(now_ms());
            long *n = static_cast<long *>(ev_data);
            std::cout << "face_netd: mqtt event WRITE id=" << c->id << " bytes=" << (n ? *n : -1)
                      << " send_pending=" << c->send.len << std::endl;
        }
        break;
    }
    case MG_EV_ERROR:
        g_rt.mqtt_last_progress_ms.store(now_ms());
        std::cerr << "face_netd: mqtt error: " << (ev_data ? static_cast<char *>(ev_data) : "unknown") << std::endl;
        break;
    case MG_EV_CLOSE:
    {
        const bool was_connected = g_rt.mqtt_connected.exchange(false);
        if (g_mqtt == c)
            g_mqtt = nullptr;
        g_rt.mqtt_last_progress_ms.store(now_ms());
        std::cout << "face_netd: mqtt event CLOSE id=" << c->id << " connected=" << g_rt.mqtt_connected.load()
                  << " state(resolving=" << c->is_resolving << ",connecting=" << c->is_connecting
                  << ",send=" << c->send.len << ",recv=" << c->recv.len << ")" << std::endl;
        mqtt_schedule_retry(was_connected);
        break;
    }
    default:
        break;
    }
}

void maybe_log_mqtt_pending()
{
    mg_connection *c = g_mqtt;
    if (!c || g_rt.mqtt_connected.load())
        return;

    const uint64_t now = now_ms();
    const uint64_t last = g_rt.mqtt_last_pending_log_ms.load();
    if (last != 0 && (now - last) < 3000)
        return;

    g_rt.mqtt_last_pending_log_ms.store(now);
    std::cout << "face_netd: mqtt pending id=" << c->id << " url=" << g_rt.cfg.mqtt_url
              << " elapsed=" << (now - g_rt.mqtt_dial_start_ms.load()) << "ms"
              << " since_progress=" << (now - g_rt.mqtt_last_progress_ms.load()) << "ms"
              << " resolving=" << c->is_resolving << " connecting=" << c->is_connecting
              << " writable=" << c->is_writable << " readable=" << c->is_readable << " send=" << c->send.len
              << " recv=" << c->recv.len << std::endl;
}

void maybe_timeout_mqtt_pending()
{
    mg_connection *c = g_mqtt;
    if (!c || g_rt.mqtt_connected.load())
        return;

    const uint64_t elapsed = now_ms() - g_rt.mqtt_dial_start_ms.load();
    if (!c->is_connecting || elapsed < static_cast<uint64_t>(kMqttConnectTimeoutMs))
        return;

    std::cerr << "face_netd: mqtt connect timeout after " << elapsed << "ms to " << g_rt.cfg.mqtt_url
              << ", closing and retrying" << std::endl;
    c->is_closing = 1;
}

void maybe_connect_mqtt()
{
    if (g_mqtt || g_rt.stop.load())
        return;

    const uint64_t now = now_ms();
    const uint64_t next_dial = g_rt.mqtt_next_dial_ms.load();
    if (next_dial != 0 && now < next_dial)
        return;

    const uint32_t attempt = g_rt.mqtt_connect_attempts.fetch_add(1) + 1;
    g_rt.mqtt_dial_start_ms.store(now);
    g_rt.mqtt_last_progress_ms.store(now);
    g_rt.mqtt_last_pending_log_ms.store(0);
    g_rt.mqtt_next_dial_ms.store(0);
    std::cout << "face_netd: mqtt dial attempt=" << attempt << " url=" << g_rt.cfg.mqtt_url << std::endl;
    g_mqtt = mg_connect(&g_mgr, g_rt.cfg.mqtt_url.c_str(), mqtt_event_handler, nullptr);
    if (!g_mqtt)
    {
        std::cerr << "face_netd: mqtt connect request failed " << g_rt.cfg.mqtt_url << std::endl;
        mqtt_schedule_retry(false);
    }
}

void shutdown_mqtt_gracefully()
{
    mg_connection *c = g_mqtt;
    if (!c)
        return;

    if (g_rt.mqtt_connected.load())
    {
        mg_mqtt_opts opts{};
        std::cout << "face_netd: mqtt graceful disconnect id=" << c->id << std::endl;
        mg_mqtt_disconnect(c, &opts);
        const uint64_t flush_deadline = now_ms() + 300;
        while (g_mqtt == c && now_ms() < flush_deadline)
            mg_mgr_poll(&g_mgr, 50);
    }

    if (g_mqtt == c)
    {
        c->is_closing = 1;
        const uint64_t close_deadline = now_ms() + 200;
        while (g_mqtt == c && now_ms() < close_deadline)
            mg_mgr_poll(&g_mgr, 50);
    }
}

bool publish_queue_empty()
{
    std::lock_guard<std::mutex> lk(g_rt.out_mu);
    return g_rt.outbox.empty();
}

void on_signal(int)
{
    g_rt.stop.store(true);
    g_rt.cmd_cv.notify_all();
}

void print_usage(const char *argv0)
{
    std::cout << "Usage: " << argv0 << " [--config <path>]\n";
}

} // namespace

int main(int argc, char **argv)
{
    std::string config_path = "./face_netd.ini";
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--config" && (i + 1) < argc)
        {
            config_path = argv[++i];
        }
        else if (arg == "-h" || arg == "--help")
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            std::cerr << "face_netd: unknown argument: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!load_config(config_path, &g_rt.cfg))
        return 1;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::cout << "face_netd: device_id=" << g_rt.cfg.device_id << " mqtt_url=" << g_rt.cfg.mqtt_url
              << " heartbeat=" << g_rt.cfg.heartbeat_interval_ms << "ms face_db_dir=" << g_rt.cfg.face_db_dir
              << " attendance_log_base=" << g_rt.cfg.attendance_log_base << std::endl;
    {
        const std::string hb = resolved_server_http_base_for_sync();
        std::cout << "face_netd: attendance_pc_http=" << (hb.empty() ? "off" : hb)
                  << " + MQTT k230/time_sync (server_pc)" << std::endl;
    }

    mg_mgr_init(&g_mgr);

    runtime_config ui_cfg;
    ui_cfg.enabled = g_rt.cfg.ui_enabled;
    ui_cfg.touch_device = g_rt.cfg.ui_touch_device;
    ui_cfg.preview_timeout_ms = g_rt.cfg.ui_preview_timeout_ms;
    ui_cfg.overlay_profile = g_rt.cfg.ui_overlay_profile;
    ui_cfg.submit_command = [](const std::string &request_id, int cmd, const std::string &name) {
        if (is_library_write_cmd(cmd) &&
            (g_rt.import_active.load() || has_pending_library_write_cmd() || ui_is_session_active()))
        {
            PendingCmd rejected{request_id, cmd, name, PendingCmdSource::ui};
            publish_local_result(rejected, false, -1,
                                 g_rt.import_active.load() ? "Import already in progress" : "Library write command busy");
            return;
        }
        enqueue_command(PendingCmd{request_id, cmd, name, PendingCmdSource::ui});
    };
    ui_cfg.log_message = [](const std::string &message) { std::cout << message << std::endl; };
    ui_runtime_start(ui_cfg);

    std::thread ipc_thread(ipc_supervisor_thread);
    std::thread cmd_thread(command_worker_thread);

    while (!g_rt.stop.load())
    {
        maybe_connect_mqtt();
        mg_mgr_poll(&g_mgr, kMqttPollMs);
        maybe_log_mqtt_pending();
        maybe_timeout_mqtt_pending();
        if (g_rt.mqtt_connected.load())
        {
            enqueue_status_publish(true, false);
            drain_publish_queue();
            if (g_rt.shutdown_after_flush.load() && publish_queue_empty())
            {
                g_rt.stop.store(true);
                g_rt.cmd_cv.notify_all();
            }
        }
    }

    enqueue_status_publish(false, true);
    drain_publish_queue();

    g_rt.cmd_cv.notify_all();
    if (cmd_thread.joinable())
        cmd_thread.join();
    if (ipc_thread.joinable())
        ipc_thread.join();

    ui_runtime_stop();
    shutdown_mqtt_gracefully();
    mg_mgr_free(&g_mgr);

    std::cout << "face_netd: stopped" << std::endl;
    return 0;
}
