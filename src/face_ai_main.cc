/* RT-Smart: AI process — face detection + recognition server over IPC (rt_channel + lwp_shm). */
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include <nncase/tensor.h>

#include "ipc_shm.h"
#include "ai_utils.h"
#include "face_detection.h"
#include "face_recognition.h"
#include "setting.h"

using std::vector;

static std::atomic<uint64_t> g_ai_ipc_recv{0};
static std::atomic<uint64_t> g_ai_reply_err_infer{0};

static bool ai_metrics_enabled(int debug_mode)
{
    const char *e = std::getenv("FACE_METRICS");
    return (debug_mode > 0) || (e && e[0] == '1' && e[1] == '\0');
}

static void notify_stranger(int evt_ch, int id, float score, const char *name)
{
    if (evt_ch < 0)
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
    ev->is_stranger = (id == -1) ? 1 : 0;
    lwp_shmdt(ep);

    struct rt_channel_msg nm;
    memset(&nm, 0, sizeof(nm));
    nm.type = RT_CHANNEL_RAW;
    nm.u.d = (void *)(intptr_t)eshm;
    if (rt_channel_send(evt_ch, &nm) != 0)
    {
        ipc_shm_free(eshm);
        std::cerr << "face_ai: notify_stranger rt_channel_send failed, shm freed\n";
    }
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

    if (argc != 8)
    {
        std::cout << "Usage: face_ai <kmodel_det> <det_thres> <nms_thres> <kmodel_recg> <recg_thres> <db_dir> <debug_mode>\n";
        return -1;
    }

    int debug_mode = atoi(argv[7]);
    FrameCHWSize image_size = {AI_FRAME_CHANNEL, AI_FRAME_HEIGHT, AI_FRAME_WIDTH};

    int evt_ch = rt_channel_open(IPC_FACE_EVT_CHANNEL, 0);
    if (evt_ch < 0)
        std::cout << "face_ai: warn: cannot open " << IPC_FACE_EVT_CHANNEL
                  << " (start face_event first) — alerts disabled\n";

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

    struct rt_channel_msg msg;
    for (;;)
    {
        rt_channel_recv(ch_ai, &msg);
        uint64_t recv_n = g_ai_ipc_recv.fetch_add(1) + 1;
        if (ai_metrics_enabled(debug_mode) && (recv_n % 200) == 0)
        {
            std::cerr << "[face_ai] metrics ipc_recv=" << recv_n
                      << " reply_err_infer=" << g_ai_reply_err_infer.load() << std::endl;
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
            FrameCHWSize fs = {(int)hdr->tensor_c, (int)hdr->tensor_h, (int)hdr->tensor_w};

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
                if (!face_recg.pre_process(input_tensor, det_results[i].sparse_kps.points))
                {
                    std::cerr << "face_ai: recognition pre_process failed\n";
                    rec_ok = false;
                    break;
                }
                if (!face_recg.inference())
                {
                    std::cerr << "face_ai: recognition inference failed\n";
                    rec_ok = false;
                    break;
                }
                FaceRecognitionInfo recg_result;
                face_recg.database_search(recg_result);
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
                reply.num_faces++;

                if (evt_ch >= 0 && hdr->cmd == IPC_CMD_INFER && recg_result.id == -1)
                    notify_stranger(evt_ch, recg_result.id, recg_result.score, recg_result.name.c_str());
            }
            if (!rec_ok)
            {
                lwp_shmdt(req_map);
                reply_status_only(ch_ai, IPC_STATUS_ERR_INFER);
                continue;
            }

            if (hdr->cmd == IPC_CMD_REGISTER_COMMIT)
            {
                if (det_results.size() == 1)
                {
                    std::string reg_name(hdr->register_name);
                    face_recg.database_add(reg_name, db_dir);
                    std::cout << "face_ai: register ok: " << reg_name << std::endl;
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

    if (evt_ch >= 0)
        rt_channel_close(evt_ch);
    rt_channel_close(ch_ai);
    return 0;
}
