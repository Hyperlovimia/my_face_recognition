/* RT-Smart multi-process face recognition — IPC payload (POD only). */
#ifndef MY_FACE_IPC_PROTO_H
#define MY_FACE_IPC_PROTO_H

#include <stdint.h>

#include "ui_overlay_shared.h"

#define IPC_MAGIC 0xFACECAFEu

#define IPC_FACE_AI_CHANNEL "face_ai_req"
#define IPC_FACE_EVT_CHANNEL "face_evt"
/* event_service -> video_service：业务状态与退出（与 IPC_CMD_* 无关） */
#define IPC_FACE_VIDEO_CTRL "face_video_ctrl"
#define IPC_FACE_VIDEO_REPLY "face_video_reply"

#define IPC_FACE_BRIDGE_SERVICE "face_bridge"
#define IPC_FACE_BRIDGE_PORT 301u
#define IPC_FACE_BRIDGE_MODULE 1u

#define IPC_MAX_FACES 8
#define IPC_NAME_MAX 64
#define IPC_REQUEST_ID_MAX 64
#define IPC_OP_MESSAGE_MAX 128

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

typedef enum {
    IPC_OP_RESULT_NONE = 0,
    IPC_OP_RESULT_OK = 1,
    IPC_OP_RESULT_FAIL = 2,
} ipc_op_result_t;

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
    int32_t op_result; /* ipc_op_result_t */
    ipc_face_bundle_t faces[IPC_MAX_FACES];
    char op_message[IPC_OP_MESSAGE_MAX];
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
    uint64_t ts_ms; /* realtime ms */
    char name[IPC_NAME_MAX];
    uint8_t is_stranger; /* 1=陌生人；与 evt_kind 一致时 STRANGER 为 1 */
    uint8_t evt_kind;    /* ipc_evt_kind_t */
    uint8_t _pad[2];
} ipc_evt_t;

typedef enum {
    IPC_VIDEO_CTRL_SRC_LOCAL = 0,
    IPC_VIDEO_CTRL_SRC_BRIDGE = 1,
} ipc_video_ctrl_source_t;

/* 数值为 MQTT/IPC 有线协议：linux_bridge/face_event/face_netd 需同源编译、同时更新板子 */
typedef enum {
    IPC_BRIDGE_CMD_NONE = 0,
    IPC_BRIDGE_CMD_DB_COUNT = 1,
    IPC_BRIDGE_CMD_DB_RESET = 2,
    IPC_BRIDGE_CMD_REGISTER_CURRENT = 3,
    IPC_BRIDGE_CMD_SHUTDOWN = 4,
    /** 抓拍+预览(步进注册 step1，对应 video state 2) */
    IPC_BRIDGE_CMD_REGISTER_PREVIEW = 5,
    /** 提交姓名完成注册(步进 step2，对应 video state 3) */
    IPC_BRIDGE_CMD_REGISTER_COMMIT = 6,
    /** 放弃注册并关闭预览小窗(对应 video state 6) */
    IPC_BRIDGE_CMD_REGISTER_CANCEL = 7,
} ipc_bridge_cmd_t;

typedef enum {
    IPC_BRIDGE_MSG_CMD_REQ = 1,
    IPC_BRIDGE_MSG_EVENT = 2,
    IPC_BRIDGE_MSG_CMD_RESULT = 3,
    IPC_BRIDGE_MSG_UI_SHARED_INFO = 4,
} ipc_bridge_msg_kind_t;

typedef enum {
    IPC_VIDEO_CTRL_OP_SET = 0,
    IPC_VIDEO_CTRL_OP_QUIT = 1,
    IPC_VIDEO_CTRL_OP_UI_ATTACH = 2,
} ipc_video_ctrl_op_t;

typedef struct {
    uint32_t magic;
    int32_t op; /* ipc_video_ctrl_op_t */
    int32_t state;
    int32_t source;     /* ipc_video_ctrl_source_t */
    int32_t bridge_cmd; /* ipc_bridge_cmd_t */
    char request_id[IPC_REQUEST_ID_MAX];
    char register_name[IPC_NAME_MAX];
    uint64_t ui_phys_addr;
    uint32_t ui_bytes;
    uint32_t ui_width;
    uint32_t ui_height;
    uint32_t ui_stride;
    uint32_t ui_generation;
} ipc_video_ctrl_t;

typedef struct {
    uint32_t magic;
    int32_t source;     /* ipc_video_ctrl_source_t */
    int32_t bridge_cmd; /* ipc_bridge_cmd_t */
    int32_t count;
    uint8_t ok;
    uint8_t _pad[3];
    char request_id[IPC_REQUEST_ID_MAX];
    char message[IPC_OP_MESSAGE_MAX];
} ipc_video_reply_t;

typedef struct {
    uint32_t magic;
    int32_t cmd; /* ipc_bridge_cmd_t */
    char request_id[IPC_REQUEST_ID_MAX];
    char name[IPC_NAME_MAX];
} bridge_cmd_req_t;

typedef struct {
    uint32_t magic;
    uint8_t accepted;
    uint8_t _pad[3];
    char reason[IPC_OP_MESSAGE_MAX];
} bridge_cmd_ack_t;

typedef struct {
    uint32_t magic;
    int32_t evt_kind; /* ipc_evt_kind_t */
    int32_t face_id;
    float score;
    uint64_t ts_ms; /* realtime ms */
    char name[IPC_NAME_MAX];
} bridge_event_t;

typedef struct {
    uint32_t magic;
    uint64_t ui_phys_addr;
    uint32_t ui_bytes;
    uint32_t ui_width;
    uint32_t ui_height;
    uint32_t ui_stride;
    uint32_t ui_generation;
} bridge_ui_shared_info_t;

typedef struct {
    uint32_t magic;
    int32_t cmd; /* ipc_bridge_cmd_t */
    int32_t count;
    uint8_t ok;
    uint8_t _pad[3];
    char request_id[IPC_REQUEST_ID_MAX];
    char message[IPC_OP_MESSAGE_MAX];
} bridge_cmd_result_t;

#endif
