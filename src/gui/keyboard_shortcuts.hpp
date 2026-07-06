#pragma once

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace axiom::gui {

struct CommandShortcutSetting {
    std::wstring command;
    std::wstring shortcut;

    bool operator==(const CommandShortcutSetting&) const = default;
};

struct ShortcutCommandInfo {
    const wchar_t* id;
    const wchar_t* label;
    const wchar_t* default_shortcut;
};

struct KeyboardShortcut {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    UINT key = 0;

    bool operator==(const KeyboardShortcut&) const = default;
};

inline constexpr std::array<ShortcutCommandInfo, 35> kShortcutCommandCatalog{{
    {L"file.open_archive", L"File: Open archive", L"Ctrl+O"},
    {L"file.exit", L"File: Exit", L"Alt+F4"},
    {L"commands.add", L"Commands: Add to archive", L"Ctrl+N"},
    {L"commands.extract", L"Commands: Extract", L"Ctrl+E"},
    {L"commands.test", L"Commands: Test archive", L"Ctrl+T"},
    {L"commands.update", L"Commands: Update archive", L"Ctrl+U"},
    {L"commands.freshen", L"Commands: Freshen archive", L"Ctrl+Shift+U"},
    {L"commands.synchronize", L"Commands: Synchronize archive", L"Ctrl+Alt+U"},
    {L"commands.delete_archive_entries", L"Commands: Delete archive entries", L"Shift+Delete"},
    {L"commands.repack", L"Commands: Repack archive", L"Ctrl+Shift+R"},
    {L"commands.view", L"Commands: View/open selection", L"Enter"},
    {L"commands.delete", L"Commands: Delete selection", L"Delete"},
    {L"commands.select_all", L"Commands: Select all", L"Ctrl+A"},
    {L"tools.info", L"Tools: Archive information", L"Ctrl+I"},
    {L"tools.find", L"Tools: Find files", L"Ctrl+F"},
    {L"tools.benchmark", L"Tools: Benchmark", L"Ctrl+B"},
    {L"tools.edit_comment", L"Tools: Edit archive comment", L"Ctrl+M"},
    {L"tools.lock", L"Tools: Lock archive", L"Ctrl+Shift+L"},
    {L"tools.repair", L"Tools: Repair archive", L"Ctrl+Shift+P"},
    {L"tools.verify_signature", L"Tools: Verify signature", L"Ctrl+Shift+V"},
    {L"tools.create_sfx", L"Tools: Create self-extracting archive", L"Ctrl+Shift+X"},
    {L"navigation.back", L"Navigation: Back", L"Alt+Left"},
    {L"navigation.forward", L"Navigation: Forward", L"Alt+Right"},
    {L"navigation.up", L"Navigation: Up one level", L"Alt+Up"},
    {L"navigation.refresh", L"Navigation: Refresh", L"F5"},
    {L"navigation.focus_address", L"Navigation: Focus address bar", L"Ctrl+L"},
    {L"navigation.go_address", L"Navigation: Go to address", L"Enter"},
    {L"options.toggle_tree", L"Options: Show/hide tree pane", L"F9"},
    {L"options.add_favorite", L"Options: Add favorite", L"Ctrl+D"},
    {L"options.remove_favorite", L"Options: Remove favorite", L"Ctrl+Shift+D"},
    {L"options.settings", L"Options: Settings", L"Ctrl+,"},
    {L"help.check_updates", L"Help: Check for updates", L"Ctrl+Alt+Shift+U"},
    {L"help.about", L"Help: About Axiom", L"F1"},
    {L"clipboard.copy_path", L"Clipboard: Copy path", L"Ctrl+Shift+C"},
    {L"clipboard.copy_crc32", L"Clipboard: Copy CRC-32", L"Ctrl+Alt+C"},
}};

inline std::wstring trim_shortcut_text(std::wstring_view text) {
    std::size_t first = 0;
    while (first < text.size() && std::iswspace(text[first])) ++first;
    std::size_t last = text.size();
    while (last > first && std::iswspace(text[last - 1])) --last;
    return std::wstring(text.substr(first, last - first));
}

inline std::wstring lower_shortcut_text(std::wstring_view text) {
    std::wstring result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return result;
}

inline bool shortcut_command_exists(std::wstring_view id) {
    return std::any_of(kShortcutCommandCatalog.begin(), kShortcutCommandCatalog.end(),
                       [&](const ShortcutCommandInfo& info) {
                           return id == std::wstring_view(info.id);
                       });
}

inline const ShortcutCommandInfo* shortcut_command_info(std::wstring_view id) {
    const auto found = std::find_if(kShortcutCommandCatalog.begin(),
                                    kShortcutCommandCatalog.end(),
                                    [&](const ShortcutCommandInfo& info) {
                                        return id == std::wstring_view(info.id);
                                    });
    return found == kShortcutCommandCatalog.end() ? nullptr : &*found;
}

inline bool shortcut_text_is_none(std::wstring_view text) {
    const std::wstring trimmed = trim_shortcut_text(text);
    if (trimmed.empty()) return true;
    const std::wstring lower = lower_shortcut_text(trimmed);
    return lower == L"none" || lower == L"disabled" || lower == L"clear";
}

inline std::vector<std::wstring> split_shortcut_tokens(std::wstring_view text) {
    std::vector<std::wstring> tokens;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t plus = text.find(L'+', start);
        const std::size_t end = plus == std::wstring_view::npos ? text.size() : plus;
        std::wstring token = trim_shortcut_text(text.substr(start, end - start));
        if (!token.empty()) tokens.push_back(std::move(token));
        if (plus == std::wstring_view::npos) break;
        start = plus + 1;
    }
    return tokens;
}

inline std::optional<UINT> shortcut_key_from_token(std::wstring_view token) {
    const std::wstring lower = lower_shortcut_text(token);
    if (lower.size() == 1) {
        const wchar_t ch = lower.front();
        if (ch >= L'a' && ch <= L'z') return static_cast<UINT>(std::towupper(ch));
        if (ch >= L'0' && ch <= L'9') return static_cast<UINT>(ch);
        if (ch == L',') return VK_OEM_COMMA;
        if (ch == L'.') return VK_OEM_PERIOD;
        if (ch == L';') return VK_OEM_1;
        if (ch == L'/') return VK_OEM_2;
        if (ch == L'`') return VK_OEM_3;
        if (ch == L'[') return VK_OEM_4;
        if (ch == L'\\') return VK_OEM_5;
        if (ch == L']') return VK_OEM_6;
        if (ch == L'\'') return VK_OEM_7;
        if (ch == L'-') return VK_OEM_MINUS;
        if (ch == L'=') return VK_OEM_PLUS;
    }
    if (lower.size() >= 2 && lower.front() == L'f') {
        try {
            const int number = std::stoi(std::wstring(lower.substr(1)));
            if (number >= 1 && number <= 24) return static_cast<UINT>(VK_F1 + number - 1);
        } catch (...) {
        }
    }
    if (lower == L"left") return VK_LEFT;
    if (lower == L"right") return VK_RIGHT;
    if (lower == L"up") return VK_UP;
    if (lower == L"down") return VK_DOWN;
    if (lower == L"home") return VK_HOME;
    if (lower == L"end") return VK_END;
    if (lower == L"pageup" || lower == L"pgup") return VK_PRIOR;
    if (lower == L"pagedown" || lower == L"pgdn") return VK_NEXT;
    if (lower == L"insert" || lower == L"ins") return VK_INSERT;
    if (lower == L"delete" || lower == L"del") return VK_DELETE;
    if (lower == L"backspace" || lower == L"back") return VK_BACK;
    if (lower == L"enter" || lower == L"return") return VK_RETURN;
    if (lower == L"space") return VK_SPACE;
    if (lower == L"escape" || lower == L"esc") return VK_ESCAPE;
    if (lower == L"tab") return VK_TAB;
    return std::nullopt;
}

inline std::optional<KeyboardShortcut> parse_keyboard_shortcut(std::wstring_view text) {
    if (shortcut_text_is_none(text)) return KeyboardShortcut{};
    KeyboardShortcut shortcut;
    bool has_key = false;
    for (const std::wstring& token : split_shortcut_tokens(text)) {
        const std::wstring lower = lower_shortcut_text(token);
        if (lower == L"ctrl" || lower == L"control") {
            shortcut.ctrl = true;
        } else if (lower == L"alt") {
            shortcut.alt = true;
        } else if (lower == L"shift") {
            shortcut.shift = true;
        } else if (auto key = shortcut_key_from_token(token)) {
            if (has_key) return std::nullopt;
            shortcut.key = *key;
            has_key = true;
        } else {
            return std::nullopt;
        }
    }
    if (!has_key) return std::nullopt;
    return shortcut;
}

inline std::wstring shortcut_key_name(UINT key) {
    if (key >= 'A' && key <= 'Z') return std::wstring(1, static_cast<wchar_t>(key));
    if (key >= '0' && key <= '9') return std::wstring(1, static_cast<wchar_t>(key));
    if (key >= VK_F1 && key <= VK_F24) return L"F" + std::to_wstring(key - VK_F1 + 1);
    switch (key) {
        case VK_LEFT: return L"Left";
        case VK_RIGHT: return L"Right";
        case VK_UP: return L"Up";
        case VK_DOWN: return L"Down";
        case VK_HOME: return L"Home";
        case VK_END: return L"End";
        case VK_PRIOR: return L"PageUp";
        case VK_NEXT: return L"PageDown";
        case VK_INSERT: return L"Insert";
        case VK_DELETE: return L"Delete";
        case VK_BACK: return L"Backspace";
        case VK_RETURN: return L"Enter";
        case VK_SPACE: return L"Space";
        case VK_ESCAPE: return L"Esc";
        case VK_TAB: return L"Tab";
        case VK_OEM_COMMA: return L",";
        case VK_OEM_PERIOD: return L".";
        case VK_OEM_1: return L";";
        case VK_OEM_2: return L"/";
        case VK_OEM_3: return L"`";
        case VK_OEM_4: return L"[";
        case VK_OEM_5: return L"\\";
        case VK_OEM_6: return L"]";
        case VK_OEM_7: return L"'";
        case VK_OEM_MINUS: return L"-";
        case VK_OEM_PLUS: return L"=";
        default: return {};
    }
}

inline std::wstring format_keyboard_shortcut(const KeyboardShortcut& shortcut) {
    if (shortcut.key == 0) return {};
    std::wstring result;
    if (shortcut.ctrl) result += L"Ctrl+";
    if (shortcut.alt) result += L"Alt+";
    if (shortcut.shift) result += L"Shift+";
    result += shortcut_key_name(shortcut.key);
    return result;
}

inline std::optional<std::wstring> canonical_keyboard_shortcut(std::wstring_view text) {
    if (shortcut_text_is_none(text)) return std::wstring{};
    const auto parsed = parse_keyboard_shortcut(text);
    if (!parsed || parsed->key == 0) return std::nullopt;
    return format_keyboard_shortcut(*parsed);
}

inline std::wstring default_shortcut_for_command(std::wstring_view id) {
    const auto* info = shortcut_command_info(id);
    return info == nullptr ? std::wstring{} : std::wstring(info->default_shortcut);
}

inline std::wstring effective_shortcut_for_command(
    const std::vector<CommandShortcutSetting>& overrides,
    std::wstring_view id) {
    for (const auto& override : overrides) {
        if (override.command == id) {
            return override.shortcut;
        }
    }
    return default_shortcut_for_command(id);
}

inline void set_shortcut_override(std::vector<CommandShortcutSetting>& overrides,
                                  std::wstring id,
                                  std::wstring shortcut) {
    auto found = std::find_if(overrides.begin(), overrides.end(),
                              [&](const CommandShortcutSetting& setting) {
                                  return setting.command == id;
                              });
    if (shortcut == default_shortcut_for_command(id)) {
        if (found != overrides.end()) overrides.erase(found);
        return;
    }
    if (found == overrides.end()) {
        overrides.push_back({std::move(id), std::move(shortcut)});
    } else {
        found->shortcut = std::move(shortcut);
    }
}

inline std::vector<CommandShortcutSetting> shortcut_overrides_from_strings(
    const std::vector<std::wstring>& values) {
    std::vector<CommandShortcutSetting> overrides;
    for (const std::wstring& value : values) {
        const std::size_t separator = value.find(L'=');
        if (separator == std::wstring::npos) continue;
        std::wstring id = value.substr(0, separator);
        if (!shortcut_command_exists(id)) continue;
        const std::wstring raw_shortcut = value.substr(separator + 1);
        if (const auto canonical = canonical_keyboard_shortcut(raw_shortcut)) {
            set_shortcut_override(overrides, std::move(id), *canonical);
        }
    }
    return overrides;
}

inline std::vector<std::wstring> shortcut_overrides_to_strings(
    const std::vector<CommandShortcutSetting>& overrides) {
    std::vector<std::wstring> values;
    for (const auto& override : overrides) {
        if (!shortcut_command_exists(override.command)) continue;
        if (canonical_keyboard_shortcut(override.shortcut)) {
            values.push_back(override.command + L"=" + override.shortcut);
        }
    }
    return values;
}

inline bool is_modifier_key(UINT key) {
    switch (key) {
        case VK_SHIFT:
        case VK_CONTROL:
        case VK_MENU:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_LMENU:
        case VK_RMENU:
            return true;
        default:
            return false;
    }
}

inline std::optional<KeyboardShortcut> keyboard_shortcut_from_message(const MSG& message) {
    if (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN) {
        return std::nullopt;
    }
    const UINT key = static_cast<UINT>(message.wParam);
    if (is_modifier_key(key)) return std::nullopt;
    return KeyboardShortcut{
        (GetKeyState(VK_CONTROL) & 0x8000) != 0,
        (GetKeyState(VK_MENU) & 0x8000) != 0,
        (GetKeyState(VK_SHIFT) & 0x8000) != 0,
        key,
    };
}

}  // namespace axiom::gui
