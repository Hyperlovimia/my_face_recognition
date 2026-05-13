#ifndef MY_FACE_UI_K230_PORT_H
#define MY_FACE_UI_K230_PORT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct k230_ui_port_config {
    const char *drm_device;
    const char *touch_device;
    int overlay_width;
    int overlay_height;
    int offset_x;
    int offset_y;
    bool align_bottom;
    bool flip_x;
    bool flip_y;
};

bool k230_ui_port_init(const struct k230_ui_port_config *cfg);
void k230_ui_port_deinit(void);
uint32_t custom_tick_get(void);

#ifdef __cplusplus
}
#endif

#endif
