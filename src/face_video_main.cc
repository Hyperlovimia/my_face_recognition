/* RT-Smart: video capture + display process — VICAP/VO/OSD, talks to face_ai via IPC. */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
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

namespace {

/* 与 main.cc 的单进程分支保持一致：OSD 消费 ARGB8888 字节序，
 * 且当前视频管线的三通道语义是 RGB 而非 BGR，故逐像素手工重排。 */
void convert_preview_to_osd_argb8888(const cv::Mat &src_preview, cv::Mat &dst_argb)
{
    dst_argb.create(src_preview.rows, src_preview.cols, CV_8UC4);
    for (int y = 0; y < src_preview.rows; ++y) {
        const cv::Vec3b *src_row = src_preview.ptr<cv::Vec3b>(y);
        cv::Vec4b *dst_row = dst_argb.ptr<cv::Vec4b>(y);
        for (int x = 0; x < src_preview.cols; ++x) {
            dst_row[x][0] = 255;            // A
            dst_row[x][1] = src_row[x][0];  // R
            dst_row[x][2] = src_row[x][1];  // G
            dst_row[x][3] = src_row[x][2];  // B
        }
    }
}

/* 竖屏模式下 VO 视频层走 K_ROTATION_90；注册预览保持同方向，并限制在角落。 */
void render_register_preview(cv::Mat &draw_frame, const cv::Mat &dump_img)
{
    cv::Mat oriented_preview;
    if (DISPLAY_ROTATE) {
        cv::rotate(dump_img, oriented_preview, cv::ROTATE_90_CLOCKWISE);
    } else {
        oriented_preview = dump_img;
    }

    const int preview_padding = 16;
    const int preview_max_width = std::max(1, OSD_WIDTH / 2);
    const int preview_max_height = std::max(1, OSD_HEIGHT / 2);
    const double scale_x = static_cast<double>(preview_max_width) / oriented_preview.cols;
    const double scale_y = static_cast<double>(preview_max_height) / oriented_preview.rows;
    const double scale = std::min(scale_x, scale_y);
    const int preview_width = std::max(1, static_cast<int>(oriented_preview.cols * scale));
    const int preview_height = std::max(1, static_cast<int>(oriented_preview.rows * scale));

    cv::Mat resized_preview;
    cv::resize(oriented_preview, resized_preview, cv::Size(preview_width, preview_height));

    cv::Mat resized_preview_argb;
    convert_preview_to_osd_argb8888(resized_preview, resized_preview_argb);

    const cv::Rect roi(preview_padding, preview_padding, preview_width, preview_height);
    resized_preview_argb.copyTo(draw_frame(roi));
}

}  // namespace

static std::atomic<bool> isp_stop(false);
static std::atomic<int> cur_state(0);
static std::atomic<bool> g_shutdown_ai_requested{false};
static std::mutex g_ui;
static std::string register_name;
static std::atomic<int> g_video_ctrl_ch{-1};
static std::atomic<int> g_video_reply_ch{-1};
static ipc_video_ctrl_t g_last_ctrl{};
static ipc_video_ctrl_t g_shutdown_ctrl{};
static bool g_shutdown_ctrl_valid = false;

/* 异步推理（仅 state==0）：采集线程只提交最新帧；独立线程 rpc，避免阻塞 GetFrame/VO。 */
static constexpr size_t k_ai_frame_bytes = (size_t)AI_FRAME_CHANNEL * AI_FRAME_HEIGHT * AI_FRAME_WIDTH;
static std::mutex g_ai_ch_mutex; /* 所有对 face_ai 的 rpc_ai 必须经此互斥（单通道 + 对端单线程） */
static std::mutex g_infer_pending_mu;
static std::condition_variable g_infer_cv;
static std::vector<uint8_t> g_infer_pending;
static std::atomic<uint64_t> g_infer_capture_seq{0};
static std::atomic<uint64_t> g_infer_last_displayed_seq{0};
static std::mutex g_infer_reply_mu;
static ipc_ai_reply_t g_last_infer_reply{};
static std::atomic<bool> g_infer_has_reply{false};

/* 固定推理请求槽：整段 IPC_CMD_INFER 复用同一 shmid，避免每帧 shm 分配 */
static int g_fixed_infer_shmid = -1;
static void *g_fixed_infer_ptr = nullptr;
static size_t g_fixed_infer_cap = 0;
static bool g_fixed_infer_ready = false;

static std::atomic<uint64_t> g_metric_capture_frames{0};
static std::atomic<uint64_t> g_metric_infer_rpc_ok{0};
static std::atomic<uint64_t> g_metric_infer_rpc_fail{0};
static std::atomic<uint64_t> g_metric_infer_stale{0};
static std::atomic<uint64_t> g_metric_infer_applied{0};
static constexpr int k_register_preview_hold_ms = 2000;

static bool metrics_log_enabled(int debug_mode)
{
    const char *e = std::getenv("FACE_METRICS");
    return (debug_mode > 0) || (e && e[0] == '1' && e[1] == '\0');
}

static const char *default_op_message(const ipc_ai_reply_t &reply, const char *fallback)
{
    return reply.op_message[0] ? reply.op_message : fallback;
}

static bool is_remote_ctrl(const ipc_video_ctrl_t &ctrl)
{
    return ctrl.source == IPC_VIDEO_CTRL_SRC_BRIDGE && ctrl.request_id[0] != '\0' &&
           ctrl.bridge_cmd != IPC_BRIDGE_CMD_NONE;
}

static int ensure_video_reply_channel()
{
    int ch = g_video_reply_ch.load();
    if (ch >= 0)
        return ch;

    for (int attempt = 0; attempt < 40 && !isp_stop.load(); ++attempt)
    {
        ch = rt_channel_open(IPC_FACE_VIDEO_REPLY, 0);
        if (ch >= 0)
        {
            g_video_reply_ch.store(ch);
            std::cout << "face_video: connected to " << IPC_FACE_VIDEO_REPLY << " (ch=" << ch << ")" << std::endl;
            return ch;
        }
        usleep(50 * 1000);
    }
    return -1;
}

static bool send_video_reply(const ipc_video_ctrl_t &ctrl, bool ok, int32_t count, const char *message)
{
    if (!is_remote_ctrl(ctrl))
        return true;

    int ch = ensure_video_reply_channel();
    if (ch < 0)
    {
        std::cerr << "face_video: " << IPC_FACE_VIDEO_REPLY << " not ready, drop result for " << ctrl.request_id
                  << std::endl;
        return false;
    }

    void *p = nullptr;
    int shmid = ipc_shm_alloc(sizeof(ipc_video_reply_t), &p);
    if (shmid < 0 || !p)
        return false;

    ipc_video_reply_t *reply = (ipc_video_reply_t *)p;
    memset(reply, 0, sizeof(*reply));
    reply->magic = IPC_MAGIC;
    reply->source = ctrl.source;
    reply->bridge_cmd = ctrl.bridge_cmd;
    reply->count = count;
    reply->ok = ok ? 1 : 0;
    strncpy(reply->request_id, ctrl.request_id, IPC_REQUEST_ID_MAX - 1);
    if (message)
        strncpy(reply->message, message, IPC_OP_MESSAGE_MAX - 1);
    lwp_shmdt(p);

    struct rt_channel_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = RT_CHANNEL_RAW;
    msg.u.d = (void *)(intptr_t)shmid;

    if (rt_channel_send(ch, &msg) == 0)
        return true;

    ipc_shm_free(shmid);
    rt_channel_close(ch);
    g_video_reply_ch.store(-1);
    std::cerr << "face_video: send " << IPC_FACE_VIDEO_REPLY << " failed, channel reset\n";
    return false;
}

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
        rt_err_t r = rt_channel_recv(ch, &msg);
        if (isp_stop.load())
            break;
        if (r != 0)
        {
            std::cout << "face_video: rt_channel_recv(ctrl) failed rc=" << (long)r << ", stopping\n";
            isp_stop = true;
            break;
        }

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
            {
                std::lock_guard<std::mutex> lk(g_ui);
                memcpy(&g_shutdown_ctrl, c, sizeof(*c));
                g_shutdown_ctrl_valid = true;
            }
            g_shutdown_ai_requested.store(true);
            isp_stop = true;
            lwp_shmdt(p);
            ipc_shm_free(shmid);
            break;
        }

        cur_state.store(c->state);
        {
            std::lock_guard<std::mutex> lk(g_ui);
            memcpy(&g_last_ctrl, c, sizeof(*c));
            register_name.assign(c->register_name);
        }
        g_infer_cv.notify_all();

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

/* 不持锁；调用方通过 rpc_ai_sync 或 ai_infer_worker_thread 内已持 g_ai_ch_mutex */
static bool rpc_ai_impl(int ai_ch, ipc_cmd_t cmd, const uint8_t *frame, size_t frame_len, uint32_t tc, uint32_t th,
                        uint32_t tw, const char *reg_name, ipc_ai_reply_t *out_reply, bool use_fixed_infer_req)
{
    static std::atomic<uint32_t> seq{0};
    int shmid = -1;
    bool free_req_shm = true;

    if (use_fixed_infer_req && g_fixed_infer_ready && g_fixed_infer_ptr && g_fixed_infer_shmid >= 0 &&
        cmd == IPC_CMD_INFER &&
        sizeof(ipc_req_hdr_t) + frame_len <= g_fixed_infer_cap)
    {
        if (ipc_request_encode_buffer(g_fixed_infer_ptr, g_fixed_infer_cap, cmd, frame, frame_len, tc, th, tw, reg_name,
                                      seq.fetch_add(1)) != 0)
            return false;
        shmid = g_fixed_infer_shmid;
        free_req_shm = false;
    }
    else
    {
        if (ipc_pack_request(cmd, frame, frame_len, tc, th, tw, reg_name, seq.fetch_add(1), &shmid) != 0)
            return false;
    }

    struct rt_channel_msg req, reply;
    memset(&req, 0, sizeof(req));
    memset(&reply, 0, sizeof(reply));
    req.type = RT_CHANNEL_RAW;
    req.u.d = (void *)(intptr_t)shmid;

    rt_err_t ch_err = rt_channel_send_recv(ai_ch, &req, &reply);
    if (ch_err != 0)
    {
        if (free_req_shm)
            ipc_shm_free(shmid);
        return false;
    }
    if (free_req_shm)
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

    if (out_reply->magic != IPC_MAGIC || out_reply->status != IPC_STATUS_OK)
        return false;
    return true;
}

static bool rpc_ai_sync(int ai_ch, ipc_cmd_t cmd, const uint8_t *frame, size_t frame_len, uint32_t tc, uint32_t th,
                        uint32_t tw, const char *reg_name, ipc_ai_reply_t *out_reply)
{
    std::lock_guard<std::mutex> lk(g_ai_ch_mutex);
    return rpc_ai_impl(ai_ch, cmd, frame, frame_len, tc, th, tw, reg_name, out_reply, false);
}

/* 资源：ai_ch 生命周期由 video_ipc_loop 持有；isp_stop 置位后退出。线程安全：仅本线程调用 rpc_ai_impl。 */
static void ai_infer_worker_thread(int ai_ch)
{
    std::vector<uint8_t> work(k_ai_frame_bytes);

    while (!isp_stop.load())
    {
        uint64_t snap = 0;
        {
            std::unique_lock<std::mutex> lk(g_infer_pending_mu);
            g_infer_cv.wait(lk, [&] {
                return isp_stop.load() ||
                       (cur_state.load() == 0 && g_infer_capture_seq.load() > g_infer_last_displayed_seq.load());
            });
            if (isp_stop.load())
                break;
            if (cur_state.load() != 0)
            {
                /* 非识别态：不取帧推理，重新挂起 */
                continue;
            }
            /* 稳定快照：若 memcpy 期间又有新帧，重拷直到 seq 不变，等价于只保留最新帧 */
            do
            {
                snap = g_infer_capture_seq.load();
                memcpy(work.data(), g_infer_pending.data(), k_ai_frame_bytes);
            } while (snap != g_infer_capture_seq.load());
        }

        ipc_ai_reply_t reply{};
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(g_ai_ch_mutex);
            if (isp_stop.load())
                break;
            ok = rpc_ai_impl(ai_ch, IPC_CMD_INFER, work.data(), k_ai_frame_bytes, AI_FRAME_CHANNEL, AI_FRAME_HEIGHT,
                             AI_FRAME_WIDTH, nullptr, &reply, g_fixed_infer_ready);
        }
        if (isp_stop.load())
            break;
        if (!ok)
        {
            g_metric_infer_rpc_fail.fetch_add(1);
            continue;
        }
        g_metric_infer_rpc_ok.fetch_add(1);
        /* 推理期间若采集侧已推进 seq，仅记指标但仍应用本次结果，
         * 否则在推理时间 > 采集间隔时 g_last_infer_reply 将永远得不到更新，
         * 反映在 OSD 上就是“启动后看不到检测框”。 */
        if (snap != g_infer_capture_seq.load())
        {
            g_metric_infer_stale.fetch_add(1);
        }

        {
            std::lock_guard<std::mutex> lk(g_infer_reply_mu);
            memcpy(&g_last_infer_reply, &reply, sizeof(reply));
            g_infer_has_reply.store(true);
        }
        g_infer_last_displayed_seq.store(snap);
        g_metric_infer_applied.fetch_add(1);
    }
}

static void video_ipc_loop(int debug_mode)
{
    cv::Mat draw_frame(OSD_HEIGHT, OSD_WIDTH, CV_8UC4, cv::Scalar(0, 0, 0, 0));
    DumpRes dump_res;
    std::vector<cv::Mat> sensor_bgr(3);
    cv::Mat dump_img(AI_FRAME_HEIGHT, AI_FRAME_WIDTH, CV_8UC3);
    /* display_state=2 进入后，缓存 dump_img 副本并保留 2 秒预览；超时或被 state==1/4 打断才回到 OSD 正常渲染。
     * 副本保证旋转/缩放每帧从同一源重做，避免与 main.cc 单进程分支行为偏差。 */
    int display_state = 0;
    cv::Mat register_preview_src;
    auto register_preview_deadline = std::chrono::steady_clock::time_point::min();

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

    g_infer_pending.assign(k_ai_frame_bytes, 0);
    g_infer_capture_seq.store(0);
    g_infer_last_displayed_seq.store(0);
    g_infer_has_reply.store(false);
    memset(&g_last_infer_reply, 0, sizeof(g_last_infer_reply));

    g_fixed_infer_cap = sizeof(ipc_req_hdr_t) + k_ai_frame_bytes;
    g_fixed_infer_shmid = ipc_shm_alloc(g_fixed_infer_cap, &g_fixed_infer_ptr);
    g_fixed_infer_ready = (g_fixed_infer_shmid >= 0 && g_fixed_infer_ptr != nullptr);
    if (g_fixed_infer_ready)
        std::cout << "face_video: fixed infer request shm shmid=" << g_fixed_infer_shmid << " cap=" << g_fixed_infer_cap
                  << std::endl;
    else
        std::cout << "face_video: warn: fixed infer shm failed, fallback to per-request alloc\n";

    std::thread th_infer(ai_infer_worker_thread, ai_ch);

    uint64_t loop_tick = 0;
    while (!isp_stop)
    {
        ScopedTiming st("total time", debug_mode);
        if (pl.GetFrame(dump_res) != 0)
        {
            isp_stop = true;
            break;
        }

        int state = cur_state.load();
        ipc_ai_reply_t ai_reply{};
        ipc_video_ctrl_t ctrl_snapshot{};
        memset(&ai_reply, 0, sizeof(ai_reply));
        {
            std::lock_guard<std::mutex> lk(g_ui);
            memcpy(&ctrl_snapshot, &g_last_ctrl, sizeof(ctrl_snapshot));
        }
        const auto now = std::chrono::steady_clock::now();
        const bool preview_active = !register_preview_src.empty() && now < register_preview_deadline;

        uint8_t *frame_ptr = reinterpret_cast<uint8_t *>(dump_res.virt_addr);
        size_t frame_len = (size_t)AI_FRAME_CHANNEL * AI_FRAME_HEIGHT * AI_FRAME_WIDTH;

        if (state == -1)
        {
            /* idle */
        }
        else if (state == 0)
        {
            draw_frame.setTo(cv::Scalar(0, 0, 0, 0));
            {
                std::lock_guard<std::mutex> lk(g_infer_pending_mu);
                memcpy(g_infer_pending.data(), frame_ptr, frame_len);
                g_infer_capture_seq.fetch_add(1);
                g_metric_capture_frames.fetch_add(1);
            }
            g_infer_cv.notify_one();
            memset(&ai_reply, 0, sizeof(ai_reply));
            if (g_infer_has_reply.load())
            {
                std::lock_guard<std::mutex> lk(g_infer_reply_mu);
                memcpy(&ai_reply, &g_last_infer_reply, sizeof(ai_reply));
            }
            /* 框/文案在下方 if/else 统一绘制，避免与预览叠加。 */
        }
        else if (state == 1)
        {
            ipc_ai_reply_t r{};
            bool ok = rpc_ai_sync(ai_ch, IPC_CMD_DB_COUNT, nullptr, 0, 1, 1, 1, nullptr, &r);
            if (ok)
                std::cout << "当前注册人数：" << r.count << " 人。" << std::endl;
            send_video_reply(ctrl_snapshot, ok && r.op_result != IPC_OP_RESULT_FAIL, ok ? r.count : -1,
                             ok ? default_op_message(r, "database count ready") : "database count rpc failed");
            cur_state = 0;
            display_state = 0;
        }
        else if (state == 2)
        {
            draw_frame.setTo(cv::Scalar(0, 0, 0, 0));
            dump_img.setTo(cv::Scalar(0, 0, 0));
            sensor_bgr.clear();
            uint8_t *data = reinterpret_cast<uint8_t *>(dump_res.virt_addr);
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
            bool ok =
                rpc_ai_sync(ai_ch, IPC_CMD_REGISTER_COMMIT, chw_vec.data(), chw_vec.size(), tc, th, tw,
                            name_copy.c_str(), &r);
            send_video_reply(ctrl_snapshot, ok && r.op_result == IPC_OP_RESULT_OK, 0,
                             ok ? default_op_message(r, "register processed") : "register rpc failed");
            cur_state = 0;
            display_state = 0;
        }
        /* 本帧抓拍并提交注册，避免「i」与姓名分开发送时 cur_state 被覆盖导致未抓拍 */
        else if (state == 5)
        {
            draw_frame.setTo(cv::Scalar(0, 0, 0, 0));
            dump_img.setTo(cv::Scalar(0, 0, 0));
            sensor_bgr.clear();
            uint8_t *cap = reinterpret_cast<uint8_t *>(dump_res.virt_addr);
            cv::Mat cap_R(AI_FRAME_HEIGHT, AI_FRAME_WIDTH, CV_8UC1, cap);
            cv::Mat cap_G(AI_FRAME_HEIGHT, AI_FRAME_WIDTH, CV_8UC1, cap + AI_FRAME_HEIGHT * AI_FRAME_WIDTH);
            cv::Mat cap_B(AI_FRAME_HEIGHT, AI_FRAME_WIDTH, CV_8UC1, cap + 2 * AI_FRAME_HEIGHT * AI_FRAME_WIDTH);
            sensor_bgr.push_back(cap_B);
            sensor_bgr.push_back(cap_G);
            sensor_bgr.push_back(cap_R);
            cv::merge(sensor_bgr, dump_img);

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
                std::vector<uint8_t> row = std::vector<uint8_t>(bgrChannels[i].reshape(1, 1));
                chw_vec.insert(chw_vec.end(), row.begin(), row.end());
            }
            uint32_t th = (uint32_t)dump_img.rows;
            uint32_t tw = (uint32_t)dump_img.cols;
            uint32_t tc = 3u;

            ipc_ai_reply_t r{};
            bool ok =
                rpc_ai_sync(ai_ch, IPC_CMD_REGISTER_COMMIT, chw_vec.data(), chw_vec.size(), tc, th, tw,
                            name_copy.c_str(), &r);
            send_video_reply(ctrl_snapshot, ok && r.op_result == IPC_OP_RESULT_OK, 0,
                             ok ? default_op_message(r, "register processed") : "register rpc failed");
            cur_state = 0;
            display_state = 2;
        }
        else if (state == 4)
        {
            ipc_ai_reply_t r{};
            bool ok = rpc_ai_sync(ai_ch, IPC_CMD_DB_RESET, nullptr, 0, 1, 1, 1, nullptr, &r);
            if (ok)
                std::cout << "人脸数据库已清空！" << std::endl;
            send_video_reply(ctrl_snapshot, ok && r.op_result != IPC_OP_RESULT_FAIL, 0,
                             ok ? default_op_message(r, "database reset") : "database reset rpc failed");
            cur_state = 0;
            display_state = 0;
        }

        if (display_state == 2)
        {
            /* 首次进入注册预览：缓存 dump_img 副本，设定 2 秒过期时刻；旋转/缩放每帧按源重做。 */
            register_preview_src = dump_img.clone();
            register_preview_deadline = now + std::chrono::milliseconds(k_register_preview_hold_ms);
            render_register_preview(draw_frame, register_preview_src);
        }
        else if (preview_active)
        {
            render_register_preview(draw_frame, register_preview_src);
        }
        else if (!register_preview_src.empty())
        {
            register_preview_src.release();
        }
        else
        {
            ipc_draw_faces_osd(draw_frame, &ai_reply);
        }

        pl.InsertFrame(draw_frame.data);
        pl.ReleaseFrame(dump_res);

        if (metrics_log_enabled(debug_mode) && (++loop_tick % 300) == 0)
        {
            std::cerr << "[face_video] metrics capture_frames=" << g_metric_capture_frames.load()
                      << " infer_rpc_ok=" << g_metric_infer_rpc_ok.load()
                      << " infer_rpc_fail=" << g_metric_infer_rpc_fail.load()
                      << " infer_stale=" << g_metric_infer_stale.load()
                      << " infer_applied=" << g_metric_infer_applied.load()
                      << " fixed_infer_shm=" << (g_fixed_infer_ready ? "on" : "off") << std::endl;
        }
    }

    isp_stop.store(true);
    g_infer_cv.notify_all();
    th_infer.join();

    if (g_shutdown_ai_requested.load())
    {
        ipc_ai_reply_t shutdown_reply{};
        bool ok = rpc_ai_sync(ai_ch, IPC_CMD_SHUTDOWN, nullptr, 0, 1, 1, 1, nullptr, &shutdown_reply);
        if (ok)
            std::cout << "face_video: AI shutdown acknowledged\n";
        else
            std::cout << "face_video: warn: AI shutdown request failed\n";

        ipc_video_ctrl_t shutdown_ctrl{};
        bool shutdown_ctrl_valid = false;
        {
            std::lock_guard<std::mutex> lk(g_ui);
            if (g_shutdown_ctrl_valid)
            {
                memcpy(&shutdown_ctrl, &g_shutdown_ctrl, sizeof(shutdown_ctrl));
                shutdown_ctrl_valid = true;
            }
        }
        if (shutdown_ctrl_valid)
        {
            send_video_reply(shutdown_ctrl, ok && shutdown_reply.op_result != IPC_OP_RESULT_FAIL, 0,
                             ok ? default_op_message(shutdown_reply, "shutdown acknowledged")
                                : "shutdown rpc failed");
        }
    }

    if (g_fixed_infer_ptr)
    {
        lwp_shmdt(g_fixed_infer_ptr);
        g_fixed_infer_ptr = nullptr;
    }
    if (g_fixed_infer_shmid >= 0)
    {
        ipc_shm_free(g_fixed_infer_shmid);
        g_fixed_infer_shmid = -1;
    }
    g_fixed_infer_ready = false;

    rt_channel_close(ai_ch);
    int reply_ch = g_video_reply_ch.load();
    if (reply_ch >= 0)
        rt_channel_close(reply_ch);
    g_video_reply_ch.store(-1);
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
    g_shutdown_ai_requested.store(false);
    g_video_reply_ch.store(-1);
    {
        std::lock_guard<std::mutex> lk(g_ui);
        memset(&g_last_ctrl, 0, sizeof(g_last_ctrl));
        memset(&g_shutdown_ctrl, 0, sizeof(g_shutdown_ctrl));
        g_shutdown_ctrl_valid = false;
        register_name.clear();
    }

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
