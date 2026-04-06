/* RT-Smart: AI process — face detection + recognition server over IPC (rt_channel + lwp_shm). */
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "ipc_shm.h"
#include "ai_utils.h"
#include "face_detection.h"
#include "face_recognition.h"
#include "setting.h"

using std::vector;

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
    rt_channel_send(evt_ch, &nm);
}

static void send_error_reply(int ch_ai, struct rt_channel_msg *orig_msg)
{
    struct rt_channel_msg err;
    memset(&err, 0, sizeof(err));
    err.type = RT_CHANNEL_RAW;
    err.u.d = (void *)-1;
    rt_channel_reply(ch_ai, &err);
    (void)orig_msg;
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
                send_error_reply(ch_ai, &msg);
                continue;
            }

            uint8_t *pixels = (uint8_t *)req_map + sizeof(ipc_req_hdr_t);
            dims_t in_shape{1, hdr->tensor_c, hdr->tensor_h, hdr->tensor_w};
            FrameCHWSize fs = {(int)hdr->tensor_c, (int)hdr->tensor_h, (int)hdr->tensor_w};

            runtime_tensor input_tensor =
                host_runtime_tensor::create(typecode_t::dt_uint8, in_shape, hrt::pool_shared).expect("create tensor");
            auto ref_buf = input_tensor.impl()
                               ->to_host()
                               .unwrap()
                               ->buffer()
                               .as_host()
                               .unwrap()
                               .map(map_access_::map_write)
                               .unwrap()
                               .buffer();
            memcpy(reinterpret_cast<char *>(ref_buf.data()), pixels, hdr->frame_bytes);
            hrt::sync(input_tensor, sync_op_t::sync_write_back, true).expect("sync wb");

            det_results.clear();
            rec_results.clear();

            face_det.pre_process(input_tensor);
            face_det.inference();
            face_det.post_process(fs, det_results);

            for (size_t i = 0; i < det_results.size() && i < (size_t)IPC_MAX_FACES; ++i)
            {
                face_recg.pre_process(input_tensor, det_results[i].sparse_kps.points);
                face_recg.inference();
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
            send_error_reply(ch_ai, &msg);
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
