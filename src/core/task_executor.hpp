#pragma once

#include "core/cpu.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace axiom::core {

// Shared cooperative work-stealing executor for nested codec work. The calling
// thread counts as worker zero; workers waiting on child tasks drain their own
// deque or steal from peers, so block tasks can fan out without nested pools or
// deadlocking. Queue count is derived from size_t and has no fixed CPU ceiling.
class TaskExecutor {
public:
    explicit TaskExecutor(std::size_t thread_count)
        : queues_(std::max<std::size_t>(1, thread_count)) {
        for (auto& queue : queues_) {
            queue = std::make_unique<WorkerQueue>();
        }

        previous_executor_ = current_executor_;
        previous_worker_ = current_worker_;
        current_executor_ = this;
        current_worker_ = 0;

        helpers_.reserve(queues_.size() - 1);
        for (std::size_t i = 1; i < queues_.size(); ++i) {
            helpers_.emplace_back([this, i] { worker_loop(i); });
        }
    }

    TaskExecutor(const TaskExecutor&) = delete;
    TaskExecutor& operator=(const TaskExecutor&) = delete;

    ~TaskExecutor() {
        stopping_.store(true, std::memory_order_release);
        ready_.notify_all();
        for (auto& helper : helpers_) {
            helper.join();
        }
        current_executor_ = previous_executor_;
        current_worker_ = previous_worker_;
    }

    // The executor owning the calling thread (the constructing thread counts
    // as worker zero), or nullptr outside any executor. Nested codec stages
    // use this to join the operation's budget instead of spawning a pool.
    static TaskExecutor* current() {
        return current_executor_;
    }

    std::size_t worker_count() const noexcept {
        return queues_.size();
    }

    template <typename Function>
    auto submit(Function&& function)
        -> std::future<std::invoke_result_t<std::decay_t<Function>>> {
        using Result = std::invoke_result_t<std::decay_t<Function>>;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            std::forward<Function>(function));
        auto future = task->get_future();
        if (stopping_.load(std::memory_order_acquire)) {
            throw std::runtime_error("task submitted after executor shutdown");
        }

        auto wrapper = [task] { (*task)(); };
        if (current_executor_ == this) {
            auto& queue = *queues_[current_worker_];
            std::lock_guard lock(queue.mutex);
            queue.tasks.emplace_back(std::move(wrapper));
            // Publish the count before releasing the queue lock. A stealing
            // worker must never remove a visible task while pending_ is still
            // zero and underflow the counter.
            pending_.fetch_add(1, std::memory_order_release);
        } else {
            std::lock_guard lock(inject_mutex_);
            injected_.emplace_back(std::move(wrapper));
            pending_.fetch_add(1, std::memory_order_release);
        }
        ready_.notify_one();
        return future;
    }

    template <typename Result>
    Result wait(std::future<Result>& future) {
        while (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            if (!run_one(active_worker())) {
                wait_for_work();
            }
        }
        return future.get();
    }

    void wait(std::future<void>& future) {
        while (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            if (!run_one(active_worker())) {
                wait_for_work();
            }
        }
        future.get();
    }

private:
    struct WorkerQueue {
        std::mutex mutex;
        std::deque<std::function<void()>> tasks;
    };

    std::size_t active_worker() const noexcept {
        return current_executor_ == this ? current_worker_ : queues_.size();
    }

    bool pop_local(std::size_t worker, std::function<void()>& task) {
        if (worker >= queues_.size()) {
            return false;
        }
        auto& queue = *queues_[worker];
        std::lock_guard lock(queue.mutex);
        if (queue.tasks.empty()) {
            return false;
        }
        task = std::move(queue.tasks.back());
        queue.tasks.pop_back();
        return true;
    }

    bool pop_injected(std::function<void()>& task) {
        std::lock_guard lock(inject_mutex_);
        if (injected_.empty()) {
            return false;
        }
        task = std::move(injected_.front());
        injected_.pop_front();
        return true;
    }

    bool steal(std::size_t worker, std::function<void()>& task) {
        const auto count = queues_.size();
        const auto start = steal_cursor_.fetch_add(1, std::memory_order_relaxed);
        for (std::size_t offset = 0; offset < count; ++offset) {
            const auto victim = (start + offset) % count;
            if (victim == worker) {
                continue;
            }
            auto& queue = *queues_[victim];
            std::lock_guard lock(queue.mutex);
            if (!queue.tasks.empty()) {
                task = std::move(queue.tasks.front());
                queue.tasks.pop_front();
                return true;
            }
        }
        return false;
    }

    bool run_one(std::size_t worker) {
        std::function<void()> task;
        if (!pop_local(worker, task) && !pop_injected(task) && !steal(worker, task)) {
            return false;
        }
        pending_.fetch_sub(1, std::memory_order_acq_rel);
        task();
        return true;
    }

    void wait_for_work() {
        std::unique_lock lock(wait_mutex_);
        ready_.wait_for(lock, std::chrono::microseconds(100), [this] {
            return stopping_.load(std::memory_order_acquire) ||
                   pending_.load(std::memory_order_acquire) != 0;
        });
    }

    void worker_loop(std::size_t worker) {
        // Windows otherwise keeps a thread inside its inherited processor
        // group on large machines. Distribute helpers proportionally across
        // all groups; single-group Intel/AMD systems take the no-op fast path.
        bind_current_thread_to_processor_group(worker);
        current_executor_ = this;
        current_worker_ = worker;
        while (true) {
            if (run_one(worker)) {
                continue;
            }
            if (stopping_.load(std::memory_order_acquire) &&
                pending_.load(std::memory_order_acquire) == 0) {
                break;
            }
            wait_for_work();
        }
        current_executor_ = nullptr;
        current_worker_ = 0;
    }

    std::vector<std::unique_ptr<WorkerQueue>> queues_;
    std::vector<std::thread> helpers_;
    std::mutex inject_mutex_;
    std::deque<std::function<void()>> injected_;
    std::mutex wait_mutex_;
    std::condition_variable ready_;
    std::atomic_size_t pending_{0};
    std::atomic_size_t steal_cursor_{0};
    std::atomic_bool stopping_{false};
    TaskExecutor* previous_executor_ = nullptr;
    std::size_t previous_worker_ = 0;

    inline static thread_local TaskExecutor* current_executor_ = nullptr;
    inline static thread_local std::size_t current_worker_ = 0;
};

}  // namespace axiom::core
