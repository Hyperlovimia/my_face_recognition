#ifndef MY_FACE_DOOR_CONTROL_H
#define MY_FACE_DOOR_CONTROL_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "ipc_proto.h"

class DoorControl
{
public:
    DoorControl();
    ~DoorControl();

    bool init(const std::string &log_base);
    void handle_ipc_event(const ipc_evt_t &ev);
    void shutdown();

    bool enabled() const;
    const char *state_name() const;

private:
    enum class State
    {
        DISABLED = 0,
        LOCKED_READY,
        UNLOCKED,
        FAULT,
    };

    struct UnlockRequest
    {
        int face_id = -1;
        float score = 0.0f;
        std::string name;
    };

    void worker_loop();
    void perform_unlock(const UnlockRequest &req);
    void perform_relock(const char *reason_tag);

    bool open_gpio_device();
    bool configure_outputs();
    bool set_outputs(bool active, std::string *error_out);
    bool set_single_pin(int pin, bool active_high, bool active, std::string *error_out);
    bool read_single_pin(int pin, int *value_out, std::string *error_out);
    void best_effort_safe_lock();
    void enter_fault(const std::string &reason);
    void log_meta(const std::string &note) const;

    static bool pin_valid(int pin);
    static const char *state_name(State state);

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::thread worker_;

    std::string log_base_;
    int gpio_fd_ = -1;
    int relay_pin_ = -1;
    int buzzer_pin_ = -1;
    int hold_ms_ = 3000;
    bool relay_active_high_ = true;
    bool buzzer_active_high_ = true;
    bool verify_readback_ = true;
    bool stop_requested_ = false;
    bool pending_unlock_ = false;
    bool worker_started_ = false;
    UnlockRequest pending_req_{};
    std::chrono::steady_clock::time_point unlock_deadline_{};
    State state_ = State::DISABLED;
};

#endif
