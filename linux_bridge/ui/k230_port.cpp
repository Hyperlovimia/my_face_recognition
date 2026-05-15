#include "k230_port.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern "C" {
#include "lvgl.h"
}

namespace {

struct TouchDeviceInfo {
    std::string path;
    std::string name;
    std::string probe_error;
    int fd = -1;
    int raw_min_x = 0;
    int raw_max_x = 0;
    int raw_min_y = 0;
    int raw_max_y = 0;
    bool have_abs_x_range = false;
    bool have_abs_y_range = false;
    bool has_abs_x = false;
    bool has_abs_y = false;
    bool has_mt_x = false;
    bool has_mt_y = false;
    bool has_btn_touch = false;
    bool has_btn_tool_finger = false;
    bool has_mt_tracking = false;
    bool has_pressure = false;
};

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
    int raw_min_x = 0;
    int raw_max_x = 0;
    int raw_min_y = 0;
    int raw_max_y = 0;
    bool have_abs_x_range = false;
    bool have_abs_y_range = false;
    bool have_btn_touch = false;
    bool have_btn_tool_finger = false;
    bool have_mt_tracking = false;
    bool touch_down_logged = false;
    bool inited = false;
    void *draw_buf_mem = nullptr;
    lv_disp_draw_buf_t draw_buf_dsc{};
    lv_disp_drv_t disp_drv{};
    lv_indev_drv_t indev_drv{};
    std::vector<uint8_t> composed_argb8888;
};

PortRuntime g_port;

template <size_t N>
bool test_bit(const unsigned long (&bits)[N], int bit)
{
    const size_t bits_per_word = sizeof(unsigned long) * 8u;
    const size_t word = static_cast<size_t>(bit) / bits_per_word;
    const size_t shift = static_cast<size_t>(bit) % bits_per_word;
    if (word >= N)
        return false;
    return (bits[word] & (1UL << shift)) != 0;
}

void close_touch_device_info(TouchDeviceInfo *info)
{
    if (!info)
        return;
    if (info->fd >= 0)
        close(info->fd);
    info->fd = -1;
}

bool probe_abs_range(int fd, unsigned int code, int *out_min, int *out_max)
{
    if (fd < 0 || !out_min || !out_max)
        return false;

    struct input_absinfo abs_info {};
    if (ioctl(fd, EVIOCGABS(code), &abs_info) != 0)
        return false;
    *out_min = abs_info.minimum;
    *out_max = abs_info.maximum;
    return *out_max > *out_min;
}

bool probe_touch_device(const std::string &path, TouchDeviceInfo *out)
{
    if (!out)
        return false;

    *out = TouchDeviceInfo{};
    out->path = path;
    out->fd = open(path.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (out->fd < 0)
    {
        out->probe_error = std::strerror(errno);
        return false;
    }

    char name_buf[128] = {0};
    if (ioctl(out->fd, EVIOCGNAME(sizeof(name_buf)), name_buf) >= 0)
        out->name = name_buf;

    unsigned long ev_bits[(EV_MAX + (sizeof(unsigned long) * 8)) / (sizeof(unsigned long) * 8)] = {0};
    if (ioctl(out->fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0)
    {
        out->probe_error = std::string("EVIOCGBIT failed: ") + std::strerror(errno);
        close_touch_device_info(out);
        return false;
    }

    if (test_bit(ev_bits, EV_KEY))
    {
        unsigned long key_bits[(KEY_MAX + (sizeof(unsigned long) * 8)) / (sizeof(unsigned long) * 8)] = {0};
        if (ioctl(out->fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0)
        {
            out->has_btn_touch = test_bit(key_bits, BTN_TOUCH);
            out->has_btn_tool_finger = test_bit(key_bits, BTN_TOOL_FINGER);
        }
    }

    if (test_bit(ev_bits, EV_ABS))
    {
        unsigned long abs_bits[(ABS_MAX + (sizeof(unsigned long) * 8)) / (sizeof(unsigned long) * 8)] = {0};
        if (ioctl(out->fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) >= 0)
        {
            out->has_abs_x = test_bit(abs_bits, ABS_X);
            out->has_abs_y = test_bit(abs_bits, ABS_Y);
            out->has_mt_x = test_bit(abs_bits, ABS_MT_POSITION_X);
            out->has_mt_y = test_bit(abs_bits, ABS_MT_POSITION_Y);
            out->has_mt_tracking = test_bit(abs_bits, ABS_MT_TRACKING_ID);
            out->has_pressure = test_bit(abs_bits, ABS_PRESSURE);
        }
    }

    if (out->has_mt_x)
        out->have_abs_x_range = probe_abs_range(out->fd, ABS_MT_POSITION_X, &out->raw_min_x, &out->raw_max_x);
    if (!out->have_abs_x_range && out->has_abs_x)
        out->have_abs_x_range = probe_abs_range(out->fd, ABS_X, &out->raw_min_x, &out->raw_max_x);

    if (out->has_mt_y)
        out->have_abs_y_range = probe_abs_range(out->fd, ABS_MT_POSITION_Y, &out->raw_min_y, &out->raw_max_y);
    if (!out->have_abs_y_range && out->has_abs_y)
        out->have_abs_y_range = probe_abs_range(out->fd, ABS_Y, &out->raw_min_y, &out->raw_max_y);

    return true;
}

bool is_touch_candidate(const TouchDeviceInfo &info)
{
    const bool has_position = (info.has_mt_x || info.has_abs_x) && (info.has_mt_y || info.has_abs_y);
    const bool has_press_signal =
        info.has_btn_touch || info.has_btn_tool_finger || info.has_mt_tracking || info.has_pressure;
    return has_position && has_press_signal;
}

void apply_touch_device_info(const TouchDeviceInfo &info)
{
    g_port.touch_fd = info.fd;
    g_port.raw_min_x = info.raw_min_x;
    g_port.raw_max_x = info.raw_max_x;
    g_port.raw_min_y = info.raw_min_y;
    g_port.raw_max_y = info.raw_max_y;
    g_port.have_abs_x_range = info.have_abs_x_range;
    g_port.have_abs_y_range = info.have_abs_y_range;
    g_port.have_btn_touch = info.has_btn_touch;
    g_port.have_btn_tool_finger = info.has_btn_tool_finger;
    g_port.have_mt_tracking = info.has_mt_tracking;
    g_port.touch_down_logged = false;
}

void log_touch_caps(const char *prefix, const TouchDeviceInfo &info)
{
    std::fprintf(stderr,
                 "%s dev=%s name=\"%s\" abs=%d/%d mt=%d/%d btn_touch=%d btn_tool_finger=%d tracking=%d pressure=%d raw_x=[%d,%d] raw_y=[%d,%d]\n",
                 prefix, info.path.c_str(), info.name.c_str(), info.has_abs_x ? 1 : 0, info.has_abs_y ? 1 : 0,
                 info.has_mt_x ? 1 : 0, info.has_mt_y ? 1 : 0, info.has_btn_touch ? 1 : 0,
                 info.has_btn_tool_finger ? 1 : 0, info.has_mt_tracking ? 1 : 0, info.has_pressure ? 1 : 0,
                 info.raw_min_x, info.raw_max_x, info.raw_min_y, info.raw_max_y);
}

std::vector<std::string> enumerate_input_event_nodes()
{
    std::vector<std::string> paths;
    DIR *dir = opendir("/dev/input");
    if (!dir)
        return paths;

    struct dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (std::strncmp(entry->d_name, "event", 5) != 0)
            continue;
        paths.emplace_back(std::string("/dev/input/") + entry->d_name);
    }
    closedir(dir);

    std::sort(paths.begin(), paths.end(), [](const std::string &a, const std::string &b) {
        const auto parse_idx = [](const std::string &path) {
            const size_t pos = path.find("event");
            if (pos == std::string::npos)
                return -1;
            return std::atoi(path.c_str() + pos + 5);
        };
        return parse_idx(a) < parse_idx(b);
    });
    return paths;
}

bool try_open_touch_device(const std::string &path, bool explicit_choice)
{
    TouchDeviceInfo info;
    if (!probe_touch_device(path, &info))
    {
        std::fprintf(stderr, "face_netd_ui: probe input dev=%s failed: %s\n", path.c_str(),
                     info.probe_error.empty() ? "unknown error" : info.probe_error.c_str());
        return false;
    }

    if (!is_touch_candidate(info))
    {
        log_touch_caps("face_netd_ui: skip non-touch input", info);
        close_touch_device_info(&info);
        return false;
    }

    apply_touch_device_info(info);
    log_touch_caps("face_netd_ui: touch ok", info);
    return true;
}

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

    const int raw_min_x = g_port.have_abs_x_range ? g_port.raw_min_x : 0;
    const int raw_max_x = g_port.have_abs_x_range ? g_port.raw_max_x : (g_port.cfg.screen_width - 1);
    const int raw_min_y = g_port.have_abs_y_range ? g_port.raw_min_y : 0;
    const int raw_max_y = g_port.have_abs_y_range ? g_port.raw_max_y : (g_port.cfg.screen_height - 1);
    const int raw_span_x = std::max(1, raw_max_x - raw_min_x);
    const int raw_span_y = std::max(1, raw_max_y - raw_min_y);

    if (g_port.cfg.flip_x)
        mx = raw_max_x - (mx - raw_min_x);
    if (g_port.cfg.flip_y)
        my = raw_max_y - (my - raw_min_y);

    mx = std::max(raw_min_x, std::min(mx, raw_max_x));
    my = std::max(raw_min_y, std::min(my, raw_max_y));

    mx = static_cast<int>((static_cast<int64_t>(mx - raw_min_x) * (g_port.cfg.logical_width - 1)) / raw_span_x);
    my = static_cast<int>((static_cast<int64_t>(my - raw_min_y) * (g_port.cfg.logical_height - 1)) / raw_span_y);

    mx = std::max(0, std::min(mx, g_port.cfg.logical_width - 1));
    my = std::max(0, std::min(my, g_port.cfg.logical_height - 1));
    *x = mx;
    *y = my;
}

void apply_touch_key_state(int code, int value)
{
    if (code == BTN_TOUCH)
    {
        g_port.have_btn_touch = true;
        g_port.touch_state = value ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
    }
    else if (code == BTN_TOOL_FINGER)
    {
        g_port.have_btn_tool_finger = true;
        g_port.touch_state = value ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
    }
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
                else if (ev.code == ABS_MT_TRACKING_ID)
                {
                    g_port.have_mt_tracking = true;
                    g_port.touch_state = (ev.value < 0) ? LV_INDEV_STATE_REL : LV_INDEV_STATE_PR;
                }
                else if (ev.code == ABS_PRESSURE && !g_port.have_btn_touch && !g_port.have_btn_tool_finger)
                {
                    g_port.touch_state = (ev.value > 0) ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
                }
            }
            else if (ev.type == EV_KEY)
            {
                apply_touch_key_state(ev.code, ev.value);
            }
        }
    }

    int mapped_x = g_port.raw_touch_x;
    int mapped_y = g_port.raw_touch_y;
    map_touch_point(&mapped_x, &mapped_y);
    data->state = static_cast<lv_indev_state_t>(g_port.touch_state);
    data->point.x = mapped_x;
    data->point.y = mapped_y;

    if (g_port.touch_state == LV_INDEV_STATE_PR)
    {
        if (!g_port.touch_down_logged)
        {
            std::fprintf(stderr, "face_netd_ui: touch press raw=(%d,%d) mapped=(%d,%d)\n", g_port.raw_touch_x,
                         g_port.raw_touch_y, mapped_x, mapped_y);
            g_port.touch_down_logged = true;
        }
    }
    else
    {
        g_port.touch_down_logged = false;
    }
}

bool init_touch()
{
    const std::string configured = g_port.cfg.touch_device ? g_port.cfg.touch_device : "";
    if (!configured.empty() && configured != "auto")
    {
        if (try_open_touch_device(configured, true))
            return true;
        std::fprintf(stderr, "face_netd_ui: configured touch dev=%s unavailable, probing /dev/input/event*\n",
                     configured.c_str());
    }

    const std::vector<std::string> candidates = enumerate_input_event_nodes();
    if (candidates.empty())
        std::fprintf(stderr, "face_netd_ui: /dev/input has no event* nodes\n");
    else
        std::fprintf(stderr, "face_netd_ui: probing %zu input event nodes\n", candidates.size());
    for (const auto &path : candidates)
    {
        if (path == configured)
            continue;
        if (try_open_touch_device(path, false))
            return true;
    }

    std::fprintf(stderr, "face_netd_ui: no touch-capable /dev/input/event* device found\n");
    return false;
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
