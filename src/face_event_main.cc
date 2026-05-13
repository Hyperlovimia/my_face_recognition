/* RT-Smart: event process — stdin 业务命令发往 face_video；接收 face_ai 事件通知，并桥接到小核 Linux。 */
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <deque>
#include <string>
#include <thread>

#include <poll.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "attendance_log.h"
#include "door_control.h"
#include "ipc_proto.h"
#include "ipc_shm.h"
#include "ipc_lwp_user.h"
#include "k_ipcmsg.h"

static_assert(IPC_BRIDGE_CMD_REGISTER_PREVIEW == 5, "wire value must match linux_bridge register_preview");

static std::atomic<int> g_video_ch{-1};
static std::atomic<int> g_bridge_id{-1};
static std::atomic<bool> g_evt_stop{false};
static std::atomic<bool> g_bridge_connected{false};
static std::mutex g_bridge_send_mu;
static std::mutex g_bridge_inflight_mu;

typedef struct
{
    bool active;
    int32_t cmd;
    char request_id[IPC_REQUEST_ID_MAX];
} bridge_inflight_t;

static constexpr size_t k_bridge_pending_cap = 32;
static bridge_inflight_t g_bridge_inflight{};
static std::deque<bridge_cmd_req_t> g_bridge_pending;
static DoorControl g_door_control;

static void copy_cstr(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
        return;
    memset(dst, 0, dst_size);
    if (src)
        strncpy(dst, src, dst_size - 1);
}

static uint64_t now_realtime_ms()
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static const char *bridge_cmd_name(int32_t cmd)
{
    switch ((ipc_bridge_cmd_t)cmd)
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
    case IPC_BRIDGE_CMD_SHUTDOWN:
        return "shutdown";
    default:
        return "unknown";
    }
}

static const char *evt_kind_name(uint8_t evt_kind)
{
    switch ((ipc_evt_kind_t)evt_kind)
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

static int bridge_cmd_to_state(ipc_bridge_cmd_t cmd)
{
    switch (cmd)
    {
    case IPC_BRIDGE_CMD_DB_COUNT:
        return 1;
    case IPC_BRIDGE_CMD_DB_RESET:
        return 4;
    case IPC_BRIDGE_CMD_REGISTER_CURRENT:
        return 5;
    case IPC_BRIDGE_CMD_REGISTER_PREVIEW:
        return 2;
    case IPC_BRIDGE_CMD_REGISTER_COMMIT:
        return 3;
    case IPC_BRIDGE_CMD_REGISTER_CANCEL:
        return 6;
    default:
        return 0;
    }
}

static bool is_supported_bridge_cmd(int32_t c)
{
    switch (c)
    {
    case IPC_BRIDGE_CMD_DB_COUNT:
    case IPC_BRIDGE_CMD_DB_RESET:
    case IPC_BRIDGE_CMD_REGISTER_CURRENT:
    case IPC_BRIDGE_CMD_SHUTDOWN:
    case IPC_BRIDGE_CMD_REGISTER_PREVIEW:
    case IPC_BRIDGE_CMD_REGISTER_COMMIT:
    case IPC_BRIDGE_CMD_REGISTER_CANCEL:
        return true;
    default:
        return false;
    }
}

static void print_help()
{
    std::cout << "======== 操作说明 (三进程模式) ========\n";
    std::cout << "  h/help : 帮助\n";
    std::cout << "  i      : 现场注册 — 抓拍当前画面，下一行输入姓名\n";
    std::cout << "  i 姓名 : 一键注册 — 本帧画面直接入库（陌生人可先对准入镜）\n";
    std::cout << "  d      : 清空数据库\n";
    std::cout << "  n      : 查询注册人数\n";
    std::cout << "  q      : 退出（结束 face_video）\n";
    std::cout << "  请在「本进程」输入命令；需已启动 face_video。\n";
    std::cout << "  单串口场景建议：先后台启动 face_ai / face_video，再前台启动 face_event。\n";
    std::cout << "  考勤日志目录默认：/sd/face_logs（根目录下按日文件 YYYY-MM-DD.jsonl，结构化 JSONL）；\n";
    std::cout << "  覆盖：启动参数传入根目录，或环境变量 FACE_ATT_LOG_BASE / FACE_ATT_LOG。\n";
    std::cout << "=======================================\n" << std::endl;
}

static void connect_video_ctrl_thread()
{
    while (!g_evt_stop.load())
    {
        if (g_video_ch.load() >= 0)
        {
            usleep(200000);
            continue;
        }

        int ch = rt_channel_open(IPC_FACE_VIDEO_CTRL, 0);
        if (ch >= 0)
        {
            g_video_ch.store(ch);
            std::cout << "face_event: connected to " << IPC_FACE_VIDEO_CTRL << " (ch=" << ch << ")" << std::endl;
            continue;
        }
        usleep(100000);
    }
}

static bool send_video_ctrl(ipc_video_ctrl_op_t op, int32_t state, const char *reg_name,
                            ipc_video_ctrl_source_t source, ipc_bridge_cmd_t bridge_cmd, const char *request_id,
                            char *reason, size_t reason_cap)
{
    int ch = g_video_ch.load();
    if (ch < 0)
    {
        copy_cstr(reason, reason_cap, "face_video not ready");
        std::cout << "face_event: face_video 尚未就绪，命令已忽略（请等待连接）" << std::endl;
        return false;
    }

    void *p = nullptr;
    int shmid = ipc_shm_alloc(sizeof(ipc_video_ctrl_t), &p);
    if (shmid < 0 || !p)
    {
        copy_cstr(reason, reason_cap, "ipc shm alloc failed");
        return false;
    }

    ipc_video_ctrl_t *c = (ipc_video_ctrl_t *)p;
    memset(c, 0, sizeof(*c));
    c->magic = IPC_MAGIC;
    c->op = (int32_t)op;
    c->state = state;
    c->source = (int32_t)source;
    c->bridge_cmd = (int32_t)bridge_cmd;
    if (reg_name)
        strncpy(c->register_name, reg_name, IPC_NAME_MAX - 1);
    if (request_id)
        strncpy(c->request_id, request_id, IPC_REQUEST_ID_MAX - 1);
    lwp_shmdt(p);

    struct rt_channel_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = RT_CHANNEL_RAW;
    msg.u.d = (void *)(intptr_t)shmid;

    constexpr int k_send_retries = 3;
    for (int attempt = 0; attempt < k_send_retries; ++attempt)
    {
        if (rt_channel_send(ch, &msg) == 0)
            return true;
        usleep(50 * 1000);
    }

    ipc_shm_free(shmid);
    rt_channel_close(ch);
    g_video_ch.store(-1);
    copy_cstr(reason, reason_cap, "send_video_ctrl failed");
    std::cerr << "face_event: send_video_ctrl failed after " << k_send_retries << " retries, shm freed\n";
    return false;
}

static void clear_bridge_inflight_locked()
{
    g_bridge_inflight.active = false;
    g_bridge_inflight.cmd = IPC_BRIDGE_CMD_NONE;
    memset(g_bridge_inflight.request_id, 0, sizeof(g_bridge_inflight.request_id));
}

static bool send_bridge_payload(uint32_t msg_kind, const void *body, uint32_t body_len)
{
    int bridge_id = g_bridge_id.load();
    if (bridge_id < 0 || !g_bridge_connected.load())
        return false;

    std::lock_guard<std::mutex> lk(g_bridge_send_mu);
    if (g_bridge_id.load() < 0 || !g_bridge_connected.load())
        return false;

    k_ipcmsg_message_t *msg = kd_ipcmsg_create_message(IPC_FACE_BRIDGE_MODULE, msg_kind, body, body_len);
    if (!msg)
        return false;

    k_s32 ret = kd_ipcmsg_send_only(bridge_id, msg);
    kd_ipcmsg_destroy_message(msg);
    if (ret != 0)
    {
        std::cerr << "face_event: send bridge payload failed kind=" << msg_kind << " ret=" << ret << std::endl;
        return false;
    }
    return true;
}

static void bridge_publish_cmd_failed(const bridge_cmd_req_t &req, const char *fail_msg)
{
    bridge_cmd_result_t out{};
    out.magic = IPC_MAGIC;
    out.cmd = req.cmd;
    out.count = -1;
    out.ok = 0;
    copy_cstr(out.request_id, sizeof(out.request_id), req.request_id);
    copy_cstr(out.message, sizeof(out.message), fail_msg ? fail_msg : "dispatch_failed");
    send_bridge_payload(IPC_BRIDGE_MSG_CMD_RESULT, &out, sizeof(out));
}

/** 持锁 g_bridge_inflight_mu：上一命令结束后拉起队列里的下一条（发往 face_video）。 */
static void bridge_dispatch_pending_unlocked()
{
    while (!g_bridge_pending.empty() && !g_bridge_inflight.active)
    {
        const bridge_cmd_req_t req = g_bridge_pending.front();
        g_bridge_pending.pop_front();

        const ipc_bridge_cmd_t bridge_cmd = (ipc_bridge_cmd_t)req.cmd;
        const ipc_video_ctrl_op_t op =
            (bridge_cmd == IPC_BRIDGE_CMD_SHUTDOWN) ? IPC_VIDEO_CTRL_OP_QUIT : IPC_VIDEO_CTRL_OP_SET;
        const int32_t state = bridge_cmd_to_state(bridge_cmd);

        char vreason[IPC_OP_MESSAGE_MAX] = {0};
        if (!send_video_ctrl(op, state, req.name, IPC_VIDEO_CTRL_SRC_BRIDGE, bridge_cmd, req.request_id, vreason,
                               sizeof(vreason)))
        {
            std::cerr << "face_event: bridge queued dispatch failed cmd=" << bridge_cmd_name(req.cmd)
                      << " request_id=" << req.request_id << " reason=" << vreason << std::endl;
            bridge_publish_cmd_failed(req, vreason[0] ? vreason : "send_video_ctrl_failed");
            continue;
        }

        g_bridge_inflight.active = true;
        g_bridge_inflight.cmd = req.cmd;
        copy_cstr(g_bridge_inflight.request_id, sizeof(g_bridge_inflight.request_id), req.request_id);
        std::cout << "face_event: bridge dispatch queued cmd=" << bridge_cmd_name(req.cmd)
                  << " request_id=" << req.request_id << std::endl;
        break;
    }
}

static void clear_bridge_inflight_if_match(const char *request_id)
{
    std::lock_guard<std::mutex> lk(g_bridge_inflight_mu);
    if (!g_bridge_inflight.active)
    {
        bridge_dispatch_pending_unlocked();
        return;
    }
    if (!request_id || request_id[0] == '\0' ||
        strncmp(g_bridge_inflight.request_id, request_id, sizeof(g_bridge_inflight.request_id)) == 0)
    {
        clear_bridge_inflight_locked();
        bridge_dispatch_pending_unlocked();
    }
}

static void forward_bridge_event(const ipc_evt_t *ev)
{
    if (!ev || ev->magic != IPC_MAGIC)
        return;

    bridge_event_t out{};
    out.magic = IPC_MAGIC;
    out.evt_kind = ev->evt_kind;
    out.face_id = ev->face_id;
    out.score = ev->score;
    out.ts_ms = ev->ts_ms ? ev->ts_ms : now_realtime_ms();
    copy_cstr(out.name, sizeof(out.name), ev->name);
    send_bridge_payload(IPC_BRIDGE_MSG_EVENT, &out, sizeof(out));
}

static void forward_bridge_result(const ipc_video_reply_t *reply)
{
    if (!reply || reply->magic != IPC_MAGIC)
        return;

    bridge_cmd_result_t out{};
    out.magic = IPC_MAGIC;
    out.cmd = reply->bridge_cmd;
    out.count = reply->count;
    out.ok = reply->ok;
    copy_cstr(out.request_id, sizeof(out.request_id), reply->request_id);
    copy_cstr(out.message, sizeof(out.message), reply->message);

    send_bridge_payload(IPC_BRIDGE_MSG_CMD_RESULT, &out, sizeof(out));
    clear_bridge_inflight_if_match(reply->request_id);

    if (reply->bridge_cmd == IPC_BRIDGE_CMD_SHUTDOWN)
        g_evt_stop.store(true);
}

static void reply_bridge_ack(k_s32 s32Id, k_ipcmsg_message_t *req_msg, bool accepted, const char *reason)
{
    bridge_cmd_ack_t ack{};
    ack.magic = IPC_MAGIC;
    ack.accepted = accepted ? 1 : 0;
    copy_cstr(ack.reason, sizeof(ack.reason), reason);

    k_ipcmsg_message_t *resp = kd_ipcmsg_create_resp_message(req_msg, accepted ? 0 : -1, &ack, sizeof(ack));
    if (!resp)
        return;
    kd_ipcmsg_send_async(s32Id, resp, NULL);
    kd_ipcmsg_destroy_message(resp);
}

static void bridge_handle_message(k_s32 s32Id, k_ipcmsg_message_t *msg)
{
    g_bridge_id.store(s32Id);

    if (!msg)
        return;

    if (msg->u32CMD != IPC_BRIDGE_MSG_CMD_REQ || msg->u32BodyLen < sizeof(bridge_cmd_req_t) || !msg->pBody)
    {
        reply_bridge_ack(s32Id, msg, false, "invalid bridge request");
        return;
    }

    bridge_cmd_req_t req{};
    memcpy(&req, msg->pBody, sizeof(req));
    if (req.magic != IPC_MAGIC || req.request_id[0] == '\0')
    {
        reply_bridge_ack(s32Id, msg, false, "invalid request payload");
        return;
    }

    int32_t raw_cmd = req.cmd;
    if (!is_supported_bridge_cmd(raw_cmd))
    {
        char detail[IPC_OP_MESSAGE_MAX];
        snprintf(detail, sizeof(detail), "unsupported command (cmd=%d) redeploy face_event.elf+ipc_proto", (int)raw_cmd);
        reply_bridge_ack(s32Id, msg, false, detail);
        return;
    }

    const ipc_bridge_cmd_t bridge_cmd = (ipc_bridge_cmd_t)raw_cmd;

    if (bridge_cmd == IPC_BRIDGE_CMD_REGISTER_CURRENT && req.name[0] == '\0')
    {
        reply_bridge_ack(s32Id, msg, false, "name is required");
        return;
    }
    if (bridge_cmd == IPC_BRIDGE_CMD_REGISTER_COMMIT && req.name[0] == '\0')
    {
        reply_bridge_ack(s32Id, msg, false, "name is required");
        return;
    }

    char reject_reason[IPC_OP_MESSAGE_MAX] = {0};
    char ack_note[IPC_OP_MESSAGE_MAX] = {0};
    bool ack_ok = false;

    {
        std::lock_guard<std::mutex> lk(g_bridge_inflight_mu);
        if (g_bridge_inflight.active)
        {
            if (g_bridge_pending.size() >= k_bridge_pending_cap)
            {
                copy_cstr(reject_reason, sizeof(reject_reason), "queue_overflow");
                ack_ok = false;
            }
            else
            {
                g_bridge_pending.push_back(req);
                ack_ok = true;
                copy_cstr(ack_note, sizeof(ack_note), "queued");
            }
        }
        else
        {
            ipc_video_ctrl_op_t op = (bridge_cmd == IPC_BRIDGE_CMD_SHUTDOWN) ? IPC_VIDEO_CTRL_OP_QUIT
                                                                             : IPC_VIDEO_CTRL_OP_SET;
            int32_t state = bridge_cmd_to_state(bridge_cmd);
            if (send_video_ctrl(op, state, req.name, IPC_VIDEO_CTRL_SRC_BRIDGE, bridge_cmd, req.request_id,
                                reject_reason, sizeof(reject_reason)))
            {
                ack_ok = true;
                g_bridge_inflight.active = true;
                g_bridge_inflight.cmd = bridge_cmd;
                copy_cstr(g_bridge_inflight.request_id, sizeof(g_bridge_inflight.request_id), req.request_id);
                ack_note[0] = '\0';
            }
            else
                ack_ok = false;
        }
    }

    if (ack_ok)
    {
        std::cout << "face_event: bridge accepted " << bridge_cmd_name(bridge_cmd) << " request_id=" << req.request_id
                  << (ack_note[0] ? " (queued)" : "") << std::endl;
        reply_bridge_ack(s32Id, msg, true, ack_note[0] ? ack_note : "");
    }
    else
    {
        std::cout << "face_event: bridge rejected " << bridge_cmd_name(bridge_cmd) << " request_id=" << req.request_id
                  << " reason=" << reject_reason << std::endl;
        reply_bridge_ack(s32Id, msg, false, reject_reason[0] ? reject_reason : "rejected");
    }
}

static void bridge_service_loop()
{
    k_ipcmsg_connect_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.u32RemoteId = 0;
    attr.u32Port = IPC_FACE_BRIDGE_PORT;
    attr.u32Priority = 0;

    k_s32 add_ret = kd_ipcmsg_add_service(IPC_FACE_BRIDGE_SERVICE, &attr);
    if (add_ret != 0)
    {
        std::cerr << "face_event: kd_ipcmsg_add_service(" << IPC_FACE_BRIDGE_SERVICE << ") failed ret=" << add_ret
                  << std::endl;
        return;
    }

    while (!g_evt_stop.load())
    {
        k_s32 bridge_id = -1;
        if (kd_ipcmsg_try_connect(&bridge_id, IPC_FACE_BRIDGE_SERVICE, bridge_handle_message) != 0)
        {
            usleep(200 * 1000);
            continue;
        }

        g_bridge_id.store(bridge_id);
        g_bridge_connected.store(true);
        std::cout << "face_event: bridge connected service=" << IPC_FACE_BRIDGE_SERVICE << " port="
                  << IPC_FACE_BRIDGE_PORT << " id=" << bridge_id << std::endl;

        kd_ipcmsg_run(bridge_id);

        kd_ipcmsg_disconnect(bridge_id);
        g_bridge_connected.store(false);
        g_bridge_id.store(-1);
        std::cout << "face_event: bridge disconnected, waiting for reconnect\n";
        usleep(200 * 1000);
    }

    int bridge_id = g_bridge_id.load();
    if (bridge_id >= 0)
        kd_ipcmsg_disconnect(bridge_id);
    g_bridge_connected.store(false);
    g_bridge_id.store(-1);
    kd_ipcmsg_del_service(IPC_FACE_BRIDGE_SERVICE);
}

static void evt_recv_loop(const std::string &log_base, int evt_ch)
{
    struct rt_channel_msg msg;
    while (!g_evt_stop.load())
    {
        rt_err_t r = rt_channel_recv(evt_ch, &msg);
        if (g_evt_stop.load())
            break;
        if (r != 0)
            break;

        int shmid = (int)(intptr_t)msg.u.d;
        if (shmid < 0)
            continue;

        ipc_evt_t *ev = (ipc_evt_t *)lwp_shmat(shmid, NULL);
        if (!ev || ev->magic != IPC_MAGIC)
        {
            if (ev)
                lwp_shmdt(ev);
            ipc_shm_free(shmid);
            continue;
        }

        if (ev->evt_kind == IPC_EVT_KIND_LIVENESS_FAIL)
        {
            printf("[ALERT] spoof / liveness failed (REAL score=%.3f)\n", ev->score);
            attendance_log_append_ipc_evt(log_base, ev, nullptr);
        }
        else if (ev->evt_kind == IPC_EVT_KIND_STRANGER || ev->is_stranger)
        {
            printf("[ALERT] 未识别 stranger (score=%.2f) — 可输入 i 或 i <姓名> 现场注册\n", ev->score);
            attendance_log_append_ipc_evt(log_base, ev, nullptr);
        }
        else
        {
            printf("[EVT] id=%d name=%s score=%.2f\n", ev->face_id, ev->name, ev->score);
            attendance_log_append_ipc_evt(log_base, ev, nullptr);
        }

        g_door_control.handle_ipc_event(*ev);

        std::cout << "face_event: bridge event " << evt_kind_name(ev->evt_kind) << " score=" << ev->score
                  << " name=" << ev->name << std::endl;
        forward_bridge_event(ev);

        lwp_shmdt(ev);
        ipc_shm_free(shmid);
    }
}

static void video_reply_recv_loop(int reply_ch)
{
    struct rt_channel_msg msg;
    while (!g_evt_stop.load())
    {
        rt_err_t r = rt_channel_recv(reply_ch, &msg);
        if (g_evt_stop.load())
            break;
        if (r != 0)
            break;

        int shmid = (int)(intptr_t)msg.u.d;
        if (shmid < 0)
            continue;

        ipc_video_reply_t *reply = (ipc_video_reply_t *)lwp_shmat(shmid, NULL);
        if (!reply || reply->magic != IPC_MAGIC)
        {
            if (reply)
                lwp_shmdt(reply);
            ipc_shm_free(shmid);
            continue;
        }

        std::cout << "face_event: bridge result cmd=" << bridge_cmd_name(reply->bridge_cmd)
                  << " request_id=" << reply->request_id << " ok=" << (int)reply->ok
                  << " message=" << reply->message << std::endl;
        try
        {
            forward_bridge_result(reply);
        }
        catch (const std::exception &e)
        {
            std::cerr << "face_event: forward_bridge_result: " << e.what() << std::endl;
            clear_bridge_inflight_if_match(reply->request_id);
        }
        catch (...)
        {
            std::cerr << "face_event: forward_bridge_result: unknown exception\n";
            clear_bridge_inflight_if_match(reply->request_id);
        }

        lwp_shmdt(reply);
        ipc_shm_free(shmid);
    }
}

static void stdin_loop()
{
    bool awaiting_register_name = false;

    print_help();
    std::cout << "输入 h/help 查看说明。\n";

    while (!g_evt_stop.load())
    {
        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;

        int poll_ret = poll(&pfd, 1, 200);
        if (g_evt_stop.load())
            break;
        if (poll_ret < 0)
        {
            continue;
        }
        if (poll_ret == 0)
            continue;

        if (!(pfd.revents & (POLLIN | POLLERR | POLLHUP)))
            continue;

        std::string input;
        if (!std::getline(std::cin, input))
        {
            std::cout << "face_event: stdin 结束，正在退出...\n";
            g_evt_stop.store(true);
            break;
        }

        if (input == "h" || input == "help")
        {
            awaiting_register_name = false;
            print_help();
        }
        else if (input == "i")
        {
            char reason[IPC_OP_MESSAGE_MAX] = {0};
            awaiting_register_name = true;
            if (!send_video_ctrl(IPC_VIDEO_CTRL_OP_SET, 2, nullptr, IPC_VIDEO_CTRL_SRC_LOCAL, IPC_BRIDGE_CMD_NONE,
                                 nullptr, reason, sizeof(reason)))
            {
                awaiting_register_name = false;
            }
        }
        else if (input.size() >= 2 && input[0] == 'i' && (input[1] == ' ' || input[1] == '\t'))
        {
            std::string rest = input.substr(2);
            while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t'))
                rest.erase(0, 1);
            if (rest.empty())
            {
                std::cout << "用法: i <姓名> — 使用当前画面一键注册\n";
            }
            else
            {
                char reason[IPC_OP_MESSAGE_MAX] = {0};
                awaiting_register_name = false;
                send_video_ctrl(IPC_VIDEO_CTRL_OP_SET, 5, rest.c_str(), IPC_VIDEO_CTRL_SRC_LOCAL, IPC_BRIDGE_CMD_NONE,
                                nullptr, reason, sizeof(reason));
            }
        }
        else if (input == "d")
        {
            char reason[IPC_OP_MESSAGE_MAX] = {0};
            awaiting_register_name = false;
            send_video_ctrl(IPC_VIDEO_CTRL_OP_SET, 4, nullptr, IPC_VIDEO_CTRL_SRC_LOCAL, IPC_BRIDGE_CMD_NONE, nullptr,
                            reason, sizeof(reason));
        }
        else if (input == "n")
        {
            char reason[IPC_OP_MESSAGE_MAX] = {0};
            awaiting_register_name = false;
            send_video_ctrl(IPC_VIDEO_CTRL_OP_SET, 1, nullptr, IPC_VIDEO_CTRL_SRC_LOCAL, IPC_BRIDGE_CMD_NONE, nullptr,
                            reason, sizeof(reason));
        }
        else if (input == "q")
        {
            char reason[IPC_OP_MESSAGE_MAX] = {0};
            usleep(100000);
            awaiting_register_name = false;
            send_video_ctrl(IPC_VIDEO_CTRL_OP_QUIT, 0, nullptr, IPC_VIDEO_CTRL_SRC_LOCAL, IPC_BRIDGE_CMD_SHUTDOWN,
                            nullptr, reason, sizeof(reason));
            g_evt_stop.store(true);
            break;
        }
        else if (awaiting_register_name)
        {
            if (input.empty())
            {
                std::cout << "请输入姓名后再回车，或输入其他命令取消当前注册。\n";
                continue;
            }

            char reason[IPC_OP_MESSAGE_MAX] = {0};
            send_video_ctrl(IPC_VIDEO_CTRL_OP_SET, 3, input.c_str(), IPC_VIDEO_CTRL_SRC_LOCAL, IPC_BRIDGE_CMD_NONE,
                            nullptr, reason, sizeof(reason));
            awaiting_register_name = false;
        }
        else if (!input.empty())
        {
            std::cout << "陌生人现场注册: 输入 i（抓拍后再输姓名），或 i <姓名> 一键注册。\n";
        }
    }
}

static void shutdown_event_process(int evt_ch, int reply_ch, const std::string &log_base)
{
    g_evt_stop.store(true);
    g_door_control.shutdown();

    auto wake_channel = [](int ch) {
        if (ch < 0)
            return;
        struct rt_channel_msg wake_msg;
        memset(&wake_msg, 0, sizeof(wake_msg));
        wake_msg.type = RT_CHANNEL_RAW;
        wake_msg.u.d = (void *)-1;
        rt_channel_send(ch, &wake_msg);
    };

    wake_channel(evt_ch);
    wake_channel(reply_ch);

    if (evt_ch >= 0)
        rt_channel_close(evt_ch);
    if (reply_ch >= 0)
        rt_channel_close(reply_ch);

    int vch = g_video_ch.exchange(-1);
    if (vch >= 0)
        rt_channel_close(vch);

    int bridge_id = g_bridge_id.exchange(-1);
    if (bridge_id >= 0)
        kd_ipcmsg_disconnect(bridge_id);
    g_bridge_connected.store(false);

    attendance_log_append_meta(log_base, "face_event_stopping", nullptr);
}

int main(int argc, char **argv)
{
    const std::string log_base = attendance_log_resolve_base_dir(argc, argv);
    attendance_log_append_meta(log_base, "face_event_started", nullptr);
    if (!g_door_control.init(log_base))
        std::cout << "face_event: door control init finished in state=" << g_door_control.state_name() << std::endl;

    int evt_ch = rt_channel_open(IPC_FACE_EVT_CHANNEL, O_CREAT);
    if (evt_ch < 0)
    {
        printf("face_event: cannot create channel %s\n", IPC_FACE_EVT_CHANNEL);
        return -1;
    }

    int reply_ch = rt_channel_open(IPC_FACE_VIDEO_REPLY, O_CREAT);
    if (reply_ch < 0)
    {
        printf("face_event: cannot create channel %s\n", IPC_FACE_VIDEO_REPLY);
        rt_channel_close(evt_ch);
        return -1;
    }

    printf("face_event: listening on %s (ch=%d), attendance_dir=%s\n", IPC_FACE_EVT_CHANNEL, evt_ch, log_base.c_str());
    printf("face_event: listening on %s (ch=%d)\n", IPC_FACE_VIDEO_REPLY, reply_ch);
    printf("face_event: stdin 在此输入；单串口建议先后台启动 face_ai、face_video，再前台启动本进程。\n");
    printf("face_event: bridge service %s port=%u ready for little-core face_netd\n", IPC_FACE_BRIDGE_SERVICE,
           IPC_FACE_BRIDGE_PORT);

    std::thread th_conn(connect_video_ctrl_thread);
    std::thread th_evt([&]() { evt_recv_loop(log_base, evt_ch); });
    std::thread th_reply([&]() { video_reply_recv_loop(reply_ch); });
    std::thread th_bridge(bridge_service_loop);
    std::thread th_stdin(stdin_loop);

    while (!g_evt_stop.load())
        usleep(100 * 1000);

    shutdown_event_process(evt_ch, reply_ch, log_base);

    th_stdin.join();
    th_evt.join();
    th_reply.join();
    th_conn.join();
    th_bridge.join();

    return 0;
}
