#include "archive/system_provider.hpp"
#include "core/windows_time.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace axiom {
namespace fs = std::filesystem;
namespace {

std::string lower_ascii(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return text;
}

std::wstring lower_ascii(std::wstring text) {
    for (wchar_t& ch : text) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return text;
}

bool has_ascii_suffix(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.substr(text.size() - suffix.size()) == suffix;
}

bool has_ascii_suffix(std::wstring_view text, std::wstring_view suffix) {
    return text.size() >= suffix.size() &&
           text.substr(text.size() - suffix.size()) == suffix;
}

std::wstring lower_filename(const fs::path& path) {
    return lower_ascii(path.filename().wstring());
}

bool file_has_magic_at(const fs::path& path,
                       std::uint64_t offset,
                       std::span<const std::uint8_t> expected) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) < offset + expected.size()) {
        return false;
    }
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::vector<std::uint8_t> bytes(expected.size());
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    return static_cast<std::size_t>(stream.gcount()) == bytes.size() &&
           std::equal(expected.begin(), expected.end(), bytes.begin());
}

bool file_starts_with_magic(const fs::path& path,
                            std::span<const std::uint8_t> expected) {
    return file_has_magic_at(path, 0, expected);
}

bool looks_like_7z_file(const fs::path& path) {
    constexpr std::array<std::uint8_t, 6> magic{{0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c}};
    return file_starts_with_magic(path, magic);
}

bool looks_like_rar_file(const fs::path& path) {
    constexpr std::array<std::uint8_t, 7> rar4{{'R', 'a', 'r', '!', 0x1a, 0x07, 0x00}};
    constexpr std::array<std::uint8_t, 8> rar5{{'R', 'a', 'r', '!', 0x1a, 0x07, 0x01, 0x00}};
    return file_starts_with_magic(path, rar4) || file_starts_with_magic(path, rar5);
}

bool looks_like_tar_file(const fs::path& path) {
    constexpr std::array<std::uint8_t, 5> magic{{'u', 's', 't', 'a', 'r'}};
    return file_has_magic_at(path, 257, magic);
}

bool looks_like_iso_file(const fs::path& path) {
    constexpr std::array<std::uint8_t, 5> magic{{'C', 'D', '0', '0', '1'}};
    return file_has_magic_at(path, 0x8001, magic);
}

bool looks_like_cab_file(const fs::path& path) {
    constexpr std::array<std::uint8_t, 4> magic{{'M', 'S', 'C', 'F'}};
    return file_starts_with_magic(path, magic);
}

bool has_7z_extension(const fs::path& path) {
    return lower_ascii(path.extension().wstring()) == L".7z";
}

bool has_rar_extension(const fs::path& path) {
    const std::wstring name = lower_filename(path);
    const std::wstring extension = lower_ascii(path.extension().wstring());
    if (extension == L".rar") return true;
    if (extension.size() == 4 && extension[0] == L'.' && extension[1] == L'r' &&
        extension[2] >= L'0' && extension[2] <= L'9' &&
        extension[3] >= L'0' && extension[3] <= L'9') {
        return true;
    }
    return name.find(L".part") != std::wstring::npos && has_ascii_suffix(name, L".rar");
}

bool has_tar_extension(const fs::path& path) {
    const std::wstring name = lower_filename(path);
    constexpr std::array<std::wstring_view, 9> suffixes{
        L".tar", L".tar.gz", L".tgz", L".tar.xz", L".txz",
        L".tar.bz2", L".tbz2", L".tar.zst", L".tzst"};
    return std::any_of(suffixes.begin(), suffixes.end(),
                       [&](std::wstring_view suffix) {
                           return has_ascii_suffix(name, suffix);
                       });
}

bool has_iso_extension(const fs::path& path) {
    return lower_ascii(path.extension().wstring()) == L".iso";
}

bool has_cab_extension(const fs::path& path) {
    return lower_ascii(path.extension().wstring()) == L".cab";
}

bool is_safe_relative(const std::string& path) {
    if (path.empty()) return false;
    const fs::path candidate(path);
    if (candidate.is_absolute() || candidate.has_root_name() || candidate.has_root_directory()) {
        return false;
    }
    for (const auto& part : candidate) {
        if (part == "..") return false;
    }
    return true;
}

bool is_same_or_child(std::string_view path, std::string_view root) {
    if (path == root) return true;
    if (root.empty()) return true;
    if (path.size() <= root.size() || path.substr(0, root.size()) != root) return false;
    return root.back() == '/' || path[root.size()] == '/';
}

bool is_within(const fs::path& base, const fs::path& target) {
    auto b = base.begin();
    auto t = target.begin();
    for (; b != base.end(); ++b, ++t) {
        if (t == target.end() || *b != *t) return false;
    }
    return true;
}

void report_operation(const std::shared_ptr<OperationControl>& operation,
                      OperationStage stage,
                      std::uint64_t completed_bytes,
                      std::uint64_t total_bytes,
                      std::uint64_t completed_items,
                      std::uint64_t total_items,
                      std::string current_path = {}) {
    if (operation) {
        operation->report(OperationProgress{stage, completed_bytes, total_bytes,
                                            completed_items, total_items,
                                            std::move(current_path)});
    }
}

void operation_checkpoint(const std::shared_ptr<OperationControl>& operation) {
    if (operation) operation->checkpoint();
}

class TempFileGuard {
public:
    explicit TempFileGuard(fs::path path) : path_(std::move(path)) {}
    ~TempFileGuard() {
        if (!path_.empty()) {
            std::error_code ignored;
            fs::remove(path_, ignored);
        }
    }
private:
    fs::path path_;
};

class TempDirectoryGuard {
public:
    explicit TempDirectoryGuard(fs::path path) : path_(std::move(path)) {}
    ~TempDirectoryGuard() {
        if (!path_.empty()) {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }
    }
    const fs::path& path() const { return path_; }
private:
    fs::path path_;
};

#ifdef _WIN32

std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (needed <= 0) return std::wstring(text.begin(), text.end());
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        text.data(), static_cast<int>(text.size()),
                        result.data(), needed);
    return result;
}

std::string wide_to_utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                           text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        std::string fallback;
        fallback.reserve(text.size());
        for (wchar_t ch : text) {
            fallback.push_back(ch >= 0 && ch <= 0x7f ? static_cast<char>(ch) : '?');
        }
        return fallback;
    }
    std::string result(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                        text.data(), static_cast<int>(text.size()),
                        result.data(), needed, nullptr, nullptr);
    return result;
}

std::string process_output_to_utf8(std::string_view bytes) {
    if (bytes.empty()) return {};
    auto convert = [&](UINT code_page, DWORD flags) -> std::optional<std::string> {
        const int needed = MultiByteToWideChar(code_page, flags, bytes.data(),
                                               static_cast<int>(bytes.size()),
                                               nullptr, 0);
        if (needed <= 0) return std::nullopt;
        std::wstring wide(static_cast<std::size_t>(needed), L'\0');
        if (MultiByteToWideChar(code_page, flags, bytes.data(),
                                static_cast<int>(bytes.size()), wide.data(),
                                needed) <= 0) {
            return std::nullopt;
        }
        return wide_to_utf8(wide);
    };
    if (auto utf8 = convert(CP_UTF8, MB_ERR_INVALID_CHARS)) return *utf8;
    if (auto ansi = convert(CP_ACP, 0)) return *ansi;
    return std::string(bytes);
}

std::wstring quote_argument(std::wstring_view argument) {
    if (argument.empty()) return L"\"\"";
    const bool needs_quotes = argument.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needs_quotes) return std::wstring(argument);
    std::wstring result = L"\"";
    unsigned backslashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(ch);
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            result.push_back(ch);
            backslashes = 0;
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring command_line_for(const fs::path& executable,
                              const std::vector<std::wstring>& arguments) {
    std::wstring command = quote_argument(executable.wstring());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += quote_argument(argument);
    }
    return command;
}

std::optional<fs::path> path_if_exists(const fs::path& path) {
    std::error_code ec;
    if (fs::exists(path, ec) && !fs::is_directory(path, ec)) return path;
    return std::nullopt;
}

std::optional<fs::path> find_on_path(std::wstring_view executable_name) {
    wchar_t found[MAX_PATH]{};
    if (SearchPathW(nullptr, std::wstring(executable_name).c_str(), nullptr,
                    MAX_PATH, found, nullptr) > 0) {
        return fs::path(found);
    }
    return std::nullopt;
}

fs::path current_executable_directory() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1) {
            return fs::path(std::wstring(buffer.data(), buffer.data() + length)).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::optional<fs::path> system_tar_executable() {
    wchar_t system_dir[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system_dir, MAX_PATH);
    if (length != 0 && length < MAX_PATH) {
        if (auto candidate = path_if_exists(fs::path(system_dir) / L"tar.exe")) {
            return candidate;
        }
    }
    return find_on_path(L"tar.exe");
}

std::optional<fs::path> system_7z_executable() {
    static const std::optional<fs::path> cached = []() -> std::optional<fs::path> {
        const fs::path exe_dir = current_executable_directory();
        if (!exe_dir.empty()) {
            for (const auto& root : {exe_dir, exe_dir.parent_path()}) {
                if (auto bundled = path_if_exists(root / L"backends" / L"7zip" / L"7z.exe")) {
                    return bundled;
                }
            }
        }

        const fs::path cwd = fs::current_path();
        for (const auto& root : {cwd, cwd.parent_path()}) {
            if (auto bundled = path_if_exists(root / L"third_party" / L"7zip" /
                                              L"win-x64" / L"7z.exe")) {
                return bundled;
            }
        }

        return std::nullopt;
    }();
    return cached;
}

struct ProcessResult {
    DWORD exit_code = 0;
    std::string output;
};

struct ProcessProgressSpec {
    OperationStage stage = OperationStage::reading;
    std::uint64_t total_bytes = 0;
    std::uint64_t total_items = 0;
};

std::optional<unsigned> latest_7z_percent(std::string_view output) {
    const std::size_t marker = output.find_last_of('%');
    if (marker == std::string_view::npos) return std::nullopt;
    std::size_t start = marker;
    while (start > 0 && output[start - 1] >= '0' && output[start - 1] <= '9') --start;
    if (start == marker) return std::nullopt;
    unsigned value = 0;
    for (std::size_t index = start; index < marker; ++index) {
        value = value * 10 + static_cast<unsigned>(output[index] - '0');
    }
    return value <= 100 ? std::optional<unsigned>(value) : std::nullopt;
}

std::string latest_7z_path(std::string_view output) {
    const std::size_t marker = output.find_last_of('%');
    if (marker == std::string_view::npos) return {};
    std::size_t end = output.find_first_of("\r\n\b", marker + 1);
    if (end == std::string_view::npos) end = output.size();
    std::string_view suffix = output.substr(marker + 1, end - marker - 1);
    std::size_t path = suffix.find(" T ");
    if (path != std::string_view::npos) {
        suffix.remove_prefix(path + 3);
    } else if ((path = suffix.find(" - ")) != std::string_view::npos) {
        suffix.remove_prefix(path + 3);
    } else {
        return {};
    }
    while (!suffix.empty() && std::isspace(static_cast<unsigned char>(suffix.front()))) {
        suffix.remove_prefix(1);
    }
    while (!suffix.empty() && std::isspace(static_cast<unsigned char>(suffix.back()))) {
        suffix.remove_suffix(1);
    }
    return std::string(suffix);
}

ProcessResult run_process(const fs::path& executable,
                          const std::vector<std::wstring>& arguments,
                          bool capture_stdout,
                          const std::shared_ptr<OperationControl>& operation,
                          std::string_view tool_name,
                          std::optional<ProcessProgressSpec> progress_spec = std::nullopt) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
        throw std::runtime_error("failed to create system archive reader pipe");
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_input == INVALID_HANDLE_VALUE) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        throw std::runtime_error("failed to open NUL for system archive reader");
    }

    HANDLE null_output = nullptr;
    if (!capture_stdout) {
        null_output = CreateFileW(L"NUL", GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (null_output == INVALID_HANDLE_VALUE) {
            CloseHandle(read_pipe);
            CloseHandle(write_pipe);
            CloseHandle(null_input);
            throw std::runtime_error("failed to open NUL for system archive reader");
        }
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_input;
    startup.hStdOutput = capture_stdout ? write_pipe : null_output;
    startup.hStdError = write_pipe;

    PROCESS_INFORMATION process{};
    std::wstring command_line = command_line_for(executable, arguments);
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    const BOOL created = CreateProcessW(
        executable.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);

    CloseHandle(write_pipe);
    CloseHandle(null_input);
    if (null_output != nullptr && null_output != INVALID_HANDLE_VALUE) CloseHandle(null_output);
    if (!created) {
        CloseHandle(read_pipe);
        throw std::runtime_error("failed to start " + std::string(tool_name));
    }
    CloseHandle(process.hThread);

    std::string output;
    std::atomic<bool> output_too_large = false;
    std::exception_ptr reader_failure;
    // A blocking reader keeps the anonymous pipe drained continuously. Polling
    // PeekNamedPipe from the process-wait loop throttled verbose listings to one
    // small pipe burst per wait interval, even though 7-Zip had data ready.
    std::thread pipe_reader([&] {
        try {
            std::array<char, 64 * 1024> buffer{};
            DWORD read = 0;
            unsigned last_percent = 101;
            while (ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()),
                            &read, nullptr) && read != 0) {
                if (output.size() + read > (64u << 20)) {
                    output_too_large.store(true, std::memory_order_relaxed);
                    TerminateProcess(process.hProcess, ERROR_NOT_ENOUGH_MEMORY);
                    break;
                }
                output.append(buffer.data(), buffer.data() + read);
                if (operation && progress_spec) {
                    const std::size_t tail_start = output.size() > 4096
                        ? output.size() - 4096 : 0;
                    const std::string_view tail =
                        std::string_view(output).substr(tail_start);
                    if (const auto percent = latest_7z_percent(tail);
                        percent && *percent != last_percent) {
                        last_percent = *percent;
                        report_operation(
                            operation, progress_spec->stage,
                            progress_spec->total_bytes * *percent / 100,
                            progress_spec->total_bytes,
                            progress_spec->total_items * *percent / 100,
                            progress_spec->total_items,
                            latest_7z_path(tail));
                    }
                }
            }
        } catch (...) {
            reader_failure = std::current_exception();
            TerminateProcess(process.hProcess, ERROR_NOT_ENOUGH_MEMORY);
        }
    });

    std::exception_ptr operation_failure;
    while (WaitForSingleObject(process.hProcess, 10) == WAIT_TIMEOUT) {
        try {
            operation_checkpoint(operation);
        } catch (...) {
            operation_failure = std::current_exception();
            TerminateProcess(process.hProcess, ERROR_CANCELLED);
            break;
        }
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = STILL_ACTIVE;
    GetExitCodeProcess(process.hProcess, &exit_code);
    pipe_reader.join();
    CloseHandle(read_pipe);
    CloseHandle(process.hProcess);

    if (operation_failure) std::rethrow_exception(operation_failure);
    if (reader_failure) std::rethrow_exception(reader_failure);
    if (output_too_large.load(std::memory_order_relaxed)) {
        throw std::runtime_error("system archive reader produced too much output");
    }
    return {exit_code, process_output_to_utf8(output)};
}

ProcessResult run_tar(const std::vector<std::wstring>& arguments,
                      bool capture_stdout,
                      const std::shared_ptr<OperationControl>& operation) {
    const auto executable = system_tar_executable();
    if (!executable) {
        throw std::runtime_error("Windows tar.exe was not found; system archive reader is unavailable");
    }
    return run_process(*executable, arguments, capture_stdout, operation, "Windows tar.exe");
}

ProcessResult run_7z(const std::vector<std::wstring>& arguments,
                     bool capture_stdout,
                     const std::shared_ptr<OperationControl>& operation,
                     std::optional<ProcessProgressSpec> progress_spec = std::nullopt) {
    const auto executable = system_7z_executable();
    if (!executable) {
        throw std::runtime_error("Axiom's bundled 7-Zip backend was not found");
    }
    return run_process(*executable, arguments, capture_stdout, operation, "7-Zip",
                       progress_spec);
}

std::runtime_error tar_error(std::string action, const ProcessResult& result) {
    std::string message = "system archive reader failed while " + std::move(action);
    if (!result.output.empty()) message += ": " + result.output;
    return std::runtime_error(message);
}

std::runtime_error seven_zip_error(std::string action, const ProcessResult& result) {
    std::string message = "7-Zip backend failed while " + std::move(action);
    if (!result.output.empty()) message += ": " + result.output;
    return std::runtime_error(message);
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string line(text.substr(start, end - start));
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (!line.empty()) result.push_back(std::move(line));
        start = end + 1;
    }
    return result;
}

std::string trim_copy(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

std::string_view trim_view(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

std::optional<std::uint64_t> parse_u64(std::string_view text) {
    text = trim_view(text);
    if (text.empty()) return std::nullopt;
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
        ? std::optional<std::uint64_t>{value}
        : std::nullopt;
}

std::optional<std::uint32_t> parse_hex_u32(std::string_view text) {
    text = trim_view(text);
    if (text.empty()) return std::nullopt;
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
        ? std::optional<std::uint32_t>{value}
        : std::nullopt;
}

bool parse_fixed_decimal(std::string_view text, std::size_t offset,
                         std::size_t length, WORD& result) {
    if (offset + length > text.size()) return false;
    unsigned value = 0;
    for (std::size_t index = offset; index < offset + length; ++index) {
        const char character = text[index];
        if (character < '0' || character > '9') return false;
        value = value * 10u + static_cast<unsigned>(character - '0');
    }
    if (value > std::numeric_limits<WORD>::max()) return false;
    result = static_cast<WORD>(value);
    return true;
}

struct SevenZipTimeCache {
    core::LocalTimeConverter converter;

    std::int64_t to_unix(std::string_view text) {
        text = trim_view(text);
        if (text.size() < 19 || text[4] != '-' || text[7] != '-' || text[10] != ' ' ||
            text[13] != ':' || text[16] != ':') {
            return 0;
        }
        SYSTEMTIME local{};
        if (!parse_fixed_decimal(text, 0, 4, local.wYear) ||
            !parse_fixed_decimal(text, 5, 2, local.wMonth) ||
            !parse_fixed_decimal(text, 8, 2, local.wDay) ||
            !parse_fixed_decimal(text, 11, 2, local.wHour) ||
            !parse_fixed_decimal(text, 14, 2, local.wMinute) ||
            !parse_fixed_decimal(text, 17, 2, local.wSecond)) {
            return 0;
        }

        return converter.local_to_unix(local);
    }
};

std::optional<ArchiveEntry> parse_verbose_line(const std::string& line) {
    std::array<std::string_view, 8> fields{};
    std::size_t cursor = 0;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        while (cursor < line.size() && std::isspace(static_cast<unsigned char>(line[cursor]))) {
            ++cursor;
        }
        const std::size_t start = cursor;
        while (cursor < line.size() && !std::isspace(static_cast<unsigned char>(line[cursor]))) {
            ++cursor;
        }
        if (start == cursor) return std::nullopt;
        fields[index] = std::string_view(line).substr(start, cursor - start);
    }

    std::string path = trim_copy(std::string(std::string_view(line).substr(cursor)));
    if (path.empty()) return std::nullopt;

    ArchiveEntry entry;
    entry.is_directory = !fields[0].empty() && fields[0].front() == 'd';
    entry.is_symlink = !fields[0].empty() && fields[0].front() == 'l';
    try {
        entry.size = static_cast<std::uint64_t>(std::stoull(std::string(fields[4])));
    } catch (...) {
        entry.size = 0;
    }
    const auto link = path.find(" -> ");
    if (entry.is_symlink && link != std::string::npos) {
        entry.link_target = path.substr(link + 4);
        path.resize(link);
    }
    while (path.size() > 1 && path.back() == '/') {
        entry.is_directory = true;
        path.pop_back();
    }
    entry.path = path;
    if (!is_safe_relative(entry.path)) {
        throw FormatError("archive contains an unsafe path: " + entry.path);
    }
    return entry;
}

struct SevenZipEntryFields {
    std::string_view path;
    std::string_view size;
    std::string_view packed_size;
    std::string_view modified;
    std::string_view attributes;
    std::string_view crc;
    std::string_view encrypted;
    std::string_view folder;
    std::string_view symlink;
};

std::string normalize_7z_path(std::string_view source) {
    source = trim_view(source);
    std::string path(source);
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

void assign_7z_field(SevenZipEntryFields& fields,
                     std::string_view key,
                     std::string_view value) {
    key = trim_view(key);
    value = trim_view(value);
    if (key == "Path") fields.path = value;
    else if (key == "Size") fields.size = value;
    else if (key == "Packed Size") fields.packed_size = value;
    else if (key == "Modified") fields.modified = value;
    else if (key == "Attributes") fields.attributes = value;
    else if (key == "CRC") fields.crc = value;
    else if (key == "Encrypted") fields.encrypted = value;
    else if (key == "Folder") fields.folder = value;
    else if (key == "Symbolic Link") fields.symlink = value;
}

std::optional<ArchiveEntry> archive_entry_from_7z_fields(const SevenZipEntryFields& fields,
                                                         bool* encrypted,
                                                         SevenZipTimeCache& time_cache) {
    if (encrypted != nullptr && trim_view(fields.encrypted) == "+") {
        *encrypted = true;
    }
    std::string path = normalize_7z_path(fields.path);
    if (path.empty()) return std::nullopt;

    ArchiveEntry entry;
    entry.path = std::move(path);
    entry.is_directory = trim_view(fields.folder) == "+" ||
                         fields.attributes.find('D') != std::string::npos;
    entry.is_symlink = !fields.symlink.empty();
    entry.link_target.assign(fields.symlink);
    if (auto size = parse_u64(fields.size)) entry.size = *size;
    if (auto packed = parse_u64(fields.packed_size)) entry.packed_size = *packed;
    if (auto crc = parse_hex_u32(fields.crc)) {
        entry.crc32 = *crc;
        entry.has_crc32 = true;
    }
    entry.mtime = time_cache.to_unix(fields.modified);
    if (!is_safe_relative(entry.path)) {
        throw FormatError("archive contains an unsafe path: " + entry.path);
    }
    return entry;
}

std::vector<ArchiveEntry> parse_7z_slt_listing(std::string_view output,
                                               bool* any_encrypted = nullptr) {
    std::vector<ArchiveEntry> entries;
    entries.reserve(output.size() / 192u);
    SevenZipTimeCache time_cache;
    SevenZipEntryFields current;
    bool in_entries = false;
    bool have_current = false;

    auto commit = [&]() {
        if (!have_current) return;
        auto entry = archive_entry_from_7z_fields(current, any_encrypted, time_cache);
        if (entry) entries.push_back(std::move(*entry));
        current = SevenZipEntryFields{};
        have_current = false;
    };

    std::size_t start = 0;
    while (start <= output.size()) {
        std::size_t end = output.find('\n', start);
        if (end == std::string_view::npos) end = output.size();
        std::string_view line = output.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.remove_suffix(1);
        }
        const std::string_view trimmed = trim_view(line);
        if (!in_entries) {
            if (trimmed == "----------") in_entries = true;
        } else if (!trimmed.empty()) {
            const std::size_t separator = line.find(" = ");
            if (separator != std::string::npos) {
                const std::string_view key = line.substr(0, separator);
                const std::string_view value = line.substr(separator + 3);
                if (trim_view(key) == "Path") {
                    commit();
                    have_current = true;
                }
                if (have_current) assign_7z_field(current, key, value);
            }
        }
        if (end == output.size()) break;
        start = end + 1;
    }
    commit();
    return entries;
}

bool seven_zip_listing_reports_encryption(std::string_view output) {
    std::size_t start = 0;
    while (start <= output.size()) {
        std::size_t end = output.find('\n', start);
        if (end == std::string_view::npos) end = output.size();
        std::string_view line = output.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                                 line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        constexpr std::string_view key = "Encrypted = ";
        if (line.size() >= key.size() &&
            line.substr(0, key.size()) == key &&
            line.substr(key.size()).find('+') != std::string_view::npos) {
            return true;
        }
        if (end == output.size()) break;
        start = end + 1;
    }
    return false;
}

bool seven_zip_error_indicates_encrypted(std::string text) {
    text = lower_ascii(std::move(text));
    return text.find("wrong password") != std::string::npos ||
           text.find("encrypted archive") != std::string::npos ||
           (text.find("password") != std::string::npos &&
            text.find("encrypted") != std::string::npos);
}

constexpr std::uint32_t kIsoSectorSize = 2048;

std::uint32_t iso_le32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint64_t iso_byte_offset(std::uint32_t extent) {
    return static_cast<std::uint64_t>(extent) * kIsoSectorSize;
}

std::vector<std::uint8_t> read_iso_bytes(std::ifstream& stream,
                                         std::uint64_t file_size,
                                         std::uint64_t offset,
                                         std::uint64_t size) {
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        throw FormatError("ISO directory is too large");
    }
    if (offset > file_size || size > file_size - offset) {
        throw FormatError("ISO directory record points outside the image");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (bytes.empty()) return bytes;
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::size_t>(stream.gcount()) != bytes.size()) {
        throw FormatError("could not read ISO directory data");
    }
    return bytes;
}

std::int64_t parse_iso_recording_time(const std::uint8_t* bytes) {
    const int year = 1900 + static_cast<int>(bytes[0]);
    if (year < 1970 || bytes[1] == 0 || bytes[2] == 0) return 0;

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = static_cast<int>(bytes[1]) - 1;
    tm.tm_mday = static_cast<int>(bytes[2]);
    tm.tm_hour = static_cast<int>(bytes[3]);
    tm.tm_min = static_cast<int>(bytes[4]);
    tm.tm_sec = static_cast<int>(bytes[5]);
    tm.tm_isdst = 0;

    const auto timestamp = _mkgmtime(&tm);
    if (timestamp == static_cast<std::time_t>(-1)) return 0;
    const auto gmt_offset_quarters =
        static_cast<int>(static_cast<signed char>(bytes[6]));
    return static_cast<std::int64_t>(timestamp) -
           static_cast<std::int64_t>(gmt_offset_quarters) * 15 * 60;
}

std::string strip_iso_version(std::string name) {
    const std::size_t separator = name.find_last_of(';');
    if (separator != std::string::npos && separator + 1 < name.size()) {
        bool version_suffix = true;
        for (std::size_t index = separator + 1; index < name.size(); ++index) {
            version_suffix = name[index] >= '0' && name[index] <= '9';
            if (!version_suffix) break;
        }
        if (version_suffix) name.resize(separator);
    }
    while (!name.empty() && name.back() == '.') name.pop_back();
    return name;
}

std::string decode_iso_identifier(const std::uint8_t* name,
                                  std::size_t name_size,
                                  bool joliet) {
    if (name_size == 1 && (name[0] == 0 || name[0] == 1)) {
        return name[0] == 0 ? "." : "..";
    }

    if (joliet) {
        std::wstring wide;
        wide.reserve((name_size + 1) / 2);
        for (std::size_t index = 0; index + 1 < name_size; index += 2) {
            const auto code_unit =
                static_cast<wchar_t>((static_cast<unsigned>(name[index]) << 8) |
                                     static_cast<unsigned>(name[index + 1]));
            if (code_unit != L'\0') wide.push_back(code_unit);
        }
        return strip_iso_version(wide_to_utf8(wide));
    }

    std::string decoded;
    decoded.reserve(name_size);
    for (std::size_t index = 0; index < name_size; ++index) {
        char ch = static_cast<char>(name[index]);
        if (ch == '\\') ch = '/';
        if (static_cast<unsigned char>(ch) < 0x20) ch = '_';
        decoded.push_back(ch);
    }
    return strip_iso_version(std::move(decoded));
}

struct IsoDirectoryRecord {
    std::uint32_t extent = 0;
    std::uint32_t data_length = 0;
    std::int64_t mtime = 0;
    bool is_directory = false;
    std::string name;
};

IsoDirectoryRecord parse_iso_directory_record(const std::vector<std::uint8_t>& bytes,
                                              std::size_t offset,
                                              bool joliet) {
    const std::uint8_t length = bytes[offset];
    if (length < 34 || offset + length > bytes.size()) {
        throw FormatError("ISO directory record is invalid");
    }

    const std::uint8_t name_length = bytes[offset + 32];
    if (33u + name_length > length) {
        throw FormatError("ISO directory record name is invalid");
    }

    IsoDirectoryRecord record;
    record.extent = iso_le32(bytes, offset + 2);
    record.data_length = iso_le32(bytes, offset + 10);
    record.mtime = parse_iso_recording_time(bytes.data() + offset + 18);
    record.is_directory = (bytes[offset + 25] & 0x02) != 0;
    record.name = decode_iso_identifier(bytes.data() + offset + 33,
                                        name_length, joliet);
    return record;
}

struct IsoVolume {
    IsoDirectoryRecord root;
    bool joliet = false;
};

bool is_joliet_descriptor(const std::vector<std::uint8_t>& descriptor) {
    return descriptor.size() >= 91 &&
           descriptor[88] == '%' &&
           descriptor[89] == '/' &&
           (descriptor[90] == '@' || descriptor[90] == 'C' || descriptor[90] == 'E');
}

IsoVolume read_iso_volume(std::ifstream& stream, std::uint64_t file_size) {
    std::optional<IsoVolume> primary;
    std::optional<IsoVolume> joliet;

    for (std::uint32_t sector = 16;
         sector < 256 && iso_byte_offset(sector + 1) <= file_size;
         ++sector) {
        auto descriptor = read_iso_bytes(stream, file_size,
                                         iso_byte_offset(sector),
                                         kIsoSectorSize);
        if (descriptor[1] != 'C' || descriptor[2] != 'D' ||
            descriptor[3] != '0' || descriptor[4] != '0' ||
            descriptor[5] != '1') {
            continue;
        }

        const std::uint8_t type = descriptor[0];
        if (type == 255) break;
        if (type != 1 && type != 2) continue;

        const bool descriptor_is_joliet = type == 2 && is_joliet_descriptor(descriptor);
        auto root = parse_iso_directory_record(descriptor, 156, descriptor_is_joliet);
        if (!root.is_directory || root.extent == 0 || root.data_length == 0) {
            continue;
        }

        IsoVolume volume{std::move(root), descriptor_is_joliet};
        if (descriptor_is_joliet) {
            joliet = std::move(volume);
        } else if (type == 1) {
            primary = std::move(volume);
        }
    }

    if (joliet) return *joliet;
    if (primary) return *primary;
    throw FormatError("ISO image does not contain a readable ISO9660 volume");
}

std::vector<ArchiveEntry> list_iso_native(const fs::path& archive_path) {
    std::ifstream stream(archive_path, std::ios::binary);
    if (!stream) throw std::runtime_error("could not open ISO image");

    stream.seekg(0, std::ios::end);
    const auto signed_size = stream.tellg();
    if (signed_size < 0) throw std::runtime_error("could not determine ISO image size");
    const auto file_size = static_cast<std::uint64_t>(signed_size);
    if (file_size < iso_byte_offset(17)) {
        throw FormatError("ISO image is too small");
    }

    const IsoVolume volume = read_iso_volume(stream, file_size);

    struct PendingDirectory {
        std::uint32_t extent = 0;
        std::uint32_t data_length = 0;
        std::string path;
    };

    std::vector<PendingDirectory> pending;
    pending.push_back({volume.root.extent, volume.root.data_length, {}});
    std::unordered_set<std::uint64_t> visited;
    std::vector<ArchiveEntry> entries;
    entries.reserve(1024);

    for (std::size_t directory_index = 0; directory_index < pending.size();
         ++directory_index) {
        const PendingDirectory directory = pending[directory_index];
        const std::uint64_t key =
            (static_cast<std::uint64_t>(directory.extent) << 32) |
            directory.data_length;
        if (!visited.insert(key).second) continue;

        const auto data = read_iso_bytes(stream, file_size,
                                         iso_byte_offset(directory.extent),
                                         directory.data_length);
        for (std::size_t offset = 0; offset < data.size();) {
            const std::uint8_t length = data[offset];
            if (length == 0) {
                const std::size_t next_sector =
                    ((offset / kIsoSectorSize) + 1) * kIsoSectorSize;
                if (next_sector <= offset) break;
                offset = next_sector;
                continue;
            }

            const auto record = parse_iso_directory_record(data, offset, volume.joliet);
            offset += length;
            if (record.name == "." || record.name == "..") continue;

            std::string path = directory.path.empty()
                ? record.name
                : directory.path + "/" + record.name;
            std::replace(path.begin(), path.end(), '\\', '/');
            if (!is_safe_relative(path)) {
                throw FormatError("ISO image contains an unsafe path: " + path);
            }

            ArchiveEntry entry;
            entry.path = std::move(path);
            entry.is_directory = record.is_directory;
            entry.size = record.is_directory ? 0 : record.data_length;
            if (!record.is_directory) {
                entry.packed_size = record.data_length;
            }
            entry.mtime = record.mtime;
            entries.push_back(entry);

            if (record.is_directory && record.extent != 0 && record.data_length != 0) {
                pending.push_back({record.extent, record.data_length, entries.back().path});
            }

            if (entries.size() > 1'000'000) {
                throw FormatError("ISO image contains too many directory entries");
            }
        }
    }

    return entries;
}

std::wstring seven_zip_password_argument(const std::string& password) {
    std::wstring argument = L"-p";
    argument += utf8_to_wide(password);
    return argument;
}

std::vector<std::wstring> seven_zip_list_arguments(const fs::path& archive_path,
                                                   const std::string& password) {
    return {L"l", L"-slt", L"-sccUTF-8",
            seven_zip_password_argument(password), archive_path.wstring()};
}

struct SevenZipListCacheEntry {
    fs::path path;
    std::uintmax_t file_size = 0;
    fs::file_time_type modified{};
    std::shared_ptr<const ProcessResult> result;
};

std::mutex& seven_zip_list_cache_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::optional<SevenZipListCacheEntry>& seven_zip_list_cache() {
    static std::optional<SevenZipListCacheEntry> cache;
    return cache;
}

std::optional<SevenZipListCacheEntry> seven_zip_cache_key(const fs::path& archive_path) {
    std::error_code ec;
    fs::path absolute = fs::absolute(archive_path, ec);
    if (ec) absolute = archive_path;
    const auto file_size = fs::file_size(archive_path, ec);
    if (ec) return std::nullopt;
    const auto modified = fs::last_write_time(archive_path, ec);
    if (ec) return std::nullopt;
    SevenZipListCacheEntry key;
    key.path = absolute.lexically_normal();
    key.file_size = file_size;
    key.modified = modified;
    return key;
}

bool same_seven_zip_cache_key(const SevenZipListCacheEntry& left,
                              const SevenZipListCacheEntry& right) {
    return left.path == right.path &&
           left.file_size == right.file_size &&
           left.modified == right.modified;
}

std::shared_ptr<const ProcessResult> run_7z_list_cached(const fs::path& archive_path,
                                                        const std::string& password) {
    if (!password.empty()) {
        return std::make_shared<const ProcessResult>(
            run_7z(seven_zip_list_arguments(archive_path, password), true, nullptr));
    }

    auto key = seven_zip_cache_key(archive_path);
    if (key) {
        std::scoped_lock lock(seven_zip_list_cache_mutex());
        const auto& cache = seven_zip_list_cache();
        if (cache && same_seven_zip_cache_key(*cache, *key)) {
            return cache->result;
        }
    }

    auto result = std::make_shared<const ProcessResult>(
        run_7z(seven_zip_list_arguments(archive_path, password), true, nullptr));
    if (key) {
        key->result = result;
        std::scoped_lock lock(seven_zip_list_cache_mutex());
        seven_zip_list_cache() = std::move(*key);
    }
    return result;
}

bool selected_entry(const ArchiveEntry& entry, const std::vector<std::string>& wanted) {
    if (wanted.empty()) return true;
    return std::any_of(wanted.begin(), wanted.end(), [&](const std::string& target) {
        return entry.path == target || is_same_or_child(entry.path, target);
    });
}

fs::path unique_temp_path(std::wstring_view prefix) {
    const fs::path root = fs::temp_directory_path();
    for (int attempt = 0; attempt < 100; ++attempt) {
        fs::path candidate = root / (std::wstring(prefix) + L"-" +
                                     std::to_wstring(GetCurrentProcessId()) + L"-" +
                                     std::to_wstring(GetTickCount64()) + L"-" +
                                     std::to_wstring(attempt));
        std::error_code ec;
        if (!fs::exists(candidate, ec)) return candidate;
    }
    throw std::runtime_error("could not allocate a temporary path");
}

void write_selection_file(const fs::path& path,
                          const std::vector<std::string>& entries) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write archive selection list");
    for (const auto& entry : entries) {
        out.write(entry.data(), static_cast<std::streamsize>(entry.size()));
        out.put('\n');
    }
}

constexpr ArchiveFormatInfo kSevenZInfo{
    ArchiveFormat::seven_z, "7z", "7z archive", "7z archive", ".7z",
    L"7z archives (*.7z)", L"*.7z", false};
constexpr ArchiveFormatInfo kRarInfo{
    ArchiveFormat::rar, "rar", "RAR archive", "RAR archive", ".rar",
    L"RAR archives (*.rar;*.r00;*.part*.rar)", L"*.rar;*.r00;*.part*.rar", false};
constexpr ArchiveFormatInfo kTarInfo{
    ArchiveFormat::tar, "tar", "TAR archive", "TAR archive", ".tar",
    L"TAR archives (*.tar;*.tar.gz;*.tgz;*.tar.xz;*.txz;*.tar.bz2;*.tbz2;*.tar.zst;*.tzst)",
    L"*.tar;*.tar.gz;*.tgz;*.tar.xz;*.txz;*.tar.bz2;*.tbz2;*.tar.zst;*.tzst", false};
constexpr ArchiveFormatInfo kIsoInfo{
    ArchiveFormat::iso, "iso", "ISO image", "ISO image", ".iso",
    L"ISO images (*.iso)", L"*.iso", false};
constexpr ArchiveFormatInfo kCabInfo{
    ArchiveFormat::cab, "cab", "CAB archive", "CAB archive", ".cab",
    L"CAB archives (*.cab)", L"*.cab", false};

class SystemArchiveProvider final : public ArchiveProvider {
public:
    SystemArchiveProvider(const ArchiveFormatInfo& info,
                          bool (*signature_match)(const fs::path&),
                          bool (*extension_match)(const fs::path&),
                          bool prefer_7z)
        : info_(info), signature_match_(signature_match),
          extension_match_(extension_match), prefer_7z_(prefer_7z) {}

    const ArchiveFormatInfo& info() const override { return info_; }
    bool matches_path(const fs::path& path) const override { return extension_match_(path); }
    bool matches_signature(const fs::path& path) const { return signature_match_(path); }

    ArchiveCapabilities capabilities(const fs::path& archive_path,
                                     const std::string& password) const override {
        ArchiveCapabilities result;
        result.metadata = true;
        const bool has_7z = use_7z_backend();
        const bool native_iso_listing = info_.format == ArchiveFormat::iso;
        if (prefer_7z_ && !has_7z && !native_iso_listing) {
            result.encryption =
                info_.format == ArchiveFormat::seven_z || info_.format == ArchiveFormat::rar;
            return result;
        }

        result.list = true;
        result.extract = !prefer_7z_ || has_7z;
        result.test = !prefer_7z_ || has_7z;
        result.selective_extract = !prefer_7z_ || has_7z;
        result.packed_sizes = has_7z || native_iso_listing;
        result.encryption = has_7z &&
            (info_.format == ArchiveFormat::seven_z || info_.format == ArchiveFormat::rar);

        if (!result.encryption) {
            return result;
        }

        std::error_code ec;
        if (!has_7z || !fs::exists(archive_path, ec) ||
            fs::is_directory(archive_path, ec)) {
            return result;
        }

        const auto listed = run_7z_list_cached(archive_path, password);
        if (listed->exit_code == 0) {
            result.encrypted = seven_zip_listing_reports_encryption(listed->output);
            result.directory_encrypted = false;
        } else if (seven_zip_error_indicates_encrypted(listed->output)) {
            result.encrypted = true;
            result.directory_encrypted = true;
        }
        return result;
    }

    std::vector<ArchiveEntry> list(const fs::path& archive_path,
                                   const std::string& password) const override {
        if (info_.format == ArchiveFormat::iso) {
            try {
                auto entries = list_iso_native(archive_path);
                if (!entries.empty() || !use_7z_backend()) return entries;
            } catch (...) {
                if (!use_7z_backend()) throw;
            }
        }

        if (prefer_7z_ && !use_7z_backend()) {
            throw std::runtime_error("7-Zip backend is required for this archive format");
        }
        if (use_7z_backend()) {
            const auto result = run_7z_list_cached(archive_path, password);
            if (result->exit_code != 0) throw seven_zip_error("listing", *result);
            auto entries = parse_7z_slt_listing(result->output);
            if (entries.empty() && !result->output.empty()) {
                throw FormatError("7-Zip archive listing could not be parsed");
            }
            return entries;
        }

        const auto result = run_tar({L"-tvf", archive_path.wstring()}, true, nullptr);
        if (result.exit_code != 0) throw tar_error("listing", result);
        std::vector<ArchiveEntry> entries;
        for (const auto& line : split_lines(result.output)) {
            auto entry = parse_verbose_line(line);
            if (entry) entries.push_back(std::move(*entry));
        }
        if (entries.empty() && !result.output.empty()) {
            throw FormatError("system archive listing could not be parsed");
        }
        return entries;
    }

    void test(const fs::path& archive_path,
              const DecompressionOptions& options) const override {
        report_operation(options.operation, OperationStage::testing, 0, 1, 0, 1,
                         wide_to_utf8(archive_path.filename().wstring()));
        if (prefer_7z_ && !use_7z_backend()) {
            throw std::runtime_error("7-Zip backend is required for this archive format");
        }
        if (use_7z_backend()) {
            const auto result = run_7z({L"t", L"-y", L"-sccUTF-8", L"-bsp1",
                                        seven_zip_password_argument(options.password),
                                        archive_path.wstring()},
                                       true, options.operation,
                                       ProcessProgressSpec{OperationStage::testing, 0, 100});
            if (result.exit_code != 0) throw seven_zip_error("testing", result);
        } else {
            const auto result = run_tar({L"-tf", archive_path.wstring()}, false,
                                        options.operation);
            if (result.exit_code != 0) throw tar_error("testing", result);
        }
        report_operation(options.operation, OperationStage::testing, 1, 1, 1, 1,
                         wide_to_utf8(archive_path.filename().wstring()));
    }

    void extract_all(const fs::path& archive_path,
                     const fs::path& dest_dir,
                     const ExtractOptions& options) const override {
        extract_matching(archive_path, {}, dest_dir, options);
    }

    void extract_selected(const fs::path& archive_path,
                          const std::vector<std::string>& entries,
                          const fs::path& dest_dir,
                          const ExtractOptions& options) const override {
        extract_matching(archive_path, entries, dest_dir, options);
    }

    void create(const std::vector<fs::path>&, const fs::path&,
                const CompressionOptions&) const override {
        throw std::runtime_error("archive format is read-only");
    }
    void add(const std::vector<fs::path>&, const fs::path&,
             const CompressionOptions&) const override {
        throw std::runtime_error("archive format is read-only");
    }
    void add_mapped(const std::vector<ArchiveInput>&, const fs::path&,
                    const CompressionOptions&) const override {
        throw std::runtime_error("archive format is read-only");
    }
    void update(const std::vector<fs::path>&, const fs::path&,
                const CompressionOptions&, bool) const override {
        throw std::runtime_error("archive format is read-only");
    }
    void sync(const std::vector<fs::path>&, const fs::path&,
              const CompressionOptions&) const override {
        throw std::runtime_error("archive format is read-only");
    }
    void delete_entries(const fs::path&, const std::vector<std::string>&,
                        const CompressionOptions&) const override {
        throw std::runtime_error("archive format is read-only");
    }

private:
    bool use_7z_backend() const {
        return prefer_7z_ && system_7z_executable().has_value();
    }

    void extract_matching(const fs::path& archive_path,
                          const std::vector<std::string>& wanted,
                          const fs::path& dest_dir,
                          const ExtractOptions& options) const {
        auto all_entries = list(archive_path, options.password);
        std::vector<ArchiveEntry> selected;
        std::uint64_t total_bytes = 0;
        for (const auto& entry : all_entries) {
            if (!selected_entry(entry, wanted)) continue;
            if (entry.is_symlink || entry.is_hardlink) {
                throw FormatError("system archive reader does not restore links yet: " +
                                  entry.path);
            }
            selected.push_back(entry);
            if (!entry.is_directory) total_bytes += entry.size;
        }
        if (selected.empty() && !wanted.empty()) {
            throw std::runtime_error("selected archive entries were not found");
        }

        std::error_code ec;
        fs::create_directories(dest_dir, ec);
        if (ec) throw std::runtime_error("cannot create destination directory: " + ec.message());
        const fs::path dest_norm = dest_dir.lexically_normal();
        for (const auto& entry : selected) {
            const fs::path target = (dest_dir / fs::path(utf8_to_wide(entry.path))).lexically_normal();
            if (!is_within(dest_norm, target)) {
                throw FormatError("archive path escapes the destination: " + entry.path);
            }
            if (!entry.is_directory && fs::exists(target, ec) &&
                options.overwrite == ExtractOptions::Overwrite::fail) {
                throw std::runtime_error("target already exists: " +
                                         wide_to_utf8(target.wstring()));
            }
        }

        report_operation(options.operation, OperationStage::extracting, 0, total_bytes,
                         0, selected.size());

        const fs::path staging_path = unique_temp_path(L"AxiomSystemArchive");
        fs::create_directories(staging_path, ec);
        if (ec) throw std::runtime_error("cannot create extraction staging directory: " + ec.message());
        TempDirectoryGuard staging(staging_path);

        std::vector<std::wstring> arguments;
        fs::path selection_file;
        std::unique_ptr<TempFileGuard> selection_guard;
        if (!wanted.empty()) {
            std::vector<std::string> names;
            names.reserve(selected.size());
            for (const auto& entry : selected) names.push_back(entry.path);
            selection_file = unique_temp_path(L"AxiomArchiveSelection");
            write_selection_file(selection_file, names);
            selection_guard = std::make_unique<TempFileGuard>(selection_file);
        }

        if (use_7z_backend()) {
            arguments = {L"x", L"-y", L"-sccUTF-8", L"-bsp1",
                         seven_zip_password_argument(options.password),
                         L"-o" + staging.path().wstring(), archive_path.wstring()};
            if (!selection_file.empty()) {
                arguments.push_back(L"-scsUTF-8");
                arguments.push_back(L"@" + selection_file.wstring());
            }
            const auto result = run_7z(
                arguments, true, options.operation,
                ProcessProgressSpec{OperationStage::extracting, total_bytes, 0});
            if (result.exit_code != 0) throw seven_zip_error("extracting", result);
        } else {
            arguments = {L"-xf", archive_path.wstring(), L"-C", staging.path().wstring()};
            if (!selection_file.empty()) {
                arguments.push_back(L"-T");
                arguments.push_back(selection_file.wstring());
            }
            const auto result = run_tar(arguments, true, options.operation);
            if (result.exit_code != 0) throw tar_error("extracting", result);
        }

        std::uint64_t completed_bytes = 0;
        std::uint64_t completed_items = 0;
        for (const auto& entry : selected) {
            operation_checkpoint(options.operation);
            const fs::path relative = fs::path(utf8_to_wide(entry.path));
            const fs::path source = (staging.path() / relative).lexically_normal();
            const fs::path target = (dest_dir / relative).lexically_normal();
            if (entry.is_directory) {
                fs::create_directories(target, ec);
            } else {
                if (!fs::exists(source, ec)) {
                    throw std::runtime_error("system archive reader did not extract: " +
                                             entry.path);
                }
                fs::create_directories(target.parent_path(), ec);
                if (fs::exists(target, ec)) {
                    if (options.overwrite == ExtractOptions::Overwrite::skip) {
                        completed_bytes += entry.size;
                        ++completed_items;
                        report_operation(options.operation, OperationStage::extracting,
                                         completed_bytes, total_bytes,
                                         completed_items, selected.size(), entry.path);
                        continue;
                    }
                    if (fs::is_directory(target, ec)) {
                        throw std::runtime_error("target is a directory: " +
                                                 wide_to_utf8(target.wstring()));
                    }
                    fs::remove(target, ec);
                }
                fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
                if (ec) throw std::runtime_error("failed to copy extracted file: " + ec.message());
                completed_bytes += entry.size;
            }
            ++completed_items;
            report_operation(options.operation, OperationStage::extracting,
                             completed_bytes, total_bytes,
                             completed_items, selected.size(), entry.path);
        }
    }

    const ArchiveFormatInfo& info_;
    bool (*signature_match_)(const fs::path&) = nullptr;
    bool (*extension_match_)(const fs::path&) = nullptr;
    bool prefer_7z_ = false;
};

const SystemArchiveProvider kSevenZProvider(kSevenZInfo, &looks_like_7z_file, &has_7z_extension, true);
const SystemArchiveProvider kRarProvider(kRarInfo, &looks_like_rar_file, &has_rar_extension, true);
const SystemArchiveProvider kTarProvider(kTarInfo, &looks_like_tar_file, &has_tar_extension, false);
const SystemArchiveProvider kIsoProvider(kIsoInfo, &looks_like_iso_file, &has_iso_extension, true);
const SystemArchiveProvider kCabProvider(kCabInfo, &looks_like_cab_file, &has_cab_extension, true);
const std::array<const SystemArchiveProvider*, 5> kProviders{
    &kSevenZProvider, &kRarProvider, &kTarProvider, &kIsoProvider, &kCabProvider};

#endif  // _WIN32

}  // namespace

const ArchiveProvider* system_archive_provider_for_contents(
    const std::filesystem::path& path) {
#ifdef _WIN32
    for (const auto* provider : kProviders) {
        if (provider->matches_signature(path)) return provider;
    }
#else
    (void)path;
#endif
    return nullptr;
}

const ArchiveProvider* system_archive_provider_for_extension(
    const std::filesystem::path& path) {
#ifdef _WIN32
    for (const auto* provider : kProviders) {
        if (provider->matches_path(path)) return provider;
    }
#else
    (void)path;
#endif
    return nullptr;
}


}  // namespace axiom
