/* RT-Smart: AI process — face detection + recognition server over IPC (rt_channel + lwp_shm). */
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <time.h>
#include <vector>

#include <nncase/tensor.h>

#include "ipc_shm.h"
#include "ai_utils.h"
#include "face_antispoof.h"
#include "face_detection.h"
#include "face_recognition.h"
#include "setting.h"

using std::vector;

static float fas_real_thresh_from_env()
{
    const char *e = std::getenv("FACE_FAS_REAL_THRESH");
    if (!e || !e[0])
        return 0.5f;
    return static_cast<float>(std::atof(e));
}

static std::atomic<uint64_t> g_ai_ipc_recv{0};
static std::atomic<uint64_t> g_ai_reply_err_infer{0};
static std::atomic<uint64_t> g_ai_evt_send_ok{0};
static std::atomic<uint64_t> g_ai_evt_send_fail{0};

static int g_evt_ch = -1;
static uint64_t g_evt_last_try_ms = 0;

static constexpr uint64_t k_evt_reconnect_interval_ms = 1000;
static constexpr uint64_t k_evt_cooldown_ms = 3000;
static constexpr size_t k_evt_cache_slots = IPC_MAX_FACES + 4;

typedef struct
{
    bool valid;
    int id;
    uint8_t evt_kind; /* ipc_evt_kind_t */
    uint64_t last_sent_ms;
    char name[IPC_NAME_MAX];
} face_evt_cache_t;

static face_evt_cache_t g_evt_cache[k_evt_cache_slots];

static uint64_t now_monotonic_ms()
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static bool ai_metrics_enabled(int debug_mode)
{
    const char *e = std::getenv("FACE_METRICS");
    return (debug_mode > 0) || (e && e[0] == '1' && e[1] == '\0');
}

static void reset_evt_channel()
{
    if (g_evt_ch >= 0)
    {
        rt_channel_close(g_evt_ch);
        g_evt_ch = -1;
    }
}

static int ensure_evt_channel()
{
    if (g_evt_ch >= 0)
        return g_evt_ch;

    uint64_t now_ms = now_monotonic_ms();
    if (now_ms - g_evt_last_try_ms < k_evt_reconnect_interval_ms)
        return -1;

    g_evt_last_try_ms = now_ms;
    int ch = rt_channel_open(IPC_FACE_EVT_CHANNEL, 0);
    if (ch < 0)
        return -1;

    g_evt_ch = ch;
    std::cout << "face_ai: connected to " << IPC_FACE_EVT_CHANNEL << " (ch=" << g_evt_ch << ")\n";
    return g_evt_ch;
}

static int find_face_event_cache_slot(int id, const char *name, ipc_evt_kind_t evt_kind, bool *rate_limited)
{
    uint64_t now_ms = now_monotonic_ms();
    int free_idx = -1;
    int oldest_idx = 0;
    *rate_limited = false;

    for (size_t i = 0; i < k_evt_cache_slots; ++i)
    {
        if (!g_evt_cache[i].valid)
        {
            if (free_idx < 0)
                free_idx = (int)i;
            continue;
        }

        if (g_evt_cache[i].last_sent_ms < g_evt_cache[oldest_idx].last_sent_ms)
            oldest_idx = (int)i;

        if (g_evt_cache[i].id != id || g_evt_cache[i].evt_kind != (uint8_t)evt_kind)
            continue;

        if (evt_kind == IPC_EVT_KIND_RECOGNIZED)
        {
            const char *lhs = g_evt_cache[i].name;
            const char *rhs = name ? name : "";
            if (strncmp(lhs, rhs, IPC_NAME_MAX) != 0)
                continue;
        }

        if (now_ms - g_evt_cache[i].last_sent_ms < k_evt_cooldown_ms)
        {
            *rate_limited = true;
            return (int)i;
        }

        return (int)i;
    }

    return (free_idx >= 0) ? free_idx : oldest_idx;
}

static void mark_face_event_sent(int slot_idx, int id, const char *name, ipc_evt_kind_t evt_kind)
{
    uint64_t now_ms = now_monotonic_ms();
    int idx = slot_idx;
    if (idx < 0 || idx >= (int)k_evt_cache_slots)
        idx = 0;

    g_evt_cache[idx].valid = true;
    g_evt_cache[idx].id = id;
    g_evt_cache[idx].evt_kind = (uint8_t)evt_kind;
    g_evt_cache[idx].last_sent_ms = now_ms;
    memset(g_evt_cache[idx].name, 0, sizeof(g_evt_cache[idx].name));
    if (name)
        strncpy(g_evt_cache[idx].name, name, IPC_NAME_MAX - 1);
}

static void notify_face_event(int id, float score, const char *name, ipc_evt_kind_t evt_kind)
{
    int evt_ch = ensure_evt_channel();
    if (evt_ch < 0)
        return;

    bool rate_limited = false;
    int cache_slot = find_face_event_cache_slot(id, name, evt_kind, &rate_limited);
    if (rate_limited)
        return;

    void *ep = nullptr;
    int eshm = ipc_shm_alloc(sizeof(ipc_evt_t), &ep);
    if (eshm < 0 || !ep)
        return;
    ipc_evt_t *ev = (ipc_evt_t *)ep;
    memset(ev, 0, sizeof(*ev));
    ev->magic = IPC_MAGIC;
    ev->face_id = id;
    ev->score = score;
    if (name)
        strncpy(ev->name, name, IPC_NAME_MAX - 1);
    ev->is_stranger = (evt_kind == IPC_EVT_KIND_STRANGER) ? 1 : 0;
    ev->evt_kind = (uint8_t)evt_kind;
    lwp_shmdt(ep);

    struct rt_channel_msg nm;
    memset(&nm, 0, sizeof(nm));
    nm.type = RT_CHANNEL_RAW;
    nm.u.d = (void *)(intptr_t)eshm;
    if (rt_channel_send(evt_ch, &nm) != 0)
    {
        g_ai_evt_send_fail.fetch_add(1);
        ipc_shm_free(eshm);
        std::cerr << "face_ai: event send failed, will reconnect " << IPC_FACE_EVT_CHANNEL << "\n";
        reset_evt_channel();
        return;
    }

    mark_face_event_sent(cache_slot, id, name, evt_kind);
    g_ai_evt_send_ok.fetch_add(1);
}

/* 请求侧 shmid 由 face_video 在 ipc_pack_request 中创建并在 rpc_ai 的 send_recv 返回后 ipc_shm_free。
 * 本进程仅 lwp_shmat / lwp_shmdt，不对 req_shmid 调用 ipc_shm_free。 */
static void send_error_reply(int ch_ai, struct rt_channel_msg *orig_msg)
{
    struct rt_channel_msg err;
    memset(&err, 0, sizeof(err));
    err.type = RT_CHANNEL_RAW;
    err.u.d = (void *)-1;
    rt_channel_reply(ch_ai, &err);
    (void)orig_msg;
}

/* 返回带 status 的 ipc_ai_reply（应答 shmid 由 face_video 在 rpc_ai 中 ipc_shm_free） */
static void reply_status_only(int ch_ai, ipc_status_t st)
{
    if (st == IPC_STATUS_ERR_INFER)
        g_ai_reply_err_infer.fetch_add(1);

    ipc_ai_reply_t reply{};
    reply.magic = IPC_MAGIC;
    reply.status = (int32_t)st;

    void *rp = nullptr;
    int rshmid = ipc_shm_alloc(sizeof(ipc_ai_reply_t), &rp);
    if (rshmid < 0 || !rp)
    {
        send_error_reply(ch_ai, nullptr);
        return;
    }
    memcpy(rp, &reply, sizeof(reply));
    lwp_shmdt(rp);

    struct rt_channel_msg out;
    memset(&out, 0, sizeof(out));
    out.type = RT_CHANNEL_RAW;
    out.u.d = (void *)(intptr_t)rshmid;
    rt_channel_reply(ch_ai, &out);
}

int main(int argc, char **argv)
{
    std::cout << "face_ai: built " << __DATE__ << " " << __TIME__ << std::endl;

    if (argc != 8 && argc != 9)
    {
        std::cout << "Usage: face_ai <kmodel_det> <det_thres> <nms_thres> <kmodel_recg> <recg_thres> <db_dir> "
                     "<debug_mode> [<face_antispoof.kmodel>]\n";
        std::cout << "  Optional 9th arg: silent liveness kmodel; omit to disable. Liveness REAL threshold "
                     "defaults to 0.5; do not rely on export on RT-Smart msh.\n";
        return -1;
    }

    int debug_mode = atoi(argv[7]);
    const float fas_real_thresh = fas_real_thresh_from_env();

    std::unique_ptr<FaceAntiSpoof> fas;
    if (argc == 9 && argv[8] && argv[8][0] != '\0')
    {
        try
        {
            fas.reset(new FaceAntiSpoof(argv[8], debug_mode));
            std::cout << "face_ai: FaceAntiSpoof loaded: " << argv[8] << " (REAL>=" << fas_real_thresh << ")\n";
        }
        catch (const std::exception &e)
        {
            std::cerr << "face_ai: FaceAntiSpoof load failed (" << e.what() << "), running without liveness\n";
            fas.reset();
        }
    }
    FrameCHWSize image_size = {AI_FRAME_CHANNEL, AI_FRAME_HEIGHT, AI_FRAME_WIDTH};

    if (ensure_evt_channel() < 0)
    {
        std::cout << "face_ai: warn: cannot open " << IPC_FACE_EVT_CHANNEL
                  << " now, event delivery will retry automatically\n";
    }

    int ch_ai = rt_channel_open(IPC_FACE_AI_CHANNEL, O_CREAT);
    if (ch_ai < 0)
    {
        std::cout << "face_ai: cannot create channel " << IPC_FACE_AI_CHANNEL << std::endl;
        return -1;
    }

    std::cout << "face_ai: server on " << IPC_FACE_AI_CHANNEL << " (ch=" << ch_ai << ")\n";

    FaceDetection face_det(argv[1], atof(argv[2]), atof(argv[3]), image_size, debug_mode);
    char *db_dir = argv[6];
    FaceRecognition face_recg(argv[4], atoi(argv[5]), image_size, debug_mode);
    face_recg.database_init(db_dir);

    vector<FaceDetectionInfo> det_results;
    vector<FaceRecognitionInfo> rec_results;
    bool should_exit = false;

    struct rt_channel_msg msg;
    while (!should_exit)
    {
        rt_channel_recv(ch_ai, &msg);
        uint64_t recv_n = g_ai_ipc_recv.fetch_add(1) + 1;
        if (ai_metrics_enabled(debug_mode) && (recv_n % 200) == 0)
        {
            std::cerr << "[face_ai] metrics ipc_recv=" << recv_n
                      << " reply_err_infer=" << g_ai_reply_err_infer.load()
                      << " evt_send_ok=" << g_ai_evt_send_ok.load()
                      << " evt_send_fail=" << g_ai_evt_send_fail.load()
                      << " evt_connected=" << ((g_evt_ch >= 0) ? "yes" : "no") << std::endl;
        }

        int req_shmid = (int)(intptr_t)msg.u.d;
        if (req_shmid < 0)
        {
            send_error_reply(ch_ai, &msg);
            continue;
        }

        void *req_map = lwp_shmat(req_shmid, NULL);
        if (!req_map)
        {
            send_error_reply(ch_ai, &msg);
            continue;
        }

        ipc_req_hdr_t *hdr = (ipc_req_hdr_t *)req_map;
        if (hdr->magic != IPC_MAGIC)
        {
            lwp_shmdt(req_map);
            send_error_reply(ch_ai, &msg);
            continue;
        }

        ipc_ai_reply_t reply{};
        memset(&reply, 0, sizeof(reply));
        reply.magic = IPC_MAGIC;
        reply.status = IPC_STATUS_OK;

        if (hdr->cmd == IPC_CMD_DB_COUNT)
        {
            reply.count = face_recg.database_count(db_dir);
            reply.num_faces = 0;
        }
        else if (hdr->cmd == IPC_CMD_SHUTDOWN)
        {
            reply.count = 0;
            reply.num_faces = 0;
            should_exit = true;
            std::cout << "face_ai: shutdown requested\n";
        }
        else if (hdr->cmd == IPC_CMD_DB_RESET)
        {
            face_recg.database_reset(db_dir);
            reply.count = 0;
            reply.num_faces = 0;
        }
        else if (hdr->cmd == IPC_CMD_INFER || hdr->cmd == IPC_CMD_REGISTER_COMMIT)
        {
            size_t expect = (size_t)hdr->tensor_c * hdr->tensor_h * hdr->tensor_w;
            if (hdr->frame_bytes != expect || hdr->frame_bytes == 0)
            {
                lwp_shmdt(req_map);
                reply_status_only(ch_ai, IPC_STATUS_ERR_PARAM);
                continue;
            }

            uint8_t *pixels = (uint8_t *)req_map + sizeof(ipc_req_hdr_t);
            dims_t in_shape{1, hdr->tensor_c, hdr->tensor_h, hdr->tensor_w};
            FrameCHWSize fs = {static_cast<size_t>(hdr->tensor_c), static_cast<size_t>(hdr->tensor_h),
                               static_cast<size_t>(hdr->tensor_w)};

            auto rt_res = host_runtime_tensor::create(typecode_t::dt_uint8, in_shape, hrt::pool_shared);
            if (!rt_res.is_ok())
            {
                lwp_shmdt(req_map);
                reply_status_only(ch_ai, IPC_STATUS_ERR_INFER);
                std::cerr << "face_ai: create input tensor failed\n";
                continue;
            }
            runtime_tensor input_tensor = std::move(rt_res.unwrap());

            auto th_r = input_tensor.impl()->to_host();
            if (!th_r.is_ok())
            {
                lwp_shmdt(req_map);
                reply_status_only(ch_ai, IPC_STATUS_ERR_INFER);
                std::cerr << "face_ai: to_host failed\n";
                continue;
            }
            nncase::tensor host_tensor = std::move(th_r.unwrap());
            auto bh_r = host_tensor->buffer().as_host();
            if (!bh_r.is_ok())
            {
                lwp_shmdt(req_map);
                reply_status_only(ch_ai, IPC_STATUS_ERR_INFER);
                std::cerr << "face_ai: buffer as_host failed\n";
                continue;
            }
            auto as_host = std::move(bh_r.unwrap());
            auto map_r = as_host.map(map_access_::map_write);
            if (!map_r.is_ok())
            {
                lwp_shmdt(req_map);
                reply_status_only(ch_ai, IPC_STATUS_ERR_INFER);
                std::cerr << "face_ai: map write failed\n";
                continue;
            }
            auto mapped = std::move(map_r.unwrap());
            auto ref_buf = mapped.buffer();
            memcpy(reinterpret_cast<char *>(ref_buf.data()), pixels, hdr->frame_bytes);

            auto sync_r = hrt::sync(input_tensor, sync_op_t::sync_write_back, true);
            if (!sync_r.is_ok())
            {
                lwp_shmdt(req_map);
                reply_status_only(ch_ai, IPC_STATUS_ERR_INFER);
                std::cerr << "face_ai: sync write_back failed\n";
                continue;
            }

            det_results.clear();
            rec_results.clear();

            if (!face_det.pre_process(input_tensor))
            {
                lwp_shmdt(req_map);
                reply_status_only(ch_ai, IPC_STATUS_ERR_INFER);
                std::cerr << "face_ai: detection pre_process failed\n";
                continue;
            }
            if (!face_det.inference())
            {
                lwp_shmdt(req_map);
                reply_status_only(ch_ai, IPC_STATUS_ERR_INFER);
                std::cerr << "face_ai: detection inference failed\n";
                continue;
            }
            face_det.post_process(fs, det_results);

            bool rec_ok = true;
            for (size_t i = 0; i < det_results.size() && i < (size_t)IPC_MAX_FACES; ++i)
            {
                float live_real = 1.f;
                float live_spoof = 0.f;
                uint8_t is_live = 1;

                /* 识别 affine 与检测关键点同源；活体与识别共用同一张对齐脸，避免检测框裁剪与识别输入不一致。 */
                if (!face_recg.pre_process(input_tensor, det_results[i].sparse_kps.points))
                {
                    std::cerr << "face_ai: recognition pre_process failed\n";
                    rec_ok = false;
                    break;
                }

                if (fas)
                {
                    cv::Mat aligned_bgr;
                    if (!face_recg.aligned_face_to_bgr(aligned_bgr) || aligned_bgr.empty() ||
                        !fas->feed_bgr_mat(aligned_bgr) || !fas->forward() ||
                        !fas->decode_liveness_scores(&live_real, &live_spoof))
                    {
                        std::cerr << "face_ai: liveness inference failed\n";
                        rec_ok = false;
                        break;
                    }
                    is_live = (live_real >= fas_real_thresh) ? 1 : 0;
                    if (debug_mode > 1)
                    {
                        std::cout << "face_ai: liveness REAL=" << live_real << " SPOOF=" << live_spoof
                                  << " pass=" << (int)is_live << std::endl;
                    }
                }

                FaceRecognitionInfo recg_result;
                if (fas && !is_live)
                {
                    recg_result.id = -1;
                    recg_result.score = 0.f;
                    recg_result.name.clear();
                }
                else
                {
                    if (!face_recg.inference())
                    {
                        std::cerr << "face_ai: recognition inference failed\n";
                        rec_ok = false;
                        break;
                    }
                    face_recg.database_search(recg_result);
                }
                rec_results.push_back(recg_result);

                reply.faces[reply.num_faces].bbox.x = det_results[i].bbox.x;
                reply.faces[reply.num_faces].bbox.y = det_results[i].bbox.y;
                reply.faces[reply.num_faces].bbox.w = det_results[i].bbox.w;
                reply.faces[reply.num_faces].bbox.h = det_results[i].bbox.h;
                memcpy(reply.faces[reply.num_faces].sparse_kps, det_results[i].sparse_kps.points,
                       sizeof(reply.faces[reply.num_faces].sparse_kps));
                reply.faces[reply.num_faces].det_score = det_results[i].score;
                reply.faces[reply.num_faces].rec.id = recg_result.id;
                reply.faces[reply.num_faces].rec.score = recg_result.score;
                strncpy(reply.faces[reply.num_faces].rec.name, recg_result.name.c_str(), IPC_NAME_MAX - 1);
                reply.faces[reply.num_faces].rec.name[IPC_NAME_MAX - 1] = '\0';
                reply.faces[reply.num_faces].liveness_real_score = live_real;
                reply.faces[reply.num_faces].is_live = is_live;
                reply.num_faces++;

                if (hdr->cmd == IPC_CMD_INFER)
                {
                    if (fas && !is_live)
                    {
                        /*活体失败：留痕考勤/串口，便于 SD 日志与后续 MQTT 扩展 */
                        notify_face_event(-1, live_real, nullptr, IPC_EVT_KIND_LIVENESS_FAIL);
                    }
                    else if (is_live)
                    {
                        bool is_stranger = (recg_result.id == -1);
                        notify_face_event(recg_result.id, recg_result.score, recg_result.name.c_str(),
                                          is_stranger ? IPC_EVT_KIND_STRANGER : IPC_EVT_KIND_RECOGNIZED);
                    }
                }
            }
            if (!rec_ok)
            {
                lwp_shmdt(req_map);
                reply_status_only(ch_ai, IPC_STATUS_ERR_INFER);
                continue;
            }

            if (hdr->cmd == IPC_CMD_REGISTER_COMMIT)
            {
                if (det_results.size() == 1 && reply.num_faces >= 1 && reply.faces[0].is_live)
                {
                    std::string reg_name(hdr->register_name);
                    face_recg.database_add(reg_name, db_dir);
                    std::cout << "face_ai: register ok: " << reg_name << std::endl;
                }
                else if (det_results.size() == 1 && fas && reply.num_faces >= 1 && !reply.faces[0].is_live)
                {
                    std::cout << "face_ai: register rejected (liveness failed)\n";
                }
                else
                {
                    std::cout << "face_ai: register failed (need exactly one face)\n";
                }
            }
        }
        else
        {
            lwp_shmdt(req_map);
            reply_status_only(ch_ai, IPC_STATUS_ERR_BAD_CMD);
            continue;
        }

        lwp_shmdt(req_map);

        void *rp = nullptr;
        int rshmid = ipc_shm_alloc(sizeof(ipc_ai_reply_t), &rp);
        if (rshmid < 0 || !rp)
        {
            send_error_reply(ch_ai, &msg);
            continue;
        }
        memcpy(rp, &reply, sizeof(reply));
        lwp_shmdt(rp);

        struct rt_channel_msg out;
        memset(&out, 0, sizeof(out));
        out.type = RT_CHANNEL_RAW;
        out.u.d = (void *)(intptr_t)rshmid;
        rt_channel_reply(ch_ai, &out);
    }

    reset_evt_channel();
    rt_channel_close(ch_ai);
    return 0;
}
