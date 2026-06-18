#pragma once

#include <windows.h>

#include <filesystem>
#include <thread>

namespace axiom::gui {

class DirectoryWatcher {
public:
    DirectoryWatcher() = default;
    ~DirectoryWatcher();

    DirectoryWatcher(const DirectoryWatcher&) = delete;
    DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;

    void start(const std::filesystem::path& directory, HWND target, UINT changed_message);
    void stop();

private:
    HANDLE directory_ = INVALID_HANDLE_VALUE;
    std::jthread thread_;
};

}  // namespace axiom::gui
