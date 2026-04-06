/* RT-Smart: video capture + display process — VICAP/VO/OSD, talks to face_ai via IPC. */
#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/imgproc.hpp>

#include "ipc_osd_draw.h"
#include "ipc_proto.h"
#include "ipc_shm.h"
#include "scoped_timing.h"
#include "video_pipeline.h"
#include "setting.h"

static std::atomic<bool> isp_stop(false);
static std::atomic<int> cur_state(0);
static std::mutex g_ui;
static std::string register_name;
static std::atomic<int> g_video_ctrl_ch{-1};

static void ctrl_recv_loop()
{
    int ch = rt_channel_open(IPC_FACE_VIDEO_CTRL, O_CREAT);
    if (ch < 0)
    {
        std::cout << "face_video: cannot create " << IPC_FACE_VIDEO_CTRL << std::endl;
        isp_stop = true;
        return;
    }
    g_video_ctrl_ch.store(ch);
    std::cout << "face_video: control channel " << IPC_FACE_VIDEO_CTRL << " (ch=" << ch << ")" << std::endl;

    struct rt_channel_msg msg;
    while (!isp_stop.load())
    {
        rt_channel_recv(ch, &msg);
        int shmid = (int)(intptr_t)msg.u.d;
        if (shmid < 0)
            continue;

        void *p = lwp_shmat(shmid, NULL);
        if (!p)
        {
            ipc_shm_free(shmid);
            continue;
        }

        ipc_video_ctrl_t *c = (ipc_video_ctrl_t *)p;
        if (c->magic != IPC_MAGIC)
        {
            lwp_shmdt(p);
            ipc_shm_free(shmid);
            continue;
        }

        if ((ipc_video_ctrl_op_t)c->op == IPC_VIDEO_CTRL_OP_QUIT)
        {
            isp_stop = true;
            lwp_shmdt(p);
            ipc_shm_free(shmid);
            break;
        }

        cur_state.store(c->state);
        {
            std::lock_guard<std::mutex> lk(g_ui);
            register_name.assign(c->register_name);
        }

        lwp_shmdt(p);
        ipc_shm_free(shmid);
    }
    /* channel 由 main 在 video_ipc_loop 结束后 close，以唤醒阻塞的 recv */
}

static int open_ai_channel_retry()
{
    for (int n = 0; n < 200; n++)
    {
        int ch = rt_channel_open(IPC_FACE_AI_CHANNEL, 0);
        if (ch >= 0)
            return ch;
        usleep(50000);
    }
    return -1;
}

static bool rpc_ai(int ai_ch, ipc_cmd_t cmd, const uint8_t *frame, size_t frame_len, uint32_t tc, uint32_t th,
                   uint32_t tw, const char *reg_name, ipc_ai_reply_t *out_reply)
{
    static std::atomic<uint32_t> seq{0};
    int shmid = -1;
    if (ipc_pack_request(cmd, frame, frame_len, tc, th, tw, reg_name, seq.fetch_add(1), &shmid) != 0)
        return false;

    struct rt_channel_msg req, reply;
    memset(&req, 0, sizeof(req));
    memset(&reply, 0, sizeof(reply));
    req.type = RT_CHANNEL_RAW;
    req.u.d = (void *)(intptr_t)shmid;

    rt_channel_send_recv(ai_ch, &req, &reply);
    ipc_shm_free(shmid);

    int rshmid = (int)(intptr_t)reply.u.d;
    if (rshmid < 0)
        return false;

    void *rp = lwp_shmat(rshmid, NULL);
    if (!rp)
    {
        ipc_shm_free(rshmid);
        return false;
    }
    memcpy(out_reply, rp, sizeof(ipc_ai_reply_t));
    lwp_shmdt(rp);
    ipc_shm_free(rshmid);
    return true;
}

static void video_ipc_loop(int debug_mode)
{
    cv::Mat draw_frame(OSD_HEIGHT, OSD_WIDTH, CV_8UC4, cv::Scalar(0, 0, 0, 0));
    DumpRes dump_res;
    std::vector<cv::Mat> sensor_bgr(3);
    cv::Mat dump_img(AI_FRAME_HEIGHT, AI_FRAME_WIDTH, CV_8UC3);

    PipeLine pl(debug_mode);
    if (pl.Create() != 0)
    {
        std::cout << "face_video: PipeLine create failed\n";
        isp_stop = true;
        return;
    }

    int ai_ch = open_ai_channel_retry();
    if (ai_ch < 0)
    {
        std::cout << "face_video: cannot open " << IPC_FACE_AI_CHANNEL << " (start face_ai)\n";
        pl.Destroy();
        isp_stop = true;
        return;
    }

    std::cout << "face_video: connected to AI channel " << ai_ch << std::endl;
    std::cout << "face_video: stdin 已移至 face_event，请在 face_event 终端输入命令。\n";

    while (!isp_stop)
    {
        ScopedTiming st("total time", debug_mode);
        if (pl.GetFrame(dump_res) != 0)
        {
            isp_stop = true;
            break;
        }

        int state = cur_state.load();
        int display_state = 0;
        ipc_ai_reply_t ai_reply{};
        memset(&ai_reply, 0, sizeof(ai_reply));

        uint8_t *frame_ptr = reinterpret_cast<uint8_t *>(dump_res.virt_addr);
        size_t frame_len = (size_t)AI_FRAME_CHANNEL * AI_FRAME_HEIGHT * AI_FRAME_WIDTH;

        if (state == -1)
        {
            /* idle */
        }
        else if (state == 0)
        {
            draw_frame.setTo(cv::Scalar(0, 0, 0, 0));
            if (rpc_ai(ai_ch, IPC_CMD_INFER, frame_ptr, frame_len, AI_FRAME_CHANNEL, AI_FRAME_HEIGHT,
                        AI_FRAME_WIDTH, nullptr, &ai_reply))
                ipc_draw_faces_osd(draw_frame, &ai_reply);
        }
        else if (state == 1)
        {
            ipc_ai_reply_t r{};
            if (rpc_ai(ai_ch, IPC_CMD_DB_COUNT, nullptr, 0, 1, 1, 1, nullptr, &r))
                std::cout << "当前注册人数：" << r.count << " 人。" << std::endl;
            cur_state = 0;
        }
        else if (state == 2)
        {
            draw_frame.setTo(cv::Scalar(0, 0, 0, 0));
            dump_img.setTo(cv::Scalar(0, 0, 0));
            sensor_bgr.clear();
            void *data = reinterpret_cast<void *>(dump_res.virt_addr);
            cv::Mat ori_img_R(AI_FRAME_HEIGHT, AI_FRAME_WIDTH, CV_8UC1, data);
            cv::Mat ori_img_G(AI_FRAME_HEIGHT, AI_FRAME_WIDTH, CV_8UC1, data + AI_FRAME_HEIGHT * AI_FRAME_WIDTH);
            cv::Mat ori_img_B(AI_FRAME_HEIGHT, AI_FRAME_WIDTH, CV_8UC1, data + 2 * AI_FRAME_HEIGHT * AI_FRAME_WIDTH);
            sensor_bgr.push_back(ori_img_B);
            sensor_bgr.push_back(ori_img_G);
            sensor_bgr.push_back(ori_img_R);
            cv::merge(sensor_bgr, dump_img);
            cur_state = 0;
            display_state = 2;
        }
        else if (state == 3)
        {
            std::string name_copy;
            {
                std::lock_guard<std::mutex> lk(g_ui);
                name_copy = register_name;
            }
            std::vector<uint8_t> chw_vec;
            std::vector<cv::Mat> bgrChannels(3);
            cv::split(dump_img, bgrChannels);
            for (auto i = 2; i > -1; i--)
            {
                std::vector<uint8_t> data = std::vector<uint8_t>(bgrChannels[i].reshape(1, 1));
                chw_vec.insert(chw_vec.end(), data.begin(), data.end());
            }
            uint32_t th = (uint32_t)dump_img.rows;
            uint32_t tw = (uint32_t)dump_img.cols;
            uint32_t tc = 3u;

            ipc_ai_reply_t r{};
            if (rpc_ai(ai_ch, IPC_CMD_REGISTER_COMMIT, chw_vec.data(), chw_vec.size(), tc, th, tw, name_copy.c_str(),
                       &r))
            {
                /* result printed in face_ai */
            }
            cur_state = 0;
            display_state = 0;
        }
        else if (state == 4)
        {
            ipc_ai_reply_t r{};
            if (rpc_ai(ai_ch, IPC_CMD_DB_RESET, nullptr, 0, 1, 1, 1, nullptr, &r))
                std::cout << "人脸数据库已清空！" << std::endl;
            cur_state = 0;
        }

        if (display_state == 2)
        {
            cv::cvtColor(dump_img, dump_img, cv::COLOR_BGR2BGRA);
            cv::Mat resized_dump;
            cv::resize(dump_img, resized_dump, cv::Size(OSD_WIDTH / 2, OSD_HEIGHT / 2));
            cv::Rect roi(0, 0, resized_dump.cols, resized_dump.rows);
            resized_dump.copyTo(draw_frame(roi));
        }

        pl.InsertFrame(draw_frame.data);
        pl.ReleaseFrame(dump_res);
    }

    rt_channel_close(ai_ch);
    pl.Destroy();
}

int main(int argc, char **argv)
{
    std::cout << "face_video: " << argv[0] << " built " << __DATE__ << " " << __TIME__ << std::endl;
    if (argc < 2)
    {
        std::cout << "Usage: " << argv[0] << " <debug_mode>\n";
        return -1;
    }
    int debug_mode = atoi(argv[1]);

    std::thread th_ctrl(ctrl_recv_loop);
    for (int i = 0; i < 100 && g_video_ctrl_ch.load() < 0; i++)
        usleep(10000);

    std::thread th_isp([&]() { video_ipc_loop(debug_mode); });
    th_isp.join();

    isp_stop = true;
    int vch = g_video_ctrl_ch.load();
    if (vch >= 0)
        rt_channel_close(vch);
    g_video_ctrl_ch.store(-1);
    th_ctrl.join();
    return 0;
}
