#ifndef MY_FACE_UI_K230_PORT_H
#define MY_FACE_UI_K230_PORT_H

#include <stdbool.h>
#include <stdint.h>

#include "../../src/ipc_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

struct k230_ui_port_config {
    const char *touch_device;
    int logical_width;
    int logical_height;
    int screen_width;
    int screen_height;
    bool flip_x;
    bool flip_y;
    bridge_ui_shared_info_t shared_info;
};

bool k230_ui_port_init(const struct k230_ui_port_config *cfg);
void k230_ui_port_deinit(void);
uint32_t custom_tick_get(void);

#ifdef __cplusplus
}
#endif

#endif
