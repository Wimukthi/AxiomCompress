#pragma once

#include "axiom/axiom.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <utility>

namespace axiom {

// Shared throughput estimator, so every frontend means the same thing by
// "speed". Two rules matter here, and both come from real misreporting:
//
//  * Prefer `OperationProgress::throughput_bytes`. Archive creation advances
//    it while assembling its first solid block, at a point where
//    `completed_bytes` is still zero -- reading `completed_bytes` there
//    reports 0 B/s during a visibly busy read.
//  * Measure over a trailing window rather than dividing totals by total
//    elapsed time. A cumulative average keeps reporting a long-dead figure
//    after the pipeline changes speed, and it never recovers within one
//    operation.
class ProgressRateTracker {
public:
    static std::uint64_t counter(const OperationProgress& progress) {
        return progress.throughput_bytes != 0 ? progress.throughput_bytes
                                              : progress.completed_bytes;
    }

    // Returns bytes per second, or 0 while the window is still filling.
    double update(const OperationProgress& progress,
                  std::chrono::steady_clock::time_point now) {
        const std::uint64_t sample = counter(progress);
        // Archive pipelines legitimately alternate between compressing and
        // writing while the same counter advances. Resetting on a stage flip
        // collapsed the window to a single point and displayed 0 B/s despite
        // visible progress, so only a counter restart or a new total starts a
        // fresh epoch.
        const bool new_epoch = !started_ || sample < last_sample_ ||
                               progress.total_bytes != last_total_;
        started_ = true;
        last_sample_ = sample;
        last_total_ = progress.total_bytes;
        if (new_epoch) {
            samples_.clear();
            rate_ = 0.0;
        }
        if (samples_.empty() || samples_.back().second != sample) {
            samples_.emplace_back(now, sample);
        }
        while (samples_.size() > 2 && now - samples_.front().first > kWindow) {
            samples_.pop_front();
        }
        if (samples_.size() >= 2) {
            const double seconds =
                std::chrono::duration<double>(samples_.back().first -
                                              samples_.front().first)
                    .count();
            const std::uint64_t first = samples_.front().second;
            const std::uint64_t last = samples_.back().second;
            if (seconds > 0.1 && last >= first) {
                rate_ = static_cast<double>(last - first) / seconds;
            }
        }
        return rate_;
    }

    [[nodiscard]] double rate() const { return rate_; }

    void reset() {
        samples_.clear();
        rate_ = 0.0;
        last_sample_ = 0;
        last_total_ = 0;
        started_ = false;
    }

private:
    static constexpr std::chrono::seconds kWindow{4};

    std::deque<std::pair<std::chrono::steady_clock::time_point, std::uint64_t>>
        samples_;
    double rate_ = 0.0;
    std::uint64_t last_sample_ = 0;
    std::uint64_t last_total_ = 0;
    bool started_ = false;
};

}  // namespace axiom
