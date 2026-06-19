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
};

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
