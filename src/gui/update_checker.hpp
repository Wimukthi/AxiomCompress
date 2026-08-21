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

// Re-hashes the downloaded installer on disk and compares it against the digest
// the release published. The download is staged under LocalAppData, which the
// user (and anything running as the user) can write, and it is launched
// elevated only after a confirmation prompt of unbounded duration. Verifying
// the bytes again immediately before that launch closes the window between the
// download check and the elevation.
bool verify_installer_file(const std::wstring& installer_path,
                           const std::wstring& expected_digest, std::wstring& error);

}  // namespace axiom::gui

