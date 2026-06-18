#define NOMINMAX
#include "gui/directory_watcher.hpp"

#include <array>
#include <cstddef>

namespace axiom::gui {

DirectoryWatcher::~DirectoryWatcher() {
    stop();
}

void DirectoryWatcher::start(const std::filesystem::path& directory,
                             HWND target,
                             UINT changed_message) {
    stop();
    directory_ = CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory_ == INVALID_HANDLE_VALUE) return;

    HANDLE handle = directory_;
    thread_ = std::jthread([handle, target, changed_message](std::stop_token stop) {
        std::array<std::byte, 64 * 1024> buffer{};
        while (!stop.stop_requested()) {
            DWORD bytes = 0;
            const BOOL ok = ReadDirectoryChangesW(
                handle, buffer.data(), static_cast<DWORD>(buffer.size()), FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                    FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                    FILE_NOTIFY_CHANGE_ATTRIBUTES,
                &bytes, nullptr, nullptr);
            if (!ok || stop.stop_requested()) break;
            if (bytes != 0) PostMessageW(target, changed_message, 0, 0);
        }
    });
}

void DirectoryWatcher::stop() {
    if (directory_ != INVALID_HANDLE_VALUE) {
        thread_.request_stop();
        CancelIoEx(directory_, nullptr);
        if (thread_.joinable()) thread_.join();
        CloseHandle(directory_);
        directory_ = INVALID_HANDLE_VALUE;
    } else if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

}  // namespace axiom::gui
