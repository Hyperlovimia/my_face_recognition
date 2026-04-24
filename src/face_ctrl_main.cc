/* RT-Smart: IPCMSG 服务 face_ctrl — 与 face_event 相同方式驱动 face_video（小核 face_gateway 经 IPC 控板） */
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#include <pthread.h>
#include <unistd.h>

#include "ipc_proto.h"
#include "ipc_shm.h"
#include "ipc_lwp_user.h"

#include "k_ipcmsg.h"
#include "k_type.h"

/* 与 src/little/src/face_gateway_protocol.h 中枚举值一致，避免 C/C++ 交叉 include 路径问题 */
#define FACE_CTRL_CMD_PING 0x1000u
#define FACE_CTRL_CMD_GET_STATUS 0x1001u
#define FACE_CTRL_CMD_GET_DB_COUNT 0x1002u
#define FACE_CTRL_CMD_DB_RESET 0x1003u
#define FACE_CTRL_CMD_REGISTER_START 0x1004u
#define FACE_CTRL_CMD_REGISTER_COMMIT 0x1005u
#define FACE_CTRL_CMD_SHUTDOWN 0x1006u

static std::atomic<int> g_video_ch{-1};
static std::atomic<bool> g_stop{false};

/** 设环境变量 FACE_DEBUG=1 时打印跨核/控板细日志（大核 msh/串口可见）。 */
static bool g_dbg = false;

static void dlog(const char *fmt, ...)
{
    if (!g_dbg)
        return;
    std::fputs("[face_ctrl] ", stdout);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

static const char *cmd_name(k_u32 cmd)
{
    switch (cmd)
    {
    case FACE_CTRL_CMD_PING:
        return "PING";
    case FACE_CTRL_CMD_GET_STATUS:
        return "GET_STATUS";
    case FACE_CTRL_CMD_GET_DB_COUNT:
        return "GET_DB_COUNT";
    case FACE_CTRL_CMD_DB_RESET:
        return "DB_RESET";
    case FACE_CTRL_CMD_REGISTER_START:
        return "REGISTER_START";
    case FACE_CTRL_CMD_REGISTER_COMMIT:
        return "REGISTER_COMMIT";
    case FACE_CTRL_CMD_SHUTDOWN:
        return "SHUTDOWN";
    default:
        return nullptr;
    }
}

static void connect_video_ctrl_thread()
{
    while (!g_stop.load())
    {
        int ch = rt_channel_open(IPC_FACE_VIDEO_CTRL, 0);
        if (ch >= 0)
        {
            g_video_ch.store(ch);
            dlog("rt_channel: connected %s (ch=%d)\n", IPC_FACE_VIDEO_CTRL, ch);
            std::cout << "face_ctrl: connected to " << IPC_FACE_VIDEO_CTRL << " (ch=" << ch << ")\n";
            return;
        }
        dlog("rt_channel: open %s failed, retry 100ms\n", IPC_FACE_VIDEO_CTRL);
        usleep(100000);
    }
}

static const char *video_op_name(ipc_video_ctrl_op_t op)
{
    switch (op)
    {
    case IPC_VIDEO_CTRL_OP_SET:
        return "SET";
    case IPC_VIDEO_CTRL_OP_QUIT:
        return "QUIT";
    default:
        return "OTHER";
    }
}

static bool send_video_ctrl(ipc_video_ctrl_op_t op, int32_t state, const char *reg_name)
{
    dlog("send_video_ctrl: op=%s(%d) state=%d reg=%s\n", video_op_name(op), (int)op, (int)state,
         reg_name ? reg_name : "(null)");

    int ch = g_video_ch.load();
    if (ch < 0)
    {
        dlog("send_video_ctrl: aborted, rt_channel to face_video not ready\n");
        std::cout << "face_ctrl: face_video not ready, command ignored\n";
        return false;
    }

    void *p = nullptr;
    int shmid = ipc_shm_alloc(sizeof(ipc_video_ctrl_t), &p);
    if (shmid < 0 || !p)
    {
        dlog("send_video_ctrl: ipc_shm_alloc failed shmid=%d p=%p\n", shmid, p);
        return false;
    }

    ipc_video_ctrl_t *c = (ipc_video_ctrl_t *)p;
    memset(c, 0, sizeof(*c));
    c->magic = IPC_MAGIC;
    c->op = (int32_t)op;
    c->state = state;
    if (reg_name)
        strncpy(c->register_name, reg_name, IPC_NAME_MAX - 1);

    lwp_shmdt(p);

    struct rt_channel_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = RT_CHANNEL_RAW;
    msg.u.d = (void *)(intptr_t)shmid;

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        if (rt_channel_send(ch, &msg) == 0)
        {
            dlog("send_video_ctrl: rt_channel_send ok shmid=%d attempt=%d\n", shmid, attempt + 1);
            return true;
        }
        dlog("send_video_ctrl: rt_channel_send fail, retry %d/3\n", attempt + 1);
        usleep(50 * 1000);
    }
    dlog("send_video_ctrl: all retries failed, free shmid=%d\n", shmid);
    ipc_shm_free(shmid);
    return false;
}

static void handle_gateway_cmd(k_u32 mod, k_u32 cmd)
{
    const char *cn = cmd_name(cmd);
    if (cn)
        dlog("IPCMSG: module=0x%x cmd=%s(0x%x)\n", mod, cn, (unsigned)cmd);
    else
        dlog("IPCMSG: module=0x%x cmd=0x%x\n", mod, (unsigned)cmd);
    (void)mod;
    switch (cmd)
    {
    case FACE_CTRL_CMD_PING:
        std::cout << "face_ctrl: PING\n";
        break;
    case FACE_CTRL_CMD_GET_STATUS:
        /* v1: 无汇总结构；仅打印。后续可经 rt_channel 拉状态。 */
        std::cout << "face_ctrl: GET_STATUS (stub)\n";
        break;
    case FACE_CTRL_CMD_GET_DB_COUNT:
        if (send_video_ctrl(IPC_VIDEO_CTRL_OP_SET, 1, nullptr))
            std::cout << "face_ctrl: GET_DB_COUNT -> video (same as 'n')\n";
        break;
    case FACE_CTRL_CMD_DB_RESET:
        if (send_video_ctrl(IPC_VIDEO_CTRL_OP_SET, 4, nullptr))
            std::cout << "face_ctrl: DB_RESET -> video (same as 'd')\n";
        break;
    case FACE_CTRL_CMD_SHUTDOWN:
        send_video_ctrl(IPC_VIDEO_CTRL_OP_QUIT, 0, nullptr);
        g_stop.store(true);
        break;
    case FACE_CTRL_CMD_REGISTER_START:
    case FACE_CTRL_CMD_REGISTER_COMMIT:
        std::cout << "face_ctrl: register flow not yet mapped from HTTP (use face_event or extend)\n";
        break;
    default:
        std::cout << "face_ctrl: unknown cmd 0x" << std::hex << cmd << std::dec << "\n";
        break;
    }
}

extern "C" void on_ipcmsg(k_s32 s32Id, k_ipcmsg_message_t *msg)
{
    (void)s32Id;
    if (msg == nullptr)
    {
        dlog("on_ipcmsg: null message\n");
        return;
    }
    /* 小核 face_gateway 对控板命令使用 send_only，大核只处理请求，不回复。 */
    if (msg->bIsResp)
    {
        dlog("on_ipcmsg: ignore response id=%d module=0x%x cmd=0x%x (send_only 路径不应出现)\n",
             (int)s32Id, (unsigned)msg->u32Module, (unsigned)msg->u32CMD);
        return;
    }
    dlog("on_ipcmsg: id=%d body_len=%u\n", (int)s32Id, (unsigned)msg->u32BodyLen);
    handle_gateway_cmd(msg->u32Module, msg->u32CMD);
}

static int run_ipcmsg_service()
{
    k_ipcmsg_connect_t attr;
    attr.u32RemoteId = 1; /* 与小核 face_gateway 默认 --ipc-remote-id 1 一致 */
    attr.u32Port = 110;   /* 与小核默认 --ipc-port 110 一致 */
    attr.u32Priority = 0;

    dlog("IPCMSG: kd_ipcmsg_add_service name=face_ctrl remoteId=%u port=%u\n", attr.u32RemoteId,
         attr.u32Port);
    if (kd_ipcmsg_add_service("face_ctrl", &attr) != K_SUCCESS)
    {
        std::cerr << "face_ctrl: kd_ipcmsg_add_service face_ctrl failed\n";
        return -1;
    }

    k_s32 id = -1;
    dlog("IPCMSG: kd_ipcmsg_connect service=face_ctrl (handler=on_ipcmsg)\n");
    if (kd_ipcmsg_connect(&id, "face_ctrl", on_ipcmsg) != K_SUCCESS)
    {
        std::cerr << "face_ctrl: kd_ipcmsg_connect face_ctrl failed\n";
        return -1;
    }

    std::cout << "face_ctrl: IPCMSG service face_ctrl, handle=" << id << " (port " << attr.u32Port
              << ")\n";
    dlog("IPCMSG: enter kd_ipcmsg_run (blocking)\n");
    kd_ipcmsg_run(id);
    kd_ipcmsg_disconnect(id);
    kd_ipcmsg_del_service("face_ctrl");
    return 0;
}

static void *ipcmsg_entry(void *arg)
{
    (void)arg;
    run_ipcmsg_service();
    return nullptr;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    {
        const char *e = std::getenv("FACE_DEBUG");
        g_dbg = (e != nullptr && e[0] == '1');
    }
    if (g_dbg)
        std::cout << "face_ctrl: FACE_DEBUG=1 (verbose log to stdout)\n";
    std::cout << "face_ctrl: start (ensure face_ai + face_video are running; optional: do not use "
                 "face_event on same control path)\n";

    std::thread thv(connect_video_ctrl_thread);
    usleep(200000);

    pthread_t t_ipc;
    if (pthread_create(&t_ipc, nullptr, ipcmsg_entry, nullptr) != 0)
    {
        std::cerr << "face_ctrl: pthread_create failed\n";
        g_stop.store(true);
        thv.join();
        return 1;
    }

    /* 阻塞到 IPC 线程退出 */
    pthread_join(t_ipc, nullptr);
    g_stop.store(true);
    thv.join();

    int v = g_video_ch.load();
    if (v >= 0)
        rt_channel_close(v);

    return 0;
}
