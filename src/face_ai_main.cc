/* RT-Smart: AI process — face detection + recognition server over IPC (rt_channel + lwp_shm). */
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <time.h>
#include <vector>

#include <nncase/tensor.h>

#include "ipc_shm.h"
#include "ai_utils.h"
#include "face_antispoof.h"
#include "face_detection.h"
#include "face_recognition.h"
#include "perf_stats.h"
#include "setting.h"

using std::vector;

static float fas_real_thresh_from_env()
{
    const char *e = std::getenv("FACE_FAS_REAL_THRESH");
    if (!e || !e[0])
        /* 略低于 0.5：静默活体单帧分数对真人往往更「抖」，默认 0.5 容易误拒；假体若 REAL 长期很低仍易区分 */
        return 0.32f;
    return static_cast<float>(std::atof(e));
}

/* 按人脸槽位对 REAL 概率做 EMA，减轻真人瞬时低分；人脸消失后对应槽位会重置 */
static float g_fas_real_ema[IPC_MAX_FACES];
static uint8_t g_fas_real_ema_inited[IPC_MAX_FACES];

/* REAL 概率 EMA：raw 高于当前 EMA 时用较大 α 快速跟上（减少「明明这一帧不错仍判假体」）；
 * raw 低于 EMA 时用较小 α 缓降，避免单帧毛刺把结果打穿，同时抑制偶发低分拖尾过长可以用 alpha_dn 微调。 */
static constexpr float k_fas_ema_alpha_up = 0.62f;
static constexpr float k_fas_ema_alpha_dn = 0.35f;
/* 注册 commit 单帧：eff REAL 允许比流式阈值低一截（仍设下限，避免明显低分入库） */
static constexpr float k_fas_register_relax = 0.065f;
static constexpr float k_fas_register_real_floor = 0.06f;

static float fas_register_eff_floor(float fas_real_thresh)
{
    return (std::max)(k_fas_register_real_floor, fas_real_thresh - k_fas_register_relax);
}

/* 库内≥2 人时 Top1-Top2 分差门槛；过高易与特征 EMA 叠加导致频繁 unknown；可用环境变量 FACE_DB_TOP2_MARGIN 覆盖 */
static float db_top2_margin_from_env()
{
    const char *e = std::getenv("FACE_DB_TOP2_MARGIN");
    if (!e || !e[0])
        return 5.0f;
    return static_cast<float>(std::atof(e));
}

/* 识别特征短时 EMA（按检测槽位）：减弱单帧噪声导致「更像错的人」；注册 commit 仍用单帧检索 */
static std::vector<float> g_rec_feat_ema[IPC_MAX_FACES];
static uint8_t g_rec_feat_inited[IPC_MAX_FACES]{};
static constexpr float k_rec_feat_ema_alpha = 0.34f;
static constexpr float k_rec_feat_ema_alpha_cap = 0.48f;

/** 按检测分数与框面积排序，稳定槽位与 EMA 曲线（多人时减少错位） */
static void sort_det_results_by_quality(vector<FaceDetectionInfo> &dets)
{
    std::sort(dets.begin(), dets.end(), [](const FaceDetectionInfo &a, const FaceDetectionInfo &b) {
        if (a.score != b.score)
            return a.score > b.score;
        const float aa = a.bbox.w * a.bbox.h;
        const float bb = b.bbox.w * b.bbox.h;
        return aa > bb;
    });
}

/* 识别阈值滞回：上一帧已确认身份且本帧 top1 仍为同一人时，允许在略低于阈值的分数上保持识别，减轻抖动。
 * 默认带宽约 5（与 score 同标尺），可用环境变量 FACE_REC_HYST 覆盖，建议范围约 2～12。多人同帧不启用。 */
static int g_rec_sticky_id[IPC_MAX_FACES];
static char g_rec_sticky_name[IPC_MAX_FACES][IPC_NAME_MAX];
static uint8_t g_rec_sticky_valid[IPC_MAX_FACES];

static float rec_hysteresis_band_from_env()
{
    const char *e = std::getenv("FACE_REC_HYST");
    float v = (e && e[0]) ? static_cast<float>(std::atof(e)) : 5.0f;
    if (v < 2.0f)
        v = 2.0f;
    if (v > 12.0f)
        v = 12.0f;
    return v;
}

static void apply_recognition_hysteresis(int slot_i, int frame_face_count, const FaceRecognition &fr,
                                         FaceRecognitionInfo &r)
{
    if (frame_face_count != 1)
        return;

    if (r.ambiguous_match)
    {
        g_rec_sticky_valid[slot_i] = 0;
        return;
    }

    const float t = fr.recognition_threshold();
    const float band = rec_hysteresis_band_from_env();
    const float relax = t - band;

    if (r.id >= 0)
    {
        g_rec_sticky_valid[slot_i] = 1;
        g_rec_sticky_id[slot_i] = r.id;
        memset(g_rec_sticky_name[slot_i], 0, sizeof(g_rec_sticky_name[slot_i]));
        if (!r.name.empty())
            strncpy(g_rec_sticky_name[slot_i], r.name.c_str(), IPC_NAME_MAX - 1);
        return;
    }

    if (r.top1_id < 0 || fr.registered_face_count() <= 0)
    {
        g_rec_sticky_valid[slot_i] = 0;
        return;
    }

    if (!g_rec_sticky_valid[slot_i])
        return;

    if (r.top1_id != g_rec_sticky_id[slot_i])
    {
        g_rec_sticky_valid[slot_i] = 0;
        return;
    }

    if (r.score >= relax)
    {
        r.id = r.top1_id;
        const std::string nm = fr.registered_name_at(r.top1_id);
        if (!nm.empty())
            r.name = nm;
        else if (g_rec_sticky_name[slot_i][0] != '\0')
            r.name = g_rec_sticky_name[slot_i];
    }
    else
        g_rec_sticky_valid[slot_i] = 0;
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

static uint64_t now_realtime_ms()
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static bool ai_metrics_enabled(int debug_mode)
{
    const char *e = std::getenv("FACE_METRICS");
    return (debug_mode > 0) || (e && e[0] == '1' && e[1] == '\0');
}

struct FixedInputTensor
{
    runtime_tensor tensor;
    size_t bytes = 0;
    bool ready = false;
};

static PerfStageStats g_perf_shmat_req("face_ai.shmat_req");
static PerfStageStats g_perf_prepare_input_tensor("face_ai.prepare_input_tensor");
static PerfStageStats g_perf_det_pre("face_ai.det_pre");
static PerfStageStats g_perf_det_infer("face_ai.det_infer");
static PerfStageStats g_perf_det_post("face_ai.det_post");
static PerfStageStats g_perf_rec_pre("face_ai.rec_pre");
static PerfStageStats g_perf_rec_infer("face_ai.rec_infer");
static PerfStageStats g_perf_rec_search("face_ai.rec_search");
static PerfStageStats g_perf_reply_send("face_ai.reply_send");

static bool init_input_tensor(uint32_t tensor_c, uint32_t tensor_h, uint32_t tensor_w, runtime_tensor *out_tensor)
{
    if (!out_tensor)
        return false;

    dims_t in_shape{1, tensor_c, tensor_h, tensor_w};
    auto rt_res = host_runtime_tensor::create(typecode_t::dt_uint8, in_shape, hrt::pool_shared);
    if (!rt_res.is_ok())
        return false;

    *out_tensor = std::move(rt_res.unwrap());
    return true;
}

static bool copy_pixels_to_tensor(runtime_tensor &tensor, const uint8_t *pixels, size_t bytes)
{
    auto map_r = hrt::map(tensor, map_access_::map_write);
    if (!map_r.is_ok())
        return false;
    auto mapped = std::move(map_r.unwrap());
    auto ref_buf = mapped.buffer();
    if (ref_buf.size_bytes() < bytes)
    {
        auto ignore_unmap_r = mapped.unmap();
        (void)ignore_unmap_r;
        return false;
    }
    memcpy(reinterpret_cast<char *>(ref_buf.data()), pixels, bytes);
    auto unmap_r = mapped.unmap();
    if (!unmap_r.is_ok())
        return false;

    auto sync_r = hrt::sync(tensor, sync_op_t::sync_write_back, true);
    return sync_r.is_ok();
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
    ev->ts_ms = now_realtime_ms();
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

static void set_reply_op(ipc_ai_reply_t *reply, ipc_op_result_t result, const char *message)
{
    if (!reply)
        return;
    reply->op_result = (int32_t)result;
    memset(reply->op_message, 0, sizeof(reply->op_message));
    if (message)
        strncpy(reply->op_message, message, sizeof(reply->op_message) - 1);
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

    if (argc != 8 && argc != 9 && argc != 10 && argc != 11)
    {
        std::cout << "Usage: face_ai <kmodel_det> <det_thres> <nms_thres> <kmodel_recg> <recg_thres> <db_dir> "
                     "<debug_mode> [<face_antispoof.kmodel> [<real_prob_threshold> [real0|idx0]]]\n";
        std::cout << "  Optional 9th: silent liveness kmodel; omit to disable.\n";
        std::cout << "  Optional 10th: REAL 概率阈值 (需同时带 9th)；默认 0.32 或环境变量 FACE_FAS_REAL_THRESH。\n";
        std::cout << "  Optional 11th: 仅当 kmodel 约定 out[0]=REAL、out[1]=SPOOF 时写 real0 或 idx0（多数模型无需）。\n";
        std::cout << "  活体启用时对 REAL 做短时平滑(EMA)，减轻真人单帧误拒；板端勿依赖 export。\n";
        return -1;
    }

    int debug_mode = atoi(argv[7]);
    float fas_real_thresh = fas_real_thresh_from_env();
    if (argc >= 10 && argv[9] && argv[9][0] != '\0')
        fas_real_thresh = static_cast<float>(std::atof(argv[9]));

    bool fas_real_idx0 = false;
    if (argc >= 11 && argv[10] && argv[10][0] != '\0')
    {
        if (std::strcmp(argv[10], "real0") == 0 || std::strcmp(argv[10], "idx0") == 0)
            fas_real_idx0 = true;
        else
        {
            std::cerr << "face_ai: 11th arg must be real0 or idx0 (out[0]=REAL), got: " << argv[10] << std::endl;
            return -1;
        }
    }

    std::unique_ptr<FaceAntiSpoof> fas;
    /* argc==9: 仅活体 kmodel；argc==10/11: kmodel + 阈值（+ 可选 real0） */
    if (argc >= 9 && argv[8] && argv[8][0] != '\0')
    {
        try
        {
            fas.reset(new FaceAntiSpoof(argv[8], debug_mode, fas_real_idx0));
            std::cout << "face_ai: FaceAntiSpoof loaded: " << argv[8] << " (REAL>=" << fas_real_thresh
                      << ", EMA up/dn=" << k_fas_ema_alpha_up << "/" << k_fas_ema_alpha_dn
                      << ", real_out_idx0=" << (fas_real_idx0 ? "yes" : "no") << ")\n";
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
    const float db_top2_margin = db_top2_margin_from_env();
    FaceRecognition face_recg(argv[4], static_cast<float>(atof(argv[5])), image_size, debug_mode, db_top2_margin);
    face_recg.database_init(db_dir);
    FixedInputTensor fixed_input{};
    fixed_input.bytes = (size_t)AI_FRAME_CHANNEL * (size_t)AI_FRAME_HEIGHT * (size_t)AI_FRAME_WIDTH;
    fixed_input.ready = init_input_tensor(AI_FRAME_CHANNEL, AI_FRAME_HEIGHT, AI_FRAME_WIDTH, &fixed_input.tensor);
    if (fixed_input.ready)
        std::cout << "face_ai: fixed input tensor ready for " << AI_FRAME_CHANNEL << "x" << AI_FRAME_HEIGHT << "x"
                  << AI_FRAME_WIDTH << " bytes=" << fixed_input.bytes << "\n";
    else
        std::cout << "face_ai: warn: fixed input tensor init failed, using dynamic input tensor fallback\n";
    std::cout << "face_ai: recognition db_top2_margin=" << db_top2_margin
              << " (env FACE_DB_TOP2_MARGIN overrides default)\n"
              << "face_ai: recognition hysteresis band=" << rec_hysteresis_band_from_env()
              << " (env FACE_REC_HYST, 2..12, eases threshold chatter for single-face frames)\n";

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

        const bool perf_enabled = ai_metrics_enabled(debug_mode);
        const auto shmat_start = std::chrono::steady_clock::now();
        void *req_map = lwp_shmat(req_shmid, NULL);
        g_perf_shmat_req.track_since(shmat_start, perf_enabled);
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
        set_reply_op(&reply, IPC_OP_RESULT_NONE, nullptr);

        if (hdr->cmd == IPC_CMD_DB_COUNT)
        {
            reply.count = face_recg.database_count(db_dir);
            reply.num_faces = 0;
            set_reply_op(&reply, IPC_OP_RESULT_OK, "database count ready");
        }
        else if (hdr->cmd == IPC_CMD_SHUTDOWN)
        {
            reply.count = 0;
            reply.num_faces = 0;
            should_exit = true;
            set_reply_op(&reply, IPC_OP_RESULT_OK, "shutdown acknowledged");
            std::cout << "face_ai: shutdown requested\n";
        }
        else if (hdr->cmd == IPC_CMD_DB_RESET)
        {
            face_recg.database_reset(db_dir);
            reply.count = 0;
            reply.num_faces = 0;
            set_reply_op(&reply, IPC_OP_RESULT_OK, "database reset");
        }
        else if (hdr->cmd == IPC_CMD_INFER || hdr->cmd == IPC_CMD_REGISTER_COMMIT || hdr->cmd == IPC_CMD_IMPORT_IMAGE)
        {
            size_t expect = (size_t)hdr->tensor_c * hdr->tensor_h * hdr->tensor_w;
            if (hdr->frame_bytes != expect || hdr->frame_bytes == 0)
            {
                lwp_shmdt(req_map);
                reply_status_only(ch_ai, IPC_STATUS_ERR_PARAM);
                continue;
            }

            uint8_t *pixels = (uint8_t *)req_map + sizeof(ipc_req_hdr_t);
            FrameCHWSize fs = {static_cast<size_t>(hdr->tensor_c), static_cast<size_t>(hdr->tensor_h),
                               static_cast<size_t>(hdr->tensor_w)};
            runtime_tensor input_tensor;
            const bool use_fixed_input = fixed_input.ready && hdr->tensor_c == AI_FRAME_CHANNEL &&
                                         hdr->tensor_h == AI_FRAME_HEIGHT && hdr->tensor_w == AI_FRAME_WIDTH &&
                                         hdr->frame_bytes == fixed_input.bytes;
            const auto prep_start = std::chrono::steady_clock::now();
            if (use_fixed_input)
            {
                input_tensor = fixed_input.tensor;
            }
            else if (!init_input_tensor(hdr->tensor_c, hdr->tensor_h, hdr->tensor_w, &input_tensor))
            {
                lwp_shmdt(req_map);
                reply_status_only(ch_ai, IPC_STATUS_ERR_INFER);
                std::cerr << "face_ai: create input tensor failed\n";
                continue;
            }

            if (!copy_pixels_to_tensor(input_tensor, pixels, hdr->frame_bytes))
            {
                lwp_shmdt(req_map);
                reply_status_only(ch_ai, IPC_STATUS_ERR_INFER);
                std::cerr << "face_ai: prepare input tensor failed\n";
                continue;
            }
            g_perf_prepare_input_tensor.track_since(prep_start, perf_enabled);

            det_results.clear();
            rec_results.clear();

            const auto det_pre_start = std::chrono::steady_clock::now();
            if (!face_det.pre_process(input_tensor))
            {
                lwp_shmdt(req_map);
                reply_status_only(ch_ai, IPC_STATUS_ERR_INFER);
                std::cerr << "face_ai: detection pre_process failed\n";
                continue;
            }
            g_perf_det_pre.track_since(det_pre_start, perf_enabled);
            const auto det_infer_start = std::chrono::steady_clock::now();
            if (!face_det.inference())
            {
                lwp_shmdt(req_map);
                reply_status_only(ch_ai, IPC_STATUS_ERR_INFER);
                std::cerr << "face_ai: detection inference failed\n";
                continue;
            }
            g_perf_det_infer.track_since(det_infer_start, perf_enabled);
            const auto det_post_start = std::chrono::steady_clock::now();
            face_det.post_process(fs, det_results);
            g_perf_det_post.track_since(det_post_start, perf_enabled);
            sort_det_results_by_quality(det_results);
            if (det_results.size() > 1u)
            {
                for (int z = 0; z < IPC_MAX_FACES; ++z)
                    g_rec_sticky_valid[z] = 0;
            }

            for (size_t j = det_results.size(); j < (size_t)IPC_MAX_FACES; ++j)
            {
                g_fas_real_ema_inited[j] = 0;
                g_rec_feat_inited[j] = 0;
                g_rec_sticky_valid[j] = 0;
            }

            bool rec_ok = true;
            for (size_t i = 0; i < det_results.size() && i < (size_t)IPC_MAX_FACES; ++i)
            {
                float live_real = 1.f;
                float live_spoof = 0.f;
                uint8_t is_live = 1;

                const float det_thresh_cfg = face_det.det_conf_thresh();
                const float det_weak_floor = (std::max)(0.24f, det_thresh_cfg * 0.68f);
                const bool det_weak =
                    (hdr->cmd == IPC_CMD_INFER) && (det_results[i].score < det_weak_floor);
                const bool suppress_infer_evt = det_weak;

                FaceRecognitionInfo recg_result;

                if (det_weak)
                {
                    g_rec_sticky_valid[i] = 0;
                    /* 极低置信检测：对齐/活体易受噪声干扰，跳过 KPU 链路与事件，仅保留框用于 OSD */
                    recg_result.id = -1;
                    recg_result.score = 0.f;
                    recg_result.name.clear();
                }
                else
                {
                    /* 识别 affine 与检测关键点同源；活体与识别共用同一张对齐脸，避免检测框裁剪与识别输入不一致。 */
                    const auto rec_pre_start = std::chrono::steady_clock::now();
                    if (!face_recg.pre_process(input_tensor, det_results[i].sparse_kps.points))
                    {
                        std::cerr << "face_ai: recognition pre_process failed\n";
                        rec_ok = false;
                        break;
                    }
                    g_perf_rec_pre.track_since(rec_pre_start, perf_enabled);

                    float fas_ema_alpha_up = k_fas_ema_alpha_up;
                    if (det_results[i].score >= 0.52f)
                        fas_ema_alpha_up += 0.04f;
                    fas_ema_alpha_up = (std::min)(fas_ema_alpha_up, 0.72f);

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
                        const float raw_real = live_real;
                        if (!g_fas_real_ema_inited[i])
                        {
                            g_fas_real_ema[i] = raw_real;
                            g_fas_real_ema_inited[i] = 1;
                        }
                        else
                        {
                            const float ema_prev = g_fas_real_ema[i];
                            const float a =
                                (raw_real >= ema_prev) ? fas_ema_alpha_up : k_fas_ema_alpha_dn;
                            g_fas_real_ema[i] = a * raw_real + (1.f - a) * ema_prev;
                        }
                        const float ema_real = g_fas_real_ema[i];
                        const float eff_real = (std::max)(ema_real, raw_real);
                        live_real = eff_real;
                        is_live = (eff_real >= fas_real_thresh) ? 1 : 0;
                        if (debug_mode > 1)
                        {
                            std::cout << "face_ai: liveness REAL_raw=" << raw_real << " REAL_ema=" << ema_real
                                      << " REAL_eff=" << eff_real << " SPOOF=" << live_spoof
                                      << " pass=" << (int)is_live << std::endl;
                        }
                    }

                    const bool skip_recg_infer =
                        fas && !is_live && hdr->cmd != IPC_CMD_IMPORT_IMAGE &&
                        !(hdr->cmd == IPC_CMD_REGISTER_COMMIT &&
                          live_real >= fas_register_eff_floor(fas_real_thresh));

                    if (skip_recg_infer)
                    {
                        recg_result.id = -1;
                        recg_result.score = 0.f;
                        recg_result.name.clear();
                    }
                    else
                    {
                        const auto rec_infer_start = std::chrono::steady_clock::now();
                        if (!face_recg.inference())
                        {
                            std::cerr << "face_ai: recognition inference failed\n";
                            rec_ok = false;
                            break;
                        }
                        g_perf_rec_infer.track_since(rec_infer_start, perf_enabled);
                        if (hdr->cmd == IPC_CMD_INFER)
                        {
                            const auto rec_search_start = std::chrono::steady_clock::now();
                            const int fd = face_recg.feature_dim();
                            const int nfaces = (int)det_results.size();
                            const bool multi_face = nfaces > 1;
                            if (fd > 0)
                            {
                                std::vector<float> feat_now((size_t)fd);
                                face_recg.export_query_l2_normalized(feat_now.data());
                                /* 多人同帧：禁用槽位 EMA，直接单帧检索，避免两人特征在槽位间平均后互串 */
                                if (multi_face)
                                {
                                    face_recg.database_search(recg_result, feat_now.data(), nfaces);
                                }
                                else
                                {
                                    float rec_feat_ema_alpha = k_rec_feat_ema_alpha;
                                    rec_feat_ema_alpha += 0.05f;
                                    if (fas && is_live)
                                    {
                                        if (live_real >= 0.38f)
                                            rec_feat_ema_alpha += 0.05f;
                                        if (live_real >= 0.52f)
                                            rec_feat_ema_alpha += 0.04f;
                                    }
                                    rec_feat_ema_alpha = (std::min)(rec_feat_ema_alpha, k_rec_feat_ema_alpha_cap);
                                    if (!g_rec_feat_inited[i])
                                    {
                                        g_rec_feat_ema[i] = std::move(feat_now);
                                        g_rec_feat_inited[i] = 1;
                                    }
                                    else
                                    {
                                        if ((int)g_rec_feat_ema[i].size() != fd)
                                        {
                                            g_rec_feat_ema[i] = std::move(feat_now);
                                        }
                                        else
                                        {
                                            for (int k = 0; k < fd; ++k)
                                            {
                                                const size_t z = (size_t)k;
                                                g_rec_feat_ema[i][z] = rec_feat_ema_alpha * feat_now[z] +
                                                                       (1.f - rec_feat_ema_alpha) * g_rec_feat_ema[i][z];
                                            }
                                            float sum = 0.f;
                                            for (int k = 0; k < fd; ++k)
                                            {
                                                const size_t z = (size_t)k;
                                                sum += g_rec_feat_ema[i][z] * g_rec_feat_ema[i][z];
                                            }
                                            sum = sqrtf(sum);
                                            if (sum > 1e-6f)
                                            {
                                                for (int k = 0; k < fd; ++k)
                                                    g_rec_feat_ema[i][(size_t)k] /= sum;
                                            }
                                        }
                                    }
                                    face_recg.database_search(recg_result, g_rec_feat_ema[i].data(), 1);
                                }
                            }
                            else
                                face_recg.database_search(recg_result, nullptr, nfaces);

                            apply_recognition_hysteresis((int)i, nfaces, face_recg, recg_result);
                            g_perf_rec_search.track_since(rec_search_start, perf_enabled);
                        }
                        else
                        {
                            const auto rec_search_start = std::chrono::steady_clock::now();
                            face_recg.database_search(recg_result, nullptr, (int)det_results.size());
                            g_perf_rec_search.track_since(rec_search_start, perf_enabled);
                        }
                    }
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

                if (hdr->cmd == IPC_CMD_INFER && !suppress_infer_evt)
                {
                    const bool is_stranger = (recg_result.id == -1);
                    /* 多人同框时次要人脸常为侧脸/远小目标，相似度不稳易刷陌生人；主脸（排序后 slot0）仍上报 */
                    const bool skip_stranger_evt_secondary =
                        is_stranger && det_results.size() > 1 && i > 0;

                    if (fas && !is_live)
                    {
                        /*活体失败：留痕考勤/串口，便于 SD 日志与后续 MQTT 扩展 */
                        notify_face_event(-1, live_real, nullptr, IPC_EVT_KIND_LIVENESS_FAIL);
                    }
                    else if (is_live && !skip_stranger_evt_secondary)
                    {
                        notify_face_event(recg_result.id, recg_result.score, recg_result.name.c_str(),
                                          is_stranger ? IPC_EVT_KIND_STRANGER : IPC_EVT_KIND_RECOGNIZED);
                    }
                    else if (is_live && skip_stranger_evt_secondary)
                    {
                        /* 次要脸仍可能已识别：放行 recognized，仅压陌生人噪声 */
                        if (!is_stranger)
                            notify_face_event(recg_result.id, recg_result.score, recg_result.name.c_str(),
                                              IPC_EVT_KIND_RECOGNIZED);
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
                const bool reg_ok_live =
                    !fas || (reply.num_faces >= 1 && reply.faces[0].liveness_real_score >=
                                                     fas_register_eff_floor(fas_real_thresh));
                if (det_results.size() == 1 && reply.num_faces >= 1 && reg_ok_live)
                {
                    const FaceRecognitionInfo &reg_result = rec_results.front();
                    if (reg_result.id >= 0)
                    {
                        std::string reject_msg = "register rejected: face already registered";
                        if (!reg_result.name.empty() && reg_result.name != "unknown")
                            reject_msg += " (" + reg_result.name + ")";
                        set_reply_op(&reply, IPC_OP_RESULT_FAIL, reject_msg.c_str());
                        std::cout << "face_ai: register rejected (already registered): "
                                  << (reg_result.name.empty() ? "<unnamed>" : reg_result.name) << std::endl;
                    }
                    else
                    {
                        std::string reg_name(hdr->register_name);
                        cv::Mat snap_landscape;
                        {
                            const int hh = static_cast<int>(hdr->tensor_h);
                            const int ww = static_cast<int>(hdr->tensor_w);
                            const size_t plane = static_cast<size_t>(hh) * static_cast<size_t>(ww);
                            cv::Mat mr(hh, ww, CV_8UC1, pixels + 0);
                            cv::Mat mg(hh, ww, CV_8UC1, pixels + plane);
                            cv::Mat mb(hh, ww, CV_8UC1, pixels + 2 * plane);
                            cv::merge(std::vector<cv::Mat>{mb, mg, mr}, snap_landscape);
                        }
                        face_recg.database_add(reg_name, db_dir, snap_landscape);
                        set_reply_op(&reply, IPC_OP_RESULT_OK, "register ok");
                        std::cout << "face_ai: register ok: " << reg_name << std::endl;
                    }
                }
                else if (det_results.size() == 1 && fas && reply.num_faces >= 1 && !reg_ok_live)
                {
                    set_reply_op(&reply, IPC_OP_RESULT_FAIL, "register rejected: liveness failed");
                    std::cout << "face_ai: register rejected (liveness failed)\n";
                }
                else
                {
                    set_reply_op(&reply, IPC_OP_RESULT_FAIL, "register failed: need exactly one face");
                    std::cout << "face_ai: register failed (need exactly one face)\n";
                }
            }
            else if (hdr->cmd == IPC_CMD_IMPORT_IMAGE)
            {
                if (det_results.size() == 1 && reply.num_faces >= 1)
                {
                    std::string reg_name(hdr->register_name);
                    cv::Mat import_bgr;
                    {
                        const int hh = static_cast<int>(hdr->tensor_h);
                        const int ww = static_cast<int>(hdr->tensor_w);
                        const size_t plane = static_cast<size_t>(hh) * static_cast<size_t>(ww);
                        cv::Mat mr(hh, ww, CV_8UC1, pixels + 0);
                        cv::Mat mg(hh, ww, CV_8UC1, pixels + plane);
                        cv::Mat mb(hh, ww, CV_8UC1, pixels + 2 * plane);
                        cv::merge(std::vector<cv::Mat>{mb, mg, mr}, import_bgr);
                    }
                    face_recg.database_add_import(reg_name, db_dir, import_bgr);
                    reply.count = face_recg.database_count(db_dir);
                    set_reply_op(&reply, IPC_OP_RESULT_OK, "import ok");
                    std::cout << "face_ai: import ok: " << reg_name << std::endl;
                }
                else if (det_results.empty())
                {
                    reply.count = face_recg.database_count(db_dir);
                    set_reply_op(&reply, IPC_OP_RESULT_FAIL, "import failed: need exactly one face");
                    std::cout << "face_ai: import failed (no face)\n";
                }
                else
                {
                    reply.count = face_recg.database_count(db_dir);
                    set_reply_op(&reply, IPC_OP_RESULT_FAIL, "import failed: need exactly one face");
                    std::cout << "face_ai: import failed (multiple faces)\n";
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

        const auto reply_send_start = std::chrono::steady_clock::now();
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
        g_perf_reply_send.track_since(reply_send_start, perf_enabled);
    }

    reset_evt_channel();
    rt_channel_close(ch_ai);
    return 0;
}
