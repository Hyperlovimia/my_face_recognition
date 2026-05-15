#ifndef MY_FACE_UI_OVERLAY_SHARED_H
#define MY_FACE_UI_OVERLAY_SHARED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_OVERLAY_SHARED_MAGIC 0x55494f56u
#define UI_OVERLAY_SHARED_VERSION 1u
#define UI_OVERLAY_SHARED_FLAG_READY 0x00000001u

#define UI_OVERLAY_SHARED_WIDTH 540u
#define UI_OVERLAY_SHARED_HEIGHT 960u
#define UI_OVERLAY_SHARED_BPP 4u
#define UI_OVERLAY_SHARED_STRIDE (UI_OVERLAY_SHARED_WIDTH * UI_OVERLAY_SHARED_BPP)
#define UI_OVERLAY_SHARED_SLOT_COUNT 2u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t slot_count;
    uint32_t front_index;
    uint32_t seq;
    uint32_t generation;
    uint32_t flags;
    uint32_t reserved[6];
} ui_overlay_shared_header_t;

static inline uint32_t ui_overlay_shared_slot_bytes(void)
{
    return UI_OVERLAY_SHARED_STRIDE * UI_OVERLAY_SHARED_HEIGHT;
}

static inline uint32_t ui_overlay_shared_total_bytes_unaligned(void)
{
    return (uint32_t)(sizeof(ui_overlay_shared_header_t) +
                      UI_OVERLAY_SHARED_SLOT_COUNT * ui_overlay_shared_slot_bytes());
}

static inline uint32_t ui_overlay_shared_total_bytes_aligned(void)
{
    const uint32_t total = ui_overlay_shared_total_bytes_unaligned();
    return (total + 4095u) & ~4095u;
}

#ifdef __cplusplus
}
#endif

#endif
