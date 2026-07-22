#include "axiom/archive.hpp"
#include "axiom/axiom.hpp"
#include "axiom/version.hpp"
#include "core/cpu.hpp"
#include "core/path_text.hpp"
#include "core/windows_time.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
fs::path executable_path;
bool interactive_command_active = false;

void print_usage() {
    std::cerr <<
        "axiomc - Axiom archiver\n"
        "\n"
        "Archive commands:\n"
        "  axiomc a [options] <archive.axar> <input>...   create, or add/replace into an existing archive\n"
        "  axiomc u [options] <archive.axar> <input>...   update: add new + replace newer (by mtime)\n"
        "  axiomc f [options] <archive.axar> <input>...   fresh: replace newer only, never add new\n"
        "  axiomc s [options] <archive.axar> <input>...   sync: mirror inputs (add/replace + delete missing)\n"
        "  axiomc delete [options] <archive.axar> <path>...  remove entries (dir removes its subtree)\n"
        "  axiomc repack [options] <archive.axar>          rebuild, reclaiming replaced/deleted space\n"
        "  axiomc comment <archive.axar> [text]            show, or set, the archive comment\n"
        "  axiomc lock <archive.axar>                      mark the archive read-only (no unlock)\n"
        "  axiomc recovery <archive.axar> [percent]        show/set recovery redundancy (0 removes)\n"
        "  axiomc repair <archive.axar>                    repair damage using its recovery record\n"
        "  axiomc split <archive.axar> <size> [rev-count]  create numbered/recovery volumes\n"
        "  axiomc join <any-volume> <archive.axar>         join or reconstruct a volume set\n"
        "  axiomc x [options] <archive.axar> [dest-dir]   extract (default: current dir)\n"
        "  axiomc l <archive.axar>                         list contents\n"
        "  axiomc t [options] <archive.axar>               test integrity\n"
        "  axiomc keygen <secret.key> <public.key>          generate an archive signing key\n"
        "  axiomc sign [options] <archive.axar> <secret.key> sign an archive\n"
        "  axiomc verify [options] <archive.axar> [public.key] verify authenticity\n"
        "  axiomc sfx <archive.axar> <output.exe> [stub.exe] create a self-extractor\n"
        "\n"
        "Single-stream commands:\n"
        "  axiomc c [options] <input> <output.axc>         compress one stream\n"
        "  axiomc d [options] <input.axc> <output>         decompress one stream\n"
        "\n"
        "Encryption:\n"
        "  -p, --password STR  encrypt blocks on 'a' (create); supply to read 'x'/'t'/'l'\n"
        "                      AXAR uses Argon2id + XChaCha20-Poly1305; ZIP uses WinZip AES-256\n"
        "  --encrypt-names     AXAR only: also encrypt the directory (ZIP names stay visible)\n"
        "\n"
        "Compression options (a, c):\n"
        "  --level N           1=fastest .. 9=max ratio (default 5)\n"
        "  --fast (=1)         --max (=9)\n"
        "  --threads N        --block-size SIZE   --parallel\n"
        "  --chain-depth N    --nice N            (match-finder speed/ratio)\n"
        "  --lazy / --no-lazy  --fast-entropy     (override level knobs)\n"
        "  --fast-lz          --no-filters        (byte-token profile / disable file filters)\n"
        "  --window SIZE                          (match window; bounds --bt memory)\n"
        "  --swarm                                (levels 1-6, 8-9: cores cooperate\n"
        "                                          inside large blocks; level 7 ignores it)\n"
        "  --recovery N                           add 1..100% Reed-Solomon recovery data\n"
        "  --bt                                   (binary-tree match finder)\n"
        "  --optimal          --optimal-depth N   --optimal-candidates N\n"
        "\n"
        "Decompression options (d, x, t):\n"
        "  --threads N        default 0 = all hardware threads\n"
        "  --overwrite MODE   extract only: fail (default), skip, all\n";
}

bool stream_is_terminal(FILE* stream) {
#ifdef _WIN32
    return _isatty(_fileno(stream)) != 0;
#else
    return isatty(fileno(stream)) != 0;
#endif
}

bool console_colors_enabled() {
    static const bool enabled = [] {
        if (!stream_is_terminal(stdout)) {
            return false;
        }
#ifdef _WIN32
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (output == INVALID_HANDLE_VALUE || output == nullptr) {
            return false;
        }
        DWORD mode = 0;
        if (!GetConsoleMode(output, &mode)) {
            return false;
        }
        if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0) {
            if (!SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
                return false;
            }
        }
#endif
        return true;
    }();
    return enabled;
}

const char* color(const char* sequence) {
    return console_colors_enabled() ? sequence : "";
}

constexpr const char* kColorReset = "\x1b[0m";
constexpr const char* kColorMuted = "\x1b[90m";
constexpr const char* kColorBright = "\x1b[97;1m";
constexpr const char* kColorAmber = "\x1b[38;2;255;191;0m";
constexpr const char* kColorGreen = "\x1b[32;1m";
constexpr const char* kColorRed = "\x1b[31;1m";

const char* stage_name(axiom::OperationStage stage) {
    switch (stage) {
        case axiom::OperationStage::scanning: return "Scanning";
        case axiom::OperationStage::estimating: return "Estimating";
        case axiom::OperationStage::reading: return "Reading";
        case axiom::OperationStage::compressing: return "Compressing";
        case axiom::OperationStage::writing: return "Writing";
        case axiom::OperationStage::testing: return "Testing";
        case axiom::OperationStage::extracting: return "Extracting";
        case axiom::OperationStage::transferring: return "Transferring";
        case axiom::OperationStage::finalizing: return "Finalizing";
    }
    return "Working";
}

std::string format_bytes_per_second(double bytes_per_second) {
    if (bytes_per_second <= 0.0) return "-";
    constexpr std::array<const char*, 5> units{"B/s", "KiB/s", "MiB/s", "GiB/s", "TiB/s"};
    std::size_t unit = 0;
    while (bytes_per_second >= 1024.0 && unit + 1 < units.size()) {
        bytes_per_second /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(bytes_per_second < 10.0 && unit > 0 ? 1 : 0)
        << bytes_per_second << ' ' << units[unit];
    return out.str();
}

std::string format_bytes(std::uint64_t bytes) {
    double value = static_cast<double>(bytes);
    constexpr std::array<const char*, 5> units{"B", "KiB", "MiB", "GiB", "TiB"};
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(value < 10.0 && unit > 0 ? 1 : 0)
        << value << ' ' << units[unit];
    return out.str();
}

int terminal_columns() {
#ifdef _WIN32
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output != INVALID_HANDLE_VALUE && output != nullptr) {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (GetConsoleScreenBufferInfo(output, &info)) {
            return std::max<int>(40, info.srWindow.Right - info.srWindow.Left + 1);
        }
    }
#endif
    return 100;
}

std::string shorten_middle(std::string text, std::size_t maximum) {
    if (text.size() <= maximum) return text;
    if (maximum <= 3) return text.substr(0, maximum);
    const std::size_t head = (maximum - 3) / 2;
    const std::size_t tail = maximum - 3 - head;
    return text.substr(0, head) + "..." + text.substr(text.size() - tail);
}

class TerminalProgressDisplay {
public:
    explicit TerminalProgressDisplay(std::string action)
        : action_(std::move(action)),
          started_(std::chrono::steady_clock::now()),
          last_draw_(started_) {}

    void start() {
        std::lock_guard lock(mutex_);
        draw_line_locked("Starting " + action_ + "...");
    }

    void report(const axiom::OperationProgress& progress) {
        std::lock_guard lock(mutex_);
        latest_ = progress;
        const auto now = std::chrono::steady_clock::now();
        const bool finished = progress.total_bytes > 0 &&
                              progress.completed_bytes >= progress.total_bytes;
        const bool item_finished = progress.total_items > 0 &&
                                   progress.completed_items >= progress.total_items;
        if (!finished && !item_finished &&
            now - last_draw_ < std::chrono::milliseconds(80)) {
            return;
        }
        last_draw_ = now;
        draw_line_locked(render(progress, now));
    }

    void finish(bool ok) {
        std::lock_guard lock(mutex_);
        if (finished_) return;
        finished_ = true;
        const auto now = std::chrono::steady_clock::now();
        const double seconds =
            std::chrono::duration<double>(now - started_).count();
        std::ostringstream line;
        line << (ok ? color(kColorGreen) : color(kColorRed))
             << (ok ? "Done" : "Stopped") << color(kColorReset)
             << color(kColorMuted) << "  " << action_ << " in "
             << std::fixed << std::setprecision(seconds < 10.0 ? 1 : 0)
             << seconds << 's' << color(kColorReset);
        draw_line_locked(line.str());
        std::cout << '\n' << std::flush;
        line_width_ = 0;
    }

private:
    std::string render(const axiom::OperationProgress& progress,
                       std::chrono::steady_clock::time_point now) const {
        const bool has_byte_total = progress.total_bytes > 0;
        const bool has_item_total = progress.total_items > 0;
        double fraction = 0.0;
        if (has_byte_total) {
            fraction = std::min(1.0, static_cast<double>(progress.completed_bytes) /
                                     static_cast<double>(progress.total_bytes));
        } else if (has_item_total) {
            fraction = std::min(1.0, static_cast<double>(progress.completed_items) /
                                     static_cast<double>(progress.total_items));
        }

        constexpr int bar_width = 24;
        const int filled = static_cast<int>(fraction * bar_width + 0.5);
        std::string bar;
        bar.reserve(bar_width);
        for (int index = 0; index < bar_width; ++index) {
            bar.push_back(index < filled ? '#' : '-');
        }

        std::ostringstream line;
        line << color(kColorAmber) << stage_name(progress.stage) << color(kColorReset)
             << " [" << bar << "] ";
        if (has_byte_total || has_item_total) {
            line << std::setw(5) << std::fixed << std::setprecision(1)
                 << (fraction * 100.0) << "%  ";
        } else {
            line << "      ";
        }

        if (has_byte_total) {
            line << format_bytes(progress.completed_bytes) << " / "
                 << format_bytes(progress.total_bytes);
            const double seconds = std::chrono::duration<double>(now - started_).count();
            if (seconds > 0.15 && progress.completed_bytes > 0) {
                line << "  " << format_bytes_per_second(
                    static_cast<double>(progress.completed_bytes) / seconds);
            }
        } else if (progress.completed_bytes > 0) {
            line << format_bytes(progress.completed_bytes);
        }

        if (has_item_total) {
            if (has_byte_total || progress.completed_bytes > 0) line << "  ";
            line << progress.completed_items << '/' << progress.total_items << " items";
        } else if (progress.completed_items > 0) {
            if (has_byte_total || progress.completed_bytes > 0) line << "  ";
            line << progress.completed_items << " items";
        }

        if (progress.current_file_total_bytes > 0) {
            const double file_fraction = std::min(
                1.0, static_cast<double>(progress.current_file_completed_bytes) /
                         static_cast<double>(progress.current_file_total_bytes));
            line << color(kColorMuted) << "  file " << std::fixed << std::setprecision(1)
                 << (file_fraction * 100.0) << "% "
                 << format_bytes(progress.current_file_completed_bytes) << " / "
                 << format_bytes(progress.current_file_total_bytes) << color(kColorReset);
        }

        if (!progress.current_path.empty()) {
            std::string current = progress.current_path;
            std::replace(current.begin(), current.end(), '\\', '/');
            line << color(kColorMuted) << "  " << shorten_middle(current, 36)
                 << color(kColorReset);
        }
        return line.str();
    }

    void draw_line_locked(const std::string& line) {
        std::string visible = line;
        const int columns = terminal_columns();
        const std::size_t maximum = columns > 4 ? static_cast<std::size_t>(columns - 1) : 79;
        if (visible.size() > maximum) visible = shorten_middle(visible, maximum);
        std::cout << '\r' << visible;
        const std::size_t clear = line_width_ > visible.size()
            ? line_width_ - visible.size()
            : 0;
        if (clear > 0) {
            std::cout << std::string(clear, ' ');
        }
        std::cout << std::flush;
        line_width_ = visible.size();
    }

    std::mutex mutex_;
    std::string action_;
    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point last_draw_;
    axiom::OperationProgress latest_;
    std::size_t line_width_ = 0;
    bool finished_ = false;
};

class ScopedInteractiveProgress {
public:
    explicit ScopedInteractiveProgress(std::string action) {
        if (!interactive_command_active || !stream_is_terminal(stdout)) return;
        display_ = std::make_shared<TerminalProgressDisplay>(std::move(action));
        control_ = std::make_shared<axiom::OperationControl>();
        telemetry_thread_ = std::jthread(
            [control = control_, display = display_](std::stop_token stop) {
                std::uint64_t sequence = 0;
                while (!stop.stop_requested()) {
                    if (auto progress = control->latest_progress();
                        progress && progress->sequence != sequence) {
                        sequence = progress->sequence;
                        display->report(*progress);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(80));
                }
            }
        );
        display_->start();
    }

    ~ScopedInteractiveProgress() {
        finish(false);
    }

    ScopedInteractiveProgress(const ScopedInteractiveProgress&) = delete;
    ScopedInteractiveProgress& operator=(const ScopedInteractiveProgress&) = delete;

    std::shared_ptr<axiom::OperationControl> operation() const {
        return control_;
    }

    bool active() const {
        return control_ != nullptr;
    }

    void complete() {
        finish(true);
    }

private:
    void finish(bool ok) {
        if (finished_) return;
        finished_ = true;
        telemetry_thread_.request_stop();
        if (telemetry_thread_.joinable()) telemetry_thread_.join();
        if (control_) {
            if (auto progress = control_->latest_progress()) display_->report(*progress);
        }
        if (display_) {
            display_->finish(ok);
        }
    }

    std::shared_ptr<TerminalProgressDisplay> display_;
    std::shared_ptr<axiom::OperationControl> control_;
    std::jthread telemetry_thread_;
    bool finished_ = false;
};

struct InteractiveCommandScope {
    InteractiveCommandScope() : previous(interactive_command_active) {
        interactive_command_active = true;
    }
    ~InteractiveCommandScope() {
        interactive_command_active = previous;
    }

    bool previous = false;
};

void print_splash() {
    std::cout <<
        color(kColorAmber) <<
        "     ___        _                \n"
        "    / _ \\__  __(_) ___  _ __ ___ \n"
        "   / /_)/\\ \\/ /| |/ _ \\| '_ ` _ \\\n"
        "  / ___/  >  < | | (_) | | | | | |\n"
        "  \\/     /_/\\_\\|_|\\___/|_| |_| |_|\n" <<
        color(kColorReset) << '\n' <<
        color(kColorAmber) << "        Axiom" << color(kColorBright) << " Command Shell"
        << color(kColorReset) << '\n' <<
        color(kColorMuted) << "        Native archive tools for Windows and terminals"
        << color(kColorReset) << "\n\n" <<
        color(kColorMuted) << "  Version:   " << color(kColorAmber) << axiom::kVersion << '\n' <<
        color(kColorMuted) << "  Build:     " << color(kColorAmber) << __DATE__ << ' ' << __TIME__ << '\n' <<
        color(kColorMuted) << "  Author:    " << color(kColorAmber) << axiom::kAuthorName << '\n' <<
        color(kColorMuted) << "  CPU:       " << color(kColorAmber)
        << axiom::core::cpu_features_string() << color(kColorReset) << "\n\n"
        "  type \"help\" for commands, \"exit\" to quit\n\n";
}

void print_interactive_help() {
    std::cout <<
        "Interactive commands:\n"
        "  help                         show full command help\n"
        "  clear                        clear the console\n"
        "  pwd                          print the current working directory\n"
        "  cd <dir>                     change the current working directory\n"
        "  exit | quit                  leave the prompt\n"
        "\n"
        "Examples:\n"
        "  a archive.axar folder file.txt\n"
        "  l archive.axar\n"
        "  t archive.axar\n"
        "  x --overwrite skip archive.axar out\n"
        "  a --level 9 --threads 0 backup.axar D:\\data\n"
        "\n";
}

std::string trim_ascii(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

std::vector<std::string> split_interactive_command(std::string_view line) {
    std::vector<std::string> result;
    std::string current;
    bool in_quotes = false;

    for (std::size_t index = 0; index < line.size(); ++index) {
        const char ch = line[index];
        if (ch == '\\') {
            if (index + 1 < line.size() &&
                (line[index + 1] == '"' || line[index + 1] == '\\')) {
                current.push_back(line[++index]);
            } else {
                current.push_back(ch);
            }
            continue;
        }
        if (ch == '"') {
            in_quotes = !in_quotes;
            continue;
        }
        if (!in_quotes && std::isspace(static_cast<unsigned char>(ch)) != 0) {
            if (!current.empty()) {
                result.push_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (in_quotes) {
        throw std::runtime_error("unterminated quote");
    }
    if (!current.empty()) result.push_back(std::move(current));
    return result;
}

std::size_t parse_size(const std::string& value) {
    std::string digits = value;
    std::size_t multiplier = 1;
    if (!digits.empty()) {
        const auto suffix = digits.back();
        if (suffix == 'k' || suffix == 'K') {
            multiplier = 1024;
            digits.pop_back();
        } else if (suffix == 'm' || suffix == 'M') {
            multiplier = 1024 * 1024;
            digits.pop_back();
        } else if (suffix == 'g' || suffix == 'G') {
            multiplier = static_cast<std::size_t>(1024) * 1024 * 1024;
            digits.pop_back();
        }
    }
    return static_cast<std::size_t>(std::stoull(digits) * multiplier);
}

template <std::size_t Size>
std::array<std::uint8_t, Size> read_key(const fs::path& path) {
    std::array<std::uint8_t, Size> key{};
    std::ifstream input(path, std::ios::binary);
    if (!input || !input.read(reinterpret_cast<char*>(key.data()),
                              static_cast<std::streamsize>(key.size())) ||
        input.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("invalid signing key file: " +
                                 axiom::core::path_to_utf8(path));
    }
    return key;
}

template <std::size_t Size>
void write_key(const fs::path& path, const std::array<std::uint8_t, Size>& key) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output || !output.write(reinterpret_cast<const char*>(key.data()),
                                 static_cast<std::streamsize>(key.size()))) {
        throw std::runtime_error("cannot write signing key file: " +
                                 axiom::core::path_to_utf8(path));
    }
}

// Pulls recognized compression flags out of args, leaving positional arguments.
bool take_compression_flags(std::vector<std::string>& args, axiom::CompressionOptions& options) {
    // First pass: pick the effort level (default 5) and apply its preset, so the
    // remaining explicit flags below act as overrides regardless of ordering.
    int level = 5;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--fast") {
            level = 1;
        } else if (args[i] == "--max") {
            level = 9;
        } else if (args[i] == "--level" && i + 1 < args.size()) {
            level = static_cast<int>(parse_size(args[i + 1]));
        }
    }
    axiom::apply_compression_level(options, level);

    std::vector<std::string> positionals;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= args.size()) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return args[++i];
        };

        if (arg == "--level") {
            (void)next("--level");  // already applied in the first pass
        } else if (arg == "--fast" || arg == "--max") {
            // already applied in the first pass
        } else if (arg == "--lazy") {
            options.lazy_matching = true;
        } else if (arg == "--no-lazy") {
            options.lazy_matching = false;
        } else if (arg == "--fast-entropy") {
            options.fast_entropy = true;
        } else if (arg == "--fast-lz") {
            options.use_fast_lz = true;
            options.use_tree_matcher = false;
            options.enable_optimal_parser = false;
        } else if (arg == "--no-filters") {
            options.enable_file_filters = false;
        } else if (arg == "--threads") {
            options.thread_count = parse_size(next("--threads"));
        } else if (arg == "-p" || arg == "--password") {
            options.password = next(arg.c_str());
        } else if (arg == "--encrypt-names" || arg == "--encrypt-header") {
            options.encrypt_header = true;
        } else if (arg == "--block-size") {
            options.block_size = parse_size(next("--block-size"));
            options.auto_block_size_for_threads = false;
        } else if (arg == "--recovery") {
            options.recovery_percent = static_cast<unsigned>(parse_size(next("--recovery")));
            if (options.recovery_percent > 100) {
                throw std::runtime_error("--recovery must be between 0 and 100");
            }
        } else if (arg == "--chain-depth") {
            options.max_chain_depth = parse_size(next("--chain-depth"));
        } else if (arg == "--nice") {
            options.nice_length = parse_size(next("--nice"));
        } else if (arg == "--window") {
            options.window_size = parse_size(next("--window"));
        } else if (arg == "--bt") {
            options.use_tree_matcher = true;
        } else if (arg == "--swarm") {
            options.swarm_parse = true;
        } else if (arg == "--parallel") {
            options.force_parallel_blocks = true;
        } else if (arg == "--optimal") {
            options.enable_optimal_parser = true;
        } else if (arg == "--optimal-depth") {
            options.enable_optimal_parser = true;
            options.optimal_chain_depth = parse_size(next("--optimal-depth"));
        } else if (arg == "--optimal-candidates") {
            options.enable_optimal_parser = true;
            options.max_parser_candidates = parse_size(next("--optimal-candidates"));
        } else if (arg.rfind("--", 0) == 0) {
            std::cerr << "axiomc: unknown option " << arg << '\n';
            return false;
        } else {
            positionals.push_back(arg);
        }
    }
    args = std::move(positionals);
    return true;
}

bool take_decompression_flags(std::vector<std::string>& args,
                              axiom::DecompressionOptions& options) {
    std::vector<std::string> positionals;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= args.size()) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return args[++i];
        };

        if (arg == "--threads") {
            options.thread_count = parse_size(next("--threads"));
        } else if (arg == "-p" || arg == "--password") {
            options.password = next(arg.c_str());
        } else if (arg.rfind("--", 0) == 0) {
            std::cerr << "axiomc: unknown option " << arg << '\n';
            return false;
        } else {
            positionals.push_back(arg);
        }
    }
    args = std::move(positionals);
    return true;
}

std::string format_time(std::int64_t seconds) {
#ifdef _WIN32
    thread_local axiom::core::LocalTimeConverter converter;
    SYSTEMTIME parts{};
    if (!converter.unix_to_local(seconds, parts)) return "-";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u %02u:%02u:%02u",
                  parts.wYear, parts.wMonth, parts.wDay,
                  parts.wHour, parts.wMinute, parts.wSecond);
    return buffer;
#else
    const auto time = static_cast<std::time_t>(seconds);
    std::tm parts{};
    localtime_r(&time, &parts);
    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts) == 0) {
        return "-";
    }
    return buffer;
#endif
}

int run_add(std::vector<std::string> args) {
    axiom::CompressionOptions options;
    if (!take_compression_flags(args, options)) {
        return 2;
    }
    if (args.size() < 2) {
        print_usage();
        return 2;
    }

    const fs::path archive = args.front();
    std::vector<fs::path> inputs(args.begin() + 1, args.end());
    ScopedInteractiveProgress progress(fs::exists(archive) ? "updating archive"
                                                           : "creating archive");
    options.operation = progress.operation();
    // `a` creates a new archive, or adds to (and updates entries in) an existing one.
    if (fs::exists(archive)) {
        axiom::add_to_archive(inputs, archive, options);
    } else {
        axiom::create_archive(inputs, archive, options);
    }
    progress.complete();
    return 0;
}

int run_update(std::vector<std::string> args, bool fresh_only) {
    axiom::CompressionOptions options;
    if (!take_compression_flags(args, options)) {
        return 2;
    }
    if (args.size() < 2) {
        print_usage();
        return 2;
    }
    const fs::path archive = args.front();
    std::vector<fs::path> inputs(args.begin() + 1, args.end());
    ScopedInteractiveProgress progress(fresh_only ? "freshening archive"
                                                  : "updating archive");
    options.operation = progress.operation();
    axiom::update_archive(inputs, archive, options, fresh_only);
    progress.complete();
    return 0;
}

int run_sync(std::vector<std::string> args) {
    axiom::CompressionOptions options;
    if (!take_compression_flags(args, options)) {
        return 2;
    }
    if (args.size() < 2) {
        print_usage();
        return 2;
    }
    const fs::path archive = args.front();
    std::vector<fs::path> inputs(args.begin() + 1, args.end());
    ScopedInteractiveProgress progress("synchronizing archive");
    options.operation = progress.operation();
    axiom::sync_archive(inputs, archive, options);
    progress.complete();
    return 0;
}

int run_comment(std::vector<std::string> args) {
    if (args.empty() || args.size() > 2) {
        print_usage();
        return 2;
    }
    const fs::path archive = args.front();
    if (args.size() == 1) {
        const std::string comment = axiom::archive_comment(archive);
        if (comment.empty()) {
            std::cout << "(no comment)\n";
        } else {
            std::cout << comment << '\n';
        }
        return 0;
    }
    axiom::CompressionOptions options;
    ScopedInteractiveProgress progress("updating archive comment");
    options.operation = progress.operation();
    axiom::set_archive_comment(archive, args[1], options);
    progress.complete();
    return 0;
}

int run_lock(std::vector<std::string> args) {
    if (args.size() != 1) {
        print_usage();
        return 2;
    }
    axiom::CompressionOptions options;
    ScopedInteractiveProgress progress("locking archive");
    options.operation = progress.operation();
    axiom::lock_archive(args.front(), options);
    progress.complete();
    return 0;
}

int run_delete(std::vector<std::string> args) {
    axiom::CompressionOptions options;
    if (!take_compression_flags(args, options)) {
        return 2;
    }
    if (args.size() < 2) {
        print_usage();
        return 2;
    }
    const fs::path archive = args.front();
    const std::vector<std::string> paths(args.begin() + 1, args.end());
    ScopedInteractiveProgress progress("deleting archive entries");
    options.operation = progress.operation();
    axiom::delete_from_archive(archive, paths, options);
    progress.complete();
    return 0;
}

int run_repack(std::vector<std::string> args) {
    axiom::CompressionOptions options;
    if (!take_compression_flags(args, options)) {
        return 2;
    }
    if (args.size() != 1) {
        print_usage();
        return 2;
    }
    ScopedInteractiveProgress progress("repacking archive");
    options.operation = progress.operation();
    axiom::repack_archive(args.front(), options);
    progress.complete();
    return 0;
}

int run_extract(std::vector<std::string> args) {
    axiom::ExtractOptions extract;
    axiom::DecompressionOptions decompression;
    std::vector<std::string> positionals;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--overwrite") {
            if (i + 1 >= args.size()) {
                print_usage();
                return 2;
            }
            const std::string mode = args[++i];
            if (mode == "fail") {
                extract.overwrite = axiom::ExtractOptions::Overwrite::fail;
            } else if (mode == "skip") {
                extract.overwrite = axiom::ExtractOptions::Overwrite::skip;
            } else if (mode == "all") {
                extract.overwrite = axiom::ExtractOptions::Overwrite::overwrite;
            } else {
                std::cerr << "axiomc: unknown overwrite mode " << mode << '\n';
                return 2;
            }
        } else if (args[i] == "--threads") {
            if (i + 1 >= args.size()) {
                print_usage();
                return 2;
            }
            decompression.thread_count = parse_size(args[++i]);
        } else if (args[i] == "-p" || args[i] == "--password") {
            if (i + 1 >= args.size()) {
                print_usage();
                return 2;
            }
            extract.password = args[++i];
        } else if (args[i].rfind("--", 0) == 0) {
            std::cerr << "axiomc: unknown option " << args[i] << '\n';
            return 2;
        } else {
            positionals.push_back(args[i]);
        }
    }

    if (positionals.empty()) {
        print_usage();
        return 2;
    }
    const fs::path archive = positionals[0];
    const fs::path dest = positionals.size() > 1 ? fs::path(positionals[1]) : fs::path(".");
    extract.thread_count = decompression.thread_count;
    const auto* provider = axiom::archive_provider_for_path(archive);
    if (provider == nullptr) {
        throw std::runtime_error("unsupported archive format: " +
                                 axiom::core::path_to_utf8(archive));
    }
    ScopedInteractiveProgress progress("extracting archive");
    extract.operation = progress.operation();
    provider->extract_all(archive, dest, extract);
    progress.complete();
    return 0;
}

int run_list(const std::vector<std::string>& args) {
    std::string password;
    std::vector<std::string> positionals;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-p" || args[i] == "--password") {
            if (i + 1 >= args.size()) {
                print_usage();
                return 2;
            }
            password = args[++i];
        } else if (args[i].rfind("--", 0) == 0) {
            std::cerr << "axiomc: unknown option " << args[i] << '\n';
            return 2;
        } else {
            positionals.push_back(args[i]);
        }
    }
    if (positionals.size() != 1) {
        print_usage();
        return 2;
    }
    const fs::path archive = positionals[0];
    const auto* provider = axiom::archive_provider_for_path(archive);
    if (provider == nullptr) {
        throw std::runtime_error("unsupported archive format: " +
                                 axiom::core::path_to_utf8(archive));
    }

    const axiom::ArchiveCapabilities capabilities = provider->capabilities(archive, password);
    if (provider->info().native) {
        const std::string comment = axiom::archive_comment(archive, password);
        if (!comment.empty()) {
            std::cout << "Comment: " << comment << '\n';
        }
    }
    if (capabilities.locked) std::cout << "[locked: read-only]\n";
    if (capabilities.encrypted) {
        std::cout << "[encrypted: password required to extract]\n";
    }
    const auto entries = provider->list(archive, password);

    std::uint64_t total_bytes = 0;
    for (const auto& entry : entries) {
        if (entry.is_symlink) {
            std::cout << "       <LINK>  " << format_time(entry.mtime) << "  " << entry.path
                      << " -> " << entry.link_target << '\n';
        } else if (entry.is_hardlink) {
            std::cout << "       <HLNK>  " << format_time(entry.mtime) << "  " << entry.path
                      << " => " << entry.link_target << '\n';
        } else if (entry.is_directory) {
            std::cout << "        <DIR>  " << format_time(entry.mtime) << "  " << entry.path << '\n';
        } else {
            std::cout.width(13);
            std::cout << entry.size << "  " << format_time(entry.mtime) << "  " << entry.path << '\n';
            total_bytes += entry.size;
        }
    }
    std::cout << entries.size() << " entries, " << total_bytes << " bytes uncompressed\n";
    return 0;
}

int run_test(std::vector<std::string> args) {
    axiom::DecompressionOptions options;
    if (!take_decompression_flags(args, options)) {
        return 2;
    }
    if (args.size() != 1) {
        print_usage();
        return 2;
    }
    const fs::path archive = args[0];
    const auto* provider = axiom::archive_provider_for_path(archive);
    if (provider == nullptr) {
        throw std::runtime_error("unsupported archive format: " +
                                 axiom::core::path_to_utf8(archive));
    }
    ScopedInteractiveProgress progress("testing archive");
    options.operation = progress.operation();
    provider->test(archive, options);
    progress.complete();
    std::cout << "archive is intact\n";
    return 0;
}

int run_recovery(const std::vector<std::string>& args) {
    if (args.empty() || args.size() > 2) {
        print_usage();
        return 2;
    }
    if (args.size() == 2) {
        const auto percent = static_cast<unsigned>(parse_size(args[1]));
        ScopedInteractiveProgress progress("updating recovery record");
        axiom::set_archive_recovery(args[0], percent, progress.operation());
        progress.complete();
    }
    const auto info = axiom::archive_recovery_info(args[0]);
    if (!info.present) {
        std::cout << "archive has no recovery record\n";
    } else {
        std::cout << "recovery record: " << info.percent << "% ("
                  << info.data_shards << " data + " << info.parity_shards
                  << " parity shards)\n";
    }
    return 0;
}

int run_repair(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        print_usage();
        return 2;
    }
    ScopedInteractiveProgress progress("repairing archive");
    if (!axiom::repair_archive(args[0], progress.operation())) {
        progress.complete();
        std::cout << "archive has no recovery record\n";
        return 3;
    }
    progress.complete();
    std::cout << "archive repaired and verified recovery data was rebuilt\n";
    return 0;
}

int run_split(const std::vector<std::string>& args) {
    if (args.size() < 2 || args.size() > 3) {
        print_usage();
        return 2;
    }
    const auto recovery_count = args.size() == 3
        ? static_cast<unsigned>(parse_size(args[2])) : 0;
    ScopedInteractiveProgress progress("creating archive volumes");
    const auto info = axiom::create_archive_volumes(args[0], parse_size(args[1]),
                                                     recovery_count,
                                                     progress.operation());
    progress.complete();
    std::cout << info.data_volumes << " data volume(s), " << info.recovery_volumes
              << " recovery volume(s) created\n";
    return 0;
}

int run_join(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        print_usage();
        return 2;
    }
    ScopedInteractiveProgress progress("joining archive volumes");
    axiom::join_archive_volumes(args[0], args[1], progress.operation());
    progress.complete();
    std::cout << "archive volumes joined\n";
    return 0;
}

int run_keygen(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        print_usage();
        return 2;
    }
    auto key = axiom::generate_archive_signing_key();
    write_key(args[0], key.secret_key);
    write_key(args[1], key.public_key);
    std::fill(key.secret_key.begin(), key.secret_key.end(), std::uint8_t{0});
    std::cout << "signing key generated\n";
    return 0;
}

int run_sign(std::vector<std::string> args) {
    axiom::CompressionOptions options;
    if (!take_compression_flags(args, options) || args.size() != 2) {
        print_usage();
        return 2;
    }
    axiom::ArchiveSigningKey key;
    key.secret_key = read_key<64>(args[1]);
    std::copy_n(key.secret_key.begin() + 32, key.public_key.size(), key.public_key.begin());
    ScopedInteractiveProgress progress("signing archive");
    options.operation = progress.operation();
    axiom::sign_archive(args[0], key, options);
    progress.complete();
    std::fill(key.secret_key.begin(), key.secret_key.end(), std::uint8_t{0});
    std::cout << "archive signed\n";
    return 0;
}

int run_verify(std::vector<std::string> args) {
    axiom::DecompressionOptions options;
    if (!take_decompression_flags(args, options) || args.empty() || args.size() > 2) {
        print_usage();
        return 2;
    }
    std::optional<std::array<std::uint8_t, 32>> trusted;
    if (args.size() == 2) trusted = read_key<32>(args[1]);
    const auto info = axiom::verify_archive_signature(args[0], options.password, trusted);
    if (!info.present) {
        std::cout << "archive is not signed\n";
        return 3;
    }
    if (!info.valid) {
        std::cout << "archive signature is invalid\n";
        return 1;
    }
    if (trusted && !info.trusted_key) {
        std::cout << "archive signature is valid but uses a different key\n";
        return 4;
    }
    std::cout << (trusted ? "archive signature is valid and trusted\n"
                          : "archive signature is valid\n");
    return 0;
}

int run_sfx(const std::vector<std::string>& args) {
    if (args.size() < 2 || args.size() > 3) {
        print_usage();
        return 2;
    }
    const fs::path stub = args.size() == 3
        ? fs::path(args[2])
        : executable_path.parent_path() / "Axiom.exe";
    ScopedInteractiveProgress progress("creating self-extractor");
    axiom::create_sfx_archive(args[0], stub, args[1], progress.operation());
    progress.complete();
    std::cout << "self-extracting archive created\n";
    return 0;
}

int run_compress(std::vector<std::string> args) {
    axiom::CompressionOptions options;
    if (!take_compression_flags(args, options)) {
        return 2;
    }
    if (args.size() != 2) {
        print_usage();
        return 2;
    }
    ScopedInteractiveProgress progress("compressing stream");
    if (auto operation = progress.operation()) {
        options.operation = operation;
        std::error_code size_error;
        const std::uint64_t total = fs::file_size(args[0], size_error);
        const std::string path = args[0];
        if (!size_error) {
            operation->report({axiom::OperationStage::reading, 0, total, 0, 1, path,
                               0, total});
            options.encoded_bytes_progress = [operation, total, path](std::uint64_t done) {
                operation->report({axiom::OperationStage::compressing, done, total, 0, 1, path,
                                   done, total});
            };
        }
    }
    axiom::compress_file(args[0], args[1], options);
    progress.complete();
    return 0;
}

int run_decompress(std::vector<std::string> args) {
    axiom::DecompressionOptions options;
    if (!take_decompression_flags(args, options)) {
        return 2;
    }
    if (args.size() != 2) {
        print_usage();
        return 2;
    }
    ScopedInteractiveProgress progress("decompressing stream");
    if (auto operation = progress.operation()) {
        options.operation = operation;
        std::error_code size_error;
        const std::uint64_t total = fs::file_size(args[0], size_error);
        const std::string path = args[0];
        if (!size_error) {
            operation->report({axiom::OperationStage::reading, 0, total, 0, 1, path,
                               0, total});
        }
        options.decoded_bytes_progress = [operation, path](std::uint64_t done,
                                                            std::uint64_t total_output) {
            operation->report({axiom::OperationStage::extracting, done, total_output, 0, 1,
                               path, done, total_output});
        };
    }
    axiom::decompress_file(args[0], args[1], options);
    progress.complete();
    return 0;
}

int run_command(std::string_view command, std::vector<std::string> args) {
    if (command == "help" || command == "-h" || command == "--help" || command == "/?") {
        print_usage();
        return 0;
    }
    if (command == "a" || command == "add") {
        return run_add(std::move(args));
    }
    if (command == "u" || command == "update") {
        return run_update(std::move(args), /*fresh_only=*/false);
    }
    if (command == "f" || command == "fresh") {
        return run_update(std::move(args), /*fresh_only=*/true);
    }
    if (command == "s" || command == "sync") {
        return run_sync(std::move(args));
    }
    if (command == "delete" || command == "rm") {
        return run_delete(std::move(args));
    }
    if (command == "repack") {
        return run_repack(std::move(args));
    }
    if (command == "comment") {
        return run_comment(std::move(args));
    }
    if (command == "lock") {
        return run_lock(std::move(args));
    }
    if (command == "recovery" || command == "rr") {
        return run_recovery(args);
    }
    if (command == "repair") {
        return run_repair(args);
    }
    if (command == "split") {
        return run_split(args);
    }
    if (command == "join") {
        return run_join(args);
    }
    if (command == "x" || command == "extract") {
        return run_extract(std::move(args));
    }
    if (command == "l" || command == "list") {
        return run_list(args);
    }
    if (command == "t" || command == "test") {
        return run_test(std::move(args));
    }
    if (command == "keygen") {
        return run_keygen(args);
    }
    if (command == "sign") {
        return run_sign(std::move(args));
    }
    if (command == "verify") {
        return run_verify(std::move(args));
    }
    if (command == "sfx") {
        return run_sfx(args);
    }
    if (command == "c" || command == "compress") {
        return run_compress(std::move(args));
    }
    if (command == "d" || command == "decompress") {
        return run_decompress(std::move(args));
    }

    std::cerr << "axiomc: unknown command " << command << '\n';
    print_interactive_help();
    return 2;
}

int run_interactive_shell() {
    if (stream_is_terminal(stdout)) {
        print_splash();
    } else {
        print_interactive_help();
    }

    std::string line;
    for (;;) {
        if (stream_is_terminal(stdout)) {
            std::cout << color(kColorAmber) << "axiom" << color(kColorReset) << "> "
                      << std::flush;
        }
        if (!std::getline(std::cin, line)) {
            std::cout << '\n';
            return 0;
        }
        line = trim_ascii(std::move(line));
        if (line.empty()) continue;

        try {
            std::vector<std::string> tokens = split_interactive_command(line);
            if (tokens.empty()) continue;
            const std::string command = tokens.front();
            tokens.erase(tokens.begin());

            if (command == "exit" || command == "quit") {
                return 0;
            }
            if (command == "clear" || command == "cls") {
                std::cout << "\x1b[2J\x1b[H";
                continue;
            }
            if (command == "pwd") {
                std::cout << axiom::core::path_to_utf8(fs::current_path()) << '\n';
                continue;
            }
            if (command == "cd") {
                if (tokens.size() != 1) {
                    std::cerr << "usage: cd <dir>\n";
                    continue;
                }
                fs::current_path(tokens.front());
                continue;
            }

            InteractiveCommandScope progress_scope;
            const int exit_code = run_command(command, std::move(tokens));
            if (exit_code != 0) {
                std::cout << "command failed with exit code " << exit_code << '\n';
            }
        } catch (const std::exception& ex) {
            std::cerr << "axiomc: " << ex.what() << '\n';
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    executable_path = fs::absolute(argv[0]);
    if (argc < 2) {
        return run_interactive_shell();
    }

    const std::string_view command = argv[1];
    if (command == "shell" || command == "--interactive") {
        return run_interactive_shell();
    }
    std::vector<std::string> args(argv + 2, argv + argc);

    try {
        return run_command(command, std::move(args));
    } catch (const std::exception& ex) {
        std::cerr << "axiomc: " << ex.what() << '\n';
        return 1;
    }
}
