#ifndef MY_FACE_PERF_STATS_H
#define MY_FACE_PERF_STATS_H

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>

class PerfStageStats
{
public:
    explicit PerfStageStats(std::string tag, size_t report_every = 300)
        : tag_(std::move(tag)), report_every_(report_every)
    {
    }

    void add_ms(double elapsed_ms, bool enabled)
    {
        if (!enabled)
            return;

        std::lock_guard<std::mutex> lk(mu_);
        ++window_count_;
        window_sum_ms_ += elapsed_ms;
        window_max_ms_ = std::max(window_max_ms_, elapsed_ms);

        if (window_count_ < report_every_)
            return;

        const double avg_ms = window_sum_ms_ / static_cast<double>(window_count_);
        std::cerr << "[" << tag_ << "] avg_ms=" << avg_ms << " max_ms=" << window_max_ms_
                  << " samples=" << window_count_ << std::endl;
        window_count_ = 0;
        window_sum_ms_ = 0.0;
        window_max_ms_ = 0.0;
    }

    template <class ClockPoint>
    void track_since(const ClockPoint &start, bool enabled)
    {
        if (!enabled)
            return;
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        add_ms(elapsed_ms, true);
    }

private:
    std::mutex mu_;
    std::string tag_;
    size_t report_every_;
    size_t window_count_ = 0;
    double window_sum_ms_ = 0.0;
    double window_max_ms_ = 0.0;
};

class ScopedPerfStage
{
public:
    ScopedPerfStage(PerfStageStats &stats, bool enabled)
        : stats_(&stats), enabled_(enabled)
    {
        if (enabled_) {
            start_ = std::chrono::steady_clock::now();
        }
    }

    ~ScopedPerfStage()
    {
        if (enabled_) {
            stats_->track_since(start_, true);
        }
    }

private:
    PerfStageStats *stats_;
    bool enabled_;
    std::chrono::steady_clock::time_point start_;
};

#endif
