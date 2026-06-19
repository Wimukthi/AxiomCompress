#include "axiom/archive.hpp"
#include "axiom/axiom.hpp"

#include <ctime>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

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
        "  axiomc x [options] <archive.axar> [dest-dir]   extract (default: current dir)\n"
        "  axiomc l <archive.axar>                         list contents\n"
        "  axiomc t [options] <archive.axar>               test integrity\n"
        "\n"
        "Single-stream commands:\n"
        "  axiomc c [options] <input> <output.axc>         compress one stream\n"
        "  axiomc d [options] <input.axc> <output>         decompress one stream\n"
        "\n"
        "Encryption:\n"
        "  -p, --password STR  encrypt blocks on 'a' (create); supply to read 'x'/'t'\n"
        "                      (XChaCha20-Poly1305 per block, Argon2id key derivation)\n"
        "\n"
        "Compression options (a, c):\n"
        "  --level N           1=fastest .. 9=max ratio (default 5)\n"
        "  --fast (=1)         --max (=9)\n"
        "  --threads N        --block-size SIZE   --parallel\n"
        "  --chain-depth N    --nice N            (match-finder speed/ratio)\n"
        "  --lazy / --no-lazy  --fast-entropy     (override level knobs)\n"
        "  --fast-lz                              (byte-token fast profile)\n"
        "  --window SIZE                          (match window; bounds --bt memory)\n"
        "  --bt                                   (binary-tree match finder)\n"
        "  --optimal          --optimal-depth N   --optimal-candidates N\n"
        "\n"
        "Decompression options (d, x, t):\n"
        "  --threads N        default 0 = all hardware threads\n"
        "  --overwrite MODE   extract only: fail (default), skip, all\n";
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

// Effort presets, fastest (1) to maximum ratio (9). Each level picks a coherent
// set of match-finder and entropy knobs; individual --flags still override. Levels
// 1-6 drive the hash-chain matcher (depth + lazy + entropy effort); 7-9 switch to
// the binary-tree matcher with growing windows, and 9 adds the optimal parser.
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
            // Maximum ratio that still finishes in bounded time/memory: one big
            // block so the binary tree sees a full window. The optimal parser is
            // left opt-in (`--optimal`, ideally with a smaller `--block-size`) — its
            // per-byte DP makes it impractical on a 100 MB single block.
            o.use_tree_matcher = true; o.max_chain_depth = 512;
            o.block_size = 512u << 20; o.window_size = 512u << 20; o.fast_entropy = false;
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
        } else if (arg == "--block-size") {
            options.block_size = parse_size(next("--block-size"));
            options.auto_block_size_for_threads = false;
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
    if (args.size() != 1) {
        print_usage();
        return 2;
    }
    const auto entries = axiom::list_archive(args[0]);

    const std::string comment = axiom::archive_comment(args[0]);
    if (!comment.empty()) {
        std::cout << "Comment: " << comment << '\n';
    }
    if (axiom::archive_is_locked(args[0])) {
        std::cout << "[locked: read-only]\n";
    }
    if (axiom::archive_is_encrypted(args[0])) {
        std::cout << "[encrypted: password required to extract]\n";
    }

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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 2;
    }

    const std::string_view command = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);

    try {
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
        if (command == "x" || command == "extract") {
            return run_extract(std::move(args));
        }
        if (command == "l" || command == "list") {
            return run_list(args);
        }
        if (command == "t" || command == "test") {
            return run_test(std::move(args));
        }
        if (command == "c" || command == "compress") {
            return run_compress(std::move(args));
        }
        if (command == "d" || command == "decompress") {
            return run_decompress(std::move(args));
        }

        print_usage();
        return 2;
    } catch (const std::exception& ex) {
        std::cerr << "axiomc: " << ex.what() << '\n';
        return 1;
    }
}
