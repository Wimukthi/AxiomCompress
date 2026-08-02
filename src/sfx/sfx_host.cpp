#define NOMINMAX
#include "sfx/sfx_host.hpp"

#include "core/file_meta.hpp"
#include "core/path_text.hpp"

#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <system_error>
#include <utility>

namespace axiom::sfx {
namespace fs = std::filesystem;
namespace {

std::optional<std::wstring> widen(std::string_view text) {
    if (text.empty()) return std::wstring{};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0);
    if (length <= 0) return std::nullopt;
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), length) !=
        length) {
        return std::nullopt;
    }
    return std::move(result);
}

std::optional<fs::path> known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw))) {
        return std::nullopt;
    }
    fs::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

std::optional<fs::path> resolve_token(std::string_view name,
                                      const fs::path& executable) {
    if (name == "ProgramFiles") return known_folder(FOLDERID_ProgramFiles);
    if (name == "ProgramFiles(x86)") return known_folder(FOLDERID_ProgramFilesX86);
    if (name == "LOCALAPPDATA") return known_folder(FOLDERID_LocalAppData);
    if (name == "APPDATA") return known_folder(FOLDERID_RoamingAppData);
    if (name == "USERPROFILE") return known_folder(FOLDERID_Profile);
    if (name == "DESKTOP") return known_folder(FOLDERID_Desktop);
    if (name == "DOCUMENTS") return known_folder(FOLDERID_Documents);
    if (name == "TEMP") {
        std::array<wchar_t, MAX_PATH + 1> buffer{};
        const DWORD length =
            GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (length == 0 || length >= buffer.size()) return std::nullopt;
        return fs::path(buffer.data());
    }
    if (name == "SFXDIR") return executable.parent_path();
    if (name == "SFXNAME") return executable.stem();
    return std::nullopt;
}

// Case-insensitive glob supporting '*' and '?', iterative so a pathological
// pattern cannot blow the stack.
bool glob_match(std::wstring_view text, std::wstring_view pattern) {
    std::size_t text_index = 0;
    std::size_t pattern_index = 0;
    std::size_t star = std::wstring_view::npos;
    std::size_t match = 0;
    while (text_index < text.size()) {
        const bool same =
            pattern_index < pattern.size() &&
            (pattern[pattern_index] == L'?' ||
             std::towlower(pattern[pattern_index]) ==
                 std::towlower(text[text_index]));
        if (same) {
            ++text_index;
            ++pattern_index;
        } else if (pattern_index < pattern.size() && pattern[pattern_index] == L'*') {
            star = pattern_index++;
            match = text_index;
        } else if (star != std::wstring_view::npos) {
            pattern_index = star + 1;
            text_index = ++match;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern.size() && pattern[pattern_index] == L'*') {
        ++pattern_index;
    }
    return pattern_index == pattern.size();
}

bool path_is_inside_root(const fs::path& root, const fs::path& target) {
    const fs::path normalized_root = root.lexically_normal();
    const fs::path normalized_target = target.lexically_normal();
    const fs::path relative = normalized_target.lexically_relative(normalized_root);
    if (relative.empty() || relative == L".") return normalized_target == normalized_root;
    if (relative.is_absolute()) return false;
    for (const auto& part : relative) {
        if (part == L"..") return false;
    }
    return true;
}

bool has_reparse_component(const fs::path& root, const fs::path& target) {
    const fs::path normalized_root = root.lexically_normal();
    const fs::path relative = target.lexically_normal().lexically_relative(normalized_root);
    if (relative.empty() || relative == L".") return false;
    fs::path current = normalized_root;
    for (const auto& part : relative) {
        if (part == L".") continue;
        current /= part;
        if (core::is_reparse_point(current)) return true;
    }
    return false;
}

}  // namespace

HostConsole::HostConsole(HINSTANCE instance) : instance_(instance) {
    // An inherited handle means the caller redirected output — `setup.exe
    // --list > out.txt`, or a pipe. Take it as-is: AttachConsole would replace
    // it with a console handle and the redirection would silently be lost.
    HANDLE inherited = GetStdHandle(STD_OUTPUT_HANDLE);
    if (inherited != nullptr && inherited != INVALID_HANDLE_VALUE) {
        output_ = inherited;
        attached_ = true;
        owns_console_ = false;
        console_ = GetFileType(output_) == FILE_TYPE_CHAR;
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        output_ = GetStdHandle(STD_OUTPUT_HANDLE);
        attached_ = output_ != INVALID_HANDLE_VALUE && output_ != nullptr;
        owns_console_ = attached_;
        console_ = attached_ && GetFileType(output_) == FILE_TYPE_CHAR;
    }
}

HostConsole::~HostConsole() {
    if (owns_console_) FreeConsole();
}

void HostConsole::write(std::wstring_view text) {
    if (!attached_) {
        buffered_.append(text);
        return;
    }
    if (console_) {
        DWORD written = 0;
        WriteConsoleW(output_, text.data(), static_cast<DWORD>(text.size()),
                      &written, nullptr);
        return;
    }
    // A file or pipe takes bytes, not UTF-16 units.
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr,
                                           0, nullptr, nullptr);
    if (length <= 0) return;
    std::string encoded(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        encoded.data(), length, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(output_, encoded.data(), static_cast<DWORD>(encoded.size()), &written,
              nullptr);
}

std::wstring HostConsole::take_buffered() {
    std::wstring result;
    result.swap(buffered_);
    return result;
}

std::optional<fs::path> expand_sfx_path_template(
    std::string_view value, const fs::path& sfx_executable) {
    if (value.empty()) return std::nullopt;
    // Literal runs are converted as whole UTF-8 chunks, never byte by byte, so
    // a non-ASCII default_path survives expansion intact.
    std::wstring expanded;
    auto append_utf8 = [&](std::string_view text) {
        const auto converted = widen(text);
        if (!converted) return false;
        expanded += *converted;
        return true;
    };
    std::size_t cursor = 0;
    while (cursor < value.size()) {
        const auto open = value.find('%', cursor);
        if (open == std::string_view::npos) {
            if (!append_utf8(value.substr(cursor))) return std::nullopt;
            break;
        }
        if (!append_utf8(value.substr(cursor, open - cursor))) return std::nullopt;
        const auto close = value.find('%', open + 1);
        if (close == std::string_view::npos) return std::nullopt;
        const auto resolved =
            resolve_token(value.substr(open + 1, close - open - 1), sfx_executable);
        if (!resolved) return std::nullopt;
        expanded += resolved->wstring();
        cursor = close + 1;
    }

    fs::path result(expanded);
    if (!result.is_absolute()) return std::nullopt;
    result = result.lexically_normal();
    for (const auto& part : result) {
        if (part == L"..") return std::nullopt;
    }
    return result;
}

std::optional<std::uint64_t> available_free_space(const fs::path& path) {
    fs::path probe = fs::absolute(path).lexically_normal();
    std::error_code error;
    while (!probe.empty() && !fs::exists(probe, error)) {
        const fs::path parent = probe.parent_path();
        if (parent == probe) break;
        probe = parent;
    }
    if (probe.empty()) return std::nullopt;
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(probe.c_str(), &available, nullptr, nullptr)) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(available.QuadPart);
}

bool destination_is_writable(const fs::path& path) {
    std::error_code error;
    fs::path existing = fs::absolute(path).lexically_normal();
    while (!existing.empty() && !fs::exists(existing, error)) {
        const fs::path parent = existing.parent_path();
        if (parent == existing) break;
        existing = parent;
    }
    if (existing.empty() || !fs::is_directory(existing, error)) return false;

    // Create and remove a unique probe file rather than inferring from the
    // path. CREATE_NEW is important: a fixed CREATE_ALWAYS probe could truncate
    // a real user file that happened to have the probe name.
    const std::wstring prefix =
        L".axiom-sfx-write-probe-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64());
    for (unsigned attempt = 0; attempt != 16; ++attempt) {
        const fs::path probe = existing / (prefix + L"-" + std::to_wstring(attempt));
        const HANDLE handle = CreateFileW(
            probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            return true;
        }
        const DWORD last_error = GetLastError();
        if (last_error != ERROR_FILE_EXISTS && last_error != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    return false;
}

bool process_is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &size) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

std::optional<DWORD> relaunch_elevated(const fs::path& executable,
                                       const std::wstring& arguments) {
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info) || info.hProcess == nullptr) return std::nullopt;
    const DWORD wait = WaitForSingleObject(info.hProcess, INFINITE);
    DWORD exit_code = 0;
    const bool read_exit = wait == WAIT_OBJECT_0 &&
                           GetExitCodeProcess(info.hProcess, &exit_code) != FALSE;
    CloseHandle(info.hProcess);
    return read_exit ? std::optional<DWORD>(exit_code) : std::nullopt;
}

std::optional<fs::path> resolve_run_program(const fs::path& root,
                                            std::string_view relative) {
    if (relative.empty()) return std::nullopt;
    const fs::path candidate = core::path_from_utf8(relative);
    // A root-relative path ("\\Windows\\...") is not absolute according to
    // every std::filesystem implementation, but still escapes this drive's
    // extraction root on Windows. Reject all root-bearing forms explicitly.
    if (candidate.is_absolute() || candidate.has_root_name() ||
        candidate.has_root_directory()) {
        return std::nullopt;
    }
    for (const auto& part : candidate) {
        if (part == L"..") return std::nullopt;
    }
    const auto resolved = (root / candidate).lexically_normal();
    if (!path_is_inside_root(root, resolved) ||
        has_reparse_component(root, resolved)) {
        return std::nullopt;
    }
    std::error_code error;
    if (!fs::is_regular_file(fs::symlink_status(resolved, error))) return std::nullopt;
    if (error) return std::nullopt;
    return resolved;
}

std::optional<fs::path> resolve_run_directory(const fs::path& root,
                                              std::string_view relative) {
    if (relative.empty()) return root.lexically_normal();
    const fs::path candidate = core::path_from_utf8(relative);
    if (candidate.is_absolute() || candidate.has_root_name() ||
        candidate.has_root_directory()) {
        return std::nullopt;
    }
    for (const auto& part : candidate) {
        if (part == L"..") return std::nullopt;
    }
    const auto resolved = (root / candidate).lexically_normal();
    if (!path_is_inside_root(root, resolved) ||
        has_reparse_component(root, resolved)) {
        return std::nullopt;
    }
    std::error_code error;
    if (!fs::is_directory(fs::symlink_status(resolved, error))) return std::nullopt;
    if (error) return std::nullopt;
    return resolved;
}

RunResult run_extracted_program(const fs::path& root, const fs::path& program,
                                const std::wstring& arguments,
                                const fs::path& working_directory,
                                bool wait_for_exit) {
    RunResult result;
    if (!path_is_inside_root(root, program) ||
        (!working_directory.empty() && !path_is_inside_root(root, working_directory)) ||
        has_reparse_component(root, program) ||
        (!working_directory.empty() && has_reparse_component(root, working_directory))) {
        return result;
    }
    const fs::path directory =
        working_directory.empty() ? root : working_directory;
    std::error_code error;
    if (!fs::is_regular_file(fs::symlink_status(program, error)) || error ||
        !fs::is_directory(fs::symlink_status(directory, error)) || error) {
        return result;
    }
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"open";
    info.lpFile = program.c_str();
    info.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    info.lpDirectory = directory.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) return result;
    result.started = true;
    if (info.hProcess == nullptr) return result;
    if (wait_for_exit) {
        WaitForSingleObject(info.hProcess, INFINITE);
        DWORD code = 0;
        if (GetExitCodeProcess(info.hProcess, &code)) result.exit_code = code;
    }
    CloseHandle(info.hProcess);
    return result;
}

bool matches_include_pattern(std::string_view archive_path,
                             std::string_view pattern) {
    const auto path = widen(archive_path);
    const auto glob = widen(pattern);
    if (!path || !glob) return false;
    if (glob_match(*path, *glob)) return true;
    if (glob->find(L'/') != std::wstring::npos) return false;
    const auto slash = path->find_last_of(L'/');
    const std::wstring name =
        slash == std::wstring::npos ? *path : path->substr(slash + 1);
    return glob_match(name, *glob);
}

}  // namespace axiom::sfx
