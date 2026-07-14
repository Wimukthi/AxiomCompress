#include "codec/block.hpp"

#include "codec/fast_lz.hpp"
#include "codec/incompressible.hpp"
#include "codec/lz77.hpp"
#include "codec/lz77_split.hpp"
#include "codec/varint.hpp"
#include "core/checksum.hpp"
#include "core/cpu.hpp"
#include "entropy/huffman.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
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
                           const CompressionOptions& options) {
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
            encode_lz77_split_payloads(fast_payloads.streams, /*fast=*/true);
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

    auto consider_lz_payload = [&](ByteVector lz_payload) {
        if (lz_payload.size() < best_size) {
            best_size = lz_payload.size();
            best.codec = BlockCodec::greedy_lz77;
            best.payload = lz_payload;
        }

        // The whole-stream Huffman candidate almost never beats split streams on
        // real data; in fast mode skip it rather than pay a full Huffman pass.
        if (!options.fast_entropy) {
            if (auto entropy_payload = entropy::encode_huffman(lz_payload);
                entropy_payload && entropy_payload->size() < best_size) {
                best_size = entropy_payload->size();
                best.codec = BlockCodec::greedy_lz77_huffman;
                best.payload = std::move(*entropy_payload);
            }
        }

        auto [split_payload, slot_payload] =
            encode_lz77_split_payloads(lz_payload, options.fast_entropy);
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
        consider_lz_payload(std::move(greedy));
        consider_lz_payload(std::move(optimal_tokens));
    } else {
        consider_lz_payload(std::move(greedy));
        if (run_optimal) {
            consider_lz_payload(encode_lz77_optimal(input, optimal_options));
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
    const auto worker_count =
        effective_compression_thread_count(options.thread_count, block_count);
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

    auto worker = [&](std::size_t worker_index) {
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
                results[block_index] = compress_block(block_input, worker_options);
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

    if (worker_count <= 1) {
        worker(0);
    } else {
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i) {
            workers.emplace_back(worker, i);
        }
        for (auto& thread : workers) {
            thread.join();
        }
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
                                  std::uint32_t* crc32) {
    if (operation) {
        operation->checkpoint();
    }
    std::size_t cursor = 0;
    const auto blocks = read_block_table(encoded, cursor, output_size);
    if (cursor != encoded.size()) {
        throw FormatError("trailing bytes after block payloads");
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
