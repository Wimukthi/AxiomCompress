#pragma once

#include "axiom/archive.hpp"
#include "gui/archive_feature_options.hpp"

#include <windows.h>

#include <cstddef>
#include <filesystem>
#include <string>

namespace axiom::gui {

struct CreateArchiveDialogOptions {
    std::filesystem::path archive_path;
    axiom::ArchiveFormat archive_format = axiom::ArchiveFormat::axar;
    bool fixed_archive_format = false;
    int level = 5;
    std::size_t thread_count = 0;
    // Zero keeps the selected compression level's preset.
    std::size_t dictionary_size = 0;
    std::size_t word_size = 0;
    std::size_t solid_block_size = 0;
    ArchiveFeatureOptions features;
    ArchiveFeatureAvailability feature_availability;
};

struct ExtractArchiveDialogOptions {
    std::filesystem::path destination;
    std::size_t thread_count = 0;
    bool overwrite = false;
    bool restore_mtime = true;
    ExtractFeatureOptions features;
    ArchiveFeatureAvailability feature_availability;
};

struct ApplicationDialogOptions {
    int default_level = 5;
    std::size_t default_thread_count = 0;
    std::size_t default_dictionary_size = 0;
    std::size_t default_word_size = 0;
    std::size_t default_solid_block_size = 0;
    int default_update_mode = 0;
    std::wstring default_volume_size;
    int default_volume_unit = 2;
    int default_recovery_percent = 0;
    bool default_recovery_volumes = false;
    bool default_create_sfx = false;
    bool default_sign_archive = false;
    std::wstring default_signing_key;
    bool confirm_delete = true;
    bool confirm_overwrite = true;
    bool show_hidden = true;
    bool restore_window_placement = true;
    int theme_mode = 0;  // 0 = system, 1 = dark, 2 = light.
    int startup_location_mode = 0;  // 0 = last, 1 = This PC, 2 = Desktop, 3 = custom.
    std::wstring startup_custom_path;
    int archive_output_mode = 0;  // 0 = source folder, 1 = last used, 2 = custom.
    std::wstring archive_output_folder;
    int extract_destination_mode = 0;  // 0 = archive folder, 1 = last used, 2 = custom.
    std::wstring extract_destination_folder;
    int temp_folder_mode = 0;  // 0 = system temp, 1 = app temp, 2 = custom.
    std::wstring temp_folder;
    int temp_cleanup_days = 7;
    bool show_parent_entry = true;
    bool show_grid_lines = true;
    bool show_horizontal_scrollbar = true;
    bool full_row_select = true;
    int recent_location_count = 12;
    bool show_address_shell_locations = true;
    bool show_address_recent_locations = true;
    bool show_address_archive_children = true;
    int file_open_mode = 0;  // 0 = extract to temp and open, 1 = prompt first.
    std::wstring external_viewer;
    std::wstring external_editor;
    bool warn_executable_open = true;
    bool keep_viewed_files_until_exit = true;
    int password_prompt_mode = 0;  // 0 = once per archive session, 1 = every operation.
    bool cache_passwords = true;
    bool verify_signatures = true;
    bool wipe_encrypted_temp_files = true;
    std::wstring trusted_keys_folder;
    bool associate_axar = false;
    bool context_open = false;
    bool context_add = false;
    bool context_extract = false;
    bool context_test = false;
    bool automatic_update_checks = false;
    int update_channel = 0;  // 0 = stable/custom URL, 1 = preview/custom URL.
    std::wstring update_url;
    int worker_priority = 0;  // 0 = normal, 1 = below normal, 2 = background.
    bool verbose_logging = false;
    std::wstring log_folder;
    int io_buffer_mode = 0;  // 0 = automatic, 1 = custom.
    std::wstring io_buffer_size;
    int memory_limit_mode = 0;  // 0 = automatic, 1 = custom.
    std::wstring memory_limit;

    bool operator==(const ApplicationDialogOptions&) const = default;
};

bool show_create_archive_dialog(HWND owner,
                                std::size_t input_count,
                                CreateArchiveDialogOptions& options);
bool show_extract_archive_dialog(HWND owner,
                                 const std::filesystem::path& archive_path,
                                 ExtractArchiveDialogOptions& options);
bool show_application_settings_dialog(HWND owner, ApplicationDialogOptions& options);

}  // namespace axiom::gui
