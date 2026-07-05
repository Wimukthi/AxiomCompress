#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace axiom {

using ByteVector = std::vector<std::uint8_t>;

class OperationCancelled final : public std::runtime_error {
public:
    OperationCancelled() : std::runtime_error("operation cancelled") {}
};

enum class OperationStage {
    scanning,
    reading,
    compressing,
    writing,
    testing,
    extracting,
    finalizing,
};

struct OperationProgress {
    OperationStage stage = OperationStage::reading;
    std::uint64_t completed_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t completed_items = 0;
    std::uint64_t total_items = 0;
    std::string current_path;
};

class OperationControl final {
public:
    using ProgressCallback = std::function<void(const OperationProgress&)>;

    void set_progress_callback(ProgressCallback callback) {
        std::lock_guard lock(callback_mutex_);
        progress_callback_ = std::move(callback);
    }

    void report(const OperationProgress& progress) const {
        checkpoint();

        ProgressCallback callback;
        {
            std::lock_guard lock(callback_mutex_);
            callback = progress_callback_;
        }
        if (callback) {
            callback(progress);
        }
    }

    void checkpoint() const {
        std::unique_lock lock(pause_mutex_);
        pause_cv_.wait(lock, [&] {
            return !paused_.load(std::memory_order_acquire) ||
                   cancelled_.load(std::memory_order_acquire);
        });
        if (cancelled_.load(std::memory_order_acquire)) {
            throw OperationCancelled();
        }
    }

    void request_cancel() {
        cancelled_.store(true, std::memory_order_release);
        set_paused(false);
    }

    void set_paused(bool paused) {
        paused_.store(paused, std::memory_order_release);
        if (!paused) {
            pause_cv_.notify_all();
        }
    }

    bool cancel_requested() const {
        return cancelled_.load(std::memory_order_acquire);
    }

    bool paused() const {
        return paused_.load(std::memory_order_acquire);
    }

private:
    std::atomic_bool cancelled_ = false;
    std::atomic_bool paused_ = false;
    mutable std::mutex pause_mutex_;
    mutable std::condition_variable pause_cv_;
    mutable std::mutex callback_mutex_;
    ProgressCallback progress_callback_;
};

struct CompressionOptions {
    std::size_t window_size = 1u << 20;
    std::size_t max_match = 273;
    // Balanced default (CLI level 5): a moderate chain plus lazy matching reaches
    // close to a deep chain's ratio at a fraction of the time. Raise it (CLI:
    // --chain-depth, or a higher --level) for more ratio; lower it for speed.
    std::size_t max_chain_depth = 128;
    // Stop the greedy chain walk once a match this long is found: searching
    // further rarely improves the result but costs many memory-latency-bound
    // chain hops. Free speedup on data with long matches; no effect otherwise.
    std::size_t nice_length = 128;
    std::size_t optimal_chain_depth = 32;
    std::size_t max_parser_candidates = 8;
    std::size_t optimal_parse_limit = 64u << 20;
    std::size_t block_size = 4u << 20;
    std::size_t thread_count = 0;
    std::size_t io_buffer_size = 0;  // 0 = automatic.
    // When thread_count is zero, the CLI/library default is "use the machine".
    // This flag lets the parallel block codec reduce an oversized preset block
    // size so there is enough independent work to keep the selected workers busy.
    bool auto_block_size_for_threads = true;
    // Lazy matching: before committing a normal match at p, check p+1; if a strictly
    // longer match starts there, emit a literal and take the better match next. Lets
    // a shallow chain reach close to a deep chain's ratio. (Rep matches stay eager.)
    // On by default (part of the balanced level-5 profile); CLI --level controls it.
    bool lazy_matching = true;
    // Fast entropy selection: pick each split substream's coder from a cheap order-0
    // (and sampled order-1) entropy estimate instead of trial-encoding every coder,
    // and skip the whole-stream Huffman candidate. Trades a little ratio for speed.
    bool fast_entropy = false;
    // Use the binary-tree (suffix-BST) match finder instead of hash chains: a
    // large effective window and better matches, often faster on data with many
    // same-prefix candidates. Intended for bounded blocks (memory ~8 bytes/byte).
    bool use_tree_matcher = false;
    // Fast profile: fixed-probe row hashing plus byte-aligned LZ tokens. This is
    // deliberately separate from the LZMA-like ratio path: no tree, no optimal
    // parser, and no probability/range model in the hot loop.
    bool use_fast_lz = false;
    bool force_store = false;
    bool force_parallel_blocks = false;
    bool enable_optimal_parser = false;
    std::shared_ptr<OperationControl> operation;
    // When non-empty, archive blocks are encrypted: a per-archive key is derived
    // from this password (Argon2id) and each solid block is sealed with
    // XChaCha20-Poly1305. Applies to the archive container, not single-stream
    // compress(). Empty = no encryption.
    std::string password;
    // With a password, also encrypt the central directory so file names, sizes, and
    // hashes are hidden (reading then requires the password even to list). Ignored
    // when password is empty.
    bool encrypt_header = false;
    // Optional archive recovery redundancy, as a percentage of protected archive
    // bytes. Zero disables the recovery service record; valid enabled values are
    // 1..100. Applies only to the multi-file archive container.
    unsigned recovery_percent = 0;
};

// Effort presets, fastest (1) to maximum ratio (9). Each level picks a coherent
// set of match-finder and entropy knobs; individual CLI/GUI flags may still
// override fields after applying the preset. Levels 1-6 use the hash-chain
// matcher; levels 7-9 switch to the binary-tree matcher with larger windows.
inline void apply_compression_level(CompressionOptions& options, int level) {
    if (level < 1) {
        level = 1;
    } else if (level > 9) {
        level = 9;
    }

    options.use_tree_matcher = false;
    options.use_fast_lz = false;
    options.enable_optimal_parser = false;
    options.auto_block_size_for_threads = true;

    switch (level) {
        case 1:
            options.max_chain_depth = 8;
            options.nice_length = 64;
            options.lazy_matching = false;
            options.fast_entropy = true;
            options.use_fast_lz = true;
            break;
        case 2:
            options.max_chain_depth = 16;
            options.nice_length = 64;
            options.lazy_matching = true;
            options.fast_entropy = true;
            break;
        case 3:
            options.max_chain_depth = 32;
            options.nice_length = 128;
            options.lazy_matching = true;
            options.fast_entropy = true;
            break;
        case 4:
            options.max_chain_depth = 64;
            options.nice_length = 128;
            options.lazy_matching = true;
            options.fast_entropy = false;
            break;
        case 5:
            options.max_chain_depth = 128;
            options.nice_length = 128;
            options.lazy_matching = true;
            options.fast_entropy = false;
            break;
        case 6:
            options.max_chain_depth = 256;
            options.nice_length = 192;
            options.lazy_matching = true;
            options.fast_entropy = false;
            break;
        case 7:
            options.use_tree_matcher = true;
            options.max_chain_depth = 128;
            options.block_size = 8u << 20;
            options.window_size = 8u << 20;
            options.fast_entropy = false;
            break;
        case 8:
            options.use_tree_matcher = true;
            options.max_chain_depth = 128;
            options.block_size = 32u << 20;
            options.window_size = 32u << 20;
            options.fast_entropy = false;
            break;
        case 9:
            // Maximum preset keeps the deepest tree search and uses larger
            // blocks than level 8 so cross-block repetition can improve ratio.
            // It is also the only preset that turns on the optimal parser:
            // levels 7 and 8 land within a fraction of a percent of each other
            // on mixed corpora, so the DP parse is what makes 9 the genuine
            // max-ratio point (multi-block inputs run it per block on all
            // workers).
            options.use_tree_matcher = true;
            options.max_chain_depth = 512;
            options.block_size = 64u << 20;
            options.window_size = 64u << 20;
            options.fast_entropy = false;
            options.enable_optimal_parser = true;
            break;
    }
}

class FormatError final : public std::runtime_error {
public:
    explicit FormatError(const char* message);
    explicit FormatError(const std::string& message);
};

// Upper bound on the uncompressed size decompress() will produce. The archive
// header's declared original size is untrusted, and a malformed stream can ask
// the decoder to expand a tiny payload into an enormous output (a decompression
// bomb). decompress() rejects any archive whose declared size exceeds this limit
// before allocating or decoding, bounding peak memory. Callers handling trusted
// large inputs may raise it.
inline constexpr std::size_t kDefaultMaxDecompressedSize = std::size_t{4} << 30;  // 4 GiB

struct DecompressionOptions {
    std::size_t max_output_size = kDefaultMaxDecompressedSize;
    std::size_t thread_count = 0;
    std::size_t io_buffer_size = 0;  // 0 = automatic.
    std::shared_ptr<OperationControl> operation;
    // Password for an encrypted archive (container blocks). Empty for plaintext
    // archives; required to read an encrypted one.
    std::string password{};
};

ByteVector compress(std::span<const std::uint8_t> input,
                    const CompressionOptions& options = {});

ByteVector decompress(std::span<const std::uint8_t> archive,
                      const DecompressionOptions& options = {});

void compress_file(const std::filesystem::path& input_path,
                   const std::filesystem::path& output_path,
                   const CompressionOptions& options = {});

void decompress_file(const std::filesystem::path& input_path,
                     const std::filesystem::path& output_path,
                     const DecompressionOptions& options = {});

}  // namespace axiom
