#pragma once

#include "axiom/archive.hpp"
#include "gui/archive_feature_options.hpp"
#include "gui/keyboard_shortcuts.hpp"
#include "gui/toolbar_icons.hpp"

#include <windows.h>

#include <array>
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace axiom::gui {

struct ToolbarCommandInfo {
    const wchar_t* id;
    const wchar_t* label;
    const wchar_t* button_text;
    ToolbarIcon icon;
    bool default_visible;
};

inline constexpr std::array<ToolbarCommandInfo, 34> kToolbarCommandCatalog{{
    {L"commands.add", L"Add to archive", L"Add", ToolbarIcon::archive, true},
    {L"commands.extract", L"Extract", L"Extract", ToolbarIcon::extract, true},
    {L"commands.test", L"Test archive", L"Test", ToolbarIcon::test, true},
    {L"commands.view", L"View/open selection", L"View", ToolbarIcon::view, true},
    {L"commands.delete", L"Delete selection", L"Delete", ToolbarIcon::delete_item, true},
    {L"tools.info", L"Archive information", L"Info", ToolbarIcon::info, true},
    {L"file.open_archive", L"Open archive", L"Open archive", ToolbarIcon::open, true},
    {L"options.settings", L"Settings", L"Settings", ToolbarIcon::settings, true},
    {L"commands.update", L"Update archive", L"Update", ToolbarIcon::update_archive, false},
    {L"commands.freshen", L"Freshen archive", L"Freshen", ToolbarIcon::freshen_archive, false},
    {L"commands.synchronize", L"Synchronize archive", L"Sync", ToolbarIcon::synchronize_archive, false},
    {L"commands.delete_archive_entries", L"Delete archive entries", L"Remove", ToolbarIcon::delete_item, false},
    {L"commands.repack", L"Repack archive", L"Repack", ToolbarIcon::repack, false},
    {L"commands.split", L"Split archive into volumes", L"Split", ToolbarIcon::split, false},
    {L"commands.join", L"Join archive volumes", L"Join", ToolbarIcon::join, false},
    {L"commands.select_all", L"Select all", L"Select all", ToolbarIcon::select_all, false},
    {L"file.compress_stream", L"Compress single stream", L"Compress", ToolbarIcon::compress_stream, false},
    {L"file.decompress_stream", L"Decompress single stream", L"Decompress", ToolbarIcon::decompress_stream, false},
    {L"tools.find", L"Find files", L"Find", ToolbarIcon::find, false},
    {L"tools.benchmark", L"Benchmark", L"Benchmark", ToolbarIcon::benchmark, false},
    {L"tools.edit_comment", L"Edit archive comment", L"Comment", ToolbarIcon::comment, false},
    {L"tools.lock", L"Lock archive", L"Lock", ToolbarIcon::lock, false},
    {L"tools.repair", L"Repair archive", L"Repair", ToolbarIcon::repair, false},
    {L"tools.recovery_record", L"Edit recovery record", L"Recovery", ToolbarIcon::recovery, false},
    {L"tools.generate_key", L"Generate signing key", L"Keygen", ToolbarIcon::key, false},
    {L"tools.sign_archive", L"Sign archive", L"Sign", ToolbarIcon::sign, false},
    {L"tools.verify_signature", L"Verify signature", L"Verify", ToolbarIcon::verify_signature, false},
    {L"tools.create_sfx", L"Create self-extracting archive", L"SFX", ToolbarIcon::sfx, false},
    {L"options.toggle_tree", L"Show/hide tree pane", L"Tree", ToolbarIcon::tree, false},
    {L"options.add_favorite", L"Add favorite", L"Favorite", ToolbarIcon::favorite, false},
    {L"options.remove_favorite", L"Remove favorite", L"Unfavorite", ToolbarIcon::unfavorite, false},
    {L"help.check_updates", L"Check for updates", L"Updates", ToolbarIcon::update_archive, false},
    {L"clipboard.copy_path", L"Copy path", L"Copy path", ToolbarIcon::copy_path, false},
    {L"clipboard.copy_crc32", L"Copy CRC-32", L"Copy CRC", ToolbarIcon::copy_crc, false},
}};

inline bool toolbar_command_exists(std::wstring_view id) {
    return std::any_of(kToolbarCommandCatalog.begin(), kToolbarCommandCatalog.end(),
                       [&](const ToolbarCommandInfo& info) {
                           return id == std::wstring_view(info.id);
                       });
}

inline std::vector<std::wstring> default_toolbar_commands() {
    std::vector<std::wstring> result;
    for (const ToolbarCommandInfo& command : kToolbarCommandCatalog) {
        if (command.default_visible) {
            result.emplace_back(command.id);
        }
    }
    return result;
}

inline std::vector<std::wstring> normalize_toolbar_commands(
    const std::vector<std::wstring>& commands) {
    std::vector<std::wstring> result;
    result.reserve(commands.size());
    for (const ToolbarCommandInfo& catalog_command : kToolbarCommandCatalog) {
        const bool present = std::any_of(commands.begin(), commands.end(),
                                         [&](const std::wstring& command) {
                                             return command == catalog_command.id;
                                         });
        if (present) {
            result.emplace_back(catalog_command.id);
        }
    }
    return result;
}

struct CompressionProfile {
    std::wstring name;
    int level = 5;
    std::size_t thread_count = 0;
    std::size_t dictionary_size = 0;
    std::size_t word_size = 0;
    std::size_t solid_block_size = 0;
    int thread_model = 0;

    bool operator==(const CompressionProfile&) const = default;
};

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
    // Threading model for the parse stage: 0 = split blocks (independent blocks,
    // one per worker), 1 = swarm (workers cooperate inside each block). Swarm
    // affects levels 1-6 and 8-9; level 7 keeps its serial lazy-tree parser.
    int thread_model = 0;
    std::vector<CompressionProfile> compression_profiles;
    bool compression_profiles_changed = false;
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
    int default_thread_model = 0;  // 0 = split blocks, 1 = swarm.
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
    bool center_child_windows = true;
    int theme_mode = 0;  // 0 = system, 1 = dark, 2 = light.
    int accent_color_mode = 0;  // 0 = Windows accent, 1 = Axiom amber, 2..5 presets, 6 = custom.
    COLORREF custom_accent_color = RGB(255, 185, 60);
    int toolbar_icon_style = 0;  // 0 = theme-tinted, 1 = colorful, 2 = accent-colored.
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
    bool associate_zip = false;
    bool associate_7z = false;
    bool associate_rar = false;
    bool associate_tar = false;
    bool associate_iso = false;
    bool associate_cab = false;
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
    std::vector<CompressionProfile> compression_profiles;
    std::vector<std::wstring> toolbar_commands = default_toolbar_commands();
    std::vector<CommandShortcutSetting> shortcut_overrides;

    bool operator==(const ApplicationDialogOptions&) const = default;
};

bool show_create_archive_dialog(HWND owner,
                                std::size_t input_count,
                                CreateArchiveDialogOptions& options);
bool show_extract_archive_dialog(HWND owner,
                                 const std::filesystem::path& archive_path,
                                 ExtractArchiveDialogOptions& options);
bool show_application_settings_dialog(HWND owner, ApplicationDialogOptions& options);
bool show_application_settings_dialog(
    HWND owner,
    ApplicationDialogOptions& options,
    const std::function<void(const ApplicationDialogOptions&)>& apply_callback);

}  // namespace axiom::gui
