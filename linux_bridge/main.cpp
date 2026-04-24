#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "k_ipcmsg.h"
}

#include "../src/ipc_proto.h"
#include "third_party/mongoose/mongoose.h"

namespace {

constexpr const char *kSchema = "k230.face.bridge.v1";
constexpr int kIpcRemoteIdLinux = 1;
constexpr int kMqttPollMs = 200;
constexpr size_t kMaxQueueDepth = 512;

struct Config {
    std::string device_id = "k230-dev-01";
    std::string mqtt_url = "mqtt://192.168.1.10:1883";
    std::string mqtt_client_id = "face-netd-k230-dev-01";
    std::string mqtt_username;
    std::string mqtt_password;
    int heartbeat_interval_ms = 5000;
    int ipc_connect_retry_ms = 500;
    int ipc_sync_timeout_ms = 3000;
    int mqtt_keepalive_s = 15;
};

struct PublishItem {
    std::string topic;
    std::string payload;
    uint8_t qos = 1;
    bool retain = false;
};

struct PendingCmd {
    std::string request_id;
    int cmd = IPC_BRIDGE_CMD_NONE;
    std::string name;
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

    std::mutex out_mu;
    std::deque<PublishItem> outbox;

    std::mutex cmd_mu;
    std::condition_variable cmd_cv;
    std::deque<PendingCmd> cmd_queue;

    std::mutex status_mu;
    uint64_t last_status_publish_ms = 0;
};

Runtime g_rt;
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
        cfg->heartbeat_interval_ms = 5000;
    if (cfg->ipc_connect_retry_ms <= 0)
        cfg->ipc_connect_retry_ms = 500;
    if (cfg->ipc_sync_timeout_ms <= 0)
        cfg->ipc_sync_timeout_ms = 3000;
    if (cfg->mqtt_keepalive_s <= 0)
        cfg->mqtt_keepalive_s = 15;

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

void publish_reply_failure(const std::string &request_id, int cmd, const std::string &message)
{
    std::ostringstream oss;
    oss << "{\"schema\":\"" << kSchema << "\""
        << ",\"device_id\":\"" << json_escape(g_rt.cfg.device_id) << "\""
        << ",\"request_id\":\"" << json_escape(request_id) << "\""
        << ",\"cmd\":\"" << json_escape(bridge_cmd_name(cmd)) << "\""
        << ",\"ok\":false"
        << ",\"message\":\"" << json_escape(message) << "\"}";
    enqueue_publish(topic_up_reply(), oss.str(), 1, false);
}

void handle_bridge_event(const bridge_event_t &ev)
{
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
    if (result.ok && result.cmd == IPC_BRIDGE_CMD_DB_COUNT)
        g_rt.db_count.store(result.count);
    if (result.ok && result.cmd == IPC_BRIDGE_CMD_DB_RESET)
        g_rt.db_count.store(0);

    std::ostringstream oss;
    oss << "{\"schema\":\"" << kSchema << "\""
        << ",\"device_id\":\"" << json_escape(g_rt.cfg.device_id) << "\""
        << ",\"request_id\":\"" << json_escape(result.request_id) << "\""
        << ",\"cmd\":\"" << json_escape(bridge_cmd_name(result.cmd)) << "\""
        << ",\"ok\":" << (result.ok ? "true" : "false")
        << ",\"count\":" << result.count
        << ",\"message\":\"" << json_escape(result.message) << "\"}";
    enqueue_publish(topic_up_reply(), oss.str(), 1, false);

    mark_rt_seen();

    if (result.cmd == IPC_BRIDGE_CMD_SHUTDOWN && result.ok)
    {
        enqueue_status_publish(false, true);
        g_rt.shutdown_after_flush.store(true);
    }
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
    }
}

void run_thread_body(int ipc_id)
{
    g_rt.runner_alive.store(true);
    kd_ipcmsg_run(ipc_id);
    g_rt.runner_alive.store(false);
}

void ipc_supervisor_thread()
{
    k_ipcmsg_connect_t attr{};
    attr.u32RemoteId = kIpcRemoteIdLinux;
    attr.u32Port = IPC_FACE_BRIDGE_PORT;
    attr.u32Priority = 0;

    if (kd_ipcmsg_add_service(IPC_FACE_BRIDGE_SERVICE, &attr) != K_SUCCESS)
    {
        std::cerr << "face_netd: kd_ipcmsg_add_service(" << IPC_FACE_BRIDGE_SERVICE << ") failed" << std::endl;
        g_rt.stop.store(true);
        g_rt.cmd_cv.notify_all();
        return;
    }

    while (!g_rt.stop.load())
    {
        k_s32 ipc_id = -1;
        if (kd_ipcmsg_try_connect(&ipc_id, IPC_FACE_BRIDGE_SERVICE, bridge_ipc_handler) != K_SUCCESS)
        {
            sleep_ms(g_rt.cfg.ipc_connect_retry_ms);
            continue;
        }

        g_rt.ipc_id.store(ipc_id);
        g_rt.rt_connected.store(true);
        mark_rt_seen();
        std::cout << "face_netd: connected to RT bridge service=" << IPC_FACE_BRIDGE_SERVICE << " id=" << ipc_id
                  << std::endl;

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

        const int ipc_id = g_rt.ipc_id.load();
        if (ipc_id < 0 || !g_rt.rt_connected.load() || !kd_ipcmsg_is_connect(ipc_id))
        {
            publish_reply_failure(cmd.request_id, cmd.cmd, "rt bridge disconnected");
            g_rt.status_dirty.store(true);
            continue;
        }

        bridge_cmd_req_t req{};
        req.magic = IPC_MAGIC;
        req.cmd = cmd.cmd;
        strncpy(req.request_id, cmd.request_id.c_str(), sizeof(req.request_id) - 1);
        strncpy(req.name, cmd.name.c_str(), sizeof(req.name) - 1);

        k_ipcmsg_message_t *msg = kd_ipcmsg_create_message(IPC_FACE_BRIDGE_MODULE, IPC_BRIDGE_MSG_CMD_REQ, &req,
                                                           sizeof(req));
        if (!msg)
        {
            publish_reply_failure(cmd.request_id, cmd.cmd, "failed to allocate ipc message");
            continue;
        }

        k_ipcmsg_message_t *resp = nullptr;
        const k_s32 ret = kd_ipcmsg_send_sync(ipc_id, msg, &resp, g_rt.cfg.ipc_sync_timeout_ms);
        kd_ipcmsg_destroy_message(msg);

        if (ret != K_SUCCESS || !resp)
        {
            publish_reply_failure(cmd.request_id, cmd.cmd,
                                  ret == K_IPCMSG_ETIMEOUT ? "bridge ack timeout" : "bridge ack failed");
            if (resp)
                kd_ipcmsg_destroy_message(resp);
            continue;
        }

        if (resp->u32BodyLen < sizeof(bridge_cmd_ack_t) || !resp->pBody)
        {
            publish_reply_failure(cmd.request_id, cmd.cmd, "invalid bridge ack");
            kd_ipcmsg_destroy_message(resp);
            continue;
        }

        bridge_cmd_ack_t ack{};
        memcpy(&ack, resp->pBody, sizeof(ack));
        kd_ipcmsg_destroy_message(resp);

        if (ack.magic != IPC_MAGIC)
        {
            publish_reply_failure(cmd.request_id, cmd.cmd, "bridge ack magic mismatch");
            continue;
        }

        mark_rt_seen();
        if (!ack.accepted)
        {
            publish_reply_failure(cmd.request_id, cmd.cmd, ack.reason[0] ? ack.reason : "bridge rejected");
            continue;
        }

        std::cout << "face_netd: accepted command " << bridge_cmd_name(cmd.cmd) << " request_id=" << cmd.request_id
                  << std::endl;
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

    int bridge_cmd = IPC_BRIDGE_CMD_NONE;
    if (request_id_s.empty())
    {
        publish_reply_failure("", IPC_BRIDGE_CMD_NONE, "request_id is required");
        return;
    }
    if (!bridge_cmd_from_string(cmd_s, &bridge_cmd))
    {
        publish_reply_failure(request_id_s, IPC_BRIDGE_CMD_NONE, "unsupported command");
        return;
    }
    if (bridge_cmd == IPC_BRIDGE_CMD_REGISTER_CURRENT && name_s.empty())
    {
        publish_reply_failure(request_id_s, bridge_cmd, "name is required");
        return;
    }

    enqueue_command(PendingCmd{request_id_s, bridge_cmd, name_s});
}

void mqtt_event_handler(mg_connection *c, int ev, void *ev_data)
{
    switch (ev)
    {
    case MG_EV_OPEN:
        break;
    case MG_EV_ERROR:
        std::cerr << "face_netd: mqtt error: " << (ev_data ? static_cast<char *>(ev_data) : "unknown") << std::endl;
        break;
    case MG_EV_CLOSE:
        g_rt.mqtt_connected.store(false);
        if (g_mqtt == c)
            g_mqtt = nullptr;
        break;
    case MG_EV_MQTT_OPEN:
    {
        g_rt.mqtt_connected.store(true);
        g_mqtt = c;
        mg_mqtt_opts sub{};
        const std::string down_topic = topic_down_cmd();
        sub.topic = mg_str(down_topic.c_str());
        sub.qos = 1;
        mg_mqtt_sub(c, &sub);
        enqueue_status_publish(true, true);
        std::cout << "face_netd: mqtt connected " << g_rt.cfg.mqtt_url << std::endl;
        break;
    }
    case MG_EV_MQTT_MSG:
    {
        mg_mqtt_message *mm = static_cast<mg_mqtt_message *>(ev_data);
        if (!mm)
            break;
        if (mg_strcmp(mm->topic, mg_str(topic_down_cmd().c_str())) != 0)
            break;
        parse_and_enqueue_command(mm->data);
        break;
    }
    default:
        break;
    }
}

void maybe_connect_mqtt()
{
    if (g_mqtt || g_rt.stop.load())
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

    g_mqtt = mg_mqtt_connect(&g_mgr, g_rt.cfg.mqtt_url.c_str(), &opts, mqtt_event_handler, nullptr);
    if (!g_mqtt)
        std::cerr << "face_netd: mqtt connect request failed " << g_rt.cfg.mqtt_url << std::endl;
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
              << " heartbeat=" << g_rt.cfg.heartbeat_interval_ms << "ms" << std::endl;

    mg_mgr_init(&g_mgr);

    std::thread ipc_thread(ipc_supervisor_thread);
    std::thread cmd_thread(command_worker_thread);

    while (!g_rt.stop.load())
    {
        maybe_connect_mqtt();
        mg_mgr_poll(&g_mgr, kMqttPollMs);
        if (g_rt.mqtt_connected.load())
        {
            drain_publish_queue();
            if (g_rt.status_dirty.load())
                enqueue_status_publish(true, false);
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

    if (g_mqtt)
        g_mqtt->is_closing = 1;
    mg_mgr_free(&g_mgr);

    std::cout << "face_netd: stopped" << std::endl;
    return 0;
}
