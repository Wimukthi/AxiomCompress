#pragma once

#include "axiom/axiom.hpp"

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace axiom::gui {

struct OperationResult {
    bool ok = false;
    bool cancelled = false;
    std::wstring title;
    std::wstring message;
};

class OperationRunner {
public:
    using Work = std::function<void(std::shared_ptr<OperationControl>)>;

    OperationRunner() = default;
    ~OperationRunner();

    OperationRunner(const OperationRunner&) = delete;
    OperationRunner& operator=(const OperationRunner&) = delete;

    bool start(HWND target,
               UINT done_message,
               UINT progress_message,
               std::wstring running_label,
               std::wstring success,
               Work work);
    void set_paused(bool paused);
    void cancel();
    void finish();

    bool running() const;

private:
    std::shared_ptr<OperationControl> control_;
    std::jthread worker_;
};

}  // namespace axiom::gui
