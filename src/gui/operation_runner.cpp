#include "gui/operation_runner.hpp"

#include <exception>
#include <string_view>
#include <utility>

namespace axiom::gui {
namespace {

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
    control_->set_progress_callback([target, progress_message](const OperationProgress& progress) {
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
