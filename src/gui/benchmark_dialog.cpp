#define NOMINMAX
#include "gui/benchmark_dialog.hpp"

#include "axiom/archive.hpp"
#include "core/cpu.hpp"
#include "gui/dialog_support.hpp"
#include "gui/message_dialog.hpp"
#include "gui/update_checker.hpp"

#include <commctrl.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace axiom::gui {
namespace {

namespace fs = std::filesystem;

constexpr wchar_t kBenchmarkDialogClass[] = L"AxiomBenchmarkDialog";
constexpr wchar_t kBenchmarkReportClass[] = L"AxiomBenchmarkReportView";
constexpr UINT kProgressMessage = WM_APP + 140;
constexpr UINT kDoneMessage = WM_APP + 141;

constexpr int kCorpusCombo = 3101;
constexpr int kSizeCombo = 3102;
constexpr int kLevelCombo = 3103;
constexpr int kThreadsCombo = 3104;
constexpr int kPassesCombo = 3105;
constexpr int kResultEdit = 3106;
constexpr int kStartButton = 3107;
constexpr int kPauseButton = 3108;
constexpr int kCancelButton = 3109;
constexpr int kCopyButton = 3110;
constexpr int kCustomInput = 3111;
constexpr int kCustomPath = 3112;
constexpr int kBrowseCustom = 3113;
constexpr int kBrowseCustomFolder = 3114;
constexpr int kCloseButton = IDCANCEL;
constexpr int kBenchmarkInitialWidth = 960;
constexpr int kBenchmarkMinimumWidth = 940;
constexpr int kBenchmarkInitialHeight = 640;
constexpr int kBenchmarkMinimumHeight = 600;

constexpr std::array<const wchar_t*, 3> kCorpusNames{
    L"Text/log corpus", L"Mixed files", L"Binary/random"};
constexpr std::array<const wchar_t*, 4> kSizeNames{
    L"16 MiB", L"64 MiB", L"256 MiB", L"1 GiB"};
constexpr std::array<std::uint64_t, 4> kSizeValues{
    16ull << 20, 64ull << 20, 256ull << 20, 1ull << 30};
constexpr std::array<const wchar_t*, 9> kLevelNames{
    L"1 - Fastest", L"2 - Very fast", L"3 - Fast", L"4 - Normal",
    L"5 - Balanced", L"6 - Strong", L"7 - High", L"8 - Very high",
    L"9 - Maximum"};
constexpr std::array<const wchar_t*, 8> kThreadNames{
    L"0 (all processors)", L"1", L"2", L"4", L"8", L"16", L"32", L"64"};
constexpr std::array<std::size_t, 8> kThreadValues{0, 1, 2, 4, 8, 16, 32, 64};
constexpr std::array<const wchar_t*, 4> kPassNames{L"1", L"3", L"5", L"10"};
constexpr std::array<int, 4> kPassValues{1, 3, 5, 10};

enum class CorpusKind {
    text,
    mixed,
    binary,
};

struct BenchmarkParams {
    CorpusKind corpus = CorpusKind::text;
    std::uint64_t bytes = 64ull << 20;
    int level = 5;
    std::size_t threads = 0;
    int passes = 3;
    std::optional<fs::path> custom_input;
    std::wstring app_version;
};

enum class BenchmarkPhase {
    preparing,
    compressing,
    testing,
    decompressing,
    finished,
};

struct CpuTimeSample {
    double process_seconds = 0.0;
    std::chrono::steady_clock::time_point wall_time{};
};

struct SystemDetails {
    std::wstring cpu_name;
    std::wstring windows_version;
    std::wstring cpu_features;
    std::wstring architecture;
    std::uint64_t total_memory = 0;
    std::size_t hardware_threads = 1;
};

struct PassMetrics {
    int pass = 0;
    std::uint64_t original_bytes = 0;
    std::uint64_t archive_bytes = 0;
    double compress_wall_seconds = 0.0;
    double compress_cpu_seconds = 0.0;
    double extract_wall_seconds = 0.0;
    double extract_cpu_seconds = 0.0;
};

struct BenchmarkLiveState {
    std::mutex mutex;
    BenchmarkParams params;
    SystemDetails system;
    axiom::CompressionOptions compression;
    std::uint64_t original_bytes = 0;
    std::uint64_t archive_bytes = 0;
    std::uint64_t estimated_memory = 0;
    BenchmarkPhase phase = BenchmarkPhase::preparing;
    int current_pass = 0;
    bool input_ready = false;
    bool verified = false;
    bool workspace_cleaned = false;
    std::wstring status;
    axiom::OperationProgress progress;
    CpuTimeSample phase_start;
    std::chrono::steady_clock::time_point started{};
    std::vector<PassMetrics> passes;
};

struct BenchmarkProgressText {
    std::wstring text;
};

struct BenchmarkDone {
    bool success = false;
    std::wstring text;
};

struct BenchmarkDialogState {
    HWND hwnd{};
    HWND owner{};
    HINSTANCE instance{};
    UINT dpi{USER_DEFAULT_SCREEN_DPI};
    bool dark = false;
    bool owner_was_enabled = false;
    HFONT font{};
    HFONT fixed_font{};
    HBRUSH background_brush{};
    HBRUSH edit_brush{};
    HWND corpus{};
    HWND size{};
    HWND level{};
    HWND threads{};
    HWND passes{};
    HWND custom_input{};
    HWND custom_path{};
    HWND browse_custom{};
    HWND browse_custom_folder{};
    HWND results{};
    HWND start{};
    HWND pause{};
    HWND cancel{};
    HWND copy{};
    HWND close{};
    std::shared_ptr<axiom::OperationControl> operation;
    std::thread worker;
    std::atomic_bool running = false;
    bool paused = false;
    bool custom_input_checked = false;
    int report_scroll = 0;
    int report_content_height = 0;
    std::wstring latest_results;
};

template <typename T>
class ComPtr {
public:
    ~ComPtr() { reset(); }
    T** put() {
        reset();
        return &value_;
    }
    T* get() const { return value_; }
    T* operator->() const { return value_; }
private:
    void reset() {
        if (value_) value_->Release();
        value_ = nullptr;
    }
    T* value_ = nullptr;
};

std::array<HWND, 14> controls(BenchmarkDialogState* state) {
    if (state == nullptr) return {};
    return {
        state->corpus, state->size, state->level, state->threads, state->passes,
        state->custom_input, state->custom_path, state->browse_custom,
        state->browse_custom_folder, state->results, state->start, state->pause,
        state->cancel, state->copy,
    };
}

int combo_selection(HWND combo, int fallback = 0) {
    const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    return selected == CB_ERR ? fallback : static_cast<int>(selected);
}

void configure_combo(HWND combo, UINT dpi) {
    if (combo == nullptr) return;
    SendMessageW(combo, CB_SETITEMHEIGHT, 0,
                 scale_for_dialog_dpi(24, dpi));
    SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1),
                 scale_for_dialog_dpi(24, dpi));
    SendMessageW(combo, CB_SETMINVISIBLE, 6, 0);
}

HFONT create_benchmark_results_font(UINT dpi) {
    LOGFONTW font{};
    font.lfHeight = -MulDiv(10, static_cast<int>(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi), 72);
    font.lfWeight = FW_NORMAL;
    font.lfCharSet = DEFAULT_CHARSET;
    font.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(font.lfFaceName, L"Consolas");
    return CreateFontIndirectW(&font);
}

std::wstring window_text(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (length != 0) {
        GetWindowTextW(window, result.data(), length + 1);
    }
    return result;
}

std::wstring trim(std::wstring value) {
    const std::size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const std::size_t last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

template <std::size_t Size>
void fill_combo(HWND combo, const std::array<const wchar_t*, Size>& values, int selected) {
    for (const wchar_t* value : values) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
    }
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
}

int report_line_height(BenchmarkDialogState* state, HDC dc) {
    HFONT old_font = static_cast<HFONT>(SelectObject(dc, state->fixed_font));
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    if (old_font) SelectObject(dc, old_font);
    return metrics.tmHeight + metrics.tmExternalLeading + scale_for_dialog_dpi(1, state->dpi);
}

int report_line_count(std::wstring_view text) {
    if (text.empty()) return 1;
    int lines = 1;
    for (const wchar_t ch : text) {
        if (ch == L'\n') ++lines;
    }
    return lines;
}

void update_report_scrollbar(BenchmarkDialogState* state) {
    if (state == nullptr || state->results == nullptr) return;
    HDC dc = GetDC(state->results);
    if (dc == nullptr) return;
    const int line_height = report_line_height(state, dc);
    ReleaseDC(state->results, dc);

    RECT client{};
    GetClientRect(state->results, &client);
    const int padding = scale_for_dialog_dpi(8, state->dpi);
    state->report_content_height =
        padding * 2 + report_line_count(state->latest_results) * line_height;
    const int visible_height = client.bottom - client.top;
    const int max_scroll = std::max(0, state->report_content_height - visible_height);
    state->report_scroll = std::clamp(state->report_scroll, 0, max_scroll);

    SCROLLINFO info{sizeof(info)};
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = std::max(0, state->report_content_height - 1);
    info.nPage = static_cast<UINT>(std::max(0, visible_height));
    info.nPos = state->report_scroll;
    SetScrollInfo(state->results, SB_VERT, &info, TRUE);
}

void set_results_text(BenchmarkDialogState* state, std::wstring text) {
    if (state == nullptr || state->results == nullptr) return;
    state->latest_results = std::move(text);
    update_report_scrollbar(state);
    RedrawWindow(state->results, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
}

void set_custom_input_checked(BenchmarkDialogState* state, bool checked) {
    if (state == nullptr || state->custom_input == nullptr) return;
    state->custom_input_checked = checked;
    SendMessageW(state->custom_input, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    InvalidateRect(state->custom_input, nullptr, TRUE);
}

std::wstring format_bytes(std::uint64_t bytes) {
    const wchar_t* suffix = L"B";
    double value = static_cast<double>(bytes);
    if (bytes >= (1ull << 30)) {
        value /= static_cast<double>(1ull << 30);
        suffix = L"GiB";
    } else if (bytes >= (1ull << 20)) {
        value /= static_cast<double>(1ull << 20);
        suffix = L"MiB";
    } else if (bytes >= (1ull << 10)) {
        value /= static_cast<double>(1ull << 10);
        suffix = L"KiB";
    }
    std::wostringstream text;
    text << std::fixed << std::setprecision(value >= 100.0 ? 0 : 1) << value << L' ' << suffix;
    return text.str();
}

std::wstring format_speed(std::uint64_t bytes, double seconds) {
    if (seconds <= 0.000001) return L"-";
    const double mib = static_cast<double>(bytes) / static_cast<double>(1u << 20);
    std::wostringstream text;
    text << std::fixed << std::setprecision(1) << (mib / seconds) << L" MiB/s";
    return text.str();
}

std::wstring format_speed_kib(std::uint64_t bytes, double seconds) {
    if (seconds <= 0.000001 || bytes == 0) return L"-";
    const double kib_per_second = static_cast<double>(bytes) / 1024.0 / seconds;
    std::wostringstream text;
    text << std::fixed << std::setprecision(0) << kib_per_second << L" KiB/s";
    return text.str();
}

std::wstring format_decimal(double value, int precision = 1) {
    if (!std::isfinite(value)) return L"-";
    std::wostringstream text;
    text << std::fixed << std::setprecision(precision) << value;
    return text.str();
}

std::wstring format_cpu_usage(double cpu_seconds, double wall_seconds) {
    if (wall_seconds <= 0.000001) return L"-";
    return format_decimal((cpu_seconds / wall_seconds) * 100.0, 0) + L"%";
}

std::wstring format_per_core_speed(std::uint64_t bytes, double wall_seconds,
                                   double cpu_seconds) {
    if (wall_seconds <= 0.000001 || cpu_seconds <= 0.000001 || bytes == 0) return L"-";
    const double mib = static_cast<double>(bytes) / static_cast<double>(1u << 20);
    return format_decimal(mib / cpu_seconds, 1) + L" MiB/s/core";
}

std::wstring format_duration(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    const auto whole = static_cast<std::uint64_t>(seconds + 0.5);
    const auto hours = whole / 3600;
    const auto minutes = (whole / 60) % 60;
    const auto secs = whole % 60;
    std::wostringstream text;
    if (hours != 0) text << hours << L"h ";
    if (hours != 0 || minutes != 0) text << minutes << L"m ";
    text << secs << L"s";
    return text.str();
}

std::wstring ratio_text(std::uint64_t original_bytes, std::uint64_t archive_bytes) {
    if (original_bytes == 0 || archive_bytes == 0) return L"-";
    const double stored_percent =
        static_cast<double>(archive_bytes) * 100.0 / static_cast<double>(original_bytes);
    const double ratio = static_cast<double>(original_bytes) / static_cast<double>(archive_bytes);
    std::wostringstream text;
    text << std::fixed << std::setprecision(2) << ratio << L"x (" << stored_percent << L"%)";
    return text.str();
}

std::wstring widen_ascii(std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

std::uint64_t file_time_ticks(const FILETIME& value) {
    ULARGE_INTEGER combined{};
    combined.LowPart = value.dwLowDateTime;
    combined.HighPart = value.dwHighDateTime;
    return combined.QuadPart;
}

double current_process_cpu_seconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return 0.0;
    }
    return static_cast<double>(file_time_ticks(kernel) + file_time_ticks(user)) / 10000000.0;
}

CpuTimeSample sample_cpu_time() {
    return CpuTimeSample{current_process_cpu_seconds(), std::chrono::steady_clock::now()};
}

double elapsed_wall_seconds(const CpuTimeSample& start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         start.wall_time).count();
}

double elapsed_cpu_seconds(const CpuTimeSample& start) {
    return current_process_cpu_seconds() - start.process_seconds;
}

std::wstring read_registry_string(HKEY root, const wchar_t* path, const wchar_t* name) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegGetValueW(root, path, name, RRF_RT_REG_SZ, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t)) {
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, path, name, RRF_RT_REG_SZ, &type, value.data(), &bytes) !=
        ERROR_SUCCESS) {
        return {};
    }
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

std::wstring windows_version_text() {
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (rtl_get_version != nullptr && rtl_get_version(&version) == 0) {
        std::wostringstream text;
        text << L"Windows " << version.dwMajorVersion << L'.' << version.dwMinorVersion
             << L'.' << version.dwBuildNumber;
        return text.str();
    }
    return L"Windows";
}

std::wstring architecture_text() {
#if defined(_M_X64) || defined(__x86_64__)
    return L"x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return L"ARM64";
#elif defined(_M_IX86) || defined(__i386__)
    return L"x86";
#else
    return L"unknown architecture";
#endif
}

SystemDetails collect_system_details() {
    SystemDetails details;
    details.cpu_name = read_registry_string(
        HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"ProcessorNameString");
    if (details.cpu_name.empty()) details.cpu_name = L"CPU";
    details.windows_version = windows_version_text();
    details.cpu_features = widen_ascii(axiom::core::cpu_features_string());
    details.architecture = architecture_text();
    details.hardware_threads = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    MEMORYSTATUSEX memory{sizeof(memory)};
    if (GlobalMemoryStatusEx(&memory)) {
        details.total_memory = memory.ullTotalPhys;
    }
    return details;
}

std::size_t selected_thread_count(std::size_t requested, std::size_t hardware_threads) {
    if (requested == 0) return std::max<std::size_t>(1, hardware_threads);
    return std::max<std::size_t>(1, requested);
}

std::uint64_t effective_solid_block_size(const axiom::CompressionOptions& options,
                                         std::size_t threads) {
    std::uint64_t block = static_cast<std::uint64_t>(std::max<std::size_t>(1, options.block_size));
    if (options.auto_block_size_for_threads && threads > 1) {
        block = std::max<std::uint64_t>(block, static_cast<std::uint64_t>(threads) << 20);
    }
    return block;
}

std::uint64_t estimate_memory_usage(const axiom::CompressionOptions& options,
                                    std::size_t threads) {
    const std::uint64_t solid_block = effective_solid_block_size(options, threads);
    const std::uint64_t internal_block =
        threads > 1 ? std::max<std::uint64_t>(1ull << 20,
                                              (solid_block + threads - 1) / threads)
                    : solid_block;
    std::uint64_t estimate = solid_block * 2;  // input block plus encoded candidates.
    estimate += static_cast<std::uint64_t>(threads) * (2ull << 20);
    estimate += 64ull << 20;  // entropy tables, directory data, and archive buffers.
    if (options.use_tree_matcher) {
        const std::uint64_t tree_window =
            std::min<std::uint64_t>(static_cast<std::uint64_t>(options.window_size),
                                    internal_block);
        estimate += static_cast<std::uint64_t>(threads) * tree_window * 8;
    } else {
        estimate += static_cast<std::uint64_t>(threads) * (1ull << 20);
    }
    return estimate;
}

std::wstring stage_text(axiom::OperationStage stage) {
    switch (stage) {
        case axiom::OperationStage::scanning: return L"Scanning";
        case axiom::OperationStage::reading: return L"Reading";
        case axiom::OperationStage::compressing: return L"Compressing";
        case axiom::OperationStage::writing: return L"Writing";
        case axiom::OperationStage::testing: return L"Testing";
        case axiom::OperationStage::extracting: return L"Extracting";
        case axiom::OperationStage::finalizing: return L"Finalizing";
    }
    return L"Working";
}

std::wstring phase_text(BenchmarkPhase phase) {
    switch (phase) {
        case BenchmarkPhase::preparing: return L"Preparing";
        case BenchmarkPhase::compressing: return L"Compressing";
        case BenchmarkPhase::testing: return L"Testing";
        case BenchmarkPhase::decompressing: return L"Decompressing";
        case BenchmarkPhase::finished: return L"Finished";
    }
    return L"Working";
}

std::wstring corpus_text(const BenchmarkParams& params) {
    if (params.custom_input) return params.custom_input->wstring();
    return kCorpusNames[static_cast<int>(params.corpus)];
}

void append_metric_header(std::wostringstream& out) {
    out << L"  " << std::left << std::setw(11) << L"Row"
        << L"  " << std::right << std::setw(12) << L"Size"
        << L"  " << std::setw(15) << L"Speed"
        << L"  " << std::setw(12) << L"CPU Usage"
        << L"  " << std::setw(21) << L"Rating / Usage"
        << L"  " << std::setw(15) << L"Rating" << L"\r\n";
}

void append_metric_row(std::wostringstream& out,
                       const wchar_t* label,
                       std::uint64_t display_size,
                       std::uint64_t processed_bytes,
                       double wall_seconds,
                       double cpu_seconds) {
    out << L"  " << std::left << std::setw(11) << label
        << L"  " << std::right << std::setw(12)
        << (display_size == 0 ? std::wstring(L"-") : format_bytes(display_size))
        << L"  " << std::setw(15) << format_speed_kib(processed_bytes, wall_seconds)
        << L"  " << std::setw(12) << format_cpu_usage(cpu_seconds, wall_seconds)
        << L"  " << std::setw(21) << format_per_core_speed(processed_bytes, wall_seconds, cpu_seconds)
        << L"  " << std::setw(15) << format_speed(processed_bytes, wall_seconds) << L"\r\n";
}

void append_placeholder_row(std::wostringstream& out, const wchar_t* label) {
    out << L"  " << std::left << std::setw(11) << label
        << L"  " << std::right << std::setw(12) << L"..."
        << L"  " << std::setw(15) << L"..."
        << L"  " << std::setw(12) << L"..."
        << L"  " << std::setw(21) << L"..."
        << L"  " << std::setw(15) << L"..." << L"\r\n";
}

void append_aggregate_row(std::wostringstream& out,
                          const wchar_t* label,
                          const BenchmarkLiveState& state,
                          bool compression) {
    if (state.passes.empty() || state.original_bytes == 0) {
        append_placeholder_row(out, label);
        return;
    }

    double wall = 0.0;
    double cpu = 0.0;
    for (const auto& pass : state.passes) {
        wall += compression ? pass.compress_wall_seconds : pass.extract_wall_seconds;
        cpu += compression ? pass.compress_cpu_seconds : pass.extract_cpu_seconds;
    }
    const std::uint64_t processed = state.original_bytes *
        static_cast<std::uint64_t>(state.passes.size());
    append_metric_row(out, label, state.original_bytes, processed, wall, cpu);
}

void append_current_phase_row(std::wostringstream& out,
                              const BenchmarkLiveState& state,
                              BenchmarkPhase phase) {
    if (state.phase != phase || state.current_pass <= 0 || state.original_bytes == 0) {
        append_placeholder_row(out, L"Current");
        return;
    }
    const double wall = elapsed_wall_seconds(state.phase_start);
    const double cpu = elapsed_cpu_seconds(state.phase_start);
    std::uint64_t processed = state.progress.completed_bytes;
    if (state.progress.total_bytes == 0 || processed > state.original_bytes) {
        processed = 0;
    }
    append_metric_row(out, L"Current", state.original_bytes, processed, wall, cpu);
}

std::wstring benchmark_status_line(const BenchmarkLiveState& state) {
    if (!state.status.empty()) return state.status;
    if (state.phase == BenchmarkPhase::finished) return L"Finished.";
    return phase_text(state.phase) + L"...";
}

std::wstring render_benchmark_report(const BenchmarkLiveState& state) {
    const std::size_t selected_threads =
        selected_thread_count(state.params.threads, state.system.hardware_threads);
    const std::uint64_t effective_block =
        effective_solid_block_size(state.compression, selected_threads);
    const auto now = std::chrono::steady_clock::now();
    const double elapsed =
        state.started.time_since_epoch().count() == 0
            ? 0.0
            : std::chrono::duration<double>(now - state.started).count();

    std::wostringstream out;
    out << L"Axiom Benchmark\r\n";
    out << L"Settings\r\n";
    out << L"  Input: " << corpus_text(state.params) << L" | Size: "
        << (state.input_ready ? format_bytes(state.original_bytes)
                              : format_bytes(state.params.bytes))
        << L" | Level: " << state.params.level
        << L" | Threads: " << selected_threads << L" / "
        << state.system.hardware_threads << L"\r\n";
    out << L"  Dict: " << format_bytes(state.compression.window_size)
        << L" | Solid: " << format_bytes(effective_block)
        << (state.compression.auto_block_size_for_threads ? L" effective (auto)" : L"")
        << L" | Memory: " << format_bytes(state.estimated_memory);
    if (state.system.total_memory != 0) {
        out << L" / " << format_bytes(state.system.total_memory);
    }
    out << L"\r\n";

    out << L"System\r\n";
    out << L"  CPU: " << state.system.cpu_name << L"\r\n";
    out << L"  OS: " << state.system.windows_version << L"  " << state.system.architecture
        << L"  Axiom " << (state.params.app_version.empty() ? L"unknown" : state.params.app_version)
        << L"\r\n";
    out << L"  Features: " << state.system.cpu_features << L"\r\n";

    out << L"Compressing\r\n";
    append_metric_header(out);
    append_current_phase_row(out, state, BenchmarkPhase::compressing);
    append_aggregate_row(out, L"Resulting", state, true);

    out << L"Decompressing\r\n";
    append_metric_header(out);
    append_current_phase_row(out, state, BenchmarkPhase::decompressing);
    append_aggregate_row(out, L"Resulting", state, false);

    out << L"Run\r\n";
    out << L"  Status: " << benchmark_status_line(state)
        << L" | Elapsed: " << format_duration(elapsed)
        << L" | Passes: " << state.passes.size();
    if (state.phase != BenchmarkPhase::finished && state.current_pass > 0) {
        out << L" completed, pass " << state.current_pass << L" active";
    }
    out << L" / " << state.params.passes << L"\r\n";
    if (state.archive_bytes != 0) {
        out << L"  Archive: " << format_bytes(state.archive_bytes)
            << L" | Ratio: " << ratio_text(state.original_bytes, state.archive_bytes)
            << L" | Verify: " << (state.verified ? L"Passed" : L"...")
            << L" | Workspace: " << (state.workspace_cleaned ? L"cleaned" : L"temporary")
            << L"\r\n";
    } else {
        out << L"  Archive: ... | Verify: " << (state.verified ? L"Passed" : L"...")
            << L" | Workspace: " << (state.workspace_cleaned ? L"cleaned" : L"temporary")
            << L"\r\n";
    }

    if (!state.passes.empty()) {
        out << L"Completed passes\r\n";
        out << L"  " << std::left << std::setw(5) << L"Pass"
            << L"  " << std::right << std::setw(14) << L"Comp Speed"
            << L"  " << std::setw(8) << L"Comp CPU"
            << L"  " << std::setw(14) << L"Dec Speed"
            << L"  " << std::setw(8) << L"Dec CPU"
            << L"  " << std::setw(18) << L"Ratio"
            << L"  " << std::setw(10) << L"Archive" << L"\r\n";
        for (const auto& pass : state.passes) {
            out << L"  " << std::left << std::setw(5) << pass.pass
                << L"  " << std::right << std::setw(14)
                << format_speed_kib(pass.original_bytes, pass.compress_wall_seconds)
                << L"  " << std::setw(8)
                << format_cpu_usage(pass.compress_cpu_seconds, pass.compress_wall_seconds)
                << L"  " << std::setw(14)
                << format_speed_kib(pass.original_bytes, pass.extract_wall_seconds)
                << L"  " << std::setw(8)
                << format_cpu_usage(pass.extract_cpu_seconds, pass.extract_wall_seconds)
                << L"  " << std::setw(18) << ratio_text(pass.original_bytes, pass.archive_bytes)
                << L"  " << std::setw(10) << format_bytes(pass.archive_bytes) << L"\r\n";
        }
    }

    return out.str();
}

fs::path local_benchmark_root() {
    PWSTR local_app_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr,
                                    &local_app_data)) ||
        local_app_data == nullptr) {
        return fs::temp_directory_path() / L"AxiomCompress" / L"Benchmark";
    }
    fs::path root(local_app_data);
    CoTaskMemFree(local_app_data);
    return root / L"AxiomCompress" / L"Benchmark";
}

void write_text_bytes(const fs::path& path, std::uint64_t bytes,
                      const std::shared_ptr<axiom::OperationControl>& operation) {
    static constexpr std::string_view line =
        "Axiom benchmark text corpus: repeated log lines, paths, numbers, and messages. "
        "The purpose is compressible structure with realistic small variations.\r\n";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create benchmark input");
    std::uint64_t written = 0;
    while (written < bytes) {
        operation->checkpoint();
        const std::uint64_t remaining = bytes - written;
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, line.size()));
        output.write(line.data(), static_cast<std::streamsize>(count));
        if (!output) throw std::runtime_error("cannot write benchmark input");
        written += count;
        if ((written & ((1ull << 20) - 1)) == 0 || written == bytes) {
            operation->report({axiom::OperationStage::writing, written, bytes, 0, 0,
                               path.string()});
        }
    }
}

void write_binary_bytes(const fs::path& path, std::uint64_t bytes,
                        const std::shared_ptr<axiom::OperationControl>& operation) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create benchmark input");
    std::array<char, 64 * 1024> buffer{};
    std::uint32_t state = 0xA71015u;
    std::uint64_t written = 0;
    std::uint64_t next_report = 1ull << 20;
    while (written < bytes) {
        operation->checkpoint();
        for (char& value : buffer) {
            state = state * 1664525u + 1013904223u;
            value = static_cast<char>((state >> 24) & 0xff);
        }
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(bytes - written, buffer.size()));
        output.write(buffer.data(), static_cast<std::streamsize>(count));
        if (!output) throw std::runtime_error("cannot write benchmark input");
        written += count;
        if (written >= next_report || written == bytes) {
            operation->report({axiom::OperationStage::writing, written, bytes, 0, 0,
                               path.string()});
            next_report = written + (1ull << 20);
        }
    }
}

void write_repeated_binary_bytes(const fs::path& path, std::uint64_t bytes,
                                 const std::shared_ptr<axiom::OperationControl>& operation) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create benchmark input");
    std::array<char, 64 * 1024> pattern{};
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        pattern[index] = static_cast<char>((index * 37u + (index >> 3)) & 0xff);
    }
    std::uint64_t written = 0;
    std::uint64_t next_report = 1ull << 20;
    while (written < bytes) {
        operation->checkpoint();
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(bytes - written, pattern.size()));
        output.write(pattern.data(), static_cast<std::streamsize>(count));
        if (!output) throw std::runtime_error("cannot write benchmark input");
        written += count;
        if (written >= next_report || written == bytes) {
            operation->report({axiom::OperationStage::writing, written, bytes, 0, 0,
                               path.string()});
            next_report = written + (1ull << 20);
        }
    }
}

std::uint64_t generate_corpus(const fs::path& input_root, const BenchmarkParams& params,
                              const std::shared_ptr<axiom::OperationControl>& operation) {
    fs::create_directories(input_root);
    switch (params.corpus) {
        case CorpusKind::text:
            write_text_bytes(input_root / L"text-log-corpus.txt", params.bytes, operation);
            return params.bytes;
        case CorpusKind::binary:
            write_binary_bytes(input_root / L"random.bin", params.bytes, operation);
            return params.bytes;
        case CorpusKind::mixed: {
            fs::create_directories(input_root / L"docs");
            fs::create_directories(input_root / L"bin");
            const std::uint64_t text = params.bytes * 45 / 100;
            const std::uint64_t random = params.bytes * 35 / 100;
            const std::uint64_t repeated = params.bytes - text - random;
            write_text_bytes(input_root / L"docs" / L"activity.log", text, operation);
            write_binary_bytes(input_root / L"bin" / L"payload.dat", random, operation);
            write_repeated_binary_bytes(input_root / L"bin" / L"records.bin", repeated, operation);
            return params.bytes;
        }
    }
    return params.bytes;
}

std::uint64_t measure_input_bytes(const fs::path& input,
                                  const std::shared_ptr<axiom::OperationControl>& operation) {
    std::error_code ec;
    if (fs::is_regular_file(input, ec)) {
        return fs::file_size(input, ec);
    }
    if (!fs::is_directory(input, ec)) {
        throw std::runtime_error("custom benchmark input is not a file or folder");
    }
    std::uint64_t total = 0;
    for (const auto& entry : fs::recursive_directory_iterator(input, ec)) {
        operation->checkpoint();
        if (ec) throw std::runtime_error("cannot scan custom benchmark input");
        if (!entry.is_regular_file(ec)) continue;
        total += entry.file_size(ec);
        if (ec) throw std::runtime_error("cannot measure custom benchmark input");
    }
    if (total == 0) {
        throw std::runtime_error("custom benchmark folder contains no regular files");
    }
    return total;
}

void post_text(HWND hwnd, const std::wstring& text) {
    PostMessageW(hwnd, kProgressMessage, 0,
                 reinterpret_cast<LPARAM>(new BenchmarkProgressText{text}));
}

void post_done(HWND hwnd, bool success, const std::wstring& text) {
    PostMessageW(hwnd, kDoneMessage, 0,
                 reinterpret_cast<LPARAM>(new BenchmarkDone{success, text}));
}

void benchmark_worker(HWND hwnd, BenchmarkParams params,
                      std::shared_ptr<axiom::OperationControl> operation) {
    const auto started = std::chrono::steady_clock::now();
    fs::path workspace;
    auto live = std::make_shared<BenchmarkLiveState>();
    {
        std::lock_guard lock(live->mutex);
        live->params = params;
        live->system = collect_system_details();
        axiom::apply_compression_level(live->compression, params.level);
        live->compression.thread_count = params.threads;
        const std::size_t threads =
            selected_thread_count(params.threads, live->system.hardware_threads);
        live->estimated_memory = estimate_memory_usage(live->compression, threads);
        live->started = started;
        live->phase_start = sample_cpu_time();
        live->status = L"Starting benchmark...";
    }

    auto publish = [&] {
        std::wstring text;
        {
            std::lock_guard lock(live->mutex);
            text = render_benchmark_report(*live);
        }
        post_text(hwnd, text);
    };

    auto begin_phase = [&](BenchmarkPhase phase, int pass, std::wstring status,
                           CpuTimeSample sample = sample_cpu_time()) {
        {
            std::lock_guard lock(live->mutex);
            live->phase = phase;
            live->current_pass = pass;
            live->progress = {};
            live->phase_start = sample;
            live->status = std::move(status);
        }
        publish();
    };

    try {
        workspace = local_benchmark_root() /
                    (L"run-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                     std::to_wstring(GetTickCount64()));
        const fs::path input = workspace / L"input";
        const fs::path archive = workspace / L"bench.axar";
        const fs::path extract = workspace / L"extract";
        fs::create_directories(workspace);

        operation->set_progress_callback([hwnd, live](const axiom::OperationProgress& progress) {
            std::wstring text;
            {
                std::lock_guard lock(live->mutex);
                live->progress = progress;
                std::wostringstream status;
                status << stage_text(progress.stage);
                if (progress.total_bytes != 0) {
                    status << L" " << format_bytes(progress.completed_bytes)
                           << L" / " << format_bytes(progress.total_bytes);
                }
                if (!progress.current_path.empty()) {
                    status << L"  " << progress.current_path.c_str();
                }
                live->status = status.str();
                text = render_benchmark_report(*live);
            }
            post_text(hwnd, text);
        });

        begin_phase(BenchmarkPhase::preparing, 0,
                    params.custom_input ? L"Scanning input..." : L"Preparing corpus...");
        const std::uint64_t original_bytes = params.custom_input
            ? measure_input_bytes(*params.custom_input, operation)
            : generate_corpus(input, params, operation);
        const std::vector<fs::path> benchmark_inputs{
            params.custom_input ? *params.custom_input : input};
        {
            std::lock_guard lock(live->mutex);
            live->original_bytes = original_bytes;
            live->input_ready = true;
            live->status = L"Input ready.";
        }
        publish();

        for (int pass = 1; pass <= params.passes; ++pass) {
            operation->checkpoint();
            fs::remove(archive);
            fs::remove_all(extract);
            axiom::CompressionOptions compression;
            axiom::apply_compression_level(compression, params.level);
            compression.thread_count = params.threads;
            compression.operation = operation;
            const CpuTimeSample compress_start = sample_cpu_time();
            begin_phase(BenchmarkPhase::compressing, pass,
                        L"Compressing pass " + std::to_wstring(pass) + L"...",
                        compress_start);
            axiom::create_archive(benchmark_inputs, archive, compression);
            const double compress_seconds = elapsed_wall_seconds(compress_start);
            const double compress_cpu_seconds = elapsed_cpu_seconds(compress_start);
            const std::uint64_t archive_bytes = fs::file_size(archive);
            {
                std::lock_guard lock(live->mutex);
                live->archive_bytes = archive_bytes;
                live->status = L"Testing archive...";
            }

            axiom::DecompressionOptions test_options;
            test_options.thread_count = params.threads;
            test_options.operation = operation;
            begin_phase(BenchmarkPhase::testing, pass,
                        L"Testing archive pass " + std::to_wstring(pass) + L"...");
            axiom::test_archive(archive, test_options);
            {
                std::lock_guard lock(live->mutex);
                live->verified = true;
            }

            axiom::ExtractOptions extract_options;
            extract_options.thread_count = params.threads;
            extract_options.operation = operation;
            extract_options.overwrite = axiom::ExtractOptions::Overwrite::overwrite;
            const CpuTimeSample extract_start = sample_cpu_time();
            begin_phase(BenchmarkPhase::decompressing, pass,
                        L"Decompressing pass " + std::to_wstring(pass) + L"...",
                        extract_start);
            axiom::extract_archive(archive, extract, extract_options);
            const double extract_seconds = elapsed_wall_seconds(extract_start);
            const double extract_cpu_seconds = elapsed_cpu_seconds(extract_start);

            {
                std::lock_guard lock(live->mutex);
                live->passes.push_back(PassMetrics{
                    pass,
                    original_bytes,
                    archive_bytes,
                    compress_seconds,
                    compress_cpu_seconds,
                    extract_seconds,
                    extract_cpu_seconds,
                });
                live->phase = BenchmarkPhase::preparing;
                live->current_pass = 0;
                live->progress = {};
                live->status = L"Pass " + std::to_wstring(pass) + L" complete.";
            }
            publish();
        }

        fs::remove_all(workspace);
        {
            std::lock_guard lock(live->mutex);
            live->phase = BenchmarkPhase::finished;
            live->current_pass = 0;
            live->progress = {};
            live->workspace_cleaned = true;
            live->status = L"Finished.";
        }
        std::wstring final_text;
        {
            std::lock_guard lock(live->mutex);
            final_text = render_benchmark_report(*live);
        }
        post_done(hwnd, true, final_text);
    } catch (const axiom::OperationCancelled&) {
        if (!workspace.empty()) {
            std::error_code ignored;
            fs::remove_all(workspace, ignored);
        }
        std::wstring final_text;
        {
            std::lock_guard lock(live->mutex);
            live->phase = BenchmarkPhase::finished;
            live->current_pass = 0;
            live->progress = {};
            live->workspace_cleaned = true;
            live->status = L"Benchmark cancelled.";
            final_text = render_benchmark_report(*live);
        }
        post_done(hwnd, false, final_text);
    } catch (const std::exception& ex) {
        if (!workspace.empty()) {
            std::error_code ignored;
            fs::remove_all(workspace, ignored);
        }
        std::wstring final_text;
        {
            std::lock_guard lock(live->mutex);
            live->phase = BenchmarkPhase::finished;
            live->current_pass = 0;
            live->progress = {};
            live->workspace_cleaned = true;
            live->status = L"Benchmark failed: " +
                std::wstring(ex.what(), ex.what() + std::strlen(ex.what()));
            final_text = render_benchmark_report(*live);
        }
        post_done(hwnd, false, final_text);
    }
}

void rebuild_fonts(BenchmarkDialogState* state) {
    delete_dialog_font(state->font);
    delete_dialog_font(state->fixed_font);
    state->font = create_dialog_font(state->dpi);
    state->fixed_font = create_benchmark_results_font(state->dpi);
    for (HWND control : controls(state)) {
        set_dialog_control_font(control, state->font);
    }
    set_dialog_control_font(state->results, state->fixed_font);
    set_dialog_control_font(state->close, state->font);
    configure_combo(state->corpus, state->dpi);
    configure_combo(state->size, state->dpi);
    configure_combo(state->level, state->dpi);
    configure_combo(state->threads, state->dpi);
    configure_combo(state->passes, state->dpi);
}

void set_running(BenchmarkDialogState* state, bool running) {
    state->running = running;
    EnableWindow(state->corpus, !running);
    EnableWindow(state->size, !running);
    EnableWindow(state->level, !running);
    EnableWindow(state->threads, !running);
    EnableWindow(state->passes, !running);
    EnableWindow(state->custom_input, !running);
    EnableWindow(state->custom_path, !running);
    EnableWindow(state->browse_custom, !running);
    EnableWindow(state->browse_custom_folder, !running);
    EnableWindow(state->start, !running);
    EnableWindow(state->pause, running);
    EnableWindow(state->cancel, running);
    EnableWindow(state->close, !running);
    SetWindowTextW(state->pause, L"Pause");
    state->paused = false;
}

void apply_theme(BenchmarkDialogState* state) {
    apply_dialog_dark_frame(state->hwnd, state->dark);
    for (HWND control : controls(state)) {
        apply_dialog_control_theme(control, state->dark);
    }
    apply_dialog_control_theme(state->close, state->dark);
}

void layout(BenchmarkDialogState* state) {
    RECT client{};
    GetClientRect(state->hwnd, &client);
    const int margin = scale_for_dialog_dpi(18, state->dpi);
    const int label_width = scale_for_dialog_dpi(86, state->dpi);
    const int combo_width = scale_for_dialog_dpi(174, state->dpi);
    const int row_height = scale_for_dialog_dpi(28, state->dpi);
    const int gap = scale_for_dialog_dpi(14, state->dpi);
    const int top = margin + scale_for_dialog_dpi(28, state->dpi);
    const int column2 = margin + label_width + combo_width + scale_for_dialog_dpi(34, state->dpi);
    const int column3 = column2 + label_width + combo_width + scale_for_dialog_dpi(34, state->dpi);
    const int combo_dropdown_height = row_height + scale_for_dialog_dpi(180, state->dpi);
    auto place_combo = [&](HWND combo, int column, int row) {
        MoveWindow(combo, column + label_width, top + row * (row_height + gap),
                   combo_width, combo_dropdown_height, TRUE);
    };
    place_combo(state->corpus, margin, 0);
    place_combo(state->size, column2, 0);
    place_combo(state->level, column3, 0);
    place_combo(state->threads, margin, 1);
    place_combo(state->passes, column2, 1);

    const int custom_top = top + 2 * (row_height + gap);
    MoveWindow(state->custom_input, margin, custom_top + scale_for_dialog_dpi(4, state->dpi),
               scale_for_dialog_dpi(142, state->dpi), row_height, TRUE);
    const int browse_width = scale_for_dialog_dpi(74, state->dpi);
    const int browse_gap = scale_for_dialog_dpi(8, state->dpi);
    MoveWindow(state->browse_custom, client.right - margin - browse_width, custom_top,
                browse_width, row_height, TRUE);
    MoveWindow(state->browse_custom_folder,
               client.right - margin - browse_width * 2 - browse_gap, custom_top,
               browse_width, row_height, TRUE);
    MoveWindow(state->custom_path, margin + scale_for_dialog_dpi(150, state->dpi),
               custom_top, client.right - margin * 2 - browse_width * 2 - browse_gap -
                   scale_for_dialog_dpi(160, state->dpi),
               row_height, TRUE);

    const int results_top = top + 3 * (row_height + gap) + scale_for_dialog_dpi(16, state->dpi);
    const int button_height = scale_for_dialog_dpi(30, state->dpi);
    const int button_width = scale_for_dialog_dpi(90, state->dpi);
    const int button_gap = scale_for_dialog_dpi(10, state->dpi);
    const int button_top = client.bottom - margin - button_height;
    MoveWindow(state->results, margin, results_top,
               client.right - margin * 2,
               button_top - results_top - scale_for_dialog_dpi(16, state->dpi), TRUE);
    update_report_scrollbar(state);
    int x = client.right - margin - button_width;
    MoveWindow(state->close, x, button_top, button_width, button_height, TRUE);
    x -= button_width + button_gap;
    MoveWindow(state->copy, x, button_top, button_width, button_height, TRUE);
    x -= button_width + button_gap;
    MoveWindow(state->cancel, x, button_top, button_width, button_height, TRUE);
    x -= button_width + button_gap;
    MoveWindow(state->pause, x, button_top, button_width, button_height, TRUE);
    x -= button_width + button_gap;
    MoveWindow(state->start, x, button_top, button_width, button_height, TRUE);
    InvalidateRect(state->hwnd, nullptr, TRUE);
}

void paint_labels(BenchmarkDialogState* state, HDC dc) {
    const DialogColors colors = dialog_colors(state->dark);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, colors.text);
    HFONT old_font = static_cast<HFONT>(SelectObject(dc, state->font));
    const int margin = scale_for_dialog_dpi(18, state->dpi);
    const int label_width = scale_for_dialog_dpi(86, state->dpi);
    const int combo_width = scale_for_dialog_dpi(174, state->dpi);
    const int row_height = scale_for_dialog_dpi(28, state->dpi);
    const int gap = scale_for_dialog_dpi(14, state->dpi);
    const int top = margin + scale_for_dialog_dpi(28, state->dpi);
    const int column2 = margin + label_width + combo_width + scale_for_dialog_dpi(34, state->dpi);
    const int column3 = column2 + label_width + combo_width + scale_for_dialog_dpi(34, state->dpi);
    auto draw_label = [&](const wchar_t* text, int column, int row) {
        RECT rect{column, top + row * (row_height + gap),
                  column + label_width, top + row * (row_height + gap) + row_height};
        DrawTextW(dc, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    };
    RECT title{margin, margin, 5000, margin + scale_for_dialog_dpi(24, state->dpi)};
    DrawTextW(dc, L"Benchmark Axiom compression and extraction throughput", -1, &title,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    draw_label(L"Corpus", margin, 0);
    draw_label(L"Size", column2, 0);
    draw_label(L"Level", column3, 0);
    draw_label(L"Threads", margin, 1);
    draw_label(L"Passes", column2, 1);
    if (old_font) SelectObject(dc, old_font);
}

LRESULT control_color(BenchmarkDialogState* state, UINT message, WPARAM wparam, LPARAM lparam) {
    const DialogColors colors = dialog_colors(state->dark);
    HDC dc = reinterpret_cast<HDC>(wparam);
    const HWND target = reinterpret_cast<HWND>(lparam);
    const bool edit_like = message == WM_CTLCOLOREDIT ||
        message == WM_CTLCOLORLISTBOX ||
        target == state->results ||
        target == state->custom_path;
    if (edit_like) {
        SetTextColor(dc, colors.text);
        SetBkColor(dc, colors.control_background);
        SetBkMode(dc, OPAQUE);
        return reinterpret_cast<LRESULT>(state->edit_brush);
    }
    SetTextColor(dc, colors.text);
    SetBkColor(dc, colors.background);
    SetBkMode(dc, TRANSPARENT);
    return reinterpret_cast<LRESULT>(state->background_brush);
}

bool report_heading(std::wstring_view line) {
    if (line.empty() || line.front() == L' ') return false;
    return line.find(L":") == std::wstring_view::npos;
}

void paint_report_view(BenchmarkDialogState* state, HWND window) {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(window, &paint);
    if (target == nullptr) return;

    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        EndPaint(window, &paint);
        return;
    }

    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);

    const DialogColors colors = dialog_colors(state->dark);
    HBRUSH background = CreateSolidBrush(colors.control_background);
    FillRect(dc, &client, background);
    DeleteObject(background);

    const int padding = scale_for_dialog_dpi(8, state->dpi);
    HFONT old_font = static_cast<HFONT>(SelectObject(dc, state->fixed_font));
    const int line_height = report_line_height(state, dc);

    int y = padding - state->report_scroll;
    std::wstring_view text = state->latest_results.empty()
        ? std::wstring_view{L"Ready."}
        : std::wstring_view{state->latest_results};
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const std::size_t end = text.find(L'\n', cursor);
        std::wstring_view line = end == std::wstring_view::npos
            ? text.substr(cursor)
            : text.substr(cursor, end - cursor);
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);

        if (y + line_height >= 0 && y < height) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, report_heading(line) ? colors.focus_border : colors.text);
            TextOutW(dc, padding, y, line.data(), static_cast<int>(line.size()));
        }

        y += line_height;
        if (end == std::wstring_view::npos) break;
        cursor = end + 1;
    }

    state->report_content_height = std::max(height, y + padding + state->report_scroll);
    SCROLLINFO scroll{sizeof(scroll)};
    scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scroll.nMin = 0;
    scroll.nMax = std::max(0, state->report_content_height - 1);
    scroll.nPage = static_cast<UINT>(height);
    scroll.nPos = state->report_scroll;
    SetScrollInfo(window, SB_VERT, &scroll, TRUE);

    if (old_font) SelectObject(dc, old_font);
    HPEN border = CreatePen(PS_SOLID, 1,
                            GetFocus() == window ? colors.focus_border : colors.border);
    HGDIOBJ old_pen = SelectObject(dc, border);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, client.left, client.top, client.right, client.bottom);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(border);

    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(window, &paint);
}

void scroll_report_view(BenchmarkDialogState* state, int delta) {
    if (state == nullptr || state->results == nullptr || delta == 0) return;
    update_report_scrollbar(state);
    RECT client{};
    GetClientRect(state->results, &client);
    const int visible_height = static_cast<int>(client.bottom - client.top);
    const int max_scroll = std::max(0, state->report_content_height - visible_height);
    const int next = std::clamp(state->report_scroll + delta, 0, max_scroll);
    if (next == state->report_scroll) return;
    state->report_scroll = next;
    SCROLLINFO info{sizeof(info)};
    info.fMask = SIF_POS;
    info.nPos = state->report_scroll;
    SetScrollInfo(state->results, SB_VERT, &info, TRUE);
    InvalidateRect(state->results, nullptr, FALSE);
}

LRESULT CALLBACK benchmark_report_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<BenchmarkDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<BenchmarkDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);

    switch (message) {
        case WM_PAINT:
            paint_report_view(state, hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            update_report_scrollbar(state);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_MOUSEWHEEL: {
            HDC dc = GetDC(hwnd);
            const int line_height = dc != nullptr
                ? report_line_height(state, dc)
                : scale_for_dialog_dpi(18, state->dpi);
            if (dc != nullptr) ReleaseDC(hwnd, dc);
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            scroll_report_view(state, -(delta / WHEEL_DELTA) * line_height * 3);
            return 0;
        }
        case WM_KEYDOWN: {
            HDC dc = GetDC(hwnd);
            const int line_height = dc != nullptr
                ? report_line_height(state, dc)
                : scale_for_dialog_dpi(18, state->dpi);
            if (dc != nullptr) ReleaseDC(hwnd, dc);
            RECT client{};
            GetClientRect(hwnd, &client);
            switch (wparam) {
                case VK_UP: scroll_report_view(state, -line_height); return 0;
                case VK_DOWN: scroll_report_view(state, line_height); return 0;
                case VK_PRIOR: scroll_report_view(state, -(client.bottom - client.top)); return 0;
                case VK_NEXT: scroll_report_view(state, client.bottom - client.top); return 0;
                case VK_HOME: scroll_report_view(state, -state->report_scroll); return 0;
                case VK_END:
                    update_report_scrollbar(state);
                    scroll_report_view(state, state->report_content_height);
                    return 0;
            }
            break;
        }
        case WM_VSCROLL: {
            SCROLLINFO info{sizeof(info)};
            info.fMask = SIF_ALL;
            GetScrollInfo(hwnd, SB_VERT, &info);
            int target = state->report_scroll;
            const int page = static_cast<int>(info.nPage);
            switch (LOWORD(wparam)) {
                case SB_LINEUP: target -= scale_for_dialog_dpi(18, state->dpi); break;
                case SB_LINEDOWN: target += scale_for_dialog_dpi(18, state->dpi); break;
                case SB_PAGEUP: target -= page; break;
                case SB_PAGEDOWN: target += page; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: target = info.nTrackPos; break;
                case SB_TOP: target = 0; break;
                case SB_BOTTOM: target = state->report_content_height; break;
                default: return 0;
            }
            scroll_report_view(state, target - state->report_scroll);
            return 0;
        }
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool register_benchmark_report_class(HINSTANCE instance) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = benchmark_report_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kBenchmarkReportClass;
    window_class.hbrBackground = nullptr;
    return RegisterClassExW(&window_class) != 0 ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

BenchmarkParams collect_params(BenchmarkDialogState* state) {
    BenchmarkParams params;
    params.corpus = static_cast<CorpusKind>(std::clamp(combo_selection(state->corpus), 0, 2));
    params.bytes = kSizeValues[std::clamp(combo_selection(state->size, 1), 0, 3)];
    params.level = std::clamp(combo_selection(state->level, 4), 0, 8) + 1;
    params.threads = kThreadValues[std::clamp(
        combo_selection(state->threads), 0, static_cast<int>(kThreadValues.size() - 1))];
    params.passes = kPassValues[std::clamp(
        combo_selection(state->passes, 1), 0, static_cast<int>(kPassValues.size() - 1))];
    if (state->custom_input_checked) {
        const std::wstring path = trim(window_text(state->custom_path));
        if (path.empty()) {
            throw std::runtime_error("choose a custom benchmark file or folder, or clear the custom input checkbox");
        }
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            throw std::runtime_error("custom benchmark input does not exist");
        }
        params.custom_input = fs::path(path);
    }
    return params;
}

void copy_results(BenchmarkDialogState* state) {
    if (state->latest_results.empty() || !OpenClipboard(state->hwnd)) return;
    EmptyClipboard();
    const std::size_t bytes = (state->latest_results.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory != nullptr) {
        void* locked = GlobalLock(memory);
        if (locked != nullptr) {
            std::memcpy(locked, state->latest_results.c_str(), bytes);
            GlobalUnlock(memory);
            SetClipboardData(CF_UNICODETEXT, memory);
            memory = nullptr;
        }
    }
    if (memory != nullptr) GlobalFree(memory);
    CloseClipboard();
}

void start_benchmark(BenchmarkDialogState* state) {
    if (state->running) return;
    if (state->worker.joinable()) state->worker.join();
    BenchmarkParams params;
    try {
        params = collect_params(state);
        params.app_version = current_executable_version(state->instance);
    } catch (const std::exception& ex) {
        show_message_dialog(state->hwnd, state->instance, state->dpi, state->dark,
                            L"Axiom Benchmark",
                            std::wstring(ex.what(), ex.what() + std::strlen(ex.what())),
                            MessageDialogIcon::warning);
        return;
    }
    state->operation = std::make_shared<axiom::OperationControl>();
    set_results_text(state, L"Starting benchmark...");
    set_running(state, true);
    state->worker = std::thread(benchmark_worker, state->hwnd, params, state->operation);
}

void browse_custom_input(BenchmarkDialogState* state) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) {
        return;
    }
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    dialog->SetTitle(L"Choose benchmark input file");
    if (FAILED(dialog->Show(state->hwnd))) return;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return;
    PWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
        SetWindowTextW(state->custom_path, path);
        set_custom_input_checked(state, true);
        CoTaskMemFree(path);
    }
}

void browse_custom_folder(BenchmarkDialogState* state) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) {
        return;
    }
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    dialog->SetTitle(L"Choose benchmark input folder");
    if (FAILED(dialog->Show(state->hwnd))) return;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return;
    PWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
        SetWindowTextW(state->custom_path, path);
        set_custom_input_checked(state, true);
        CoTaskMemFree(path);
    }
}

void close_benchmark_dialog(BenchmarkDialogState* state) {
    if (state == nullptr) return;
    restore_dialog_owner(state->owner, state->owner_was_enabled);
    state->owner_was_enabled = false;
    if (state->owner != nullptr && IsWindow(state->owner)) {
        RedrawWindow(state->owner, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ALLCHILDREN |
                         RDW_UPDATENOW | RDW_NOERASE);
    }
    if (state->hwnd != nullptr && IsWindow(state->hwnd)) {
        DestroyWindow(state->hwnd);
    }
}

LRESULT CALLBACK benchmark_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<BenchmarkDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<BenchmarkDialogState*>(create->lpCreateParams);
        state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    switch (message) {
        case WM_CREATE: {
            state->background_brush = CreateSolidBrush(dialog_colors(state->dark).background);
            state->edit_brush = CreateSolidBrush(dialog_colors(state->dark).control_background);
            state->font = create_dialog_font(state->dpi);
            auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND control = CreateWindowExW(
                    0, cls, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style,
                    0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                    state->instance, nullptr);
                set_dialog_control_font(control, state->font);
                return control;
            };
            const DWORD combo_style = WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED |
                                      CBS_HASSTRINGS | WS_VSCROLL;
            state->corpus = make(L"COMBOBOX", L"", combo_style, kCorpusCombo);
            state->size = make(L"COMBOBOX", L"", combo_style, kSizeCombo);
            state->level = make(L"COMBOBOX", L"", combo_style, kLevelCombo);
            state->threads = make(L"COMBOBOX", L"", combo_style, kThreadsCombo);
            state->passes = make(L"COMBOBOX", L"", combo_style, kPassesCombo);
            configure_combo(state->corpus, state->dpi);
            configure_combo(state->size, state->dpi);
            configure_combo(state->level, state->dpi);
            configure_combo(state->threads, state->dpi);
            configure_combo(state->passes, state->dpi);
            fill_combo(state->corpus, kCorpusNames, 0);
            fill_combo(state->size, kSizeNames, 1);
            fill_combo(state->level, kLevelNames, 4);
            fill_combo(state->threads, kThreadNames, 0);
            fill_combo(state->passes, kPassNames, 1);
            state->custom_input = make(
                L"BUTTON", L"Custom input",
                WS_TABSTOP | BS_AUTOCHECKBOX | BS_OWNERDRAW, kCustomInput);
            state->custom_path = CreateWindowExW(
                0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | ES_AUTOHSCROLL,
                0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCustomPath)),
                state->instance, nullptr);
            set_dialog_control_font(state->custom_path, state->font);
            state->browse_custom = make(
                L"BUTTON", L"File...", WS_TABSTOP | BS_OWNERDRAW, kBrowseCustom);
            state->browse_custom_folder = make(
                L"BUTTON", L"Folder...", WS_TABSTOP | BS_OWNERDRAW, kBrowseCustomFolder);
            state->fixed_font = create_benchmark_results_font(state->dpi);
            // The benchmark report is painted by Axiom so live metrics update
            // without EDIT-control flicker or scroll jumps.
            state->results = CreateWindowExW(
                0, kBenchmarkReportClass, L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | WS_VSCROLL,
                0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kResultEdit)),
                state->instance, state);
            set_dialog_control_font(state->results, state->fixed_font);
            state->start = make(L"BUTTON", L"Start", WS_TABSTOP | BS_OWNERDRAW, kStartButton);
            state->pause = make(L"BUTTON", L"Pause", WS_TABSTOP | BS_OWNERDRAW, kPauseButton);
            state->cancel = make(L"BUTTON", L"Cancel", WS_TABSTOP | BS_OWNERDRAW, kCancelButton);
            state->copy = make(L"BUTTON", L"Copy", WS_TABSTOP | BS_OWNERDRAW, kCopyButton);
            state->close = make(L"BUTTON", L"Close", WS_TABSTOP | BS_OWNERDRAW, kCloseButton);
            EnableWindow(state->pause, FALSE);
            EnableWindow(state->cancel, FALSE);
            set_custom_input_checked(state, false);
            set_results_text(
                state,
                L"Ready.\r\n\r\nSelect a corpus, size, compression level, thread count, and pass count, then press Start.");
            apply_theme(state);
            layout(state);
            return 0;
        }
        case WM_SIZE:
            layout(state);
            return 0;
        case WM_DPICHANGED: {
            state->dpi = HIWORD(wparam);
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            rebuild_fonts(state);
            layout(state);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = scale_for_dialog_dpi(kBenchmarkMinimumWidth, state->dpi);
            info->ptMinTrackSize.y = scale_for_dialog_dpi(kBenchmarkMinimumHeight, state->dpi);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            FillRect(dc, &paint.rcPaint, state->background_brush);
            paint_labels(state, dc);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(hwnd, &client);
            FillRect(reinterpret_cast<HDC>(wparam), &client, state->background_brush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN:
            return control_color(state, message, wparam, lparam);
        case WM_DRAWITEM:
            if (lparam != 0) {
                const auto& draw = *reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
                if (draw.CtlType == ODT_COMBOBOX) {
                    draw_dialog_combo_item(draw, state->dark);
                    return TRUE;
                }
                if (draw.CtlID == kCustomInput) {
                    draw_dialog_checkbox(draw, state->dark, state->custom_input_checked);
                    return TRUE;
                }
                draw_dialog_button(draw, state->dark);
                return TRUE;
            }
            break;
        case WM_MEASUREITEM:
            if (lparam != 0) {
                auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
                measure->itemHeight = scale_for_dialog_dpi(22, state->dpi);
                return TRUE;
            }
            break;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case kStartButton:
                    start_benchmark(state);
                    return 0;
                case kBrowseCustom:
                    browse_custom_input(state);
                    return 0;
                case kBrowseCustomFolder:
                    browse_custom_folder(state);
                    return 0;
                case kCustomInput:
                    if (IsWindowEnabled(state->custom_input)) {
                        set_custom_input_checked(state, !state->custom_input_checked);
                    }
                    return 0;
                case kPauseButton:
                    if (state->running && state->operation) {
                        state->paused = !state->paused;
                        state->operation->set_paused(state->paused);
                        SetWindowTextW(state->pause, state->paused ? L"Resume" : L"Pause");
                    }
                    return 0;
                case kCancelButton:
                    if (state->running && state->operation) {
                        state->operation->request_cancel();
                        set_results_text(state, state->latest_results + L"\r\nCancelling...");
                    }
                    return 0;
                case kCopyButton:
                    copy_results(state);
                    return 0;
                case kCloseButton:
                    if (state->running && state->operation) {
                        state->operation->request_cancel();
                        return 0;
                    }
                    close_benchmark_dialog(state);
                    return 0;
            }
            break;
        case kProgressMessage: {
            std::unique_ptr<BenchmarkProgressText> progress(
                reinterpret_cast<BenchmarkProgressText*>(lparam));
            if (progress) {
                set_results_text(state, std::move(progress->text));
            }
            return 0;
        }
        case kDoneMessage: {
            std::unique_ptr<BenchmarkDone> done(reinterpret_cast<BenchmarkDone*>(lparam));
            if (state->worker.joinable()) state->worker.join();
            set_running(state, false);
            if (done) {
                set_results_text(state, std::move(done->text));
            }
            return 0;
        }
        case WM_CLOSE:
            if (state->running && state->operation) {
                state->operation->request_cancel();
                return 0;
            }
            close_benchmark_dialog(state);
            return 0;
        case WM_NCDESTROY:
            if (state != nullptr) {
                if (state->operation) state->operation->request_cancel();
                if (state->worker.joinable()) state->worker.join();
                delete_dialog_font(state->font);
                delete_dialog_font(state->fixed_font);
                if (state->background_brush) DeleteObject(state->background_brush);
                if (state->edit_brush) DeleteObject(state->edit_brush);
                state->hwnd = nullptr;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

void show_benchmark_dialog(HWND owner, HINSTANCE instance, UINT dpi, bool dark) {
    if (!register_benchmark_report_class(instance)) {
        show_message_dialog(owner, instance, dpi, dark, L"Axiom Benchmark",
                            last_error_text(), MessageDialogIcon::error);
        return;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = benchmark_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kBenchmarkDialogClass;
    window_class.hbrBackground = nullptr;
    assign_axiom_window_class_icons(window_class, instance);
    if (RegisterClassExW(&window_class) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        show_message_dialog(owner, instance, dpi, dark, L"Axiom Benchmark",
                            last_error_text(), MessageDialogIcon::error);
        return;
    }

    const UINT effective_dpi = dpi == 0 ? GetDpiForWindow(owner) : dpi;
    RECT owner_rect{};
    GetWindowRect(owner, &owner_rect);
    const int width = scale_for_dialog_dpi(kBenchmarkInitialWidth, effective_dpi);
    const int height = scale_for_dialog_dpi(kBenchmarkInitialHeight, effective_dpi);
    BenchmarkDialogState state{};
    state.owner = owner;
    state.instance = instance;
    state.dpi = effective_dpi;
    state.dark = dark;
    HWND dialog = CreateWindowExW(
        WS_EX_WINDOWEDGE | WS_EX_CONTROLPARENT, kBenchmarkDialogClass, L"Axiom Benchmark",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
        owner_rect.left + (owner_rect.right - owner_rect.left - width) / 2,
        owner_rect.top + (owner_rect.bottom - owner_rect.top - height) / 2,
        width, height, owner, nullptr, instance, &state);
    if (dialog == nullptr) {
        show_message_dialog(owner, instance, effective_dpi, dark, L"Axiom Benchmark",
                            last_error_text(), MessageDialogIcon::error);
        return;
    }
    apply_axiom_window_icons(dialog, instance);
    state.owner_was_enabled = disable_dialog_owner(owner);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    MSG message{};
    while (IsWindow(dialog)) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (message_targets_window(dialog, message) &&
            message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            SendMessageW(dialog, WM_CLOSE, 0, 0);
            continue;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    restore_dialog_owner(owner, state.owner_was_enabled);
    state.owner_was_enabled = false;
}

}  // namespace axiom::gui
