#pragma once

#include "gui/archive_feature_options.hpp"
#include "gui/browser_model.hpp"

#include <windows.h>

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace axiom::gui {

bool show_archive_password_dialog(HWND owner, std::wstring& password);
bool show_archive_comment_dialog(HWND owner, std::wstring& comment);

using ArchiveSummaryRows = std::vector<std::pair<std::wstring, std::wstring>>;

void show_archive_information_dialog(
    HWND owner,
    const std::filesystem::path& archive_path,
    const ArchiveSummaryRows& details,
    const ArchiveCapabilities& capabilities,
    std::wstring archive_comment = {},
    std::function<void(HWND)> estimate_action = {});

void show_information_summary_dialog(
    HWND owner,
    std::wstring heading,
    std::wstring subheading,
    ArchiveSummaryRows details);

void show_archive_feature_summary_dialog(
    HWND owner,
    const std::filesystem::path& archive_path,
    const ArchiveCapabilities& capabilities);

}  // namespace axiom::gui
