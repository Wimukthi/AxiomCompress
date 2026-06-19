#include "gui/operation_runner.hpp"

#include <chrono>
#include <exception>
#include <mutex>
#include <string_view>
#include <utility>

namespace axiom::gui {
namespace {

constexpr auto kProgressPostInterval = std::chrono::milliseconds(33);

struct ProgressPostState {
    std::mutex mutex;
    std::chrono::steady_clock::time_point last_post{};
    OperationStage last_stage = OperationStage::reading;
    std::uint64_t last_completed_bytes = 0;
    bool has_posted = false;
};

std::wstring widen_error(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            text.data(), static_cast<int>(text.size()),
                                            nullptr, 0);
    if (length <= 0) {
        return L"Operation failed.";
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        text.data(), static_cast<int>(text.size()),
                        result.data(), length);
    return result;
}

}  // namespace

OperationRunner::~OperationRunner() {
    cancel();
    finish();
}

bool OperationRunner::start(HWND target,
                            UINT done_message,
                            UINT progress_message,
                            std::wstring running_label,
                            std::wstring success,
                            Work work) {
    if (running()) {
        return false;
    }
    finish();

    control_ = std::make_shared<OperationControl>();
    auto progress_state = std::make_shared<ProgressPostState>();
    control_->set_progress_callback([target, progress_message, progress_state](
                                        const OperationProgress& progress) {
        const auto now = std::chrono::steady_clock::now();
        bool should_post = false;
        {
            std::lock_guard lock(progress_state->mutex);
            const bool stage_changed = !progress_state->has_posted ||
                                       progress.stage != progress_state->last_stage;
            const bool completed = progress.total_bytes > 0 &&
                                   progress.completed_bytes >= progress.total_bytes &&
                                   progress.completed_bytes != progress_state->last_completed_bytes;
            const bool interval_elapsed = !progress_state->has_posted ||
                now - progress_state->last_post >= kProgressPostInterval;
            should_post = stage_changed || completed || interval_elapsed;
            if (should_post) {
                progress_state->last_post = now;
                progress_state->last_stage = progress.stage;
                progress_state->last_completed_bytes = progress.completed_bytes;
                progress_state->has_posted = true;
            }
        }
        if (!should_post) return;

        auto* copy = new OperationProgress(progress);
        if (!PostMessageW(target, progress_message, 0, reinterpret_cast<LPARAM>(copy))) {
            delete copy;
        }
    });

    auto operation = control_;
    worker_ = std::jthread([target,
                            done_message,
                            operation,
                            running = std::move(running_label),
                            success = std::move(success),
                            work = std::move(work)]() mutable {
        auto result = std::make_unique<OperationResult>();
        result->title = running;
        try {
            work(operation);
            result->ok = true;
            result->message = std::move(success);
        } catch (const OperationCancelled&) {
            result->cancelled = true;
            result->message = L"Operation cancelled.";
        } catch (const std::exception& error) {
            result->message = widen_error(error.what());
        } catch (...) {
            result->message = L"Unknown failure.";
        }

        OperationResult* payload = result.release();
        if (!PostMessageW(target, done_message, 0, reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    });
    return true;
}

void OperationRunner::set_paused(bool paused) {
    if (control_) {
        control_->set_paused(paused);
    }
}

void OperationRunner::cancel() {
    if (control_) {
        control_->request_cancel();
    }
}

void OperationRunner::finish() {
    if (worker_.joinable()) {
        worker_.join();
    }
    control_.reset();
}

bool OperationRunner::running() const {
    return control_ != nullptr;
}

}  // namespace axiom::gui
