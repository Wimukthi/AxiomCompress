#define NOMINMAX
#include "gui/benchmark_dialog.hpp"

#include "axiom/archive.hpp"
#include "gui/dialog_support.hpp"
#include "gui/message_dialog.hpp"

#include <commctrl.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
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
constexpr int kCloseButton = IDCANCEL;

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
constexpr std::array<const wchar_t*, 6> kThreadNames{
    L"0 (all processors)", L"1", L"2", L"4", L"8", L"16"};
constexpr std::array<std::size_t, 6> kThreadValues{0, 1, 2, 4, 8, 16};
constexpr std::array<const wchar_t*, 3> kPassNames{L"1", L"3", L"5"};
constexpr std::array<int, 3> kPassValues{1, 3, 5};

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
    HFONT font{};
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

std::array<HWND, 13> controls(BenchmarkDialogState* state) {
    if (state == nullptr) return {};
    return {
        state->corpus, state->size, state->level, state->threads, state->passes,
        state->custom_input, state->custom_path, state->browse_custom, state->results,
        state->start, state->pause, state->cancel, state->copy,
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

void set_results_text(BenchmarkDialogState* state, std::wstring text) {
    if (state == nullptr || state->results == nullptr) return;
    state->latest_results = std::move(text);
    SetWindowTextW(state->results, state->latest_results.c_str());
    RedrawWindow(state->results, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
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
    std::wostringstream results;
    try {
        workspace = local_benchmark_root() /
                    (L"run-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                     std::to_wstring(GetTickCount64()));
        const fs::path input = workspace / L"input";
        const fs::path archive = workspace / L"bench.axar";
        const fs::path extract = workspace / L"extract";
        fs::create_directories(workspace);

        operation->set_progress_callback([hwnd](const axiom::OperationProgress& progress) {
            std::wostringstream status;
            status << stage_text(progress.stage);
            if (progress.total_bytes != 0) {
                status << L" " << format_bytes(progress.completed_bytes)
                       << L" / " << format_bytes(progress.total_bytes);
            }
            if (!progress.current_path.empty()) {
                status << L"\r\n" << progress.current_path.c_str();
            }
            post_text(hwnd, status.str());
        });

        results << L"Axiom Benchmark\r\n\r\n";
        if (params.custom_input) {
            results << L"Input: " << params.custom_input->wstring() << L"\r\n";
        } else {
            results << L"Corpus: " << kCorpusNames[static_cast<int>(params.corpus)] << L"\r\n";
            results << L"Input size: " << format_bytes(params.bytes) << L"\r\n";
        }
        results << L"Level: " << params.level << L"\r\n";
        results << L"Threads: " << (params.threads == 0 ? L"all processors" : std::to_wstring(params.threads))
                << L"\r\n";
        results << L"Passes: " << params.passes << L"\r\n\r\n";
        results << (params.custom_input ? L"Scanning input...\r\n" : L"Preparing corpus...\r\n");
        post_text(hwnd, results.str());
        const std::uint64_t original_bytes = params.custom_input
            ? measure_input_bytes(*params.custom_input, operation)
            : generate_corpus(input, params, operation);
        const std::vector<fs::path> benchmark_inputs{
            params.custom_input ? *params.custom_input : input};

        results << L"\r\nPass  Compress       Decompress     Ratio     Archive\r\n";
        results << L"----  -------------  -------------  --------  --------\r\n";
        post_text(hwnd, results.str());

        double compress_total = 0.0;
        double extract_total = 0.0;
        double ratio_total = 0.0;
        std::uint64_t archive_bytes = 0;
        for (int pass = 1; pass <= params.passes; ++pass) {
            operation->checkpoint();
            fs::remove(archive);
            fs::remove_all(extract);
            axiom::CompressionOptions compression;
            axiom::apply_compression_level(compression, params.level);
            compression.thread_count = params.threads;
            compression.operation = operation;
            const auto compress_start = std::chrono::steady_clock::now();
            axiom::create_archive(benchmark_inputs, archive, compression);
            const auto compress_end = std::chrono::steady_clock::now();
            archive_bytes = fs::file_size(archive);

            axiom::DecompressionOptions test_options;
            test_options.thread_count = params.threads;
            test_options.operation = operation;
            axiom::test_archive(archive, test_options);

            axiom::ExtractOptions extract_options;
            extract_options.thread_count = params.threads;
            extract_options.operation = operation;
            extract_options.overwrite = axiom::ExtractOptions::Overwrite::overwrite;
            const auto extract_start = std::chrono::steady_clock::now();
            axiom::extract_archive(archive, extract, extract_options);
            const auto extract_end = std::chrono::steady_clock::now();

            const double compress_seconds =
                std::chrono::duration<double>(compress_end - compress_start).count();
            const double extract_seconds =
                std::chrono::duration<double>(extract_end - extract_start).count();
            const double ratio = original_bytes == 0
                ? 0.0
                : (static_cast<double>(archive_bytes) * 100.0 / static_cast<double>(original_bytes));
            compress_total += compress_seconds;
            extract_total += extract_seconds;
            ratio_total += ratio;

            results << std::setw(4) << pass << L"  "
                    << std::left << std::setw(13)
                    << format_speed(original_bytes, compress_seconds) << L"  "
                    << std::left << std::setw(13)
                    << format_speed(original_bytes, extract_seconds) << L"  "
                    << std::right << std::setw(6) << std::fixed << std::setprecision(1)
                    << ratio << L"%  "
                    << format_bytes(archive_bytes) << L"\r\n";
            post_text(hwnd, results.str());
        }

        results << L"----  -------------  -------------  --------  --------\r\n";
        results << L"Avg   " << std::left << std::setw(13)
                << format_speed(original_bytes, compress_total / params.passes) << L"  "
                << std::left << std::setw(13)
                << format_speed(original_bytes, extract_total / params.passes) << L"  "
                << std::right << std::setw(6) << std::fixed << std::setprecision(1)
                << (ratio_total / params.passes) << L"%  "
                << format_bytes(archive_bytes) << L"\r\n\r\n";
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        results << L"Round-trip verify: Passed\r\n";
        results << L"Elapsed: " << std::fixed << std::setprecision(1) << elapsed << L"s\r\n";
        results << L"Workspace: cleaned\r\n";
        fs::remove_all(workspace);
        post_done(hwnd, true, results.str());
    } catch (const axiom::OperationCancelled&) {
        if (!workspace.empty()) {
            std::error_code ignored;
            fs::remove_all(workspace, ignored);
        }
        post_done(hwnd, false, L"Benchmark cancelled.");
    } catch (const std::exception& ex) {
        if (!workspace.empty()) {
            std::error_code ignored;
            fs::remove_all(workspace, ignored);
        }
        std::wstring message = L"Benchmark failed:\r\n";
        message += std::wstring(ex.what(), ex.what() + std::strlen(ex.what()));
        post_done(hwnd, false, message);
    }
}

void rebuild_fonts(BenchmarkDialogState* state) {
    delete_dialog_font(state->font);
    state->font = create_dialog_font(state->dpi);
    for (HWND control : controls(state)) {
        set_dialog_control_font(control, state->font);
    }
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
    const int browse_width = scale_for_dialog_dpi(86, state->dpi);
    MoveWindow(state->browse_custom, client.right - margin - browse_width, custom_top,
               browse_width, row_height, TRUE);
    MoveWindow(state->custom_path, margin + scale_for_dialog_dpi(150, state->dpi),
               custom_top, client.right - margin * 2 - browse_width -
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

BenchmarkParams collect_params(BenchmarkDialogState* state) {
    BenchmarkParams params;
    params.corpus = static_cast<CorpusKind>(std::clamp(combo_selection(state->corpus), 0, 2));
    params.bytes = kSizeValues[std::clamp(combo_selection(state->size, 1), 0, 3)];
    params.level = std::clamp(combo_selection(state->level, 4), 0, 8) + 1;
    params.threads = kThreadValues[std::clamp(combo_selection(state->threads), 0, 5)];
    params.passes = kPassValues[std::clamp(combo_selection(state->passes, 1), 0, 2)];
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
                L"BUTTON", L"Browse...", WS_TABSTOP | BS_OWNERDRAW, kBrowseCustom);
            state->results = CreateWindowExW(
                0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | ES_MULTILINE |
                    ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY | WS_VSCROLL | WS_HSCROLL,
                0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kResultEdit)),
                state->instance, nullptr);
            set_dialog_control_font(state->results, state->font);
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
            info->ptMinTrackSize.x = scale_for_dialog_dpi(760, state->dpi);
            info->ptMinTrackSize.y = scale_for_dialog_dpi(520, state->dpi);
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
                    DestroyWindow(hwnd);
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
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (state != nullptr) {
                if (state->operation) state->operation->request_cancel();
                if (state->worker.joinable()) state->worker.join();
                delete_dialog_font(state->font);
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
    const int width = scale_for_dialog_dpi(860, effective_dpi);
    const int height = scale_for_dialog_dpi(600, effective_dpi);
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
    EnableWindow(owner, FALSE);
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
    if (IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
        SetFocus(owner);
    }
}

}  // namespace axiom::gui
