#pragma once

#include <windows.h>

#include <string>

namespace axiom::gui {

constexpr UINT kUpdateCheckCompleteMessage = WM_APP + 304;
constexpr UINT kUpdateDownloadCompleteMessage = WM_APP + 305;

enum class UpdateCheckKind {
    manual,
    automatic,
};

struct UpdateInfo {
    std::wstring version;
    std::wstring asset_name;
    std::wstring download_url;
    std::wstring digest;
    std::wstring release_url;
};

struct UpdateCheckResult {
    UpdateCheckKind kind{UpdateCheckKind::manual};
    bool success{};
    bool update_available{};
    UpdateInfo update;
    std::wstring message;
};

struct UpdateDownloadResult {
    bool success{};
    UpdateInfo update;
    std::wstring installer_path;
    std::wstring message;
};

bool automatic_update_checks_enabled();
void set_automatic_update_checks_enabled(bool enabled);
bool automatic_update_check_due();

std::wstring current_executable_version(HINSTANCE instance);
void start_update_check(HWND notify_window, HINSTANCE instance, UpdateCheckKind kind);
void start_update_download(HWND notify_window, UpdateInfo update);

}  // namespace axiom::gui

