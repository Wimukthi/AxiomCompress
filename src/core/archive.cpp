#include "core/archive.hpp"

#include "codec/block.hpp"
#include "codec/incompressible.hpp"
#include "codec/lz77.hpp"
#include "codec/lz77_split.hpp"
#include "core/checksum.hpp"
#include "core/file_replace.hpp"
#include "core/path_text.hpp"
#include "entropy/huffman.hpp"

#include <algorithm>
#include <array>
#include <future>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace axiom {

FormatError::FormatError(const char* message) : std::runtime_error(message) {}

FormatError::FormatError(const std::string& message) : std::runtime_error(message) {}

namespace core {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic = {'A', 'X', 'I', 'O', 'M', 'C', '1', '\0'};
constexpr std::uint16_t kVersion = 4;
constexpr std::size_t kHeaderSize = 32;

void write_u16(ByteVector& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8));
}

void write_u32(ByteVector& output, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void write_u64(ByteVector& output, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::uint16_t read_u16(std::span<const std::uint8_t> input, std::size_t& cursor) {
    const auto value = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(input[cursor]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(input[cursor + 1]) << 8));
    cursor += 2;
    return value;
}

std::uint32_t read_u32(std::span<const std::uint8_t> input, std::size_t& cursor) {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(input[cursor++]) << shift;
    }
    return value;
}

std::uint64_t read_u64(std::span<const std::uint8_t> input, std::size_t& cursor) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(input[cursor++]) << shift;
    }
    return value;
}

ByteVector read_file(const std::filesystem::path& path,
                     const std::function<void(std::uint64_t, std::uint64_t)>& progress = {}) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw std::runtime_error("failed to open input file: " + core::path_to_utf8(path));
    }

    const auto end = stream.tellg();
    if (end < std::streampos{0}) {
        throw std::runtime_error("failed to determine input size: " + core::path_to_utf8(path));
    }
    const auto size = static_cast<std::uintmax_t>(end);
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("input file exceeds platform size limit: " +
                                 core::path_to_utf8(path));
    }

    ByteVector bytes(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    if (progress) {
        progress(0, static_cast<std::uint64_t>(bytes.size()));
    }
    if (!bytes.empty()) {
        // Bulk reads avoid the byte-at-a-time iterator path, which dominated the
        // fastest compression profiles before worker threads even started.
        std::size_t offset = 0;
        constexpr std::size_t kReadChunk = std::size_t{8} << 20;
        while (offset < bytes.size()) {
            const auto want = std::min(kReadChunk, bytes.size() - offset);
            stream.read(reinterpret_cast<char*>(bytes.data() + offset),
                        static_cast<std::streamsize>(want));
            if (stream.gcount() != static_cast<std::streamsize>(want)) {
                throw std::runtime_error("failed to read input file: " +
                                         core::path_to_utf8(path));
            }
            offset += want;
            if (progress) {
                progress(static_cast<std::uint64_t>(offset),
                         static_cast<std::uint64_t>(bytes.size()));
            }
        }
    }

    return bytes;
}

void write_file(const std::filesystem::path& path,
                std::span<const std::uint8_t> bytes,
                const std::function<void(std::uint64_t, std::uint64_t)>& progress = {}) {
    // Write to a sibling temporary and rename into place so a mid-write failure
    // (disk full, crash) never leaves a truncated file at the destination.
    const std::filesystem::path temporary = core::unique_sibling_path(path, L"write");

    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            throw std::runtime_error("failed to open output file: " +
                                     core::path_to_utf8(temporary));
        }

        constexpr std::size_t kWriteChunk = std::size_t{8} << 20;
        std::size_t offset = 0;
        if (progress) {
            progress(0, static_cast<std::uint64_t>(bytes.size()));
        }
        while (offset < bytes.size()) {
            const auto count = std::min(kWriteChunk, bytes.size() - offset);
            stream.write(reinterpret_cast<const char*>(bytes.data() + offset),
                         static_cast<std::streamsize>(count));
            if (!stream) {
                break;
            }
            offset += count;
            if (progress) {
                progress(static_cast<std::uint64_t>(offset),
                         static_cast<std::uint64_t>(bytes.size()));
            }
        }
        stream.flush();
        if (!stream) {
            stream.close();
            std::error_code cleanup_error;
            std::filesystem::remove(temporary, cleanup_error);
            throw std::runtime_error("failed to write output file: " +
                                     core::path_to_utf8(path));
        }
    }

    try {
        core::replace_file(temporary, path, "output file");
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        throw;
    }
}

std::size_t checked_size(std::uint64_t size, const char* field_name) {
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw FormatError(std::string(field_name) + " exceeds platform size limit");
    }

    return static_cast<std::size_t>(size);
}

std::size_t lz_payload_limit(std::uint64_t original_size) {
    const auto base = checked_size(original_size, "original size");
    const auto max = std::numeric_limits<std::size_t>::max();

    if (base > max - (base / 8) - 4096) {
        return max;
    }

    return base + (base / 8) + 4096;
}

}  // namespace

ByteVector write_archive(std::span<const std::uint8_t> payload,
                         const ArchiveHeader& header) {
    ByteVector output;
    output.reserve(kHeaderSize + payload.size());

    output.insert(output.end(), kMagic.begin(), kMagic.end());
    write_u16(output, kVersion);
    output.push_back(static_cast<std::uint8_t>(header.codec));
    output.push_back(0);
    write_u64(output, header.original_size);
    write_u64(output, header.payload_size);
    write_u32(output, header.crc32);
    output.insert(output.end(), payload.begin(), payload.end());

    return output;
}

ArchiveHeader read_archive_header(std::span<const std::uint8_t> archive) {
    if (archive.size() < kHeaderSize) {
        throw FormatError("archive is smaller than the fixed header");
    }

    if (!std::equal(kMagic.begin(), kMagic.end(), archive.begin())) {
        throw FormatError("invalid archive magic");
    }

    std::size_t cursor = kMagic.size();
    const auto version = read_u16(archive, cursor);
    if (version != kVersion) {
        throw FormatError("unsupported archive version");
    }

    const auto codec_byte = archive[cursor++];
    ++cursor;  // reserved flags byte

    ArchiveHeader header;
    if (codec_byte == static_cast<std::uint8_t>(CodecId::store)) {
        header.codec = CodecId::store;
    } else if (codec_byte == static_cast<std::uint8_t>(CodecId::greedy_lz77)) {
        header.codec = CodecId::greedy_lz77;
    } else if (codec_byte == static_cast<std::uint8_t>(CodecId::greedy_lz77_huffman)) {
        header.codec = CodecId::greedy_lz77_huffman;
    } else if (codec_byte == static_cast<std::uint8_t>(CodecId::parallel_blocks)) {
        header.codec = CodecId::parallel_blocks;
    } else if (codec_byte == static_cast<std::uint8_t>(CodecId::greedy_lz77_split)) {
        header.codec = CodecId::greedy_lz77_split;
    } else if (codec_byte == static_cast<std::uint8_t>(CodecId::greedy_lz77_split_slots)) {
        header.codec = CodecId::greedy_lz77_split_slots;
    } else {
        throw FormatError("unsupported codec id");
    }

    header.original_size = read_u64(archive, cursor);
    header.payload_size = read_u64(archive, cursor);
    header.crc32 = read_u32(archive, cursor);

    if (header.payload_size > archive.size() - kHeaderSize) {
        throw FormatError("payload size exceeds archive size");
    }

    if (archive.size() != kHeaderSize + header.payload_size) {
        throw FormatError("archive has trailing bytes after payload");
    }

    return header;
}

std::span<const std::uint8_t> archive_payload(std::span<const std::uint8_t> archive,
                                              const ArchiveHeader& header) {
    return archive.subspan(kHeaderSize, static_cast<std::size_t>(header.payload_size));
}

}  // namespace core

ByteVector compress(std::span<const std::uint8_t> input,
                    const CompressionOptions& options) {
    if (options.operation) {
        options.operation->checkpoint();
    }
    if (options.encoded_bytes_progress) {
        options.encoded_bytes_progress(0);
    }

    ByteVector payload;
    auto codec = core::CodecId::store;
    std::optional<std::uint32_t> payload_crc;

    if (options.force_store) {
        payload.assign(input.begin(), input.end());
    } else if (!options.force_parallel_blocks && codec::likely_incompressible(input)) {
        payload.assign(input.begin(), input.end());
    } else {
        auto parallel_options = options;
        parallel_options.block_size = codec::effective_parallel_block_size(input.size(), options);
        if (options.enable_optimal_parser) {
            // With the optimal parser on, per-block match windows dominate the
            // ratio: thread-count auto-sizing can shrink blocks to 1 MiB, which
            // throws away more ratio than idle workers cost wall time. Keep
            // thorough blocks at least 4 MiB (bounded by the preset size).
            constexpr std::size_t kMinThoroughBlock = std::size_t{4} << 20;
            parallel_options.block_size =
                std::max(parallel_options.block_size,
                         std::min(kMinThoroughBlock, options.block_size));
            // encode_parallel_blocks re-applies thread-count auto-sizing from
            // its options; pin the chosen size so the floor survives.
            parallel_options.auto_block_size_for_threads = false;
        }
        const auto block_size = parallel_options.block_size;
        const auto block_count = (input.size() + block_size - 1) / block_size;
        const auto workers =
            codec::effective_compression_thread_count(options.thread_count, block_count);

        if (options.force_parallel_blocks) {
            std::uint32_t block_crc = 0;
            const auto block_payload =
                codec::encode_parallel_blocks(input, parallel_options, &block_crc);
            const core::ArchiveHeader header{
                core::CodecId::parallel_blocks,
                static_cast<std::uint64_t>(input.size()),
                static_cast<std::uint64_t>(block_payload.size()),
                block_crc,
            };
            if (options.encoded_bytes_progress) {
                options.encoded_bytes_progress(static_cast<std::uint64_t>(input.size()));
            }
            return core::write_archive(block_payload, header);
        }

        if (options.use_fast_lz) {
            std::uint32_t block_crc = 0;
            auto block_payload =
                codec::encode_parallel_blocks(input, parallel_options, &block_crc);
            if (block_payload.size() < input.size()) {
                const core::ArchiveHeader header{
                    core::CodecId::parallel_blocks,
                    static_cast<std::uint64_t>(input.size()),
                    static_cast<std::uint64_t>(block_payload.size()),
                    block_crc,
                };
                if (options.encoded_bytes_progress) {
                    options.encoded_bytes_progress(static_cast<std::uint64_t>(input.size()));
                }
                return core::write_archive(block_payload, header);
            }

            payload.assign(input.begin(), input.end());
            const core::ArchiveHeader header{
                core::CodecId::store,
                static_cast<std::uint64_t>(input.size()),
                static_cast<std::uint64_t>(payload.size()),
                core::crc32(input),
            };
            if (options.encoded_bytes_progress) {
                options.encoded_bytes_progress(static_cast<std::uint64_t>(input.size()));
            }
            return core::write_archive(payload, header);
        }

        const auto evaluate_parallel_candidate = block_count > 1 && workers > 1;
        // Whole-input serial analysis runs the greedy, tree, and optimal parses
        // on one core (minutes on large inputs) while the parallel codec runs
        // the same optimal parse per block on every worker and measures within
        // a percent of it. So the serial candidates only run where their edge
        // is real and affordable: single-block/single-worker encodes, and small
        // inputs whose whole-input window meaningfully beats per-block windows.
        // A single worker (--threads 1) still forces the full serial analysis.
        constexpr std::size_t kSerialThoroughLimit = std::size_t{16} << 20;
        const bool thorough = options.enable_optimal_parser &&
                              (!evaluate_parallel_candidate ||
                               (input.size() <= kSerialThoroughLimit &&
                                input.size() <= options.optimal_parse_limit));
        std::future<ByteVector> block_future;
        if (evaluate_parallel_candidate && thorough) {
            block_future = std::async(std::launch::async, [&input, parallel_options] {
                return codec::encode_parallel_blocks(input, parallel_options);
            });
        }

        payload.assign(input.begin(), input.end());

        auto consider_lz_payload = [&](ByteVector lz_payload) {
            if (options.operation) {
                options.operation->checkpoint();
            }
            // Build every candidate from lz_payload before moving it away below.
            // The split and slot payloads are produced together so their shared
            // streams (including the costly literal coding) are encoded once. In
            // fast mode the whole-stream Huffman candidate (which rarely wins) is
            // skipped and the split encoder uses its cheap coder chooser.
            std::optional<ByteVector> entropy_payload;
            if (!options.fast_entropy) {
                entropy_payload = entropy::encode_huffman(lz_payload);
            }
            auto [split_payload, slot_payload] =
                codec::encode_lz77_split_payloads(lz_payload, options.fast_entropy);

            if (lz_payload.size() < payload.size()) {
                codec = core::CodecId::greedy_lz77;
                payload = std::move(lz_payload);
            }

            if (entropy_payload && entropy_payload->size() < payload.size()) {
                codec = core::CodecId::greedy_lz77_huffman;
                payload = std::move(*entropy_payload);
            }

            if (split_payload.size() < payload.size()) {
                codec = core::CodecId::greedy_lz77_split;
                payload = std::move(split_payload);
            }

            if (slot_payload && slot_payload->size() < payload.size()) {
                codec = core::CodecId::greedy_lz77_split_slots;
                payload = std::move(*slot_payload);
            }
        };

        // For large, multi-block inputs the serial whole-input greedy parse is
        // the compression bottleneck, while the parallel block codec threads it
        // across cores at a small ratio cost. The default fast path therefore
        // uses the parallel result; the slower single-stream (and optimal)
        // analysis runs only for small inputs or when maximum effort is asked
        // for via the optimal parser.
        if (evaluate_parallel_candidate && !thorough) {
            std::uint32_t block_crc = 0;
            auto block_payload =
                codec::encode_parallel_blocks(input, parallel_options, &block_crc);
            if (block_payload.size() < payload.size()) {
                codec = core::CodecId::parallel_blocks;
                payload = std::move(block_payload);
                payload_crc = block_crc;
            }
        } else {
            if (options.operation) {
                options.operation->checkpoint();
            }
            // Serial candidates parse the whole input back to back; map the
            // encoders' fine-grained scan fractions onto the input size so the
            // bar keeps moving during single-block and single-worker encodes.
            auto serial_options = options;
            if (options.encoded_bytes_progress) {
                serial_options.encode_progress =
                    [report = options.encoded_bytes_progress,
                     total = input.size()](double fraction) {
                        report(static_cast<std::uint64_t>(
                            static_cast<double>(total) *
                            std::clamp(fraction, 0.0, 1.0)));
                    };
            }
            const bool run_optimal =
                thorough && input.size() <= options.optimal_parse_limit;
            const double greedy_share =
                !run_optimal ? 0.90 : options.optimal_two_pass ? 0.18 : 0.35;
            auto greedy = codec::encode_lz77(
                input, codec::scoped_progress_options(serial_options, 0.0, greedy_share));
            const auto optimal_options =
                codec::scoped_progress_options(serial_options, greedy_share, 0.90);
            if (run_optimal && !options.optimal_two_pass) {
                if (options.operation) {
                    options.operation->checkpoint();
                }
                // Single-pass optimal measures its cost model from the greedy
                // tokens, so they must outlive the parse (see compress_block).
                auto optimal_tokens =
                    codec::encode_lz77_optimal(input, optimal_options, &greedy);
                consider_lz_payload(std::move(greedy));
                consider_lz_payload(std::move(optimal_tokens));
            } else {
                consider_lz_payload(std::move(greedy));
                if (run_optimal) {
                    if (options.operation) {
                        options.operation->checkpoint();
                    }
                    consider_lz_payload(codec::encode_lz77_optimal(input, optimal_options));
                }
            }
            // Max-effort mode also tries the binary-tree finder, whose large
            // effective window typically finds the most (and longest) matches.
            if (thorough && !options.use_tree_matcher) {
                auto tree_options =
                    codec::scoped_progress_options(serial_options, 0.90, 1.0);
                tree_options.use_tree_matcher = true;
                if (options.operation) {
                    options.operation->checkpoint();
                }
                consider_lz_payload(codec::encode_lz77(input, tree_options));
            }
            if (evaluate_parallel_candidate) {
                auto block_payload = block_future.get();
                if (block_payload.size() < payload.size()) {
                    codec = core::CodecId::parallel_blocks;
                    payload = std::move(block_payload);
                }
            }
        }
    }

    const core::ArchiveHeader header{
        codec,
        static_cast<std::uint64_t>(input.size()),
        static_cast<std::uint64_t>(payload.size()),
        payload_crc ? *payload_crc : core::crc32(input),
    };

    if (options.operation) {
        options.operation->checkpoint();
    }
    if (options.encoded_bytes_progress) {
        options.encoded_bytes_progress(static_cast<std::uint64_t>(input.size()));
    }
    return core::write_archive(payload, header);
}

ByteVector decompress(std::span<const std::uint8_t> archive,
                      const DecompressionOptions& options) {
    if (options.operation) {
        options.operation->checkpoint();
    }
    const auto header = core::read_archive_header(archive);

    // Reject decompression bombs before allocating or decoding: the declared
    // original size is attacker-controlled, and an overlapping match can expand a
    // tiny payload to fill it. Every decode path below produces at most
    // original_size bytes, so this single check bounds peak memory.
    if (header.original_size > options.max_output_size) {
        throw FormatError("declared output size exceeds the allowed limit");
    }

    if (options.decoded_bytes_progress) {
        options.decoded_bytes_progress(0, header.original_size);
    }

    const auto payload = core::archive_payload(archive, header);

    ByteVector restored;
    std::optional<std::uint32_t> restored_crc;
    if (header.codec == core::CodecId::store) {
        restored.assign(payload.begin(), payload.end());
    } else if (header.codec == core::CodecId::greedy_lz77) {
        restored = codec::decode_lz77(payload, core::checked_size(header.original_size, "original size"));
    } else if (header.codec == core::CodecId::greedy_lz77_huffman) {
        const auto lz_payload = entropy::decode_huffman(
            payload, core::lz_payload_limit(header.original_size));
        restored = codec::decode_lz77(lz_payload, core::checked_size(header.original_size, "original size"));
    } else if (header.codec == core::CodecId::greedy_lz77_split) {
        restored = codec::decode_lz77_split_streams(
            payload, core::checked_size(header.original_size, "original size"));
    } else if (header.codec == core::CodecId::greedy_lz77_split_slots) {
        restored = codec::decode_lz77_split_streams_slots(
            payload, core::checked_size(header.original_size, "original size"));
    } else if (header.codec == core::CodecId::parallel_blocks) {
        std::uint32_t combined_crc = 0;
        restored = codec::decode_parallel_blocks(
            payload,
            core::checked_size(header.original_size, "original size"),
            options.thread_count,
            options.operation,
            [progress = options.decoded_bytes_progress,
             total = header.original_size](std::uint64_t done) {
                if (progress) {
                    progress(done, total);
                }
            },
            &combined_crc);
        restored_crc = combined_crc;
    } else {
        throw FormatError("unsupported codec id");
    }

    if (restored.size() != header.original_size) {
        throw FormatError("restored size does not match archive header");
    }

    if (options.operation) {
        options.operation->checkpoint();
    }
    const auto actual_crc = restored_crc ? *restored_crc : core::crc32(restored);
    if (actual_crc != header.crc32) {
        throw FormatError("CRC check failed");
    }

    if (options.decoded_bytes_progress) {
        // Serial codecs cannot expose safe token-level progress yet, but still
        // finish the same public progress range as parallel block streams.
        options.decoded_bytes_progress(header.original_size, header.original_size);
    }

    return restored;
}

void compress_file(const std::filesystem::path& input_path,
                   const std::filesystem::path& output_path,
                   const CompressionOptions& options) {
    auto file_options = options;
    const std::string input_name = core::path_to_utf8(input_path);
    const auto operation = file_options.operation;
    const auto input = core::read_file(
        input_path, [operation, input_name](std::uint64_t done, std::uint64_t total) {
            if (operation) {
                operation->report({OperationStage::reading, done, total, 0, 1, input_name,
                                   done, total});
            }
        });
    if (file_options.operation && !file_options.encoded_bytes_progress) {
        const auto progress_operation = file_options.operation;
        const auto total = static_cast<std::uint64_t>(input.size());
        file_options.encoded_bytes_progress = [progress_operation, input_name, total](std::uint64_t done) {
            progress_operation->report({OperationStage::compressing, done, total, 0, 1, input_name,
                                        done, total});
        };
    }
    const auto archive = compress(input, file_options);
    core::write_file(output_path, archive);
}

void decompress_file(const std::filesystem::path& input_path,
                     const std::filesystem::path& output_path,
                     const DecompressionOptions& options) {
    auto file_options = options;
    const std::string input_name = core::path_to_utf8(input_path);
    if (file_options.operation && !file_options.decoded_bytes_progress) {
        const auto operation = file_options.operation;
        file_options.decoded_bytes_progress = [operation, input_name](std::uint64_t done,
                                                                       std::uint64_t total) {
            operation->report({OperationStage::extracting, done, total, 0, 1, input_name,
                               done, total});
        };
    }
    const auto operation = file_options.operation;
    const auto archive = core::read_file(
        input_path, [operation, input_name](std::uint64_t done, std::uint64_t total) {
            if (operation) {
                operation->report({OperationStage::reading, done, total, 0, 1, input_name,
                                   done, total});
            }
        });
    const auto output = decompress(archive, file_options);
    const std::string output_name = core::path_to_utf8(output_path);
    core::write_file(
        output_path, output,
        [operation, output_name](std::uint64_t done, std::uint64_t total) {
            if (operation) {
                operation->report({OperationStage::writing, done, total, 0, 1, output_name,
                                   done, total});
            }
        });
}

}  // namespace axiom
