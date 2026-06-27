#pragma once

#include "gui/archive_feature_options.hpp"
#include "gui/browser_model.hpp"

#include <windows.h>

#include <filesystem>

namespace axiom::gui {

enum class ArchiveFeatureDialogContext {
    create_or_update,
    extract,
    password_prompt,
    comment,
};

bool show_archive_feature_options_dialog(
    HWND owner,
    ArchiveFeatureDialogContext context,
    ArchiveFeatureOptions& archive_options,
    ExtractFeatureOptions& extract_options,
    const ArchiveFeatureAvailability& availability = {},
    std::wstring suggested_sfx_output = {});

bool show_archive_password_dialog(HWND owner, std::wstring& password);
bool show_archive_comment_dialog(HWND owner, std::wstring& comment);

void show_archive_feature_summary_dialog(
    HWND owner,
    const std::filesystem::path& archive_path,
    const ArchiveCapabilities& capabilities);

}  // namespace axiom::gui
