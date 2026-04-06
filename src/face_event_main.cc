/* RT-Smart: event process — stdin 业务命令发往 face_video；接收 face_ai 事件通知。 */
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <stdio.h>
#include <unistd.h>

#include "ipc_proto.h"
#include "ipc_shm.h"
#include "ipc_lwp_user.h"

static std::atomic<int> g_video_ch{-1};
static std::atomic<bool> g_evt_stop{false};

static void print_help()
{
    std::cout << "======== 操作说明 (三进程模式) ========\n";
    std::cout << "  h/help : 帮助\n";
    std::cout << "  i      : 注册（截图后输入姓名）\n";
    std::cout << "  d      : 清空数据库\n";
    std::cout << "  n      : 查询注册人数\n";
    std::cout << "  q      : 退出（结束 face_video）\n";
    std::cout << "  请在「本进程」输入命令；需已启动 face_video。\n";
    std::cout << "=======================================\n" << std::endl;
}

static void connect_video_ctrl_thread()
{
    while (!g_evt_stop.load())
    {
        int ch = rt_channel_open(IPC_FACE_VIDEO_CTRL, 0);
        if (ch >= 0)
        {
            g_video_ch.store(ch);
            std::cout << "face_event: connected to " << IPC_FACE_VIDEO_CTRL << " (ch=" << ch << ")" << std::endl;
            return;
        }
        usleep(100000);
    }
}

static bool send_video_ctrl(ipc_video_ctrl_op_t op, int32_t state, const char *reg_name)
{
    int ch = g_video_ch.load();
    if (ch < 0)
    {
        std::cout << "face_event: face_video 尚未就绪，命令已忽略（请等待连接）" << std::endl;
        return false;
    }

    void *p = nullptr;
    int shmid = ipc_shm_alloc(sizeof(ipc_video_ctrl_t), &p);
    if (shmid < 0 || !p)
        return false;

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

    constexpr int k_send_retries = 3;
    for (int attempt = 0; attempt < k_send_retries; ++attempt)
    {
        if (rt_channel_send(ch, &msg) == 0)
            return true;
        usleep(50 * 1000);
    }
    ipc_shm_free(shmid);
    std::cerr << "face_event: send_video_ctrl failed after " << k_send_retries << " retries, shm freed\n";
    return false;
}

static void evt_recv_loop(FILE *fp, int evt_ch)
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

        if (ev->is_stranger)
        {
            printf("[ALERT] stranger (score=%.2f)\n", ev->score);
            if (fp)
            {
                fprintf(fp, "[STRANGER] score=%.2f\n", ev->score);
                fflush(fp);
            }
        }
        else
        {
            printf("[EVT] id=%d name=%s score=%.2f\n", ev->face_id, ev->name, ev->score);
            if (fp)
            {
                fprintf(fp, "[OK] id=%d name=%s score=%.2f\n", ev->face_id, ev->name, ev->score);
                fflush(fp);
            }
        }

        lwp_shmdt(ev);
        ipc_shm_free(shmid);
    }
}

static void shutdown_event_process(int evt_ch, FILE *fp)
{
    g_evt_stop.store(true);

    if (evt_ch >= 0)
        rt_channel_close(evt_ch);

    int vch = g_video_ch.load();
    if (vch >= 0)
        rt_channel_close(vch);

    if (fp)
    {
        fprintf(fp, "--- face_event stopping ---\n");
        fflush(fp);
    }
}

int main(int argc, char **argv)
{
    const char *log_path = (argc >= 2) ? argv[1] : "/tmp/attendance.log";

    FILE *fp = fopen(log_path, "a");
    if (fp)
    {
        fprintf(fp, "--- face_event started ---\n");
        fflush(fp);
    }

    int evt_ch = rt_channel_open(IPC_FACE_EVT_CHANNEL, O_CREAT);
    if (evt_ch < 0)
    {
        printf("face_event: cannot create channel %s\n", IPC_FACE_EVT_CHANNEL);
        if (fp)
            fclose(fp);
        return -1;
    }
    printf("face_event: listening on %s (ch=%d), log=%s\n", IPC_FACE_EVT_CHANNEL, evt_ch, log_path);
    printf("face_event: stdin 在此输入；请先启动 face_ai，再启动 face_video。\n");

    std::thread th_conn(connect_video_ctrl_thread);
    std::thread th_evt([&]() { evt_recv_loop(fp, evt_ch); });

    print_help();
    std::cout << "输入 h/help 查看说明。\n";

    std::string last_input;
    for (;;)
    {
        std::string input;
        if (!std::getline(std::cin, input))
        {
            std::cout << "face_event: stdin 结束，正在退出...\n";
            break;
        }

        if (input == "h" || input == "help")
            print_help();
        else if (input == "i")
        {
            last_input = "i";
            send_video_ctrl(IPC_VIDEO_CTRL_OP_SET, 2, nullptr);
        }
        else if (input == "d")
            send_video_ctrl(IPC_VIDEO_CTRL_OP_SET, 4, nullptr);
        else if (input == "n")
            send_video_ctrl(IPC_VIDEO_CTRL_OP_SET, 1, nullptr);
        else if (input == "q")
        {
            usleep(100000);
            send_video_ctrl(IPC_VIDEO_CTRL_OP_QUIT, 0, nullptr);
            break;
        }
        else
        {
            if (last_input == "i")
            {
                send_video_ctrl(IPC_VIDEO_CTRL_OP_SET, 3, input.c_str());
                last_input.clear();
            }
            else
                std::cout << "请先输入 i 进入注册模式。\n";
        }
    }

    shutdown_event_process(evt_ch, fp);

    th_evt.join();
    th_conn.join();

    g_video_ch.store(-1);

    if (fp)
        fclose(fp);
    return 0;
}
