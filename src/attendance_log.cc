#include "attendance_log.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <atomic>
#include <iostream>
#include <sstream>
#include <sys/stat.h>

namespace {

constexpr char k_default_sd_base[] = "/sd/face_logs";
constexpr char k_fallback_base[] = "/tmp/face_logs";

/** throttle stderr when primary path repeatedly fails */
static std::atomic<uint64_t> g_primary_fail_warn_sec{0};

static bool ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode);
    if (errno != ENOENT)
        return false;
    return mkdir(path, 0755) == 0;
}

static bool ymd_from_epoch_ms(uint64_t ms, char out[16])
{
    const time_t sec = static_cast<time_t>(ms / 1000ULL);
    struct tm tm_local {};
    if (!localtime_r(&sec, &tm_local))
        return false;
    if (strftime(out, 16, "%Y-%m-%d", &tm_local) <= 0)
        return false;
    return true;
}

static std::string json_escape_bounded(const char *s, size_t max_len)
{
    std::string out;
    out.reserve(max_len * 2 + 2);
    out.push_back('"');
    size_t n = 0;
    while (s && *s && n < max_len)
    {
        const unsigned char c = static_cast<unsigned char>(*s++);
        ++n;
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                out += buf;
            }
            else
            {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
    return out;
}

static const char *ipc_evt_kind_str(uint8_t k)
{
    switch (static_cast<ipc_evt_kind_t>(k))
    {
    case IPC_EVT_KIND_RECOGNIZED:
        return "recognized";
    case IPC_EVT_KIND_STRANGER:
        return "stranger";
    case IPC_EVT_KIND_LIVENESS_FAIL:
        return "liveness_fail";
    default:
        return "unknown";
    }
}

static bool append_jsonl_line(const std::string &base_dir, uint64_t ts_ms, const std::string &json_object_no_outer_newline,
                              std::string *used_file_out)
{
    char ymd[16];
    uint64_t eff_ms = ts_ms;
    if (eff_ms == 0)
        eff_ms = static_cast<uint64_t>(time(nullptr)) * 1000ULL;
    if (!ymd_from_epoch_ms(eff_ms, ymd))
        std::strncpy(ymd, "1970-01-01", sizeof(ymd));

    auto try_one_base = [&](const std::string &base) -> bool {
        if (!ensure_dir(base.c_str()))
            return false;
        const std::string path = base + "/" + ymd + ".jsonl";
        FILE *fp = fopen(path.c_str(), "a");
        if (!fp)
            return false;
        fputs(json_object_no_outer_newline.c_str(), fp);
        fputc('\n', fp);
        fflush(fp);
        fclose(fp);
        if (used_file_out)
            *used_file_out = path;
        return true;
    };

    if (try_one_base(base_dir))
        return true;

    if (base_dir != k_fallback_base)
    {
        const int err = errno;
        const uint64_t sec = static_cast<uint64_t>(time(nullptr));
        uint64_t prev = g_primary_fail_warn_sec.load(std::memory_order_relaxed);
        if (prev == 0 || sec >= prev + 60)
        {
            if (g_primary_fail_warn_sec.compare_exchange_strong(prev, sec, std::memory_order_relaxed))
                std::cerr << "face_event: attendance log write failed under " << base_dir << " (errno=" << err
                          << "), trying " << k_fallback_base << "; canonical TF logs on Linux under attendance_log_base"
                          << std::endl;
        }
        if (try_one_base(k_fallback_base))
            return true;
    }

    return false;
}

} // namespace

std::string attendance_log_resolve_base_dir(int argc, char **argv)
{
    /* msh 执行脚本时常把 "$ATT_ROOT" 原样传入，得到非法路径 */
    auto argv_ok = [](const char *s) -> bool {
        return s && s[0] != '\0' && s[0] != '$';
    };

    if (argc >= 2 && argv[1])
    {
        if (!argv_ok(argv[1]))
            std::cerr << "face_event: ignoring invalid attendance argv (shell did not expand variables?): \"" << argv[1]
                      << "\"" << std::endl;
        else
            return std::string(argv[1]);
    }
    const char *env = std::getenv("FACE_ATT_LOG_BASE");
    if (env && env[0] != '\0')
        return std::string(env);
    env = std::getenv("FACE_ATT_LOG");
    if (env && env[0] != '\0')
        return std::string(env);
    return std::string(k_default_sd_base);
}

bool attendance_log_append_ipc_evt(const std::string &base_dir, const ipc_evt_t *ev, std::string *used_file_out)
{
    if (!ev || ev->magic != IPC_MAGIC)
        return false;

    const int live_ok = (ev->evt_kind == IPC_EVT_KIND_LIVENESS_FAIL) ? 0 : 1;
    const std::time_t tt = time(nullptr);
    struct tm tm_local{};
    if (!localtime_r(&tt, &tm_local))
        return false;
    char hms[12]{};
    if (strftime(hms, sizeof(hms), "%H:%M:%S", &tm_local) <= 0)
        return false;

    std::ostringstream oss;
    oss << "{\"wall_time\":\"" << hms << "\",\"evt_kind\":\""
        << ipc_evt_kind_str(ev->evt_kind) << "\""
        << ",\"face_id\":" << ev->face_id << ",\"name\":" << json_escape_bounded(ev->name, IPC_NAME_MAX - 1)
        << ",\"score\":" << ev->score << ",\"is_stranger\":" << static_cast<int>(ev->is_stranger)
        << ",\"live_ok\":" << live_ok << "}";

    return append_jsonl_line(base_dir, static_cast<uint64_t>(tt) * 1000ULL, oss.str(), used_file_out);
}

bool attendance_log_append_meta(const std::string &base_dir, const char *note, std::string *used_file_out)
{
    const std::time_t tt = time(nullptr);
    struct tm tm_local{};
    if (!localtime_r(&tt, &tm_local))
        return false;
    char hms[12]{};
    if (strftime(hms, sizeof(hms), "%H:%M:%S", &tm_local) <= 0)
        return false;

    const uint64_t ms = static_cast<uint64_t>(tt) * 1000ULL;
    std::ostringstream oss;
    oss << "{\"evt_kind\":\"meta\",\"wall_time\":\"" << hms << "\""
        << ",\"note\":" << json_escape_bounded(note ? note : "", 256) << "}";
    return append_jsonl_line(base_dir, ms, oss.str(), used_file_out);
}
