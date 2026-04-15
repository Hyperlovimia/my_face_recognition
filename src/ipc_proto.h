/* RT-Smart multi-process face recognition — IPC payload (POD only). */
#ifndef MY_FACE_IPC_PROTO_H
#define MY_FACE_IPC_PROTO_H

#include <stdint.h>

#define IPC_MAGIC 0xFACECAFEu

#define IPC_FACE_AI_CHANNEL "face_ai_req"
#define IPC_FACE_EVT_CHANNEL "face_evt"
/* event_service -> video_service：业务状态与退出（与 IPC_CMD_* 无关） */
#define IPC_FACE_VIDEO_CTRL "face_video_ctrl"

#define IPC_MAX_FACES 8
#define IPC_NAME_MAX 64

typedef enum {
    IPC_CMD_INFER = 0,
    IPC_CMD_DB_COUNT = 1,
    IPC_CMD_DB_RESET = 2,
    IPC_CMD_REGISTER_COMMIT = 3,
    IPC_CMD_SHUTDOWN = 4,
} ipc_cmd_t;

/* ipc_ai_reply_t.status：成功为 IPC_STATUS_OK；错误应答若仍通过 reply shmid 返回则填非 0 */
typedef enum {
    IPC_STATUS_OK = 0,
    IPC_STATUS_ERR_MAGIC = 1,
    IPC_STATUS_ERR_PARAM = 2,
    IPC_STATUS_ERR_SHM = 3,
    IPC_STATUS_ERR_INFER = 4,
    IPC_STATUS_ERR_BAD_CMD = 5,
} ipc_status_t;

typedef struct {
    float x;
    float y;
    float w;
    float h;
} ipc_bbox_t;

typedef struct {
    uint32_t magic;
    ipc_cmd_t cmd;
    uint32_t seq;
    uint32_t frame_bytes;
    uint32_t tensor_c;
    uint32_t tensor_h;
    uint32_t tensor_w;
    char register_name[IPC_NAME_MAX];
} ipc_req_hdr_t;

typedef struct {
    int32_t id;
    float score;
    char name[IPC_NAME_MAX];
} ipc_rec_face_t;

typedef struct {
    ipc_bbox_t bbox;
    float sparse_kps[10];
    float det_score;
    ipc_rec_face_t rec;
    float liveness_real_score; /* REAL 概率；未启用活体检测时为 1.0 */
    uint8_t is_live;           /* 1=活体通过或未启用活体；0=未通过 */
    uint8_t _pad[3];
} ipc_face_bundle_t;

typedef struct {
    uint32_t magic;
    int32_t status; /* ipc_status_t */
    int32_t count;
    int32_t num_faces;
    ipc_face_bundle_t faces[IPC_MAX_FACES];
} ipc_ai_reply_t;

/** face_ai -> face_event：业务事件类型（考勤/报警/留痕） */
typedef enum {
    IPC_EVT_KIND_RECOGNIZED = 0,    /* 活体通过且识别为已注册人员 */
    IPC_EVT_KIND_STRANGER = 1,      /* 活体通过，陌生人 */
    IPC_EVT_KIND_LIVENESS_FAIL = 2, /* 启用活体且未通过（疑似翻拍/攻击），score 为 REAL 概率 */
} ipc_evt_kind_t;

typedef struct {
    uint32_t magic;
    int32_t face_id;
    float score;
    char name[IPC_NAME_MAX];
    uint8_t is_stranger; /* 1=陌生人；与 evt_kind 一致时 STRANGER 为 1 */
    uint8_t evt_kind;    /* ipc_evt_kind_t */
    uint8_t _pad[2];
} ipc_evt_t;

typedef enum {
    IPC_VIDEO_CTRL_OP_SET = 0,
    IPC_VIDEO_CTRL_OP_QUIT = 1,
} ipc_video_ctrl_op_t;

typedef struct {
    uint32_t magic;
    int32_t op; /* ipc_video_ctrl_op_t */
    int32_t state;
    char register_name[IPC_NAME_MAX];
} ipc_video_ctrl_t;

#endif
