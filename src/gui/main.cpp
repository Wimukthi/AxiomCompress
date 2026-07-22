#include "gui/app.hpp"

#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <thread>
#include <utility>

namespace {

bool is_shell_placeholder(const std::wstring& value) {
    return _wcsicmp(value.c_str(), L"%1") == 0 ||
           _wcsicmp(value.c_str(), L"%V") == 0 ||
           _wcsicmp(value.c_str(), L"%*") == 0;
}

void append_unique_shell_path(std::vector<std::wstring>& paths,
                              const std::wstring& value) {
    if (value.empty() || is_shell_placeholder(value)) return;
    const bool duplicate = std::any_of(
        paths.begin(), paths.end(), [&](const std::wstring& existing) {
            return _wcsicmp(existing.c_str(), value.c_str()) == 0;
        });
    if (!duplicate) paths.push_back(value);
}

bool write_all(HANDLE pipe, const void* data, DWORD size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    DWORD written_total = 0;
    while (written_total < size) {
        DWORD written = 0;
        if (!WriteFile(pipe, bytes + written_total, size - written_total,
                       &written, nullptr) || written == 0) {
            return false;
        }
        written_total += written;
    }
    return true;
}

bool read_all(HANDLE pipe, void* data, DWORD size) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    DWORD read_total = 0;
    while (read_total < size) {
        DWORD read = 0;
        if (!ReadFile(pipe, bytes + read_total, size - read_total,
                      &read, nullptr) || read == 0) {
            return false;
        }
        read_total += read;
    }
    return true;
}

bool send_shell_add_paths(const std::wstring& pipe_name,
                          const std::vector<std::wstring>& paths) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    while (std::chrono::steady_clock::now() < deadline) {
        pipe = CreateFileW(pipe_name.c_str(), GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(pipe_name.c_str(), 100);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;

    const std::uint32_t count = static_cast<std::uint32_t>(paths.size());
    bool ok = write_all(pipe, &count, sizeof(count));
    for (const auto& path : paths) {
        const std::uint32_t length = static_cast<std::uint32_t>(path.size());
        ok = ok && write_all(pipe, &length, sizeof(length));
        if (ok && length != 0) {
            ok = write_all(pipe, path.data(), length * sizeof(wchar_t));
        }
        if (!ok) break;
    }
    FlushFileBuffers(pipe);
    CloseHandle(pipe);
    return ok;
}

bool receive_shell_add_paths(HANDLE pipe, std::vector<std::wstring>& paths) {
    std::uint32_t count = 0;
    if (!read_all(pipe, &count, sizeof(count)) || count > 1000) return false;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t length = 0;
        if (!read_all(pipe, &length, sizeof(length)) || length > 32767) return false;
        std::wstring path(length, L'\0');
        if (length != 0 &&
            !read_all(pipe, path.data(), length * sizeof(wchar_t))) {
            return false;
        }
        append_unique_shell_path(paths, path);
    }
    return true;
}

// Explorer can start one legacy static-verb process per selected item even
// with MultiSelectModel=Player. Coalesce that launch burst before showing UI so
// the user gets one archive dialog containing the complete selection.
bool coordinate_shell_add(std::vector<std::wstring>& paths) {
    DWORD session_id = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session_id);
    const std::wstring suffix = std::to_wstring(session_id);
    const std::wstring mutex_name =
        L"Local\\AxiomCompress.ShellAdd." + suffix;
    const std::wstring pipe_name =
        L"\\\\.\\pipe\\AxiomCompress.ShellAdd." + suffix;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
    if (mutex == nullptr) return true;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return !send_shell_add_paths(pipe_name, paths);
    }

    HANDLE pipe = CreateNamedPipeW(
        pipe_name.c_str(), PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
        1, 64 * 1024, 64 * 1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return true;
    }

    auto quiet_deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < quiet_deadline) {
        const BOOL connected = ConnectNamedPipe(pipe, nullptr);
        const DWORD error = connected ? ERROR_SUCCESS : GetLastError();
        if (connected || error == ERROR_PIPE_CONNECTED) {
            DWORD wait_mode = PIPE_READMODE_BYTE | PIPE_WAIT;
            SetNamedPipeHandleState(pipe, &wait_mode, nullptr, nullptr);
            receive_shell_add_paths(pipe, paths);
            DisconnectNamedPipe(pipe);
            DWORD poll_mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
            SetNamedPipeHandleState(pipe, &poll_mode, nullptr, nullptr);
            quiet_deadline = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(250);
        } else if (error == ERROR_NO_DATA) {
            DisconnectNamedPipe(pipe);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    CloseHandle(pipe);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    int argument_count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    std::wstring initial_path;
    AxiomGuiStartupCommand startup;
    if (arguments != nullptr && argument_count > 1) {
        const std::wstring first = arguments[1];
        if (_wcsicmp(first.c_str(), L"--add") == 0) {
            startup.kind = AxiomGuiStartupCommand::Kind::add_to_archive;
            for (int index = 2; index < argument_count; ++index) {
                append_unique_shell_path(startup.paths, arguments[index]);
            }
        } else if (_wcsicmp(first.c_str(), L"--extract") == 0 && argument_count > 2) {
            startup.kind = AxiomGuiStartupCommand::Kind::extract_archive;
            startup.paths.emplace_back(arguments[2]);
            initial_path = arguments[2];
        } else if (_wcsicmp(first.c_str(), L"--test") == 0 && argument_count > 2) {
            startup.kind = AxiomGuiStartupCommand::Kind::test_archive;
            startup.paths.emplace_back(arguments[2]);
            initial_path = arguments[2];
        } else {
            initial_path = first;
        }
    }
    if (arguments != nullptr) LocalFree(arguments);
    if (startup.kind == AxiomGuiStartupCommand::Kind::add_to_archive &&
        !coordinate_shell_add(startup.paths)) {
        return 0;
    }
    return run_axiom_gui(instance, show_command, std::move(initial_path), std::move(startup));
}
