#include "door_control.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstring>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "attendance_log.h"
#include "door_control_config.h"

namespace
{

constexpr char k_gpio_dev_path[] = "/dev/gpio";
constexpr char k_mem_dev_path[] = "/dev/mem";
constexpr int k_max_gpio_pin = 71;
constexpr off_t k_iomux_base_addr = 0x91105000;
constexpr size_t k_iomux_map_size = 0x1000;
constexpr uint32_t k_iomux_sel_shift = 11;
constexpr uint32_t k_iomux_sel_mask = (0x7u << k_iomux_sel_shift);
constexpr uint32_t k_iomux_oe_mask = (0x1u << 7);
constexpr uint32_t k_iomux_ie_mask = (0x1u << 8);
constexpr uint32_t k_iomux_gpio_func = 0u;

/* Local mirror of RT-Smart /dev/gpio userspace ABI. */
typedef struct
{
    unsigned short pin;
    unsigned short value;
} gpio_cfg_t;

#define KD_GPIO_DM_OUTPUT _IOW('G', 0, int)
#define KD_GPIO_WRITE_LOW _IOW('G', 4, int)
#define KD_GPIO_WRITE_HIGH _IOW('G', 5, int)
#define KD_GPIO_READ_VALUE _IOW('G', 12, int)

static int gpio_level_for(bool active, bool active_high)
{
    return ((active ? 1 : 0) == (active_high ? 1 : 0)) ? 1 : 0;
}

static std::string safe_name(const std::string &name)
{
    return name.empty() ? "unknown" : name;
}

static bool output_enabled(int pin)
{
    return pin >= 0;
}

} // namespace

DoorControl::DoorControl()
{
    relay_pin_ = FACE_DOOR_RELAY_PIN;
    buzzer_pin_ = FACE_DOOR_BUZZER_PIN;
    relay_active_high_ = (FACE_DOOR_RELAY_ACTIVE_HIGH != 0);
    buzzer_active_high_ = (FACE_DOOR_BUZZER_ACTIVE_HIGH != 0);
    hold_ms_ = (FACE_DOOR_HOLD_MS > 0) ? FACE_DOOR_HOLD_MS : 3000;
    verify_readback_ = (FACE_DOOR_VERIFY_READBACK != 0);
    state_ = (FACE_DOOR_ENABLE != 0) ? State::LOCKED_READY : State::DISABLED;
}

DoorControl::~DoorControl()
{
    shutdown();
}

bool DoorControl::init(const std::string &log_base)
{
    log_base_ = log_base;

    if (FACE_DOOR_ENABLE == 0)
    {
        state_ = State::DISABLED;
        std::cout << "face_event: door control disabled at build time\n";
        log_meta("door_control_disabled");
        return true;
    }

    if (!pin_valid(relay_pin_))
    {
        enter_fault("invalid relay pin");
        return false;
    }

    if (output_enabled(buzzer_pin_) && !pin_valid(buzzer_pin_))
    {
        enter_fault("invalid buzzer pin");
        return false;
    }

    if (!open_gpio_device())
        return false;

    std::string error;
    if (!configure_iomux_gpio(relay_pin_, &error))
    {
        enter_fault("configure relay iomux failed: " + error);
        return false;
    }

    if (output_enabled(buzzer_pin_) && !configure_iomux_gpio(buzzer_pin_, &error))
    {
        enter_fault("configure aux iomux failed: " + error);
        return false;
    }

    if (!configure_outputs())
        return false;

    error.clear();
    if (!set_outputs(false, &error))
    {
        enter_fault("initial safe lock failed: " + error);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != State::FAULT)
            state_ = State::LOCKED_READY;
    }

    worker_ = std::thread(&DoorControl::worker_loop, this);
    worker_started_ = true;

    std::cout << "face_event: door control ready door_gpio=" << relay_pin_ << " active="
              << (relay_active_high_ ? "high" : "low");
    if (output_enabled(buzzer_pin_))
        std::cout << " aux_gpio=" << buzzer_pin_ << " aux_active=" << (buzzer_active_high_ ? "high" : "low");
    else
        std::cout << " aux_gpio=disabled";
    std::cout << " hold_ms=" << hold_ms_ << " verify_readback=" << (verify_readback_ ? "yes" : "no")
              << std::endl;
    log_meta("door_control_ready");
    return true;
}

void DoorControl::handle_ipc_event(const ipc_evt_t &ev)
{
    if (FACE_DOOR_ENABLE == 0 || ev.magic != IPC_MAGIC || ev.evt_kind != IPC_EVT_KIND_RECOGNIZED)
        return;

    UnlockRequest req;
    req.face_id = ev.face_id;
    req.score = ev.score;
    if (ev.name[0] != '\0')
        req.name = ev.name;

    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ == State::DISABLED)
            return;
        if (state_ == State::FAULT)
        {
            std::cout << "face_event: door control faulted, ignore recognized event name=" << ev.name << std::endl;
            return;
        }
        if (state_ == State::UNLOCKED)
        {
            std::cout << "face_event: door already unlocked, ignore recognized event name=" << ev.name << std::endl;
            return;
        }
        pending_req_ = req;
        pending_unlock_ = true;
    }

    cv_.notify_all();
}

void DoorControl::shutdown()
{
    bool joined = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_requested_ && gpio_fd_ < 0 && !worker_started_)
            return;
        stop_requested_ = true;
    }
    cv_.notify_all();

    if (worker_started_ && worker_.joinable())
    {
        worker_.join();
        joined = true;
    }
    worker_started_ = false;

    if (gpio_fd_ >= 0)
    {
        best_effort_safe_lock();
        close(gpio_fd_);
        gpio_fd_ = -1;
    }

    if (joined)
        std::cout << "face_event: door control stopped\n";
}

bool DoorControl::enabled() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return state_ != State::DISABLED;
}

const char *DoorControl::state_name() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return state_name(state_);
}

void DoorControl::worker_loop()
{
    for (;;)
    {
        UnlockRequest req;

        std::unique_lock<std::mutex> lk(mu_);
        if (!pending_unlock_ && state_ != State::UNLOCKED && !stop_requested_)
            cv_.wait(lk, [this]() { return stop_requested_ || pending_unlock_ || state_ == State::UNLOCKED; });

        if (stop_requested_)
            break;

        if (pending_unlock_)
        {
            req = pending_req_;
            pending_req_ = UnlockRequest{};
            pending_unlock_ = false;
            lk.unlock();
            perform_unlock(req);
            continue;
        }

        if (state_ == State::UNLOCKED)
        {
            const auto deadline = unlock_deadline_;
            if (cv_.wait_until(lk, deadline,
                               [this, deadline]() {
                                   return stop_requested_ || pending_unlock_ || state_ != State::UNLOCKED ||
                                          unlock_deadline_ != deadline;
                               }))
                continue;
            lk.unlock();
            perform_relock("timeout");
        }
    }
}

void DoorControl::perform_unlock(const UnlockRequest &req)
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_requested_ || state_ == State::FAULT || state_ == State::UNLOCKED)
            return;
    }

    std::string error;
    if (!set_outputs(true, &error))
    {
        enter_fault("unlock failed: " + error);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        state_ = State::UNLOCKED;
        unlock_deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(hold_ms_);
    }

    std::ostringstream note;
    note << "door_unlock name=" << safe_name(req.name) << " face_id=" << req.face_id << " score=" << std::fixed
         << std::setprecision(2) << req.score << " hold_ms=" << hold_ms_;
    log_meta(note.str());
    std::cout << "face_event: door unlock name=" << safe_name(req.name) << " face_id=" << req.face_id
              << " score=" << std::fixed << std::setprecision(2) << req.score << " hold_ms=" << hold_ms_
              << std::endl;
}

void DoorControl::perform_relock(const char *reason_tag)
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != State::UNLOCKED)
            return;
    }

    std::string error;
    if (!set_outputs(false, &error))
    {
        enter_fault(std::string("relock failed: ") + error);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        state_ = State::LOCKED_READY;
    }

    std::ostringstream note;
    note << "door_relock reason=" << (reason_tag ? reason_tag : "unknown");
    log_meta(note.str());
    std::cout << "face_event: door relock reason=" << (reason_tag ? reason_tag : "unknown") << std::endl;
}

bool DoorControl::open_gpio_device()
{
    gpio_fd_ = open(k_gpio_dev_path, O_RDWR);
    if (gpio_fd_ >= 0)
        return true;

    std::ostringstream oss;
    oss << "open " << k_gpio_dev_path << " failed errno=" << errno;
    enter_fault(oss.str());
    return false;
}

bool DoorControl::configure_iomux_gpio(int pin, std::string *error_out)
{
    if (!output_enabled(pin) || FACE_DOOR_FORCE_IOMUX_GPIO == 0)
        return true;

    const int mem_fd = open(k_mem_dev_path, O_RDWR | O_SYNC);
    if (mem_fd < 0)
    {
        if (error_out)
        {
            std::ostringstream oss;
            oss << "open " << k_mem_dev_path << " failed errno=" << errno;
            *error_out = oss.str();
        }
        return false;
    }

    void *mapped = mmap(nullptr, k_iomux_map_size, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, k_iomux_base_addr);
    if (mapped == MAP_FAILED)
    {
        if (error_out)
        {
            std::ostringstream oss;
            oss << "mmap iomux failed errno=" << errno;
            *error_out = oss.str();
        }
        close(mem_fd);
        return false;
    }

    volatile uint32_t *regs = reinterpret_cast<volatile uint32_t *>(mapped);
    const size_t reg_idx = static_cast<size_t>(pin);
    const uint32_t old_val = regs[reg_idx];
    uint32_t new_val = old_val;
    new_val &= ~k_iomux_sel_mask;
    new_val |= (k_iomux_gpio_func << k_iomux_sel_shift);
    new_val |= k_iomux_oe_mask;
    new_val |= k_iomux_ie_mask;
    regs[reg_idx] = new_val;
    const uint32_t verify_val = regs[reg_idx];

    munmap(mapped, k_iomux_map_size);
    close(mem_fd);

    if ((verify_val & k_iomux_sel_mask) != 0)
    {
        if (error_out)
        {
            std::ostringstream oss;
            oss << "iomux verify failed pin=" << pin << " reg=0x" << std::hex << verify_val;
            *error_out = oss.str();
        }
        return false;
    }

    std::cout << "face_event: iomux pin " << pin << " forced to gpio old=0x" << std::hex << old_val
              << " new=0x" << verify_val << std::dec << std::endl;
    return true;
}

bool DoorControl::configure_outputs()
{
    gpio_cfg_t cfg{};

    cfg.pin = static_cast<unsigned short>(relay_pin_);
    cfg.value = 0;
    if (ioctl(gpio_fd_, KD_GPIO_DM_OUTPUT, &cfg) != 0)
    {
        std::ostringstream oss;
        oss << "set relay output mode failed errno=" << errno;
        enter_fault(oss.str());
        return false;
    }

    if (output_enabled(buzzer_pin_) && buzzer_pin_ != relay_pin_)
    {
        cfg.pin = static_cast<unsigned short>(buzzer_pin_);
        if (ioctl(gpio_fd_, KD_GPIO_DM_OUTPUT, &cfg) != 0)
        {
            std::ostringstream oss;
            oss << "set buzzer output mode failed errno=" << errno;
            enter_fault(oss.str());
            return false;
        }
    }

    return true;
}

bool DoorControl::set_outputs(bool active, std::string *error_out)
{
    if (!set_single_pin(relay_pin_, relay_active_high_, active, error_out))
        return false;

    if (output_enabled(buzzer_pin_) && buzzer_pin_ != relay_pin_ &&
        !set_single_pin(buzzer_pin_, buzzer_active_high_, active, error_out))
        return false;

    return true;
}

bool DoorControl::set_single_pin(int pin, bool active_high, bool active, std::string *error_out)
{
    gpio_cfg_t cfg{};
    cfg.pin = static_cast<unsigned short>(pin);
    cfg.value = 0;

    const int expected = gpio_level_for(active, active_high);
    const unsigned long cmd = expected ? KD_GPIO_WRITE_HIGH : KD_GPIO_WRITE_LOW;
    if (ioctl(gpio_fd_, cmd, &cfg) != 0)
    {
        if (error_out)
        {
            std::ostringstream oss;
            oss << "write pin " << pin << " failed errno=" << errno;
            *error_out = oss.str();
        }
        return false;
    }

    if (!verify_readback_)
        return true;

    int actual = -1;
    if (!read_single_pin(pin, &actual, error_out))
        return false;

    if (actual != expected)
    {
        if (error_out)
        {
            std::ostringstream oss;
            oss << "pin " << pin << " readback mismatch expect=" << expected << " actual=" << actual;
            *error_out = oss.str();
        }
        return false;
    }

    return true;
}

bool DoorControl::read_single_pin(int pin, int *value_out, std::string *error_out)
{
    if (!value_out)
        return false;

    gpio_cfg_t cfg{};
    cfg.pin = static_cast<unsigned short>(pin);
    cfg.value = 0;
    if (ioctl(gpio_fd_, KD_GPIO_READ_VALUE, &cfg) != 0)
    {
        if (error_out)
        {
            std::ostringstream oss;
            oss << "read pin " << pin << " failed errno=" << errno;
            *error_out = oss.str();
        }
        return false;
    }

    *value_out = static_cast<int>(cfg.value);
    return true;
}

void DoorControl::best_effort_safe_lock()
{
    if (gpio_fd_ < 0)
        return;

    std::string ignored;
    if (!set_outputs(false, &ignored))
        std::cerr << "face_event: door control safe lock failed during cleanup: " << ignored << std::endl;
}

void DoorControl::enter_fault(const std::string &reason)
{
    bool already_fault = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        already_fault = (state_ == State::FAULT);
        state_ = State::FAULT;
        pending_unlock_ = false;
    }

    if (gpio_fd_ >= 0)
        best_effort_safe_lock();

    if (!already_fault)
    {
        std::cerr << "face_event: door control fault: " << reason << std::endl;
        log_meta(std::string("door_fault reason=") + reason);
    }
}

void DoorControl::log_meta(const std::string &note) const
{
    if (log_base_.empty())
        return;
    attendance_log_append_meta(log_base_, note.c_str(), nullptr);
}

bool DoorControl::pin_valid(int pin)
{
    return pin >= 0 && pin <= k_max_gpio_pin;
}

const char *DoorControl::state_name(State state)
{
    switch (state)
    {
    case State::DISABLED:
        return "disabled";
    case State::LOCKED_READY:
        return "locked_ready";
    case State::UNLOCKED:
        return "unlocked";
    case State::FAULT:
        return "fault";
    default:
        return "unknown";
    }
}
