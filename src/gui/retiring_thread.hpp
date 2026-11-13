#pragma once

#include <atomic>
#include <memory>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace axiom::gui {

// A worker slot whose replacement never waits on the UI thread.
//
// Assigning a new std::jthread over a running one requests a stop and then
// joins it, so starting a folder load while a slow archive open is still
// running froze the window until that open finished (an archive open cannot be
// interrupted). start() instead asks the running worker to stop and sets it
// aside; workers that have since finished are joined on the next start(), which
// is immediate. Only join_all() and the destructor wait, at shutdown.
//
// The work must not touch its owner: it reports by posting to a window, and
// whoever handles that message discards results from a superseded generation.
class RetiringThread {
public:
    RetiringThread() = default;
    RetiringThread(const RetiringThread&) = delete;
    RetiringThread& operator=(const RetiringThread&) = delete;

    ~RetiringThread() { join_all(); }

    template <typename Work>
    void start(Work&& work) {
        retire_current();
        reap_finished();
        auto done = std::make_shared<std::atomic<bool>>(false);
        current_.done = done;
        current_.thread = std::jthread(
            [work = std::forward<Work>(work), done](std::stop_token stop) mutable {
                struct MarkDone {
                    std::atomic<bool>& flag;
                    ~MarkDone() { flag.store(true, std::memory_order_release); }
                } mark{*done};
                work(stop);
            });
    }

    void request_stop() {
        current_.thread.request_stop();
        for (auto& worker : retired_) worker.thread.request_stop();
    }

    void join_all() {
        request_stop();
        if (current_.thread.joinable()) current_.thread.join();
        for (auto& worker : retired_) {
            if (worker.thread.joinable()) worker.thread.join();
        }
        retired_.clear();
    }

private:
    struct Worker {
        std::jthread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    void retire_current() {
        if (!current_.thread.joinable()) return;
        current_.thread.request_stop();
        retired_.push_back(std::move(current_));
        current_ = {};
    }

    void reap_finished() {
        std::erase_if(retired_, [](Worker& worker) {
            if (!worker.done || !worker.done->load(std::memory_order_acquire)) return false;
            if (worker.thread.joinable()) worker.thread.join();
            return true;
        });
    }

    Worker current_;
    std::vector<Worker> retired_;
};

}  // namespace axiom::gui
