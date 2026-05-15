#include "k230_port.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

extern "C" {
#include "lvgl.h"
}

namespace {

struct PortRuntime {
    k230_ui_port_config cfg{};
    int mem_fd = -1;
    void *map_base = nullptr;
    size_t map_len = 0;
    uint8_t *shared_base = nullptr;
    ui_overlay_shared_header_t *header = nullptr;
    uint8_t *slots[UI_OVERLAY_SHARED_SLOT_COUNT]{};
    int touch_fd = -1;
    int raw_touch_x = 0;
    int raw_touch_y = 0;
    int touch_state = LV_INDEV_STATE_REL;
    bool inited = false;
    void *draw_buf_mem = nullptr;
    lv_disp_draw_buf_t draw_buf_dsc{};
    lv_disp_drv_t disp_drv{};
    lv_indev_drv_t indev_drv{};
    std::vector<uint8_t> composed_argb8888;
};

PortRuntime g_port;

size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

size_t frame_bytes()
{
    return static_cast<size_t>(g_port.cfg.shared_info.ui_stride) * g_port.cfg.shared_info.ui_height;
}

void cleanup_shared_map()
{
    if (g_port.map_base && g_port.map_len > 0)
        munmap(g_port.map_base, g_port.map_len);
    if (g_port.mem_fd >= 0)
        close(g_port.mem_fd);

    g_port.mem_fd = -1;
    g_port.map_base = nullptr;
    g_port.map_len = 0;
    g_port.shared_base = nullptr;
    g_port.header = nullptr;
    for (auto &slot : g_port.slots)
        slot = nullptr;
}

bool validate_shared_surface_locked()
{
    if (!g_port.header)
        return false;

    const ui_overlay_shared_header_t &hdr = *g_port.header;
    if (hdr.magic != UI_OVERLAY_SHARED_MAGIC)
    {
        std::fprintf(stderr, "face_netd_ui: invalid shared header magic=0x%x\n", hdr.magic);
        return false;
    }
    if (hdr.version != UI_OVERLAY_SHARED_VERSION)
    {
        std::fprintf(stderr, "face_netd_ui: unsupported shared header version=%u\n", hdr.version);
        return false;
    }
    if (hdr.width != g_port.cfg.shared_info.ui_width || hdr.height != g_port.cfg.shared_info.ui_height ||
        hdr.stride != g_port.cfg.shared_info.ui_stride)
    {
        std::fprintf(stderr,
                     "face_netd_ui: shared geometry mismatch hdr=%ux%u stride=%u info=%ux%u stride=%u\n", hdr.width,
                     hdr.height, hdr.stride, g_port.cfg.shared_info.ui_width, g_port.cfg.shared_info.ui_height,
                     g_port.cfg.shared_info.ui_stride);
        return false;
    }
    if (hdr.slot_count != UI_OVERLAY_SHARED_SLOT_COUNT)
    {
        std::fprintf(stderr, "face_netd_ui: unsupported shared slot_count=%u\n", hdr.slot_count);
        return false;
    }
    if (hdr.generation != g_port.cfg.shared_info.ui_generation)
    {
        std::fprintf(stderr, "face_netd_ui: shared generation mismatch hdr=%u info=%u\n", hdr.generation,
                     g_port.cfg.shared_info.ui_generation);
        return false;
    }
    return true;
}

bool init_shared_map()
{
    const bridge_ui_shared_info_t &info = g_port.cfg.shared_info;
    if (info.ui_phys_addr == 0 || info.ui_bytes < ui_overlay_shared_total_bytes_unaligned())
    {
        std::fprintf(stderr, "face_netd_ui: invalid shared info phys=0x%llx bytes=%u\n",
                     static_cast<unsigned long long>(info.ui_phys_addr), info.ui_bytes);
        return false;
    }

    g_port.mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (g_port.mem_fd < 0)
    {
        std::fprintf(stderr, "face_netd_ui: open /dev/mem failed errno=%d (%s)\n", errno, std::strerror(errno));
        return false;
    }

    const size_t page_size = 4096u;
    const uint64_t phys = info.ui_phys_addr;
    const size_t page_off = static_cast<size_t>(phys & (page_size - 1u));
    const off_t map_phys = static_cast<off_t>(phys - page_off);
    g_port.map_len = align_up(page_off + info.ui_bytes, page_size);
    g_port.map_base = mmap(nullptr, g_port.map_len, PROT_READ | PROT_WRITE, MAP_SHARED, g_port.mem_fd, map_phys);
    if (g_port.map_base == MAP_FAILED)
    {
        g_port.map_base = nullptr;
        std::fprintf(stderr, "face_netd_ui: mmap shared buffer failed errno=%d (%s)\n", errno, std::strerror(errno));
        cleanup_shared_map();
        return false;
    }

    g_port.shared_base = reinterpret_cast<uint8_t *>(g_port.map_base) + page_off;
    g_port.header = reinterpret_cast<ui_overlay_shared_header_t *>(g_port.shared_base);
    if (!validate_shared_surface_locked())
    {
        cleanup_shared_map();
        return false;
    }

    uint8_t *slot_base = g_port.shared_base + sizeof(ui_overlay_shared_header_t);
    const size_t slot_bytes = ui_overlay_shared_slot_bytes();
    for (size_t i = 0; i < UI_OVERLAY_SHARED_SLOT_COUNT; ++i)
        g_port.slots[i] = slot_base + i * slot_bytes;

    std::fprintf(stderr,
                 "face_netd_ui: shared surface mapped phys=0x%llx bytes=%u generation=%u geometry=%ux%u stride=%u\n",
                 static_cast<unsigned long long>(info.ui_phys_addr), info.ui_bytes, info.ui_generation, info.ui_width,
                 info.ui_height, info.ui_stride);
    return true;
}

void map_touch_point(int *x, int *y)
{
    if (!x || !y)
        return;

    int mx = *x;
    int my = *y;
    if (g_port.cfg.flip_x)
        mx = g_port.cfg.screen_width - mx;
    if (g_port.cfg.flip_y)
        my = g_port.cfg.screen_height - my;

    mx = std::max(0, std::min(mx, g_port.cfg.screen_width - 1));
    my = std::max(0, std::min(my, g_port.cfg.screen_height - 1));

    mx = static_cast<int>((static_cast<int64_t>(mx) * g_port.cfg.logical_width) / std::max(1, g_port.cfg.screen_width));
    my =
        static_cast<int>((static_cast<int64_t>(my) * g_port.cfg.logical_height) / std::max(1, g_port.cfg.screen_height));

    mx = std::max(0, std::min(mx, g_port.cfg.logical_width - 1));
    my = std::max(0, std::min(my, g_port.cfg.logical_height - 1));
    *x = mx;
    *y = my;
}

void touchpad_read(lv_indev_drv_t *, lv_indev_data_t *data)
{
    if (!data)
        return;

    if (g_port.touch_fd >= 0)
    {
        struct input_event ev{};
        while (read(g_port.touch_fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev)))
        {
            if (ev.type == EV_ABS)
            {
                if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X)
                    g_port.raw_touch_x = ev.value;
                else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y)
                    g_port.raw_touch_y = ev.value;
            }
            else if (ev.type == EV_KEY && ev.code == BTN_TOUCH)
            {
                g_port.touch_state = ev.value ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
            }
        }
    }

    int mapped_x = g_port.raw_touch_x;
    int mapped_y = g_port.raw_touch_y;
    map_touch_point(&mapped_x, &mapped_y);
    data->state = static_cast<lv_indev_state_t>(g_port.touch_state);
    data->point.x = mapped_x;
    data->point.y = mapped_y;
}

bool init_touch()
{
    g_port.touch_fd = open(g_port.cfg.touch_device, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (g_port.touch_fd < 0)
        std::fprintf(stderr, "face_netd_ui: open touch failed dev=%s errno=%d (%s)\n", g_port.cfg.touch_device, errno,
                     std::strerror(errno));
    else
        std::fprintf(stderr, "face_netd_ui: touch ok dev=%s fd=%d\n", g_port.cfg.touch_device, g_port.touch_fd);
    return g_port.touch_fd >= 0;
}

void cleanup_touch()
{
    if (g_port.touch_fd >= 0)
        close(g_port.touch_fd);
    g_port.touch_fd = -1;
}

void publish_full_frame()
{
    if (!g_port.header)
        return;
    if (!validate_shared_surface_locked())
        return;

    uint32_t front = g_port.header->front_index;
    if (front >= UI_OVERLAY_SHARED_SLOT_COUNT)
        front = 0;
    const uint32_t back = (front == 0) ? 1u : 0u;
    std::memcpy(g_port.slots[back], g_port.composed_argb8888.data(), frame_bytes());
    std::atomic_thread_fence(std::memory_order_release);
    g_port.header->seq += 1u;
    g_port.header->front_index = back;
    g_port.header->flags |= UI_OVERLAY_SHARED_FLAG_READY;
}

void disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    lv_color32_t *src = reinterpret_cast<lv_color32_t *>(color_p);
    const uint32_t stride = g_port.cfg.shared_info.ui_stride;
    for (int y = area->y1; y <= area->y2; ++y)
    {
        uint8_t *dst = g_port.composed_argb8888.data() + static_cast<size_t>(stride) * y + area->x1 * 4;
        for (int x = area->x1; x <= area->x2; ++x)
        {
            dst[0] = src->ch.alpha;
            dst[1] = src->ch.red;
            dst[2] = src->ch.green;
            dst[3] = src->ch.blue;
            ++src;
            dst += 4;
        }
    }

    lv_disp_flush_ready(disp_drv);
    if (disp_drv->draw_buf->last_area != 1 || disp_drv->draw_buf->last_part != 1)
        return;

    publish_full_frame();
}

}  // namespace

bool k230_ui_port_init(const struct k230_ui_port_config *cfg)
{
    if (!cfg || !cfg->touch_device || cfg->logical_width <= 0 || cfg->logical_height <= 0 || cfg->screen_width <= 0 ||
        cfg->screen_height <= 0)
    {
        std::fprintf(stderr, "face_netd_ui: invalid port config\n");
        return false;
    }

    g_port.cfg = *cfg;
    g_port.composed_argb8888.assign(frame_bytes(), 0);

    if (!init_shared_map() || !init_touch())
    {
        cleanup_touch();
        cleanup_shared_map();
        g_port.composed_argb8888.clear();
        return false;
    }

    g_port.draw_buf_mem = std::malloc(static_cast<size_t>(cfg->logical_width) * 100 * sizeof(lv_color_t));
    if (!g_port.draw_buf_mem)
    {
        cleanup_touch();
        cleanup_shared_map();
        g_port.composed_argb8888.clear();
        return false;
    }

    lv_disp_draw_buf_init(&g_port.draw_buf_dsc, g_port.draw_buf_mem, nullptr, cfg->logical_width * 100);
    lv_disp_drv_init(&g_port.disp_drv);
    g_port.disp_drv.hor_res = cfg->logical_width;
    g_port.disp_drv.ver_res = cfg->logical_height;
    g_port.disp_drv.flush_cb = disp_flush;
    g_port.disp_drv.draw_buf = &g_port.draw_buf_dsc;
    g_port.disp_drv.screen_transp = 1;
    lv_disp_drv_register(&g_port.disp_drv);

    lv_indev_drv_init(&g_port.indev_drv);
    g_port.indev_drv.type = LV_INDEV_TYPE_POINTER;
    g_port.indev_drv.read_cb = touchpad_read;
    lv_indev_drv_register(&g_port.indev_drv);

    g_port.inited = true;
    std::fprintf(stderr, "face_netd_ui: offscreen port init complete logical=%dx%d screen=%dx%d\n",
                 cfg->logical_width, cfg->logical_height, cfg->screen_width, cfg->screen_height);
    return true;
}

void k230_ui_port_deinit(void)
{
    if (!g_port.inited)
        return;

    cleanup_touch();
    cleanup_shared_map();
    if (g_port.draw_buf_mem)
        std::free(g_port.draw_buf_mem);
    g_port.draw_buf_mem = nullptr;
    g_port.composed_argb8888.clear();
    g_port.inited = false;
}

extern "C" uint32_t custom_tick_get(void)
{
    static uint64_t start_ms = 0;
    if (start_ms == 0)
    {
        struct timeval tv_start{};
        gettimeofday(&tv_start, nullptr);
        start_ms = (static_cast<uint64_t>(tv_start.tv_sec) * 1000000ULL + tv_start.tv_usec) / 1000ULL;
    }

    struct timeval tv_now{};
    gettimeofday(&tv_now, nullptr);
    const uint64_t now_ms =
        (static_cast<uint64_t>(tv_now.tv_sec) * 1000000ULL + static_cast<uint64_t>(tv_now.tv_usec)) / 1000ULL;
    return static_cast<uint32_t>(now_ms - start_ms);
}
