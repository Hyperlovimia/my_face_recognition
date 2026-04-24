#ifndef FACE_GATEWAY_PROTOCOL_H
#define FACE_GATEWAY_PROTOCOL_H

#include <stdint.h>

/*
 * 与大核 face_ctrl（IPCMSG，见 src/face_ctrl_main.cc）对齐的协议头。
 * 唯一维护：my_face_recognition/src/little；调试：小核 FACE_DEBUG=1/--debug，大核 FACE_DEBUG=1。
 */

typedef enum {
    FACE_CTRL_MODULE_WEB = 0x01,
    FACE_CTRL_MODULE_EVENT = 0x02,
} face_ctrl_module_e;

typedef enum {
    FACE_CTRL_CMD_PING = 0x1000,
    FACE_CTRL_CMD_GET_STATUS = 0x1001,
    FACE_CTRL_CMD_GET_DB_COUNT = 0x1002,
    FACE_CTRL_CMD_DB_RESET = 0x1003,
    FACE_CTRL_CMD_REGISTER_START = 0x1004,
    FACE_CTRL_CMD_REGISTER_COMMIT = 0x1005,
    FACE_CTRL_CMD_SHUTDOWN = 0x1006,
} face_ctrl_cmd_e;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t status;
    uint32_t db_count;
    uint32_t flags;
} face_ctrl_status_t;

#define FACE_CTRL_STATUS_MAGIC 0x46434757u /* "FCGW" */
#define FACE_CTRL_STATUS_VERSION 1u

#endif /* FACE_GATEWAY_PROTOCOL_H */
