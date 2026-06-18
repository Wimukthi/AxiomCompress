#pragma once

#include <windows.h>

#include <cstddef>
#include <filesystem>

namespace axiom::gui {

struct CreateArchiveDialogOptions {
    std::filesystem::path archive_path;
    int level = 5;
    std::size_t thread_count = 0;
};

struct ExtractArchiveDialogOptions {
    std::filesystem::path destination;
    std::size_t thread_count = 0;
    bool overwrite = false;
    bool restore_mtime = true;
};

struct ApplicationDialogOptions {
    int default_level = 5;
    std::size_t default_thread_count = 0;
    bool confirm_delete = true;
    bool show_hidden = true;
};

bool show_create_archive_dialog(HWND owner,
                                std::size_t input_count,
                                CreateArchiveDialogOptions& options);
bool show_extract_archive_dialog(HWND owner,
                                 const std::filesystem::path& archive_path,
                                 ExtractArchiveDialogOptions& options);
bool show_application_settings_dialog(HWND owner, ApplicationDialogOptions& options);

}  // namespace axiom::gui
