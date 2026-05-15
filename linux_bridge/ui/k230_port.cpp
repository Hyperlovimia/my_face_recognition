#include "k230_port.h"

#include <algorithm>
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
#include <pthread.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

extern "C" {
#include "lvgl.h"
}

#include "../../../../src/little/buildroot-ext/package/libdisp/src/disp.h"
#include "../../../../src/little/buildroot-ext/package/door_lock/src/ui/lvgl_port/k230/buf_mgt.hpp"

namespace {

constexpr int kDefaultUiPlaneIndex = 2;
constexpr int kUiBufferCount = 2;

struct PortRuntime {
    k230_ui_port_config cfg{};
    drm_dev drm{};
    drm_buffer bufs[kUiBufferCount]{};
    buf_mgt_t buf_mgt{};
    uint32_t screen_width = 0;
    uint32_t screen_height = 0;
    int resolved_offset_x = 0;
    int resolved_offset_y = 0;
    int touch_fd = -1;
    int raw_touch_x = 0;
    int raw_touch_y = 0;
    int touch_state = LV_INDEV_STATE_REL;
    int plane_index = -1;
    bool inited = false;
    bool stop = false;
    bool pflip_thread_started = false;
    void *draw_buf_mem = nullptr;
    lv_disp_draw_buf_t draw_buf_dsc{};
    lv_disp_drv_t disp_drv{};
    lv_indev_drv_t indev_drv{};
    std::vector<uint16_t> composed_argb4444;
    std::thread pflip_thread;
};

PortRuntime g_port;

uint16_t argb8888_to_argb4444(uint32_t data)
{
    return static_cast<uint16_t>(((data >> 16) & 0xf000) | ((data >> 12) & 0x0f00) |
                                 ((data >> 8) & 0x00f0) | ((data >> 4) & 0x000f));
}

void map_touch_point(int *x, int *y)
{
    if (!x || !y)
        return;
    int mx = *x;
    int my = *y;
    if (g_port.cfg.flip_x)
        mx = static_cast<int>(g_port.screen_width) - mx;
    if (g_port.cfg.flip_y)
        my = static_cast<int>(g_port.screen_height) - my;
    mx -= g_port.resolved_offset_x;
    my -= g_port.resolved_offset_y;
    mx = std::max(0, std::min(mx, g_port.cfg.overlay_width - 1));
    my = std::max(0, std::min(my, g_port.cfg.overlay_height - 1));
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

void drm_wait_vsync(drm_dev *dev)
{
    static drmEventContext ctx{};
    ctx.version = DRM_EVENT_CONTEXT_VERSION;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(dev->fd, &fds);
    int ret = 0;
    do
    {
        ret = select(dev->fd + 1, &fds, nullptr, nullptr, nullptr);
    } while (ret == -1 && errno == EINTR);
    if (ret > 0 && FD_ISSET(dev->fd, &fds))
    {
        drmHandleEvent(dev->fd, &ctx);
        dev->pflip_pending = false;
    }
    else if (ret < 0)
    {
        std::fprintf(stderr, "face_netd_ui: drm_wait_vsync select failed errno=%d (%s)\n", errno, std::strerror(errno));
    }
}

int plane_config(drm_dev *dev, drm_buffer *buf, int plane_index)
{
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req)
        return -1;

    int ret = drmModeCreatePropertyBlob(dev->fd, &dev->mode, sizeof(dev->mode), &dev->mode_blob_id);
    if (ret != 0)
    {
        drmModeAtomicFree(req);
        return ret;
    }

    if ((ret = drm_set_object_property(req, &dev->conn, "CRTC_ID", dev->crtc_id)) < 0 ||
        (ret = drm_set_object_property(req, &dev->crtc, "MODE_ID", dev->mode_blob_id)) < 0 ||
        (ret = drm_set_object_property(req, &dev->crtc, "ACTIVE", 1)) < 0 ||
        (ret = drm_set_object_property(req, &dev->planes[plane_index], "FB_ID", buf->fb)) < 0 ||
        (ret = drm_set_object_property(req, &dev->planes[plane_index], "CRTC_ID", dev->crtc_id)) < 0 ||
        (ret = drm_set_object_property(req, &dev->planes[plane_index], "SRC_X", 0)) < 0 ||
        (ret = drm_set_object_property(req, &dev->planes[plane_index], "SRC_Y", 0)) < 0 ||
        (ret = drm_set_object_property(req, &dev->planes[plane_index], "SRC_W", buf->width << 16)) < 0 ||
        (ret = drm_set_object_property(req, &dev->planes[plane_index], "SRC_H", buf->height << 16)) < 0 ||
        (ret = drm_set_object_property(req, &dev->planes[plane_index], "CRTC_X", buf->offset_x)) < 0 ||
        (ret = drm_set_object_property(req, &dev->planes[plane_index], "CRTC_Y", buf->offset_y)) < 0 ||
        (ret = drm_set_object_property(req, &dev->planes[plane_index], "CRTC_W", buf->width)) < 0 ||
        (ret = drm_set_object_property(req, &dev->planes[plane_index], "CRTC_H", buf->height)) < 0)
    {
        drmModeAtomicFree(req);
        return ret;
    }

    ret = drmModeAtomicCommit(dev->fd, req, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
    if (ret == 0)
        ret = drmModeAtomicCommit(dev->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT, nullptr);
    if (ret == 0)
        dev->pflip_pending = true;
    else
        std::fprintf(stderr,
                     "face_netd_ui: plane_config failed ret=%d plane=%d fb=%u pos=%d,%d size=%ux%u\n",
                     ret, plane_index, buf ? buf->fb : 0, buf ? buf->offset_x : 0, buf ? buf->offset_y : 0,
                     buf ? buf->width : 0, buf ? buf->height : 0);
    drmModeAtomicFree(req);
    return ret;
}

int plane_update(drm_dev *dev, drm_buffer *buf, int plane_index)
{
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req)
        return -1;
    const uint32_t fb = buf ? buf->fb : 0;
    int ret = 0;
    if ((ret = drm_set_object_property(req, &dev->planes[plane_index], "FB_ID", fb)) < 0 ||
        (ret = drm_set_object_property(req, &dev->planes[plane_index], "CRTC_ID", fb ? dev->crtc_id : 0)) < 0)
    {
        drmModeAtomicFree(req);
        return ret;
    }
    ret = drmModeAtomicCommit(dev->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT, nullptr);
    if (ret == 0)
        dev->pflip_pending = true;
    else
        std::fprintf(stderr, "face_netd_ui: plane_update failed ret=%d plane=%d fb=%u\n", ret, plane_index, fb);
    drmModeAtomicFree(req);
    return ret;
}

bool select_plane(drm_dev *dev, drm_buffer *buf)
{
    if (!dev || !buf || dev->plane_count == 0)
        return false;

    auto try_plane = [&](int idx) -> bool {
        if (idx < 0 || static_cast<uint32_t>(idx) >= dev->plane_count)
            return false;
        if (plane_config(dev, buf, idx) == 0)
        {
            g_port.plane_index = idx;
            std::fprintf(stderr, "face_netd_ui: selected plane=%d plane_id=%u\n", idx, dev->planes[idx].id);
            return true;
        }
        return false;
    };

    if (try_plane(kDefaultUiPlaneIndex))
        return true;

    for (uint32_t i = 0; i < dev->plane_count; ++i)
    {
        if (static_cast<int>(i) == kDefaultUiPlaneIndex)
            continue;
        if (try_plane(static_cast<int>(i)))
            return true;
    }
    return false;
}

void pflip_thread_body()
{
    while (!g_port.stop)
    {
        if (g_port.drm.pflip_pending)
        {
            drm_wait_vsync(&g_port.drm);
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        if (g_port.drm.cleanup)
            break;

        uint64_t idx = 0;
        const int stat = buf_mgt_reader_get(&g_port.buf_mgt, reinterpret_cast<void **>(&idx), 1);
        if (stat < 0 || idx >= kUiBufferCount)
            idx = 0;
        if (g_port.plane_index >= 0)
            plane_update(&g_port.drm, &g_port.bufs[idx], g_port.plane_index);
    }
}

void disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t *src = reinterpret_cast<uint32_t *>(color_p);
    for (int y = area->y1; y <= area->y2; ++y)
    {
        uint16_t *dst = g_port.composed_argb4444.data() + g_port.cfg.overlay_width * y + area->x1;
        for (int x = area->x1; x <= area->x2; ++x)
            *dst++ = argb8888_to_argb4444(*src++);
    }

    lv_disp_flush_ready(disp_drv);
    if (disp_drv->draw_buf->last_area != 1 || disp_drv->draw_buf->last_part != 1)
        return;

    while (!g_port.stop)
    {
        uint64_t idx = 0;
        if (buf_mgt_writer_get(&g_port.buf_mgt, reinterpret_cast<void **>(&idx), 1) < 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (idx >= kUiBufferCount)
            idx = 0;
        std::memcpy(g_port.bufs[idx].map, g_port.composed_argb4444.data(),
                    static_cast<size_t>(g_port.cfg.overlay_width) * g_port.cfg.overlay_height * sizeof(uint16_t));
        buf_mgt_writer_put(&g_port.buf_mgt, reinterpret_cast<void *>(idx));
        static bool first_frame = true;
        if (first_frame)
        {
            first_frame = false;
            if (!select_plane(&g_port.drm, &g_port.bufs[idx]))
                std::fprintf(stderr, "face_netd_ui: no usable DRM plane found for overlay\n");
        }
        break;
    }
}

bool init_drm()
{
    if (drm_dev_setup(&g_port.drm, g_port.cfg.drm_device) != 0)
    {
        std::fprintf(stderr, "face_netd_ui: drm_dev_setup failed for %s\n", g_port.cfg.drm_device);
        return false;
    }

    drm_get_resolution(&g_port.drm, &g_port.screen_width, &g_port.screen_height);
    g_port.resolved_offset_x = std::max(0, g_port.cfg.offset_x);
    g_port.resolved_offset_y = g_port.cfg.align_bottom
                                   ? std::max(0, static_cast<int>(g_port.screen_height) - g_port.cfg.overlay_height)
                                   : std::max(0, g_port.cfg.offset_y);
    std::fprintf(stderr,
                 "face_netd_ui: drm ok dev=%s screen=%ux%u overlay=%dx%d offset=%d,%d plane_count=%u preferred_plane=%d\n",
                 g_port.cfg.drm_device, g_port.screen_width, g_port.screen_height, g_port.cfg.overlay_width,
                 g_port.cfg.overlay_height, g_port.resolved_offset_x, g_port.resolved_offset_y, g_port.drm.plane_count,
                 kDefaultUiPlaneIndex);
    for (uint32_t i = 0; i < g_port.drm.plane_count; ++i)
        std::fprintf(stderr, "face_netd_ui: plane[%u] id=%u\n", i, g_port.drm.planes[i].id);

    for (int i = 0; i < kUiBufferCount; ++i)
    {
        g_port.bufs[i].width = ALIGNED_UP_POWER_OF_TWO(g_port.cfg.overlay_width, 3);
        g_port.bufs[i].height = ALIGNED_DOWN_POWER_OF_TWO(g_port.cfg.overlay_height, 0);
        g_port.bufs[i].offset_x = g_port.resolved_offset_x;
        g_port.bufs[i].offset_y = g_port.resolved_offset_y;
        g_port.bufs[i].fourcc = DRM_FORMAT_ARGB4444;
        g_port.bufs[i].bpp = 16;
        buf_mgt_reader_put(&g_port.buf_mgt, reinterpret_cast<void *>(static_cast<uint64_t>(i)));
        if (drm_create_fb(g_port.drm.fd, &g_port.bufs[i]) != 0)
        {
            std::fprintf(stderr, "face_netd_ui: drm_create_fb failed idx=%d size=%ux%u\n", i, g_port.bufs[i].width,
                         g_port.bufs[i].height);
            return false;
        }
        std::memset(g_port.bufs[i].map, 0,
                    static_cast<size_t>(g_port.cfg.overlay_width) * g_port.cfg.overlay_height * sizeof(uint16_t));
    }

    g_port.drm.cleanup = false;
    g_port.stop = false;
    g_port.pflip_thread = std::thread(pflip_thread_body);
    g_port.pflip_thread_started = true;
    return true;
}

void cleanup_drm()
{
    if (g_port.pflip_thread_started)
    {
        g_port.stop = true;
        g_port.drm.cleanup = true;
        if (g_port.pflip_thread.joinable())
            g_port.pflip_thread.join();
        g_port.pflip_thread_started = false;
    }

    for (auto &buf : g_port.bufs)
    {
        if (buf.map)
            drm_destroy_fb(g_port.drm.fd, &buf);
        buf = drm_buffer{};
    }
    if (g_port.drm.fd > 0)
        drm_dev_cleanup(&g_port.drm);
    g_port.drm = drm_dev{};
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

}  // namespace

bool k230_ui_port_init(const struct k230_ui_port_config *cfg)
{
    if (!cfg || !cfg->drm_device || !cfg->touch_device || cfg->overlay_width <= 0 || cfg->overlay_height <= 0)
    {
        std::fprintf(stderr, "face_netd_ui: invalid port config\n");
        return false;
    }

    g_port.cfg = *cfg;
    g_port.plane_index = -1;
    g_port.composed_argb4444.assign(static_cast<size_t>(cfg->overlay_width) * cfg->overlay_height, 0);
    if (!init_drm() || !init_touch())
    {
        cleanup_touch();
        cleanup_drm();
        return false;
    }

    g_port.draw_buf_mem = std::malloc(static_cast<size_t>(cfg->overlay_width) * 100 * sizeof(lv_color_t));
    if (!g_port.draw_buf_mem)
    {
        cleanup_touch();
        cleanup_drm();
        return false;
    }

    lv_disp_draw_buf_init(&g_port.draw_buf_dsc, g_port.draw_buf_mem, nullptr, cfg->overlay_width * 100);
    lv_disp_drv_init(&g_port.disp_drv);
    g_port.disp_drv.hor_res = cfg->overlay_width;
    g_port.disp_drv.ver_res = cfg->overlay_height;
    g_port.disp_drv.flush_cb = disp_flush;
    g_port.disp_drv.draw_buf = &g_port.draw_buf_dsc;
    g_port.disp_drv.screen_transp = 1;
    lv_disp_drv_register(&g_port.disp_drv);

    lv_indev_drv_init(&g_port.indev_drv);
    g_port.indev_drv.type = LV_INDEV_TYPE_POINTER;
    g_port.indev_drv.read_cb = touchpad_read;
    lv_indev_drv_register(&g_port.indev_drv);

    g_port.inited = true;
    std::fprintf(stderr, "face_netd_ui: port init complete\n");
    return true;
}

void k230_ui_port_deinit(void)
{
    if (!g_port.inited)
        return;
    cleanup_touch();
    cleanup_drm();
    if (g_port.draw_buf_mem)
        std::free(g_port.draw_buf_mem);
    g_port.draw_buf_mem = nullptr;
    g_port.composed_argb4444.clear();
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
        (static_cast<uint64_t>(tv_now.tv_sec) * 1000000ULL + tv_now.tv_usec) / 1000ULL;
    return static_cast<uint32_t>(now_ms - start_ms);
}
