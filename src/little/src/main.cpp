#include "face_gateway_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "k_ipcmsg.h"

namespace {

constexpr uint16_t kDefaultHttpPort = 8080;
constexpr uint32_t kDefaultIpcRemoteId = 1;
constexpr uint32_t kDefaultIpcPort = 110;
constexpr uint32_t kDefaultIpcPriority = 0;
constexpr const char *kDefaultIpcService = "face_ctrl";
constexpr const char *kLoopbackAddress = "0.0.0.0";
constexpr size_t kMaxRequestBytes = 8192;

std::atomic<bool> g_should_stop(false);

struct GatewayConfig {
    std::string bind_address = kLoopbackAddress;
    uint16_t http_port = kDefaultHttpPort;
    bool enable_ipc = true;
    /** 无板/无 ipcm 时调 HTTP 与 /api/ipc/* 契约；不连真实 IPCMSG */
    bool mock_mode = false;
    std::string ipc_service = kDefaultIpcService;
    uint32_t ipc_remote_id = kDefaultIpcRemoteId;
    uint32_t ipc_port = kDefaultIpcPort;
    uint32_t ipc_priority = kDefaultIpcPriority;
};

struct NetworkAddress {
    std::string ifname;
    std::string family;
    std::string address;
};

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> query;
};

struct IpcSnapshot {
    bool enabled = true;
    bool service_added = false;
    bool connected = false;
    uint64_t sent_count = 0;
    uint64_t recv_count = 0;
    uint32_t last_cmd = 0;
    uint32_t last_module = 0;
    int32_t last_ret = 0;
    bool last_is_resp = false;
    std::string last_payload;
    std::string last_event_time;
    std::string last_error;
};

std::string json_escape(const std::string &input)
{
    std::ostringstream oss;

    for (unsigned char ch : input) {
        switch (ch) {
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
            if (ch < 0x20) {
                oss << "\\u"
                    << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(ch)
                    << std::dec << std::setfill(' ');
            } else {
                oss << static_cast<char>(ch);
            }
            break;
        }
    }

    return oss.str();
}

std::string now_local_iso8601()
{
    using clock = std::chrono::system_clock;
    auto now = clock::now();
    std::time_t t = clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    return buffer;
}

uint64_t monotonic_millis()
{
    using clock = std::chrono::steady_clock;
    auto now = clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

bool is_printable_buffer(const void *data, size_t len)
{
    const unsigned char *bytes = static_cast<const unsigned char *>(data);

    if (data == nullptr || len == 0)
        return true;

    for (size_t i = 0; i < len; ++i) {
        if (bytes[i] == '\0')
            return false;
        if (!std::isprint(bytes[i]) && !std::isspace(bytes[i]))
            return false;
    }

    return true;
}

std::string bytes_to_hex(const void *data, size_t len)
{
    const unsigned char *bytes = static_cast<const unsigned char *>(data);
    std::ostringstream oss;

    for (size_t i = 0; i < len; ++i) {
        if (i != 0)
            oss << ' ';
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned int>(bytes[i]);
    }

    return oss.str();
}

std::string body_to_text(const void *data, size_t len)
{
    if (data == nullptr || len == 0)
        return "";

    if (is_printable_buffer(data, len))
        return std::string(static_cast<const char *>(data), len);

    return bytes_to_hex(data, len);
}

std::string url_decode(const std::string &input)
{
    std::string output;
    output.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '+') {
            output.push_back(' ');
            continue;
        }

        if (input[i] == '%' && i + 2 < input.size()) {
            char hex[3] = {input[i + 1], input[i + 2], '\0'};
            char *end = nullptr;
            long value = std::strtol(hex, &end, 16);
            if (end != nullptr && *end == '\0') {
                output.push_back(static_cast<char>(value));
                i += 2;
                continue;
            }
        }

        output.push_back(input[i]);
    }

    return output;
}

std::map<std::string, std::string> parse_query(const std::string &query)
{
    std::map<std::string, std::string> result;
    size_t begin = 0;

    while (begin <= query.size()) {
        size_t end = query.find('&', begin);
        if (end == std::string::npos)
            end = query.size();

        std::string item = query.substr(begin, end - begin);
        size_t equal = item.find('=');
        if (!item.empty()) {
            if (equal == std::string::npos) {
                result[url_decode(item)] = "";
            } else {
                result[url_decode(item.substr(0, equal))] =
                    url_decode(item.substr(equal + 1));
            }
        }

        begin = end + 1;
        if (end == query.size())
            break;
    }

    return result;
}

std::vector<NetworkAddress> collect_network_addresses()
{
    std::vector<NetworkAddress> addresses;
    struct ifaddrs *ifaddr = nullptr;

    if (getifaddrs(&ifaddr) != 0)
        return addresses;

    for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr)
            continue;

        int family = ifa->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6)
            continue;

        char host[NI_MAXHOST];
        int rc = getnameinfo(ifa->ifa_addr,
                             family == AF_INET
                                 ? sizeof(struct sockaddr_in)
                                 : sizeof(struct sockaddr_in6),
                             host, sizeof(host), nullptr, 0, NI_NUMERICHOST);
        if (rc != 0)
            continue;

        NetworkAddress addr;
        addr.ifname = ifa->ifa_name;
        addr.family = family == AF_INET ? "ipv4" : "ipv6";
        addr.address = host;
        addresses.push_back(addr);
    }

    freeifaddrs(ifaddr);
    return addresses;
}

std::string build_network_json()
{
    auto addresses = collect_network_addresses();
    std::ostringstream oss;

    oss << "{";
    oss << "\"ok\":true,";
    oss << "\"time\":\"" << json_escape(now_local_iso8601()) << "\",";
    oss << "\"interfaces\":[";

    for (size_t i = 0; i < addresses.size(); ++i) {
        if (i != 0)
            oss << ",";
        oss << "{"
            << "\"name\":\"" << json_escape(addresses[i].ifname) << "\","
            << "\"family\":\"" << json_escape(addresses[i].family) << "\","
            << "\"address\":\"" << json_escape(addresses[i].address) << "\""
            << "}";
    }

    oss << "]";
    oss << "}";
    return oss.str();
}

std::string build_system_json()
{
    struct utsname uts;
    struct sysinfo info;
    char hostname[128];
    double loads[3] = {0.0, 0.0, 0.0};

    std::memset(&uts, 0, sizeof(uts));
    std::memset(&info, 0, sizeof(info));
    std::memset(hostname, 0, sizeof(hostname));

    uname(&uts);
    sysinfo(&info);
    gethostname(hostname, sizeof(hostname) - 1);
    getloadavg(loads, 3);

    unsigned long total_kb = static_cast<unsigned long>(info.totalram);
    unsigned long free_kb = static_cast<unsigned long>(info.freeram);
    total_kb = total_kb * info.mem_unit / 1024;
    free_kb = free_kb * info.mem_unit / 1024;

    std::ostringstream oss;
    oss << "{";
    oss << "\"ok\":true,";
    oss << "\"time\":\"" << json_escape(now_local_iso8601()) << "\",";
    oss << "\"hostname\":\"" << json_escape(hostname) << "\",";
    oss << "\"sysname\":\"" << json_escape(uts.sysname) << "\",";
    oss << "\"release\":\"" << json_escape(uts.release) << "\",";
    oss << "\"machine\":\"" << json_escape(uts.machine) << "\",";
    oss << "\"uptime_sec\":" << info.uptime << ",";
    oss << "\"mem_total_kb\":" << total_kb << ",";
    oss << "\"mem_free_kb\":" << free_kb << ",";
    oss << "\"loadavg\":["
        << loads[0] << "," << loads[1] << "," << loads[2] << "]";
    oss << "}";
    return oss.str();
}

bool parse_u32(const std::string &value, uint32_t *out)
{
    if (out == nullptr || value.empty())
        return false;

    char *end = nullptr;
    unsigned long parsed = std::strtoul(value.c_str(), &end, 0);
    if (end == nullptr || *end != '\0')
        return false;

    *out = static_cast<uint32_t>(parsed);
    return true;
}

uint32_t resolve_named_cmd(const std::string &value)
{
    if (value == "PING")
        return FACE_CTRL_CMD_PING;
    if (value == "GET_STATUS")
        return FACE_CTRL_CMD_GET_STATUS;
    if (value == "GET_DB_COUNT")
        return FACE_CTRL_CMD_GET_DB_COUNT;
    if (value == "DB_RESET")
        return FACE_CTRL_CMD_DB_RESET;
    if (value == "REGISTER_START")
        return FACE_CTRL_CMD_REGISTER_START;
    if (value == "REGISTER_COMMIT")
        return FACE_CTRL_CMD_REGISTER_COMMIT;
    if (value == "SHUTDOWN")
        return FACE_CTRL_CMD_SHUTDOWN;

    uint32_t numeric = 0;
    if (parse_u32(value, &numeric))
        return numeric;

    return 0;
}

uint32_t resolve_named_module(const std::string &value)
{
    if (value == "WEB")
        return FACE_CTRL_MODULE_WEB;
    if (value == "EVENT")
        return FACE_CTRL_MODULE_EVENT;

    uint32_t numeric = 0;
    if (parse_u32(value, &numeric))
        return numeric;

    return FACE_CTRL_MODULE_WEB;
}

class IpcClient {
public:
    explicit IpcClient(const GatewayConfig &config)
        : config_(config)
    {
        snapshot_.enabled = config.enable_ipc || config.mock_mode;
        if (config_.mock_mode) {
            snapshot_.service_added = true;
            snapshot_.connected = true;
            snapshot_.last_error.clear();
        }
    }

    ~IpcClient()
    {
        stop();
    }

    void start()
    {
        if (config_.mock_mode || !config_.enable_ipc)
            return;

        worker_ = std::thread(&IpcClient::worker_loop, this);
    }

    void stop()
    {
        stop_requested_.store(true);
        if (worker_.joinable())
            worker_.join();
    }

    IpcSnapshot snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        IpcSnapshot copy = snapshot_;
        if (!config_.mock_mode && handle_ >= 0)
            copy.connected = kd_ipcmsg_is_connect(handle_);
        return copy;
    }

    bool send_only(uint32_t module, uint32_t cmd, const std::string &payload,
                   std::string *error_out)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (config_.mock_mode) {
            snapshot_.sent_count++;
            snapshot_.last_cmd = cmd;
            snapshot_.last_module = module;
            snapshot_.last_ret = 0;
            snapshot_.last_is_resp = false;
            snapshot_.last_payload = payload;
            snapshot_.last_event_time = now_local_iso8601();
            snapshot_.last_error.clear();
            snapshot_.connected = true;
            if (error_out != nullptr)
                error_out->clear();
            return true;
        }

        if (!config_.enable_ipc) {
            if (error_out != nullptr)
                *error_out = "ipc disabled";
            return false;
        }

        if (handle_ < 0 || !kd_ipcmsg_is_connect(handle_)) {
            snapshot_.connected = false;
            snapshot_.last_error = "ipc not connected";
            if (error_out != nullptr)
                *error_out = snapshot_.last_error;
            return false;
        }

        if (payload.size() > K_IPCMSG_MAX_CONTENT_LEN) {
            snapshot_.last_error = "payload is larger than K_IPCMSG_MAX_CONTENT_LEN";
            if (error_out != nullptr)
                *error_out = snapshot_.last_error;
            return false;
        }

        k_ipcmsg_message_t *msg = kd_ipcmsg_create_message(
            module, cmd, payload.empty() ? nullptr : payload.data(),
            payload.size());
        if (msg == nullptr) {
            snapshot_.last_error = "kd_ipcmsg_create_message failed";
            if (error_out != nullptr)
                *error_out = snapshot_.last_error;
            return false;
        }

        int ret = kd_ipcmsg_send_only(handle_, msg);
        kd_ipcmsg_destroy_message(msg);
        if (ret != 0) {
            snapshot_.last_error = "kd_ipcmsg_send_only failed";
            if (error_out != nullptr)
                *error_out = snapshot_.last_error;
            return false;
        }

        snapshot_.sent_count++;
        snapshot_.last_cmd = cmd;
        snapshot_.last_module = module;
        snapshot_.last_payload = payload;
        snapshot_.last_event_time = now_local_iso8601();
        snapshot_.last_error.clear();
        snapshot_.connected = true;

        if (error_out != nullptr)
            error_out->clear();
        return true;
    }

private:
    static void handle_message_trampoline(k_s32 s32Id, k_ipcmsg_message_t *msg)
    {
        if (instance_ != nullptr)
            instance_->handle_message(s32Id, msg);
    }

    void handle_message(k_s32, k_ipcmsg_message_t *msg)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.recv_count++;
        snapshot_.last_cmd = msg->u32CMD;
        snapshot_.last_module = msg->u32Module;
        snapshot_.last_ret = msg->s32RetVal;
        snapshot_.last_is_resp = msg->bIsResp;
        snapshot_.last_payload = body_to_text(msg->pBody, msg->u32BodyLen);
        snapshot_.last_event_time = now_local_iso8601();
        snapshot_.last_error.clear();
        snapshot_.connected = true;
    }

    void worker_loop()
    {
        instance_ = this;

        while (!stop_requested_.load()) {
            if (access("/dev/ipcm_user", R_OK | W_OK) != 0) {
                update_error("waiting for /dev/ipcm_user");
                sleep_with_break(500);
                continue;
            }

            if (!add_service_once()) {
                sleep_with_break(1000);
                continue;
            }

            int handle = -1;
            int ret = kd_ipcmsg_try_connect(&handle, config_.ipc_service.c_str(),
                                            handle_message_trampoline);
            if (ret != 0) {
                update_error("kd_ipcmsg_try_connect failed");
                sleep_with_break(1000);
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                handle_ = handle;
                snapshot_.connected = kd_ipcmsg_is_connect(handle_);
                snapshot_.last_error.clear();
            }

            kd_ipcmsg_run(handle_);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (handle_ >= 0)
                    kd_ipcmsg_disconnect(handle_);
                handle_ = -1;
                snapshot_.connected = false;
                if (snapshot_.last_error.empty())
                    snapshot_.last_error = "ipc receive loop exited";
            }

            sleep_with_break(500);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (handle_ >= 0)
                kd_ipcmsg_disconnect(handle_);
            handle_ = -1;
            snapshot_.connected = false;
        }
    }

    bool add_service_once()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (snapshot_.service_added)
            return true;

        k_ipcmsg_connect_t attr;
        attr.u32RemoteId = config_.ipc_remote_id;
        attr.u32Port = config_.ipc_port;
        attr.u32Priority = config_.ipc_priority;

        int ret = kd_ipcmsg_add_service(config_.ipc_service.c_str(), &attr);
        if (ret != 0) {
            snapshot_.last_error = "kd_ipcmsg_add_service failed";
            return false;
        }

        snapshot_.service_added = true;
        snapshot_.last_error.clear();
        return true;
    }

    void update_error(const std::string &message)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.last_error = message;
        snapshot_.last_event_time = now_local_iso8601();
    }

    void sleep_with_break(int millis)
    {
        int remaining = millis;
        while (!stop_requested_.load() && remaining > 0) {
            constexpr int chunk = 100;
            int current = remaining > chunk ? chunk : remaining;
            usleep(current * 1000);
            remaining -= current;
        }
    }

    GatewayConfig config_;
    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> stop_requested_{false};
    int handle_ = -1;
    IpcSnapshot snapshot_;

    static IpcClient *instance_;
};

IpcClient *IpcClient::instance_ = nullptr;

class HttpServer {
public:
    HttpServer(const GatewayConfig &config, IpcClient &ipc_client)
        : config_(config), ipc_client_(ipc_client), started_at_ms_(monotonic_millis())
    {
    }

    int run()
    {
        int ret = start_listen();
        if (ret != 0)
            return ret;

        while (!g_should_stop.load()) {
            struct pollfd pfd;
            pfd.fd = listen_fd_;
            pfd.events = POLLIN;
            pfd.revents = 0;

            int rc = poll(&pfd, 1, 500);
            if (rc < 0) {
                if (errno == EINTR)
                    continue;
                std::perror("poll");
                break;
            }

            if (rc == 0)
                continue;

            if ((pfd.revents & POLLIN) == 0)
                continue;

            int client_fd = accept(listen_fd_, nullptr, nullptr);
            if (client_fd < 0) {
                if (errno == EINTR)
                    continue;
                std::perror("accept");
                continue;
            }

            handle_client(client_fd);
            close(client_fd);
        }

        if (listen_fd_ >= 0)
            close(listen_fd_);
        return 0;
    }

private:
    int start_listen()
    {
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            std::perror("socket");
            return -1;
        }

        int reuse = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config_.http_port);
        addr.sin_addr.s_addr = inet_addr(config_.bind_address.c_str());

        if (bind(listen_fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
            std::perror("bind");
            return -1;
        }

        if (listen(listen_fd_, 8) != 0) {
            std::perror("listen");
            return -1;
        }

        std::printf("face_gateway listening on %s:%u\n",
                    config_.bind_address.c_str(), config_.http_port);
        return 0;
    }

    void handle_client(int client_fd)
    {
        std::string request_buffer;
        request_buffer.reserve(kMaxRequestBytes);

        char temp[1024];
        while (request_buffer.size() < kMaxRequestBytes) {
            ssize_t n = recv(client_fd, temp, sizeof(temp), 0);
            if (n <= 0)
                break;
            request_buffer.append(temp, static_cast<size_t>(n));
            if (request_buffer.find("\r\n\r\n") != std::string::npos)
                break;
        }

        HttpRequest request;
        if (!parse_request(request_buffer, &request)) {
            send_response(client_fd, "400 Bad Request", "application/json",
                          "{\"ok\":false,\"error\":\"bad request\"}");
            return;
        }

        std::string body;
        std::string content_type = "application/json";
        std::string status = "200 OK";

        route_request(request, &status, &content_type, &body);
        send_response(client_fd, status, content_type, body);
    }

    static bool parse_request(const std::string &buffer, HttpRequest *request)
    {
        if (request == nullptr)
            return false;

        size_t line_end = buffer.find("\r\n");
        if (line_end == std::string::npos)
            return false;

        std::string line = buffer.substr(0, line_end);
        std::istringstream iss(line);
        std::string target;
        std::string version;
        if (!(iss >> request->method >> target >> version))
            return false;

        size_t query_pos = target.find('?');
        if (query_pos == std::string::npos) {
            request->path = target;
            request->query.clear();
        } else {
            request->path = target.substr(0, query_pos);
            request->query = parse_query(target.substr(query_pos + 1));
        }

        return true;
    }

    void route_request(const HttpRequest &request, std::string *status,
                       std::string *content_type, std::string *body)
    {
        if (request.method != "GET" && request.method != "POST") {
            *status = "405 Method Not Allowed";
            *body = "{\"ok\":false,\"error\":\"method not allowed\"}";
            return;
        }

        if (request.path == "/") {
            *content_type = "text/html; charset=utf-8";
            *body = build_index_html();
            return;
        }

        if (request.path == "/api/ping") {
            *body = build_ping_json();
            return;
        }

        if (request.path == "/api/status") {
            *body = build_status_json();
            return;
        }

        if (request.path == "/api/system") {
            *body = build_system_json();
            return;
        }

        if (request.path == "/api/network") {
            *body = build_network_json();
            return;
        }

        if (request.path == "/api/ipc/last") {
            *body = build_ipc_json(ipc_client_.snapshot());
            return;
        }

        if (request.path == "/api/ipc/send") {
            *body = handle_ipc_send(request.query);
            return;
        }

        if (request.path == "/api/help") {
            *body = build_help_json();
            return;
        }

        *status = "404 Not Found";
        *body = "{\"ok\":false,\"error\":\"not found\"}";
    }

    std::string build_ping_json() const
    {
        std::ostringstream oss;
        oss << "{"
            << "\"ok\":true,"
            << "\"service\":\"face_gateway\","
            << "\"time\":\"" << json_escape(now_local_iso8601()) << "\","
            << "\"uptime_ms\":" << (monotonic_millis() - started_at_ms_)
            << "}";
        return oss.str();
    }

    std::string build_ipc_json(const IpcSnapshot &snapshot) const
    {
        std::ostringstream oss;
        oss << "{"
            << "\"enabled\":" << (snapshot.enabled ? "true" : "false") << ","
            << "\"service_added\":" << (snapshot.service_added ? "true" : "false") << ","
            << "\"connected\":" << (snapshot.connected ? "true" : "false") << ","
            << "\"sent_count\":" << snapshot.sent_count << ","
            << "\"recv_count\":" << snapshot.recv_count << ","
            << "\"last_module\":" << snapshot.last_module << ","
            << "\"last_cmd\":" << snapshot.last_cmd << ","
            << "\"last_ret\":" << snapshot.last_ret << ","
            << "\"last_is_resp\":" << (snapshot.last_is_resp ? "true" : "false") << ","
            << "\"last_event_time\":\"" << json_escape(snapshot.last_event_time) << "\","
            << "\"last_payload\":\"" << json_escape(snapshot.last_payload) << "\","
            << "\"last_error\":\"" << json_escape(snapshot.last_error) << "\""
            << "}";
        return oss.str();
    }

    std::string build_status_json() const
    {
        struct utsname uts;
        char hostname[128];
        std::memset(&uts, 0, sizeof(uts));
        std::memset(hostname, 0, sizeof(hostname));
        uname(&uts);
        gethostname(hostname, sizeof(hostname) - 1);

        std::ostringstream oss;
        oss << "{"
            << "\"ok\":true,"
            << "\"service\":\"face_gateway\","
            << "\"mock\":" << (config_.mock_mode ? "true" : "false") << ","
            << "\"time\":\"" << json_escape(now_local_iso8601()) << "\","
            << "\"hostname\":\"" << json_escape(hostname) << "\","
            << "\"machine\":\"" << json_escape(uts.machine) << "\","
            << "\"kernel\":\"" << json_escape(uts.release) << "\","
            << "\"http\":{"
            << "\"bind\":\"" << json_escape(config_.bind_address) << "\","
            << "\"port\":" << config_.http_port
            << "},"
            << "\"ipc_config\":{"
            << "\"enabled\":" << (config_.enable_ipc ? "true" : "false") << ","
            << "\"service\":\"" << json_escape(config_.ipc_service) << "\","
            << "\"remote_id\":" << config_.ipc_remote_id << ","
            << "\"port\":" << config_.ipc_port << ","
            << "\"priority\":" << config_.ipc_priority
            << "},"
            << "\"ipc_state\":" << build_ipc_json(ipc_client_.snapshot())
            << "}";
        return oss.str();
    }

    std::string build_help_json() const
    {
        return "{"
               "\"ok\":true,"
               "\"endpoints\":["
               "\"GET /\","
               "\"GET /api/ping\","
               "\"GET /api/status\","
               "\"GET /api/system\","
               "\"GET /api/network\","
               "\"GET /api/ipc/last\","
               "\"GET /api/ipc/send?cmd=GET_STATUS\","
               "\"GET /api/ipc/send?cmd=GET_DB_COUNT\","
               "\"GET /api/ipc/send?cmd=DB_RESET\","
               "\"GET /api/ipc/send?cmd=0x1234&module=0x1&payload=hello\""
               "]"
               "}";
    }

    std::string handle_ipc_send(const std::map<std::string, std::string> &query)
    {
        auto cmd_it = query.find("cmd");
        if (cmd_it == query.end()) {
            return "{\"ok\":false,\"error\":\"missing cmd query parameter\"}";
        }

        uint32_t cmd = resolve_named_cmd(cmd_it->second);
        if (cmd == 0) {
            return "{\"ok\":false,\"error\":\"unknown cmd\"}";
        }

        uint32_t module = FACE_CTRL_MODULE_WEB;
        auto module_it = query.find("module");
        if (module_it != query.end())
            module = resolve_named_module(module_it->second);

        std::string payload;
        auto payload_it = query.find("payload");
        if (payload_it != query.end())
            payload = payload_it->second;

        std::string error;
        bool ok = ipc_client_.send_only(module, cmd, payload, &error);

        std::ostringstream oss;
        oss << "{"
            << "\"ok\":" << (ok ? "true" : "false") << ","
            << "\"module\":" << module << ","
            << "\"cmd\":" << cmd << ","
            << "\"payload\":\"" << json_escape(payload) << "\","
            << "\"error\":\"" << json_escape(error) << "\""
            << "}";
        return oss.str();
    }

    std::string build_index_html() const
    {
        return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>face_gateway</title>
  <style>
    body { font-family: monospace; max-width: 980px; margin: 24px auto; padding: 0 16px; background: #f3f5f7; color: #111; }
    .card { background: #fff; border-radius: 12px; padding: 16px; margin-bottom: 16px; box-shadow: 0 6px 20px rgba(0,0,0,0.06); }
    h1, h2 { margin-top: 0; }
    button { margin-right: 8px; margin-bottom: 8px; padding: 8px 12px; }
    input { padding: 8px; width: 180px; }
    pre { white-space: pre-wrap; word-break: break-word; }
  </style>
</head>
<body>
  <div class="card">
    <h1>face_gateway</h1>
    <p>Initial small-core HTTP gateway for LAN connectivity checks, board status,
       and future big-core control integration.</p>
    <button onclick="refreshAll()">Refresh</button>
    <button onclick="sendCmd('PING')">IPC PING</button>
    <button onclick="sendCmd('GET_STATUS')">IPC GET_STATUS</button>
    <button onclick="sendCmd('GET_DB_COUNT')">IPC GET_DB_COUNT</button>
    <button onclick="sendCmd('DB_RESET')">IPC DB_RESET</button>
  </div>

  <div class="card">
    <h2>Custom IPC Command</h2>
    <input id="cmd" placeholder="GET_STATUS or 0x1001">
    <input id="module" placeholder="WEB or 0x1">
    <input id="payload" placeholder="payload">
    <button onclick="sendCustom()">Send</button>
    <pre id="sendResult">No command sent yet</pre>
  </div>

  <div class="card">
    <h2>Status</h2>
    <pre id="status">Loading...</pre>
  </div>

  <div class="card">
    <h2>Network</h2>
    <pre id="network">Loading...</pre>
  </div>

  <div class="card">
    <h2>System</h2>
    <pre id="system">Loading...</pre>
  </div>

  <script>
    async function fetchJson(url) {
      const resp = await fetch(url);
      return await resp.json();
    }

    async function refreshAll() {
      document.getElementById('status').textContent = JSON.stringify(await fetchJson('/api/status'), null, 2);
      document.getElementById('network').textContent = JSON.stringify(await fetchJson('/api/network'), null, 2);
      document.getElementById('system').textContent = JSON.stringify(await fetchJson('/api/system'), null, 2);
    }

    async function sendCmd(cmd) {
      document.getElementById('sendResult').textContent =
        JSON.stringify(await fetchJson('/api/ipc/send?cmd=' + encodeURIComponent(cmd)), null, 2);
      await refreshAll();
    }

    async function sendCustom() {
      const cmd = document.getElementById('cmd').value;
      const module = document.getElementById('module').value;
      const payload = document.getElementById('payload').value;
      const params = new URLSearchParams();
      if (cmd) params.set('cmd', cmd);
      if (module) params.set('module', module);
      if (payload) params.set('payload', payload);
      document.getElementById('sendResult').textContent =
        JSON.stringify(await fetchJson('/api/ipc/send?' + params.toString()), null, 2);
      await refreshAll();
    }

    refreshAll();
    setInterval(refreshAll, 3000);
  </script>
</body>
</html>
)HTML";
    }

    static void send_response(int client_fd, const std::string &status,
                              const std::string &content_type,
                              const std::string &body)
    {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << status << "\r\n";
        oss << "Content-Type: " << content_type << "\r\n";
        oss << "Content-Length: " << body.size() << "\r\n";
        oss << "Connection: close\r\n";
        oss << "Access-Control-Allow-Origin: *\r\n";
        oss << "\r\n";
        oss << body;

        std::string response = oss.str();
        size_t written = 0;
        while (written < response.size()) {
            ssize_t n = send(client_fd, response.data() + written,
                             response.size() - written, 0);
            if (n <= 0)
                break;
            written += static_cast<size_t>(n);
        }
    }

    GatewayConfig config_;
    IpcClient &ipc_client_;
    int listen_fd_ = -1;
    uint64_t started_at_ms_;
};

void signal_handler(int)
{
    g_should_stop.store(true);
}

void print_usage(const char *argv0)
{
    std::printf(
        "Usage: %s [--bind 0.0.0.0] [--port 8080]\n"
        "          [--ipc-service face_ctrl] [--ipc-remote-id 1]\n"
        "          [--ipc-port 110] [--ipc-priority 0] [--no-ipc] [--mock]\n",
        argv0);
}

bool parse_args(int argc, char **argv, GatewayConfig *config)
{
    if (config == nullptr)
        return false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return false;
        }

        if (arg == "--no-ipc") {
            config->enable_ipc = false;
            continue;
        }

        if (arg == "--mock") {
            config->mock_mode = true;
            continue;
        }

        auto need_value = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--bind") {
            const char *value = need_value("--bind");
            if (value == nullptr)
                return false;
            config->bind_address = value;
            continue;
        }

        if (arg == "--port") {
            const char *value = need_value("--port");
            if (value == nullptr)
                return false;
            config->http_port = static_cast<uint16_t>(std::strtoul(value, nullptr, 0));
            continue;
        }

        if (arg == "--ipc-service") {
            const char *value = need_value("--ipc-service");
            if (value == nullptr)
                return false;
            config->ipc_service = value;
            continue;
        }

        if (arg == "--ipc-remote-id") {
            const char *value = need_value("--ipc-remote-id");
            if (value == nullptr)
                return false;
            config->ipc_remote_id = static_cast<uint32_t>(std::strtoul(value, nullptr, 0));
            continue;
        }

        if (arg == "--ipc-port") {
            const char *value = need_value("--ipc-port");
            if (value == nullptr)
                return false;
            config->ipc_port = static_cast<uint32_t>(std::strtoul(value, nullptr, 0));
            continue;
        }

        if (arg == "--ipc-priority") {
            const char *value = need_value("--ipc-priority");
            if (value == nullptr)
                return false;
            config->ipc_priority = static_cast<uint32_t>(std::strtoul(value, nullptr, 0));
            continue;
        }

        std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
        return false;
    }

    return true;
}

} // namespace

int main(int argc, char **argv)
{
    GatewayConfig config;
    if (!parse_args(argc, argv, &config))
        return argc > 1 ? 1 : 0;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    IpcClient ipc_client(config);
    ipc_client.start();

    HttpServer server(config, ipc_client);
    int ret = server.run();

    ipc_client.stop();
    return ret;
}
