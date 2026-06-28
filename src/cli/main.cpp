#include "axiom/archive.hpp"
#include "axiom/axiom.hpp"

#include <ctime>
#include <array>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
fs::path executable_path;

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
        "                      (XChaCha20-Poly1305 per block, Argon2id key derivation)\n"
        "  --encrypt-names     with -p, also encrypt the directory (hide names; 'l' needs -p)\n"
        "\n"
        "Compression options (a, c):\n"
        "  --level N           1=fastest .. 9=max ratio (default 5)\n"
        "  --fast (=1)         --max (=9)\n"
        "  --threads N        --block-size SIZE   --parallel\n"
        "  --chain-depth N    --nice N            (match-finder speed/ratio)\n"
        "  --lazy / --no-lazy  --fast-entropy     (override level knobs)\n"
        "  --fast-lz                              (byte-token fast profile)\n"
        "  --window SIZE                          (match window; bounds --bt memory)\n"
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

void print_splash() {
    std::cout <<
        "     ___        _                \n"
        "    / _ \\__  __(_) ___  _ __ ___ \n"
        "   / /_)/\\ \\/ /| |/ _ \\| '_ ` _ \\\n"
        "  / ___/  >  < | | (_) | | | | | |\n"
        "  \\/     /_/\\_\\|_|\\___/|_| |_| |_|\n"
        "\n"
        "        Axiom Archive Manager\n"
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
        throw std::runtime_error("invalid signing key file: " + path.string());
    }
    return key;
}

template <std::size_t Size>
void write_key(const fs::path& path, const std::array<std::uint8_t, Size>& key) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output || !output.write(reinterpret_cast<const char*>(key.data()),
                                 static_cast<std::streamsize>(key.size()))) {
        throw std::runtime_error("cannot write signing key file: " + path.string());
    }
}

// Effort presets, fastest (1) to maximum ratio (9). Each level picks a coherent
// set of match-finder and entropy knobs; individual --flags still override. Levels
// 1-6 drive the hash-chain matcher (depth + lazy + entropy effort); 7-9 switch to
// the binary-tree matcher with growing windows. The optimal parser remains an
// explicit --optimal override because its per-byte DP cost is significant.
void apply_level(axiom::CompressionOptions& o, int level) {
    if (level < 1) level = 1;
    if (level > 9) level = 9;

    o.use_tree_matcher = false;
    o.use_fast_lz = false;
    o.enable_optimal_parser = false;
    o.auto_block_size_for_threads = true;

    switch (level) {
        case 1:
            o.max_chain_depth = 8; o.nice_length = 64; o.lazy_matching = false;
            o.fast_entropy = true; o.use_fast_lz = true;
            break;
        case 2: o.max_chain_depth = 16;  o.nice_length = 64;  o.lazy_matching = true;  o.fast_entropy = true;  break;
        case 3: o.max_chain_depth = 32;  o.nice_length = 128; o.lazy_matching = true;  o.fast_entropy = true;  break;
        case 4: o.max_chain_depth = 64;  o.nice_length = 128; o.lazy_matching = true;  o.fast_entropy = false; break;
        case 5: o.max_chain_depth = 128; o.nice_length = 128; o.lazy_matching = true;  o.fast_entropy = false; break;
        case 6: o.max_chain_depth = 256; o.nice_length = 192; o.lazy_matching = true;  o.fast_entropy = false; break;
        case 7:
            o.use_tree_matcher = true; o.max_chain_depth = 128;
            o.block_size = 8u << 20;  o.window_size = 8u << 20;  o.fast_entropy = false;
            o.auto_block_size_for_threads = false;
            break;
        case 8:
            o.use_tree_matcher = true; o.max_chain_depth = 256;
            o.block_size = 32u << 20; o.window_size = 32u << 20; o.fast_entropy = false;
            o.auto_block_size_for_threads = false;
            break;
        case 9:
            // Maximum preset keeps the deepest tree search, but avoids a single
            // huge mixed-content block where random regions dominate runtime and
            // still end up stored.
            o.use_tree_matcher = true; o.max_chain_depth = 512;
            o.block_size = 16u << 20; o.window_size = 64u << 20; o.fast_entropy = false;
            o.auto_block_size_for_threads = false;
            break;
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
    apply_level(options, level);

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
    const auto time = static_cast<std::time_t>(seconds);
    std::tm parts{};
#ifdef _WIN32
    localtime_s(&parts, &time);
#else
    localtime_r(&time, &parts);
#endif
    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts) == 0) {
        return "-";
    }
    return buffer;
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
    // `a` creates a new archive, or adds to (and updates entries in) an existing one.
    if (fs::exists(archive)) {
        axiom::add_to_archive(inputs, archive, options);
    } else {
        axiom::create_archive(inputs, archive, options);
    }
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
    axiom::update_archive(inputs, archive, options, fresh_only);
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
    axiom::sync_archive(inputs, archive, options);
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
    axiom::set_archive_comment(archive, args[1]);
    return 0;
}

int run_lock(std::vector<std::string> args) {
    if (args.size() != 1) {
        print_usage();
        return 2;
    }
    axiom::lock_archive(args.front());
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
    axiom::delete_from_archive(archive, paths, options);
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
    axiom::repack_archive(args.front(), options);
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
    axiom::extract_archive(archive, dest, extract);
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

    const std::string comment = axiom::archive_comment(archive, password);
    if (!comment.empty()) {
        std::cout << "Comment: " << comment << '\n';
    }
    if (axiom::archive_is_locked(archive, password)) {
        std::cout << "[locked: read-only]\n";
    }
    if (axiom::archive_is_encrypted(archive)) {
        std::cout << "[encrypted: password required to extract]\n";
    }
    const auto entries = axiom::list_archive(archive, password);

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
    axiom::test_archive(args[0], options);
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
        axiom::set_archive_recovery(args[0], percent);
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
    if (!axiom::repair_archive(args[0])) {
        std::cout << "archive has no recovery record\n";
        return 3;
    }
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
    const auto info = axiom::create_archive_volumes(args[0], parse_size(args[1]),
                                                     recovery_count);
    std::cout << info.data_volumes << " data volume(s), " << info.recovery_volumes
              << " recovery volume(s) created\n";
    return 0;
}

int run_join(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        print_usage();
        return 2;
    }
    axiom::join_archive_volumes(args[0], args[1]);
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
    axiom::sign_archive(args[0], key, options);
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
    axiom::create_sfx_archive(args[0], stub, args[1]);
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
    axiom::compress_file(args[0], args[1], options);
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
    axiom::decompress_file(args[0], args[1], options);
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
            std::cout << "axiom> " << std::flush;
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
                std::cout << fs::current_path().string() << '\n';
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
