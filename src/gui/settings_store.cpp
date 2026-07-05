#include "gui/settings_store.hpp"

#include <algorithm>
#include <array>
#include <cwchar>
#include <sstream>
#include <vector>

namespace axiom::gui {
namespace {

constexpr const wchar_t* kRegistryPath = L"Software\\AxiomCompress\\GUI";

DWORD read_dword(HKEY key, const wchar_t* name, DWORD fallback) {
    DWORD value = fallback;
    DWORD size = sizeof(value);
    RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value;
}

std::wstring read_string(HKEY key, const wchar_t* name) {
    DWORD size = 0;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size < sizeof(wchar_t)) {
        return {};
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS) {
        return {};
    }
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

std::vector<std::wstring> read_string_list(HKEY key, const wchar_t* name) {
    DWORD size = 0;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_MULTI_SZ,
                     nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size < sizeof(wchar_t)) {
        return {};
    }
    std::wstring raw(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_MULTI_SZ,
                     nullptr, raw.data(), &size) != ERROR_SUCCESS) {
        return {};
    }

    std::vector<std::wstring> values;
    const wchar_t* cursor = raw.c_str();
    const wchar_t* const end = raw.c_str() + raw.size();
    while (cursor < end && *cursor != L'\0') {
        const std::size_t length = wcslen(cursor);
        if (length != 0) values.emplace_back(cursor, length);
        cursor += length + 1;
    }
    return values;
}

void write_dword(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

void write_string(HKEY key, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void write_string_list(HKEY key, const wchar_t* name,
                       const std::vector<std::wstring>& values) {
    std::wstring raw;
    for (const auto& value : values) {
        if (value.empty()) continue;
        raw += value;
        raw.push_back(L'\0');
    }
    raw.push_back(L'\0');
    RegSetValueExW(key, name, 0, REG_MULTI_SZ,
                   reinterpret_cast<const BYTE*>(raw.c_str()),
                   static_cast<DWORD>(raw.size() * sizeof(wchar_t)));
}

int read_clamped_int(HKEY key, const wchar_t* name, int fallback, int minimum, int maximum) {
    return static_cast<int>(std::clamp<DWORD>(
        read_dword(key, name, static_cast<DWORD>(fallback)),
        static_cast<DWORD>(minimum), static_cast<DWORD>(maximum)));
}

bool read_bool(HKEY key, const wchar_t* name, bool fallback) {
    return read_dword(key, name, fallback ? 1u : 0u) != 0;
}

std::vector<int> read_int_list(HKEY key, const wchar_t* name) {
    std::vector<int> values;
    std::wstring text = read_string(key, name);
    const wchar_t* cursor = text.c_str();
    while (*cursor != L'\0') {
        wchar_t* end = nullptr;
        const long value = std::wcstol(cursor, &end, 10);
        if (end == cursor) break;
        values.push_back(static_cast<int>(std::clamp<long>(value, 48, 2000)));
        cursor = end;
        while (*cursor == L',' || *cursor == L' ' || *cursor == L'\t') ++cursor;
    }
    return values;
}

std::wstring join_int_list(const std::vector<int>& values) {
    std::wostringstream out;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) out << L',';
        out << std::clamp(values[index], 48, 2000);
    }
    return out.str();
}

}  // namespace

PersistedGuiSettings load_gui_settings() {
    PersistedGuiSettings settings;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return settings;
    }
    settings.application.default_level = static_cast<int>(
        std::clamp<DWORD>(read_dword(key, L"CompressionLevel", 5), 1, 9));
    settings.application.default_thread_count = read_dword(key, L"ThreadCount", 0);
    settings.application.default_dictionary_size = read_dword(key, L"DictionarySize", 0);
    settings.application.default_word_size = read_dword(key, L"WordSize", 0);
    settings.application.default_solid_block_size = read_dword(key, L"SolidBlockSize", 0);
    settings.application.default_update_mode = read_clamped_int(key, L"UpdateMode", 0, 0, 4);
    settings.application.default_volume_size = read_string(key, L"VolumeSize");
    settings.application.default_volume_unit = read_clamped_int(key, L"VolumeUnit", 2, 0, 3);
    settings.application.default_recovery_percent =
        read_clamped_int(key, L"RecoveryPercent", 0, 0, 100);
    settings.application.default_recovery_volumes = read_bool(key, L"RecoveryVolumes", false);
    settings.application.default_create_sfx = read_bool(key, L"CreateSfx", false);
    settings.application.default_sign_archive = read_bool(key, L"SignArchive", false);
    settings.application.default_signing_key = read_string(key, L"SigningKey");
    settings.application.confirm_delete = read_bool(key, L"ConfirmDelete", true);
    settings.application.confirm_overwrite = read_bool(key, L"ConfirmOverwrite", true);
    settings.application.show_hidden = read_bool(key, L"ShowHidden", true);
    settings.application.restore_window_placement =
        read_bool(key, L"RestoreWindowPlacement", true);
    settings.application.theme_mode = read_clamped_int(key, L"ThemeMode", 0, 0, 2);
    settings.application.accent_color_mode =
        read_clamped_int(key, L"AccentColorMode", 0, 0, 6);
    settings.application.custom_accent_color = static_cast<COLORREF>(
        read_dword(key, L"CustomAccentColor", RGB(255, 185, 60)));
    settings.application.toolbar_icon_style =
        read_clamped_int(key, L"ToolbarIconStyle", 0, 0, 2);
    settings.application.startup_location_mode =
        read_clamped_int(key, L"StartupLocationMode", 0, 0, 3);
    settings.application.startup_custom_path = read_string(key, L"StartupCustomPath");
    settings.application.archive_output_mode =
        read_clamped_int(key, L"ArchiveOutputMode", 0, 0, 2);
    settings.application.archive_output_folder = read_string(key, L"ArchiveOutputFolder");
    settings.application.extract_destination_mode =
        read_clamped_int(key, L"ExtractDestinationMode", 0, 0, 2);
    settings.application.extract_destination_folder =
        read_string(key, L"ExtractDestinationFolder");
    settings.application.temp_folder_mode = read_clamped_int(key, L"TempFolderMode", 0, 0, 2);
    settings.application.temp_folder = read_string(key, L"TempFolder");
    settings.application.temp_cleanup_days =
        read_clamped_int(key, L"TempCleanupDays", 7, 0, 365);
    settings.application.show_parent_entry = read_bool(key, L"ShowParentEntry", true);
    settings.application.show_grid_lines = read_bool(key, L"ShowGridLines", true);
    settings.application.show_horizontal_scrollbar =
        read_bool(key, L"ShowHorizontalScrollbar", true);
    settings.application.full_row_select = read_bool(key, L"FullRowSelect", true);
    settings.application.recent_location_count =
        read_clamped_int(key, L"RecentLocationCount", 12, 0, 50);
    settings.application.show_address_shell_locations =
        read_bool(key, L"ShowAddressShellLocations", true);
    settings.application.show_address_recent_locations =
        read_bool(key, L"ShowAddressRecentLocations", true);
    settings.application.show_address_archive_children =
        read_bool(key, L"ShowAddressArchiveChildren", true);
    settings.application.file_open_mode = read_clamped_int(key, L"FileOpenMode", 0, 0, 1);
    settings.application.external_viewer = read_string(key, L"ExternalViewer");
    settings.application.external_editor = read_string(key, L"ExternalEditor");
    settings.application.warn_executable_open = read_bool(key, L"WarnExecutableOpen", true);
    settings.application.keep_viewed_files_until_exit =
        read_bool(key, L"KeepViewedFilesUntilExit", true);
    settings.application.password_prompt_mode =
        read_clamped_int(key, L"PasswordPromptMode", 0, 0, 1);
    settings.application.cache_passwords = read_bool(key, L"CachePasswords", true);
    settings.application.verify_signatures = read_bool(key, L"VerifySignatures", true);
    settings.application.wipe_encrypted_temp_files =
        read_bool(key, L"WipeEncryptedTempFiles", true);
    settings.application.trusted_keys_folder = read_string(key, L"TrustedKeysFolder");
    settings.application.associate_axar = read_bool(key, L"AssociateAxar", false);
    settings.application.associate_zip = read_bool(key, L"AssociateZip", false);
    settings.application.associate_7z = read_bool(key, L"Associate7z", false);
    settings.application.associate_rar = read_bool(key, L"AssociateRar", false);
    settings.application.associate_tar = read_bool(key, L"AssociateTar", false);
    settings.application.associate_iso = read_bool(key, L"AssociateIso", false);
    settings.application.associate_cab = read_bool(key, L"AssociateCab", false);
    settings.application.context_open = read_bool(key, L"ContextOpen", false);
    settings.application.context_add = read_bool(key, L"ContextAdd", false);
    settings.application.context_extract = read_bool(key, L"ContextExtract", false);
    settings.application.context_test = read_bool(key, L"ContextTest", false);
    settings.application.automatic_update_checks = read_bool(key, L"CheckForUpdates", false);
    settings.application.update_channel = read_clamped_int(key, L"UpdateChannel", 0, 0, 1);
    settings.application.update_url = read_string(key, L"UpdateUrl");
    settings.application.worker_priority = read_clamped_int(key, L"WorkerPriority", 0, 0, 2);
    settings.application.verbose_logging = read_bool(key, L"VerboseLogging", false);
    settings.application.log_folder = read_string(key, L"LogFolder");
    settings.application.io_buffer_mode = read_clamped_int(key, L"IoBufferMode", 0, 0, 1);
    settings.application.io_buffer_size = read_string(key, L"IoBufferSize");
    settings.application.memory_limit_mode =
        read_clamped_int(key, L"MemoryLimitMode", 0, 0, 1);
    settings.application.memory_limit = read_string(key, L"MemoryLimit");
    settings.application.shortcut_overrides =
        shortcut_overrides_from_strings(read_string_list(key, L"ShortcutOverrides"));
    settings.sort_column = static_cast<int>(std::clamp<DWORD>(read_dword(key, L"SortColumn", 0), 0, 6));
    settings.sort_ascending = read_dword(key, L"SortAscending", 1) != 0;
    settings.tree_width =
        read_clamped_int(key, L"TreeWidth", 0, 0, 2000);
    settings.tree_pane_visible = read_bool(key, L"TreePaneVisible", true);
    settings.column_widths = read_int_list(key, L"ColumnWidths");
    settings.last_location = read_string(key, L"LastLocation");
    settings.last_archive_output_folder = read_string(key, L"LastArchiveOutputFolder");
    settings.last_extract_destination_folder = read_string(key, L"LastExtractDestinationFolder");
    settings.recent_locations = read_string_list(key, L"RecentLocations");
    settings.recent_archives = read_string_list(key, L"RecentArchives");
    settings.favorite_locations = read_string_list(key, L"FavoriteLocations");

    DWORD placement_size = sizeof(settings.placement);
    DWORD type = 0;
    if (RegQueryValueExW(key, L"WindowPlacement", nullptr, &type,
                         reinterpret_cast<BYTE*>(&settings.placement),
                         &placement_size) == ERROR_SUCCESS &&
        type == REG_BINARY && placement_size == sizeof(settings.placement)) {
        settings.placement.length = sizeof(WINDOWPLACEMENT);
        settings.has_placement = true;
    }
    RegCloseKey(key);
    return settings;
}

void save_gui_settings(const PersistedGuiSettings& settings) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    write_dword(key, L"CompressionLevel", static_cast<DWORD>(settings.application.default_level));
    write_dword(key, L"ThreadCount", static_cast<DWORD>(std::min<std::size_t>(
        settings.application.default_thread_count, MAXDWORD)));
    write_dword(key, L"DictionarySize", static_cast<DWORD>(std::min<std::size_t>(
        settings.application.default_dictionary_size, MAXDWORD)));
    write_dword(key, L"WordSize", static_cast<DWORD>(std::min<std::size_t>(
        settings.application.default_word_size, MAXDWORD)));
    write_dword(key, L"SolidBlockSize", static_cast<DWORD>(std::min<std::size_t>(
        settings.application.default_solid_block_size, MAXDWORD)));
    write_dword(key, L"UpdateMode",
                static_cast<DWORD>(std::clamp(settings.application.default_update_mode, 0, 4)));
    write_string(key, L"VolumeSize", settings.application.default_volume_size);
    write_dword(key, L"VolumeUnit",
                static_cast<DWORD>(std::clamp(settings.application.default_volume_unit, 0, 3)));
    write_dword(key, L"RecoveryPercent",
                static_cast<DWORD>(std::clamp(
                    settings.application.default_recovery_percent, 0, 100)));
    write_dword(key, L"RecoveryVolumes",
                settings.application.default_recovery_volumes ? 1 : 0);
    write_dword(key, L"CreateSfx", settings.application.default_create_sfx ? 1 : 0);
    write_dword(key, L"SignArchive", settings.application.default_sign_archive ? 1 : 0);
    write_string(key, L"SigningKey", settings.application.default_signing_key);
    write_dword(key, L"ConfirmDelete", settings.application.confirm_delete ? 1 : 0);
    write_dword(key, L"ConfirmOverwrite", settings.application.confirm_overwrite ? 1 : 0);
    write_dword(key, L"ShowHidden", settings.application.show_hidden ? 1 : 0);
    write_dword(key, L"RestoreWindowPlacement",
                settings.application.restore_window_placement ? 1 : 0);
    write_dword(key, L"ThemeMode",
                static_cast<DWORD>(std::clamp(settings.application.theme_mode, 0, 2)));
    write_dword(key, L"AccentColorMode",
                static_cast<DWORD>(std::clamp(settings.application.accent_color_mode, 0, 6)));
    write_dword(key, L"CustomAccentColor",
                static_cast<DWORD>(settings.application.custom_accent_color));
    write_dword(key, L"ToolbarIconStyle",
                static_cast<DWORD>(std::clamp(
                    settings.application.toolbar_icon_style, 0, 2)));
    write_dword(key, L"StartupLocationMode",
                static_cast<DWORD>(std::clamp(settings.application.startup_location_mode, 0, 3)));
    write_string(key, L"StartupCustomPath", settings.application.startup_custom_path);
    write_dword(key, L"ArchiveOutputMode",
                static_cast<DWORD>(std::clamp(settings.application.archive_output_mode, 0, 2)));
    write_string(key, L"ArchiveOutputFolder", settings.application.archive_output_folder);
    write_dword(key, L"ExtractDestinationMode",
                static_cast<DWORD>(std::clamp(settings.application.extract_destination_mode, 0, 2)));
    write_string(key, L"ExtractDestinationFolder",
                 settings.application.extract_destination_folder);
    write_dword(key, L"TempFolderMode",
                static_cast<DWORD>(std::clamp(settings.application.temp_folder_mode, 0, 2)));
    write_string(key, L"TempFolder", settings.application.temp_folder);
    write_dword(key, L"TempCleanupDays",
                static_cast<DWORD>(std::clamp(settings.application.temp_cleanup_days, 0, 365)));
    write_dword(key, L"ShowParentEntry", settings.application.show_parent_entry ? 1 : 0);
    write_dword(key, L"ShowGridLines", settings.application.show_grid_lines ? 1 : 0);
    write_dword(key, L"ShowHorizontalScrollbar",
                settings.application.show_horizontal_scrollbar ? 1 : 0);
    write_dword(key, L"FullRowSelect", settings.application.full_row_select ? 1 : 0);
    write_dword(key, L"RecentLocationCount",
                static_cast<DWORD>(std::clamp(settings.application.recent_location_count, 0, 50)));
    write_dword(key, L"ShowAddressShellLocations",
                settings.application.show_address_shell_locations ? 1 : 0);
    write_dword(key, L"ShowAddressRecentLocations",
                settings.application.show_address_recent_locations ? 1 : 0);
    write_dword(key, L"ShowAddressArchiveChildren",
                settings.application.show_address_archive_children ? 1 : 0);
    write_dword(key, L"FileOpenMode",
                static_cast<DWORD>(std::clamp(settings.application.file_open_mode, 0, 1)));
    write_string(key, L"ExternalViewer", settings.application.external_viewer);
    write_string(key, L"ExternalEditor", settings.application.external_editor);
    write_dword(key, L"WarnExecutableOpen",
                settings.application.warn_executable_open ? 1 : 0);
    write_dword(key, L"KeepViewedFilesUntilExit",
                settings.application.keep_viewed_files_until_exit ? 1 : 0);
    write_dword(key, L"PasswordPromptMode",
                static_cast<DWORD>(std::clamp(settings.application.password_prompt_mode, 0, 1)));
    write_dword(key, L"CachePasswords", settings.application.cache_passwords ? 1 : 0);
    write_dword(key, L"VerifySignatures", settings.application.verify_signatures ? 1 : 0);
    write_dword(key, L"WipeEncryptedTempFiles",
                settings.application.wipe_encrypted_temp_files ? 1 : 0);
    write_string(key, L"TrustedKeysFolder", settings.application.trusted_keys_folder);
    write_dword(key, L"AssociateAxar", settings.application.associate_axar ? 1 : 0);
    write_dword(key, L"AssociateZip", settings.application.associate_zip ? 1 : 0);
    write_dword(key, L"Associate7z", settings.application.associate_7z ? 1 : 0);
    write_dword(key, L"AssociateRar", settings.application.associate_rar ? 1 : 0);
    write_dword(key, L"AssociateTar", settings.application.associate_tar ? 1 : 0);
    write_dword(key, L"AssociateIso", settings.application.associate_iso ? 1 : 0);
    write_dword(key, L"AssociateCab", settings.application.associate_cab ? 1 : 0);
    write_dword(key, L"ContextOpen", settings.application.context_open ? 1 : 0);
    write_dword(key, L"ContextAdd", settings.application.context_add ? 1 : 0);
    write_dword(key, L"ContextExtract", settings.application.context_extract ? 1 : 0);
    write_dword(key, L"ContextTest", settings.application.context_test ? 1 : 0);
    write_dword(key, L"CheckForUpdates",
                settings.application.automatic_update_checks ? 1 : 0);
    write_dword(key, L"UpdateChannel",
                static_cast<DWORD>(std::clamp(settings.application.update_channel, 0, 1)));
    write_string(key, L"UpdateUrl", settings.application.update_url);
    write_dword(key, L"WorkerPriority",
                static_cast<DWORD>(std::clamp(settings.application.worker_priority, 0, 2)));
    write_dword(key, L"VerboseLogging", settings.application.verbose_logging ? 1 : 0);
    write_string(key, L"LogFolder", settings.application.log_folder);
    write_dword(key, L"IoBufferMode",
                static_cast<DWORD>(std::clamp(settings.application.io_buffer_mode, 0, 1)));
    write_string(key, L"IoBufferSize", settings.application.io_buffer_size);
    write_dword(key, L"MemoryLimitMode",
                static_cast<DWORD>(std::clamp(settings.application.memory_limit_mode, 0, 1)));
    write_string(key, L"MemoryLimit", settings.application.memory_limit);
    write_string_list(key, L"ShortcutOverrides",
                      shortcut_overrides_to_strings(settings.application.shortcut_overrides));
    write_dword(key, L"SortColumn", static_cast<DWORD>(settings.sort_column));
    write_dword(key, L"SortAscending", settings.sort_ascending ? 1 : 0);
    write_dword(key, L"TreeWidth",
                static_cast<DWORD>(std::clamp(settings.tree_width, 0, 2000)));
    write_dword(key, L"TreePaneVisible", settings.tree_pane_visible ? 1 : 0);
    write_string(key, L"ColumnWidths", join_int_list(settings.column_widths));
    write_string(key, L"LastLocation", settings.last_location);
    write_string(key, L"LastArchiveOutputFolder", settings.last_archive_output_folder);
    write_string(key, L"LastExtractDestinationFolder", settings.last_extract_destination_folder);
    write_string_list(key, L"RecentLocations", settings.recent_locations);
    write_string_list(key, L"RecentArchives", settings.recent_archives);
    write_string_list(key, L"FavoriteLocations", settings.favorite_locations);
    if (settings.has_placement) {
        RegSetValueExW(key, L"WindowPlacement", 0, REG_BINARY,
                       reinterpret_cast<const BYTE*>(&settings.placement),
                       sizeof(settings.placement));
    }
    RegCloseKey(key);
}

}  // namespace axiom::gui
