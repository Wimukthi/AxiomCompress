#include "gui/settings_store.hpp"

#include <algorithm>
#include <array>

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

void write_dword(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
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
    settings.application.confirm_delete = read_dword(key, L"ConfirmDelete", 1) != 0;
    settings.application.show_hidden = read_dword(key, L"ShowHidden", 1) != 0;
    settings.sort_column = static_cast<int>(std::clamp<DWORD>(read_dword(key, L"SortColumn", 0), 0, 6));
    settings.sort_ascending = read_dword(key, L"SortAscending", 1) != 0;
    settings.last_location = read_string(key, L"LastLocation");

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
    write_dword(key, L"ConfirmDelete", settings.application.confirm_delete ? 1 : 0);
    write_dword(key, L"ShowHidden", settings.application.show_hidden ? 1 : 0);
    write_dword(key, L"SortColumn", static_cast<DWORD>(settings.sort_column));
    write_dword(key, L"SortAscending", settings.sort_ascending ? 1 : 0);
    RegSetValueExW(key, L"LastLocation", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(settings.last_location.c_str()),
                   static_cast<DWORD>((settings.last_location.size() + 1) * sizeof(wchar_t)));
    if (settings.has_placement) {
        RegSetValueExW(key, L"WindowPlacement", 0, REG_BINARY,
                       reinterpret_cast<const BYTE*>(&settings.placement),
                       sizeof(settings.placement));
    }
    RegCloseKey(key);
}

}  // namespace axiom::gui
