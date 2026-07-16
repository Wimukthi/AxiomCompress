#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace axiom {

using ByteVector = std::vector<std::uint8_t>;

namespace core {
class TaskExecutor;
}

class OperationCancelled final : public std::runtime_error {
public:
    OperationCancelled() : std::runtime_error("operation cancelled") {}
};

enum class OperationStage {
    scanning,
    estimating,
    reading,
    compressing,
    writing,
    testing,
    extracting,
    transferring,
    finalizing,
};

struct OperationProgress {
    OperationStage stage = OperationStage::reading;
    std::uint64_t completed_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t completed_items = 0;
    std::uint64_t total_items = 0;
    std::string current_path;
    // The current item is reported independently from the operation total so
    // frontends can present an overall bar and an exact per-file bar together.
    // A zero total means the current stage has no file-sized unit (for example,
    // an archive solid block that contains several files).
    std::uint64_t current_file_completed_bytes = 0;
    std::uint64_t current_file_total_bytes = 0;
    // Source-byte counter used only for throughput sampling. Archive creation
    // can advance this while assembling its first solid block even though no
    // bytes are complete yet, avoiding a false 0 B/s during small-file reads.
    // A zero value asks frontends to fall back to completed_bytes.
    std::uint64_t throughput_bytes = 0;
    // Monotonically increasing telemetry sequence assigned by OperationControl.
    // Producers leave this at zero; frontends use it to distinguish a fresh
    // atomic snapshot from a heartbeat repaint.
    std::uint64_t sequence = 0;
};

struct OperationWarning {
    std::string path;
    std::string message;
};

class OperationControl final {
public:
    using ProgressCallback = std::function<void(const OperationProgress&)>;

    void set_progress_callback(ProgressCallback callback) {
        const bool present = static_cast<bool>(callback);
        auto value = callback
            ? std::make_shared<ProgressCallback>(std::move(callback))
            : std::shared_ptr<ProgressCallback>{};
        progress_callback_.store(std::move(value), std::memory_order_release);
        progress_callback_present_.store(present, std::memory_order_release);
    }

    void report(const OperationProgress& progress) const {
        checkpoint();
        constexpr std::uint64_t kPublishQuantum = 1u << 20;
        const auto previous_stage = static_cast<OperationStage>(
            progress_stage_.load(std::memory_order_relaxed));
        const auto previous_bytes =
            progress_completed_bytes_.load(std::memory_order_relaxed);
        const auto previous_total = progress_total_bytes_.load(std::memory_order_relaxed);
        const auto previous_items =
            progress_completed_items_.load(std::memory_order_relaxed);
        const auto previous_item_total = progress_total_items_.load(std::memory_order_relaxed);
        const auto previous_file_bytes =
            progress_file_completed_bytes_.load(std::memory_order_relaxed);
        const auto previous_file_total =
            progress_file_total_bytes_.load(std::memory_order_relaxed);
        const auto previous_throughput_bytes =
            progress_throughput_bytes_.load(std::memory_order_relaxed);
        const bool first = progress_version_.load(std::memory_order_relaxed) == 0;
        const bool boundary = first || progress.stage != previous_stage ||
            progress.total_bytes != previous_total ||
            progress.total_items != previous_item_total ||
            progress.completed_items != previous_items ||
            progress.current_file_total_bytes != previous_file_total ||
            progress.completed_bytes < previous_bytes ||
            progress.current_file_completed_bytes < previous_file_bytes ||
            progress.throughput_bytes < previous_throughput_bytes ||
            (progress.total_bytes > 0 &&
             progress.completed_bytes >= progress.total_bytes) ||
            (progress.current_file_total_bytes > 0 &&
             progress.current_file_completed_bytes >= progress.current_file_total_bytes);
        const bool byte_quantum = progress.completed_bytes >= previous_bytes + kPublishQuantum ||
            progress.current_file_completed_bytes >= previous_file_bytes + kPublishQuantum ||
            progress.throughput_bytes >= previous_throughput_bytes + kPublishQuantum;
        if (!boundary && !byte_quantum) return;
        while (progress_writer_.test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        progress_version_.fetch_add(1, std::memory_order_acq_rel); // writer active
        progress_stage_.store(static_cast<unsigned>(progress.stage),
                              std::memory_order_relaxed);
        progress_completed_bytes_.store(progress.completed_bytes,
                                        std::memory_order_relaxed);
        progress_total_bytes_.store(progress.total_bytes, std::memory_order_relaxed);
        progress_completed_items_.store(progress.completed_items,
                                        std::memory_order_relaxed);
        progress_total_items_.store(progress.total_items, std::memory_order_relaxed);
        progress_file_completed_bytes_.store(progress.current_file_completed_bytes,
                                             std::memory_order_relaxed);
        progress_file_total_bytes_.store(progress.current_file_total_bytes,
                                         std::memory_order_relaxed);
        progress_throughput_bytes_.store(progress.throughput_bytes,
                                         std::memory_order_relaxed);
        if (progress_writer_path_ != progress.current_path) {
            progress_writer_path_ = progress.current_path;
            progress_path_.store(
                std::make_shared<const std::string>(progress.current_path),
                std::memory_order_relaxed);
        }
        const std::uint64_t version =
            progress_version_.fetch_add(1, std::memory_order_release) + 1;
        progress_writer_.clear(std::memory_order_release);

        if (progress_callback_present_.load(std::memory_order_acquire)) {
            auto callback = progress_callback_.load(std::memory_order_acquire);
            if (!callback) return;
            OperationProgress published = progress;
            published.sequence = version / 2;
            (*callback)(published);
        }
    }

    // A coherent, non-blocking telemetry snapshot. The operation writer never
    // waits for the UI; readers retry only if they race the few atomic stores
    // that publish a new sample.
    std::optional<OperationProgress> latest_progress() const {
        for (;;) {
            const std::uint64_t before = progress_version_.load(std::memory_order_acquire);
            if (before == 0) return std::nullopt;
            if ((before & 1u) != 0) continue;
            OperationProgress result;
            result.stage = static_cast<OperationStage>(
                progress_stage_.load(std::memory_order_relaxed));
            result.completed_bytes =
                progress_completed_bytes_.load(std::memory_order_relaxed);
            result.total_bytes = progress_total_bytes_.load(std::memory_order_relaxed);
            result.completed_items =
                progress_completed_items_.load(std::memory_order_relaxed);
            result.total_items = progress_total_items_.load(std::memory_order_relaxed);
            result.current_file_completed_bytes =
                progress_file_completed_bytes_.load(std::memory_order_relaxed);
            result.current_file_total_bytes =
                progress_file_total_bytes_.load(std::memory_order_relaxed);
            result.throughput_bytes =
                progress_throughput_bytes_.load(std::memory_order_relaxed);
            const auto path = progress_path_.load(std::memory_order_relaxed);
            if (path) result.current_path = *path;
            const std::uint64_t after = progress_version_.load(std::memory_order_acquire);
            if (before == after && (after & 1u) == 0) {
                result.sequence = after / 2;
                return result;
            }
        }
    }

    void checkpoint() const {
        if (!paused_.load(std::memory_order_acquire)) {
            if (cancelled_.load(std::memory_order_acquire)) throw OperationCancelled();
            return;
        }
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

    void add_warning(OperationWarning warning) const {
        std::lock_guard lock(warning_mutex_);
        warnings_.push_back(std::move(warning));
    }

    std::vector<OperationWarning> warnings() const {
        std::lock_guard lock(warning_mutex_);
        return warnings_;
    }

private:
    std::atomic_bool cancelled_ = false;
    std::atomic_bool paused_ = false;
    mutable std::mutex pause_mutex_;
    mutable std::condition_variable pause_cv_;
    mutable std::mutex warning_mutex_;
    mutable std::vector<OperationWarning> warnings_;
    mutable std::atomic<std::shared_ptr<ProgressCallback>> progress_callback_;
    mutable std::atomic_bool progress_callback_present_{false};
    mutable std::atomic_flag progress_writer_ = ATOMIC_FLAG_INIT;
    mutable std::atomic_uint64_t progress_version_{0};
    mutable std::atomic_uint progress_stage_{
        static_cast<unsigned>(OperationStage::reading)};
    mutable std::atomic_uint64_t progress_completed_bytes_{0};
    mutable std::atomic_uint64_t progress_total_bytes_{0};
    mutable std::atomic_uint64_t progress_completed_items_{0};
    mutable std::atomic_uint64_t progress_total_items_{0};
    mutable std::atomic_uint64_t progress_file_completed_bytes_{0};
    mutable std::atomic_uint64_t progress_file_total_bytes_{0};
    mutable std::atomic_uint64_t progress_throughput_bytes_{0};
    mutable std::atomic<std::shared_ptr<const std::string>> progress_path_{
        std::make_shared<const std::string>()};
    mutable std::string progress_writer_path_;
};

enum class CompressionTransform : std::uint8_t {
    none = 0,
    x86_branch = 1,
    delta = 2,
    word16_predict = 3,
};

// A reversible pre-compression transform applied to one byte range. Archive
// solid blocks can contain several files, so ranges carry their own logical
// source position and reset independently at file and block boundaries.
struct CompressionTransformRange {
    CompressionTransform transform = CompressionTransform::none;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint64_t source_offset = 0;
    std::uint8_t parameter = 0;  // Delta byte stride; zero for x86 branch filtering.
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
    // Maximum-ratio auto geometry may align variable codec blocks with validated
    // container/member boundaries. Explicit block-size callers keep uniform
    // blocks, and the decoder already accepts arbitrary per-block lengths.
    bool content_adaptive_blocks = false;
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
    // Two-pass optimal parsing re-parses with costs measured from the first DP
    // pass and keeps the smaller coded result; single-pass (false) measures
    // costs from the cheap greedy parse and runs the DP once — roughly half
    // the time for most of the ratio. Levels 8-9 use the measured-cost single
    // pass; callers enabling --optimal on another preset retain two passes.
    bool optimal_two_pass = true;
    // New-archive frontends may continue past inputs that disappear or remain
    // unreadable after retries. Each omission is recorded on OperationControl;
    // update paths deliberately ignore this option to preserve old entries.
    bool skip_unreadable_files = false;
    unsigned input_open_retries = 4;
    // File-aware reversible filters are selected only when a fast trial predicts
    // a net win. Archive writers populate ranges from individual file content;
    // direct compress() calls auto-detect a supported whole-input transform.
    bool enable_file_filters = true;
    std::vector<CompressionTransformRange> transform_ranges;
    std::shared_ptr<OperationControl> operation;
    // Internal operation-scoped executor. Archive writers populate this so
    // every solid block and nested codec task shares one global CPU budget;
    // ordinary callers leave it empty and compress() creates a local executor.
    std::shared_ptr<core::TaskExecutor> task_executor;
    // Within-compress progress: when set, the parallel block codec calls this
    // with the cumulative number of this compress() call's input bytes whose
    // blocks have finished encoding. Invoked from worker threads, possibly
    // concurrently and out of order — the callback must be thread-safe and
    // cheap, and should treat the value as a monotonic high-water mark. The
    // archive writer uses it to report progress inside large solid blocks;
    // serial (single-worker) encodes receive the start and completion updates.
    std::function<void(std::uint64_t)> encoded_bytes_progress;
    // Internal fine-grained progress: the parse loops call this with the
    // fraction (0..1) of the *current encode call's* work completed, every few
    // hundred KiB of scanned input. Wrappers compose it: compress_block maps
    // each phase (greedy parse, optimal passes, entropy) into its share of the
    // block, and the parallel block codec aggregates per-worker fractions into
    // encoded_bytes_progress. This is what keeps the progress bar moving inside
    // multi-second optimal parses instead of stalling until a block completes.
    // Cheap to leave unset; encoders check once per reporting quantum.
    std::function<void(double)> encode_progress;
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
    options.optimal_two_pass = true;
    options.optimal_chain_depth = 32;
    options.max_parser_candidates = 8;
    options.auto_block_size_for_threads = true;
    options.content_adaptive_blocks = false;

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
            options.lazy_matching = true;
            options.fast_entropy = false;
            break;
        case 8:
            // Mid-high ratio point: the tree matcher plus a single-pass
            // optimal parse (costs measured from the greedy parse) reaches
            // most of level 9's ratio at roughly half its time.
            options.use_tree_matcher = true;
            options.max_chain_depth = 128;
            options.block_size = 32u << 20;
            options.window_size = 32u << 20;
            options.lazy_matching = false;
            options.fast_entropy = false;
            options.enable_optimal_parser = true;
            options.optimal_two_pass = false;
            options.optimal_chain_depth = 16;
            options.max_parser_candidates = 6;
            break;
        case 9:
            // Maximum preset keeps the deepest tree search, extends matches to
            // 4 KiB, and uses larger blocks than level 8. A measured-cost single
            // DP pass proved both smaller and faster once v7 numeric transforms
            // exposed longer runs; multi-block inputs still parse in parallel.
            options.use_tree_matcher = true;
            options.max_chain_depth = 512;
            options.max_match = 4096;
            options.block_size = 64u << 20;
            options.window_size = 64u << 20;
            options.lazy_matching = false;
            options.fast_entropy = false;
            options.enable_optimal_parser = true;
            options.optimal_two_pass = false;
            options.content_adaptive_blocks = true;
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
    // Within-decompress progress. The callback receives completed and total
    // uncompressed bytes for this decompress() call. Parallel block decodes
    // invoke it from worker threads, so implementations must be thread-safe
    // and treat completed bytes as a monotonic high-water mark.
    std::function<void(std::uint64_t, std::uint64_t)> decoded_bytes_progress;
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
