#include "codec/block.hpp"

#include "codec/fast_lz.hpp"
#include "codec/incompressible.hpp"
#include "codec/lz77.hpp"
#include "codec/lz77_split.hpp"
#include "codec/varint.hpp"
#include "core/checksum.hpp"
#include "core/cpu.hpp"
#include "core/task_executor.hpp"
#include "entropy/huffman.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <thread>

namespace axiom::codec {
namespace {

enum class BlockCodec : std::uint8_t {
    store = 0,
    greedy_lz77 = 1,
    greedy_lz77_huffman = 2,
    greedy_lz77_split = 3,
    greedy_lz77_split_slots = 4,
    fast_lz = 5,
    lz77_sequences = 6,
    lz77_context_split = 7,
};

struct BlockResult {
    BlockCodec codec = BlockCodec::store;
    std::size_t original_size = 0;
    ByteVector payload;
};

struct EncodedBlock {
    BlockCodec codec = BlockCodec::store;
    std::size_t original_size = 0;
    std::size_t output_offset = 0;
    std::size_t payload_offset = 0;
    std::size_t payload_size = 0;
};

struct LegacyPayloads {
    ByteVector split;
    std::optional<ByteVector> slots;
    std::optional<ByteVector> context_split;
};

BlockCodec read_block_codec(std::uint8_t value) {
    if (value == static_cast<std::uint8_t>(BlockCodec::store)) {
        return BlockCodec::store;
    }
    if (value == static_cast<std::uint8_t>(BlockCodec::greedy_lz77)) {
        return BlockCodec::greedy_lz77;
    }
    if (value == static_cast<std::uint8_t>(BlockCodec::greedy_lz77_huffman)) {
        return BlockCodec::greedy_lz77_huffman;
    }
    if (value == static_cast<std::uint8_t>(BlockCodec::greedy_lz77_split)) {
        return BlockCodec::greedy_lz77_split;
    }
    if (value == static_cast<std::uint8_t>(BlockCodec::greedy_lz77_split_slots)) {
        return BlockCodec::greedy_lz77_split_slots;
    }
    if (value == static_cast<std::uint8_t>(BlockCodec::fast_lz)) {
        return BlockCodec::fast_lz;
    }
    if (value == static_cast<std::uint8_t>(BlockCodec::lz77_sequences)) {
        return BlockCodec::lz77_sequences;
    }
    if (value == static_cast<std::uint8_t>(BlockCodec::lz77_context_split)) {
        return BlockCodec::lz77_context_split;
    }

    throw FormatError("unsupported block codec");
}

std::size_t lz_payload_limit(std::size_t original_size) {
    const auto max = std::numeric_limits<std::size_t>::max();

    if (original_size > max - (original_size / 8) - 4096) {
        return max;
    }

    return original_size + (original_size / 8) + 4096;
}

BlockResult compress_block(std::span<const std::uint8_t> input,
                           const CompressionOptions& options,
                           core::TaskExecutor* executor) {
    BlockResult best;
    best.codec = BlockCodec::store;
    best.original_size = input.size();
    std::size_t best_size = input.size();

    auto finish_best = [&]() {
        if (best.codec == BlockCodec::store && best.payload.empty()) {
            best.payload.assign(input.begin(), input.end());
        }
        return std::move(best);
    };

    if (likely_incompressible(input)) {
        return finish_best();
    }

    if (options.use_fast_lz) {
        // The fast path's primary codec is LZ77 sequences entropy-coded with the
        // split-stream rANS backend. The parser fills split streams directly and
        // only estimates the raw LZ77 candidate size; raw bytes are materialized
        // only if that candidate can actually win.
        auto fast_payloads = encode_fast_lz77_split_payloads(input, options);
        auto [split_payload, slot_payload] =
            encode_lz77_split_payloads(fast_payloads.streams, /*fast=*/true, executor);
        if (split_payload.size() < best_size) {
            best_size = split_payload.size();
            best.codec = BlockCodec::greedy_lz77_split;
            best.payload = std::move(split_payload);
        }
        if (slot_payload && slot_payload->size() < best_size) {
            best_size = slot_payload->size();
            best.codec = BlockCodec::greedy_lz77_split_slots;
            best.payload = std::move(*slot_payload);
        }
        if (fast_payloads.lz77_size < input.size() &&
            fast_payloads.lz77_size <= best_size) {
            auto lz_payload = encode_fast_lz77(input, options);
            if (lz_payload.size() <= best_size) {
                best_size = lz_payload.size();
                best.codec = BlockCodec::greedy_lz77;
                best.payload = std::move(lz_payload);
            }
        }

        // The standalone nibble format only wins where the entropy stage cannot
        // help (high-entropy but LZ-repetitive data). On normal compressible
        // input the split path wins outright, so skip its second matcher pass
        // unless the block barely compressed (saved < 10%).
        if (best_size * 10 > input.size() * 9) {
            auto fast_payload = encode_fast_lz(input, options);
            if (fast_payload.size() < best_size) {
                best_size = fast_payload.size();
                best.codec = BlockCodec::fast_lz;
                best.payload = std::move(fast_payload);
            }
        }
        return finish_best();
    }

    auto consider_lz_payload = [&](ByteVector lz_payload, bool try_sequence) {
        // Record a raw winner without copying its full token buffer. Most real
        // blocks replace it with an entropy-coded candidate below; move it into
        // the result only if it is still the winner after that bake-off.
        bool raw_payload_pending = false;
        if (lz_payload.size() < best_size) {
            best_size = lz_payload.size();
            best.codec = BlockCodec::greedy_lz77;
            raw_payload_pending = true;
        }

        auto accept_encoded_payload = [&](BlockCodec codec, ByteVector payload) {
            if (payload.size() < best_size) {
                best_size = payload.size();
                best.codec = codec;
                best.payload = std::move(payload);
                raw_payload_pending = false;
            }
        };

        std::optional<ByteVector> entropy_payload;
        std::optional<ByteVector> split_payload;
        std::optional<ByteVector> slot_payload;
        std::optional<ByteVector> sequence_payload;
        std::optional<ByteVector> context_split_payload;
        // Fast-entropy presets already accept estimate-driven selection, so they
        // share analysis and prune a provably inferior legacy bake-off. Thorough
        // presets retain independent candidates to keep their exhaustive policy.
        if (try_sequence && options.fast_entropy) {
            auto candidates =
                encode_lz77_payload_candidates(input, lz_payload, options.fast_entropy,
                                               executor);
            split_payload = std::move(candidates.split);
            slot_payload = std::move(candidates.slots);
            sequence_payload = std::move(candidates.sequence);
        } else if (executor != nullptr && !options.fast_entropy &&
                   lz_payload.size() >= (std::size_t{256} << 10)) {
            // Huffman, legacy split layouts, and the v6 sequence layout only
            // read the token stream. Run those independent trials through the
            // encode-wide executor; deterministic selection still happens in
            // the original order below.
            auto huffman_task = executor->submit([&lz_payload] {
                return entropy::encode_huffman(lz_payload);
            });
            auto legacy_task = executor->submit([&input, &lz_payload, &options,
                                                  try_sequence, executor] {
                auto pair = encode_lz77_split_payloads(lz_payload, options.fast_entropy,
                                                       executor);
                LegacyPayloads result{std::move(pair.first), std::move(pair.second),
                                      std::nullopt};
                if (try_sequence && result.slots) {
                    result.context_split = encode_lz77_context_split_streams(
                        input, lz_payload, *result.slots, executor);
                }
                return result;
            });

            std::optional<std::future<std::optional<ByteVector>>> sequence_task;
            if (try_sequence) {
                const auto useful_size = std::min(best_size, lz_payload.size());
                sequence_task.emplace(executor->submit(
                    [&input, &lz_payload, &options, useful_size, executor] {
                        return encode_lz77_sequence_streams(
                            input, lz_payload, options.fast_entropy, useful_size, executor);
                    }));
            }

            std::exception_ptr task_error;
            std::optional<LegacyPayloads> legacy;
            auto collect = [&](auto& task, auto& destination) {
                try {
                    destination = executor->wait(task);
                } catch (...) {
                    if (!task_error) {
                        task_error = std::current_exception();
                    }
                }
            };
            // Drain every sibling before references captured by those tasks go
            // out of scope, even if allocation or a coder fails in one trial.
            collect(huffman_task, entropy_payload);
            collect(legacy_task, legacy);
            if (sequence_task) {
                collect(*sequence_task, sequence_payload);
            }
            if (task_error) {
                std::rethrow_exception(task_error);
            }
            if (legacy) {
                split_payload = std::move(legacy->split);
                slot_payload = std::move(legacy->slots);
                context_split_payload = std::move(legacy->context_split);
            }
        } else {
            // The whole-stream Huffman candidate almost never beats split
            // streams, but thorough presets keep the exhaustive comparison.
            if (!options.fast_entropy) {
                entropy_payload = entropy::encode_huffman(lz_payload);
            }
            auto legacy =
                encode_lz77_split_payloads(lz_payload, options.fast_entropy, executor);
            split_payload = std::move(legacy.first);
            slot_payload = std::move(legacy.second);
            if (try_sequence) {
                // Candidate policy must not depend on whether an executor is
                // present. The parallel path starts this trial before legacy
                // split sizes are known, so use the same stable upper bound in
                // serial mode; thread count then cannot change archive bytes.
                const auto useful_size = std::min(best_size, lz_payload.size());
                sequence_payload =
                    encode_lz77_sequence_streams(input, lz_payload, options.fast_entropy,
                                                 useful_size, executor);
                if (slot_payload) {
                    context_split_payload = encode_lz77_context_split_streams(
                        input, lz_payload, *slot_payload, executor);
                }
            }
        }
        if (entropy_payload && entropy_payload->size() < best_size) {
            accept_encoded_payload(BlockCodec::greedy_lz77_huffman,
                                   std::move(*entropy_payload));
        }
        if (split_payload && split_payload->size() < best_size) {
            accept_encoded_payload(BlockCodec::greedy_lz77_split,
                                   std::move(*split_payload));
        }
        if (slot_payload && slot_payload->size() < best_size) {
            accept_encoded_payload(BlockCodec::greedy_lz77_split_slots,
                                   std::move(*slot_payload));
        }
        if (sequence_payload && sequence_payload->size() < best_size) {
            accept_encoded_payload(BlockCodec::lz77_sequences,
                                   std::move(*sequence_payload));
        }
        if (context_split_payload && context_split_payload->size() < best_size) {
            accept_encoded_payload(BlockCodec::lz77_context_split,
                                   std::move(*context_split_payload));
        }
        if (raw_payload_pending) {
            best.payload = std::move(lz_payload);
        }
    };

    // Phase plan for fine-grained progress: place the greedy parse and the
    // optimal passes into rough shares of this block's wall time so the
    // fraction rises continuously across a multi-second encode. The trailing
    // ~5% covers the entropy bake-off; block completion reports the remainder.
    const bool run_optimal =
        options.enable_optimal_parser && input.size() <= options.optimal_parse_limit;
    const double greedy_share =
        !run_optimal ? 0.90 : options.optimal_two_pass ? 0.18 : 0.35;
    auto greedy = encode_lz77(input, scoped_progress_options(options, 0.0, greedy_share));
    const auto optimal_options = scoped_progress_options(options, greedy_share, 0.95);
    if (run_optimal && !options.optimal_two_pass) {
        // Single-pass optimal measures its cost model from the greedy tokens,
        // so they must outlive the parse; two-pass mode releases them first —
        // keeping the extra buffer live through the long DP measurably hurts
        // cache behaviour.
        auto optimal_tokens = encode_lz77_optimal(input, optimal_options, &greedy);
        consider_lz_payload(std::move(greedy), /*try_sequence=*/false);
        consider_lz_payload(std::move(optimal_tokens), /*try_sequence=*/true);
    } else {
        consider_lz_payload(std::move(greedy), /*try_sequence=*/!run_optimal);
        if (run_optimal) {
            consider_lz_payload(encode_lz77_optimal(input, optimal_options),
                                /*try_sequence=*/true);
        }
    }

    return finish_best();
}

void decompress_block_into(std::span<const std::uint8_t> payload,
                           BlockCodec codec,
                           std::span<std::uint8_t> output) {
    if (codec == BlockCodec::store) {
        if (payload.size() != output.size()) {
            throw FormatError("stored block size does not match block header");
        }

        std::copy(payload.begin(), payload.end(), output.begin());
        return;
    }

    if (codec == BlockCodec::greedy_lz77) {
        decode_lz77_into(payload, output);
        return;
    }

    if (codec == BlockCodec::greedy_lz77_huffman) {
        const auto lz_payload = entropy::decode_huffman(payload, lz_payload_limit(output.size()));
        decode_lz77_into(lz_payload, output);
        return;
    }

    if (codec == BlockCodec::greedy_lz77_split) {
        decode_lz77_split_streams_into(payload, output);
        return;
    }

    if (codec == BlockCodec::greedy_lz77_split_slots) {
        decode_lz77_split_streams_slots_into(payload, output);
        return;
    }

    if (codec == BlockCodec::fast_lz) {
        decode_fast_lz_into(payload, output);
        return;
    }

    if (codec == BlockCodec::lz77_sequences) {
        decode_lz77_sequence_streams_into(payload, output);
        return;
    }

    if (codec == BlockCodec::lz77_context_split) {
        decode_lz77_context_split_streams_into(payload, output);
        return;
    }

    throw FormatError("unsupported block codec");
}

void write_block(ByteVector& output, const BlockResult& block) {
    write_varuint(output, block.original_size);
    output.push_back(static_cast<std::uint8_t>(block.codec));
    write_varuint(output, block.payload.size());
    output.insert(output.end(), block.payload.begin(), block.payload.end());
}

std::vector<EncodedBlock> read_block_table(std::span<const std::uint8_t> encoded,
                                           std::size_t& cursor,
                                           std::size_t output_size) {
    const auto block_count = static_cast<std::size_t>(read_varuint(encoded, cursor));
    std::vector<EncodedBlock> blocks;
    // block_count is attacker-controlled; cap the pre-reserve. The loop below
    // validates each block against the payload, so a bogus count fails fast.
    constexpr std::size_t kMaxBlockReserve = std::size_t{1} << 20;
    blocks.reserve(block_count < kMaxBlockReserve ? block_count : kMaxBlockReserve);

    std::size_t total_output = 0;

    for (std::size_t i = 0; i < block_count; ++i) {
        const auto original_size = static_cast<std::size_t>(read_varuint(encoded, cursor));

        if (cursor >= encoded.size()) {
            throw FormatError("truncated block codec id");
        }
        const auto block_codec = read_block_codec(encoded[cursor++]);

        const auto payload_size = static_cast<std::size_t>(read_varuint(encoded, cursor));
        if (payload_size > encoded.size() - cursor) {
            throw FormatError("block payload exceeds archive payload");
        }
        if (original_size > output_size - total_output) {
            throw FormatError("block output exceeds declared archive size");
        }

        blocks.push_back(EncodedBlock{block_codec, original_size, total_output, cursor, payload_size});
        cursor += payload_size;
        total_output += original_size;
    }

    if (total_output != output_size) {
        throw FormatError("block outputs do not sum to declared archive size");
    }

    return blocks;
}

}  // namespace

std::size_t effective_thread_count(std::size_t requested_threads, std::size_t work_items) {
    if (work_items == 0) {
        return 1;
    }

    auto threads = requested_threads;
    if (threads == 0) {
        threads = std::thread::hardware_concurrency();
    }
    if (threads == 0) {
        threads = 1;
    }

    return std::min<std::size_t>(threads, work_items);
}

std::size_t effective_compression_thread_count(std::size_t requested_threads,
                                               std::size_t work_items) {
    return effective_thread_count(
        requested_threads == 0 ? core::physical_core_count() : requested_threads,
        work_items);
}

std::size_t effective_parallel_block_size(std::size_t input_size,
                                          const CompressionOptions& options) {
    auto block_size = std::max<std::size_t>(1, options.block_size);
    if (!options.auto_block_size_for_threads || input_size == 0) {
        return block_size;
    }

    auto target_threads = options.thread_count;
    if (target_threads == 0) {
        target_threads = core::physical_core_count();
    }
    if (target_threads <= 1) {
        return block_size;
    }

    // Keep enough independent blocks for the selected workers unless the input
    // is too small to amortize block framing. This makes the default CLI scale
    // with the detected machine without over-splitting and throwing away ratio.
    constexpr std::size_t kMinAutoBlockSize = std::size_t{1} << 20;
    const auto max_useful_blocks = std::max<std::size_t>(1, input_size / kMinAutoBlockSize);
    const auto target_blocks = std::min<std::size_t>(target_threads, max_useful_blocks);
    if (target_blocks <= 1) {
        return block_size;
    }

    const auto auto_size = std::max<std::size_t>(
        kMinAutoBlockSize, (input_size + target_blocks - 1) / target_blocks);
    return std::min(block_size, auto_size);
}

ByteVector encode_parallel_blocks(std::span<const std::uint8_t> input,
                                  const CompressionOptions& options,
                                  std::uint32_t* crc32) {
    if (options.operation) {
        options.operation->checkpoint();
    }
    const auto block_size = effective_parallel_block_size(input.size(), options);
    const auto block_count = (input.size() + block_size - 1) / block_size;

    std::vector<BlockResult> results(block_count);
    std::vector<std::uint32_t> block_crcs(crc32 != nullptr ? block_count : 0);
    std::atomic_size_t next_block = 0;
    std::atomic_uint64_t encoded_bytes = 0;
    // The total executor budget is independent of the number of logical
    // blocks. A single ratio-friendly block can still fan out entropy work to
    // every requested worker, while block workers remain capped by block count.
    constexpr std::size_t kMinimumNestedTaskInput = std::size_t{256} << 10;
    const auto executor_thread_count = block_count == 0 ||
                                               (block_count == 1 &&
                                                input.size() < kMinimumNestedTaskInput)
        ? std::size_t{1}
        : effective_compression_thread_count(
              options.thread_count, std::numeric_limits<std::size_t>::max());
    const auto worker_count =
        effective_thread_count(executor_thread_count, block_count);
    std::mutex exception_mutex;
    std::exception_ptr first_exception;
    // Once any worker fails, the whole encode fails; let the other workers
    // drain instead of finishing blocks whose results will be discarded.
    std::atomic_bool failed = false;

    // Fine-grained progress: each worker publishes an estimate of how far into
    // its current block the encoder has scanned (via the encode_progress phase
    // plan inside compress_block); the aggregate completed + in-flight sum is
    // reported through encoded_bytes_progress. Slow optimal-parse blocks then
    // advance the bar continuously instead of stalling until they complete.
    // Reports are throttled here so hot parse loops never fan out one call per
    // tick per worker; the consumer additionally treats values as a monotonic
    // high-water mark, which absorbs the benign completed/in-flight race.
    std::vector<std::atomic<std::uint64_t>> in_flight(
        options.encoded_bytes_progress ? worker_count : 0);
    std::atomic_uint64_t last_reported = 0;
    const auto report_aggregate = [&](bool force) {
        std::uint64_t total = encoded_bytes.load(std::memory_order_relaxed);
        for (const auto& slot : in_flight) {
            total += slot.load(std::memory_order_relaxed);
        }
        constexpr std::uint64_t kReportQuantum = 512u << 10;
        auto previous = last_reported.load(std::memory_order_relaxed);
        while (force || total >= previous + kReportQuantum) {
            if (last_reported.compare_exchange_weak(previous, total,
                                                    std::memory_order_relaxed)) {
                options.encoded_bytes_progress(total);
                return;
            }
        }
    };

    auto worker = [&](std::size_t worker_index, core::TaskExecutor* executor) {
        try {
            auto worker_options = options;
            std::size_t current_length = 0;
            if (options.encoded_bytes_progress) {
                worker_options.encode_progress = [&, worker_index](double fraction) {
                    const auto scanned = static_cast<std::uint64_t>(
                        static_cast<double>(current_length) *
                        std::clamp(fraction, 0.0, 1.0));
                    in_flight[worker_index].store(scanned, std::memory_order_relaxed);
                    report_aggregate(false);
                };
            }

            while (!failed.load(std::memory_order_relaxed)) {
                const auto block_index = next_block.fetch_add(1);
                if (block_index >= block_count) {
                    return;
                }

                if (options.operation) {
                    options.operation->checkpoint();
                }
                const auto start = block_index * block_size;
                const auto length = std::min(block_size, input.size() - start);
                const auto block_input = input.subspan(start, length);
                current_length = length;
                results[block_index] = compress_block(block_input, worker_options, executor);
                if (crc32 != nullptr) {
                    // The archive header still stores the normal whole-input
                    // CRC. Computing block CRCs here lets large parallel
                    // compressions avoid a later serial pass over the input.
                    block_crcs[block_index] = core::crc32(block_input);
                }
                if (options.encoded_bytes_progress) {
                    encoded_bytes.fetch_add(length, std::memory_order_relaxed);
                    in_flight[worker_index].store(0, std::memory_order_relaxed);
                    report_aggregate(true);
                }
                if (options.operation) {
                    options.operation->checkpoint();
                }
            }
        } catch (...) {
            failed.store(true, std::memory_order_relaxed);
            std::lock_guard lock(exception_mutex);
            if (!first_exception) {
                first_exception = std::current_exception();
            }
        }
    };

    auto run_workers = [&](core::TaskExecutor& executor) {
        std::vector<std::future<void>> workers;
        workers.reserve(worker_count > 0 ? worker_count - 1 : 0);
        for (std::size_t i = 1; i < worker_count; ++i) {
            workers.push_back(executor.submit([&, i] { worker(i, &executor); }));
        }
        // The submitting thread is part of a local executor's budget. With an
        // archive-scoped executor it is an outer solid-block worker already
        // accounted for by that operation's global budget.
        worker(0, &executor);
        for (auto& task : workers) {
            executor.wait(task);
        }
    };

    if (executor_thread_count <= 1) {
        worker(0, nullptr);
    } else if (options.task_executor) {
        run_workers(*options.task_executor);
    } else {
        // One cooperative executor owns every worker for this encode. Block
        // tasks can later fan out into smaller codec tasks without spawning a
        // nested pool; a waiting worker helps drain the same queue.
        core::TaskExecutor executor(executor_thread_count);
        run_workers(executor);
    }
    if (first_exception) {
        std::rethrow_exception(first_exception);
    }

    ByteVector output;
    write_varuint(output, block_count);
    for (const auto& block : results) {
        write_block(output, block);
    }
    if (crc32 != nullptr) {
        auto combined = core::crc32(std::span<const std::uint8_t>{});
        for (std::size_t i = 0; i < results.size(); ++i) {
            combined = core::crc32_combine(combined, block_crcs[i], results[i].original_size);
        }
        *crc32 = combined;
    }

    return output;
}

ByteVector decode_parallel_blocks(std::span<const std::uint8_t> encoded,
                                  std::size_t output_size,
                                  std::size_t thread_count,
                                  const std::shared_ptr<OperationControl>& operation,
                                  const std::function<void(std::uint64_t)>& decoded_bytes_progress,
                                  std::uint32_t* crc32,
                                  bool allow_sequence_codec,
                                  bool allow_context_split_codec) {
    if (operation) {
        operation->checkpoint();
    }
    std::size_t cursor = 0;
    const auto blocks = read_block_table(encoded, cursor, output_size);
    if (cursor != encoded.size()) {
        throw FormatError("trailing bytes after block payloads");
    }
    if (!allow_sequence_codec &&
        std::any_of(blocks.begin(), blocks.end(), [](const auto& block) {
            return block.codec == BlockCodec::lz77_sequences;
        })) {
        throw FormatError("sequence block codec requires AXC version 6");
    }
    if (!allow_context_split_codec &&
        std::any_of(blocks.begin(), blocks.end(), [](const auto& block) {
            return block.codec == BlockCodec::lz77_context_split;
        })) {
        throw FormatError("context-split block codec requires AXC version 7");
    }

    ByteVector output(output_size);
    std::vector<std::uint32_t> block_crcs(blocks.size());
    std::atomic_size_t next_block = 0;
    std::atomic<std::uint64_t> decoded_bytes = 0;
    std::mutex progress_mutex;
    std::uint64_t reported_decoded_bytes = 0;
    const auto worker_count = effective_thread_count(thread_count, blocks.size());
    std::mutex exception_mutex;
    std::exception_ptr first_exception;
    // A failed block makes the whole decode throw; stop handing out blocks so a
    // corrupt archive fails fast instead of decoding the rest for nothing.
    std::atomic_bool failed = false;

    auto worker = [&] {
        try {
            while (!failed.load(std::memory_order_relaxed)) {
                const auto block_index = next_block.fetch_add(1);
                if (block_index >= blocks.size()) {
                    return;
                }

                if (operation) {
                    operation->checkpoint();
                }
                const auto& block = blocks[block_index];
                const auto payload = encoded.subspan(block.payload_offset, block.payload_size);
                if (block.output_offset > output.size() ||
                    block.original_size > output.size() - block.output_offset) {
                    throw FormatError("decoded block size does not match block table");
                }
                auto target = std::span<std::uint8_t>(output)
                    .subspan(block.output_offset, block.original_size);
                decompress_block_into(payload, block.codec, target);
                block_crcs[block_index] = core::crc32(target);
                if (decoded_bytes_progress) {
                    const auto done = decoded_bytes.fetch_add(block.original_size,
                                                               std::memory_order_relaxed) +
                                      block.original_size;
                    // Workers finish out of order. Serialize the small callback
                    // section so consumers never see a later byte count followed
                    // by an older one.
                    std::lock_guard lock(progress_mutex);
                    if (done > reported_decoded_bytes) {
                        reported_decoded_bytes = done;
                        decoded_bytes_progress(done);
                    }
                }
                if (operation) {
                    operation->checkpoint();
                }
            }
        } catch (...) {
            failed.store(true, std::memory_order_relaxed);
            std::lock_guard lock(exception_mutex);
            if (!first_exception) {
                first_exception = std::current_exception();
            }
        }
    };

    if (worker_count <= 1) {
        worker();
    } else {
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i) {
            workers.emplace_back(worker);
        }
        for (auto& thread : workers) {
            thread.join();
        }
    }
    if (first_exception) {
        std::rethrow_exception(first_exception);
    }

    if (crc32 != nullptr) {
        auto combined = core::crc32(std::span<const std::uint8_t>{});
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            combined = core::crc32_combine(combined, block_crcs[i], blocks[i].original_size);
        }
        *crc32 = combined;
    }

    return output;
}

}  // namespace axiom::codec
