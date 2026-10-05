#include "codec/external_codecs.hpp"

#include "core/cpu.hpp"
#include "core/task_executor.hpp"
#include "third_party/lzma-sdk/Lzma2Dec.h"
#include "third_party/lzma-sdk/Lzma2Enc.h"
// miniz declares its full static helper set in the header, so every
// translation unit that includes it leaves most of them unused. The
// vendored file is dependency-locked and cannot carry the suppression.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "third_party/miniz/miniz.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#include "third_party/zstd/lib/zstd.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace axiom::codec {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'A', 'X', 'E', 'C'};
constexpr std::uint8_t kPayloadVersion = 1;
constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kRecordHeaderSize = 12;
constexpr std::uint8_t kStoredChunk = 1;
constexpr std::size_t kMinChunkSize = std::size_t{256} << 10;
constexpr std::size_t kMaxFastCodecChunkSize = std::size_t{4} << 20;
// AXEC stores chunk sizes as u32. LZMA2 uses the full representable range;
// Zstandard and Deflate intentionally keep their smaller independent limits.
constexpr std::size_t kMaxLzmaChunkSize = kMaxLzmaDictionarySize;
constexpr std::size_t kMinLzmaDictionarySize = std::size_t{1} << 12;
constexpr std::size_t kMaxPropertySize = 16;

struct EncodedChunk {
    std::uint32_t raw_size = 0;
    bool stored = false;
    ByteVector bytes;
};

void append_u16(ByteVector& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8));
}

void append_u32(ByteVector& output, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::uint16_t read_u16(std::span<const std::uint8_t> input, std::size_t& cursor) {
    if (cursor > input.size() || input.size() - cursor < 2) {
        throw FormatError("external codec header is truncated");
    }
    const auto value = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(input[cursor]) |
        (static_cast<std::uint16_t>(input[cursor + 1]) << 8));
    cursor += 2;
    return value;
}

std::uint32_t read_u32(std::span<const std::uint8_t> input, std::size_t& cursor) {
    if (cursor > input.size() || input.size() - cursor < 4) {
        throw FormatError("external codec header is truncated");
    }
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift != 32; shift += 8) {
        value |= static_cast<std::uint32_t>(input[cursor++]) << shift;
    }
    return value;
}

int effective_level(CompressionMethod method, const CompressionOptions& options) {
    if (options.codec_level != kAutomaticCodecLevel) {
        switch (method) {
            case CompressionMethod::zstandard:
                return std::clamp(options.codec_level, -5, 22);
            case CompressionMethod::lzma2:
            case CompressionMethod::deflate:
                return std::clamp(options.codec_level, 0, 9);
            default:
                return options.codec_level;
        }
    }

    const int portable = std::clamp(options.level, 1, 9);
    if (method == CompressionMethod::zstandard) {
        constexpr std::array<int, 9> levels{1, 2, 3, 5, 7, 10, 14, 18, 22};
        return levels[static_cast<std::size_t>(portable - 1)];
    }
    return portable;
}

std::size_t default_lzma_dictionary_size(int level) {
    level = std::clamp(level, 0, 9);
    const std::size_t sdk_default = level <= 4
        ? std::size_t{1} << (level * 2 + 16)
        : std::size_t{1} << std::min(level + 20, 30);
    return std::min(sdk_default, kMaxLzmaChunkSize);
}

std::size_t default_lzma_fast_bytes(int level) {
    return level < 7 ? 32 : 64;
}

void validate_lzma2_property(std::uint8_t property, std::size_t chunk_size) {
    if (property > 40) {
        throw FormatError("LZMA2 dictionary property is invalid");
    }
    const std::uint64_t dictionary_size = property == 40
        ? std::numeric_limits<std::uint32_t>::max()
        : (static_cast<std::uint64_t>(2u | (property & 1u))
           << (property / 2u + 11u));
    if (dictionary_size > std::max(chunk_size, kMinLzmaDictionarySize)) {
        throw FormatError("LZMA2 dictionary exceeds the chunk memory bound");
    }
}

void checkpoint(const std::shared_ptr<OperationControl>& operation) {
    if (operation) {
        operation->checkpoint();
    }
}

void report_encoded(const CompressionOptions& options, std::uint64_t done) {
    if (options.encoded_bytes_progress) {
        options.encoded_bytes_progress(done);
    }
}

ByteVector encode_zstandard(std::span<const std::uint8_t> input, int level) {
    const std::size_t bound = ZSTD_compressBound(input.size());
    if (ZSTD_isError(bound) || bound > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("Zstandard could not determine an output bound");
    }
    ByteVector output(bound);
    const std::size_t size =
        ZSTD_compress(output.data(), output.size(), input.data(), input.size(), level);
    if (ZSTD_isError(size)) {
        throw std::runtime_error(std::string("Zstandard compression failed: ") +
                                 ZSTD_getErrorName(size));
    }
    output.resize(size);
    return output;
}

// The decoders write into memory the caller owns, so independent chunks can be
// decoded straight into their place in the output.
void decode_zstandard_into(std::span<const std::uint8_t> input,
                           std::span<std::uint8_t> output) {
    const std::size_t expected_size = output.size();
    const std::size_t frame_size =
        ZSTD_findFrameCompressedSize(input.data(), input.size());
    if (ZSTD_isError(frame_size) || frame_size != input.size()) {
        throw FormatError("Zstandard chunk is truncated or has trailing data");
    }
    ZSTD_DCtx* context = ZSTD_createDCtx();
    if (context == nullptr) {
        throw std::bad_alloc();
    }
    int window_log = 10;
    std::size_t window_bound = std::size_t{1} << window_log;
    while (window_bound < std::max(expected_size, std::size_t{1}) &&
           window_log < 31) {
        ++window_log;
        window_bound <<= 1;
    }
    const auto parameter_result =
        ZSTD_DCtx_setParameter(context, ZSTD_d_windowLogMax, window_log);
    if (ZSTD_isError(parameter_result)) {
        ZSTD_freeDCtx(context);
        throw FormatError("Zstandard chunk window exceeds the decoder bound");
    }
    const std::size_t size = ZSTD_decompressDCtx(
        context, output.data(), output.size(), input.data(), input.size());
    ZSTD_freeDCtx(context);
    if (ZSTD_isError(size) || size != expected_size) {
        throw FormatError("Zstandard chunk does not match its declared size");
    }
}

ByteVector decode_zstandard(std::span<const std::uint8_t> input,
                           std::size_t expected_size) {
    ByteVector output(expected_size);
    decode_zstandard_into(input, output);
    return output;
}

ByteVector encode_deflate(std::span<const std::uint8_t> input, int level) {
    if (input.size() > std::numeric_limits<mz_ulong>::max()) {
        throw std::runtime_error("Deflate chunk exceeds the codec size limit");
    }
    const auto source_size = static_cast<mz_ulong>(input.size());
    mz_ulong output_size = mz_compressBound(source_size);
    ByteVector output(static_cast<std::size_t>(output_size));
    const int result = mz_compress2(output.data(), &output_size, input.data(),
                                    source_size, level);
    if (result != MZ_OK) {
        throw std::runtime_error("Deflate compression failed");
    }
    output.resize(static_cast<std::size_t>(output_size));
    return output;
}

void decode_deflate_into(std::span<const std::uint8_t> input,
                         std::span<std::uint8_t> output) {
    const std::size_t expected_size = output.size();
    if (input.size() > std::numeric_limits<mz_ulong>::max() ||
        expected_size > std::numeric_limits<mz_ulong>::max()) {
        throw FormatError("Deflate chunk exceeds the codec size limit");
    }
    mz_ulong output_size = static_cast<mz_ulong>(expected_size);
    mz_ulong input_size = static_cast<mz_ulong>(input.size());
    const int result = mz_uncompress2(output.data(), &output_size, input.data(), &input_size);
    if (result != MZ_OK || output_size != expected_size || input_size != input.size()) {
        throw FormatError("Deflate chunk does not match its declared size");
    }
}

ByteVector decode_deflate(std::span<const std::uint8_t> input,
                         std::size_t expected_size) {
    ByteVector output(expected_size);
    decode_deflate_into(input, output);
    return output;
}

void* lzma_alloc(ISzAllocPtr, size_t size) {
    return std::malloc(size);
}

void lzma_free(ISzAllocPtr, void* address) {
    std::free(address);
}

const ISzAlloc kLzmaAllocator{lzma_alloc, lzma_free};

struct LzmaOutputStream {
    ISeqOutStream interface{};
    ByteVector bytes;
};

size_t lzma_write(ISeqOutStreamPtr stream, const void* data, size_t size) {
    auto* output = Z7_CONTAINER_FROM_VTBL_SIMPLE(stream, LzmaOutputStream, interface);
    try {
        const auto* begin = static_cast<const std::uint8_t*>(data);
        output->bytes.insert(output->bytes.end(), begin, begin + size);
        return size;
    } catch (...) {
        return 0;
    }
}

struct LzmaProgress {
    ICompressProgress interface{};
    std::shared_ptr<OperationControl> operation;
    bool cancelled = false;
};

SRes lzma_progress(ICompressProgressPtr progress, UInt64, UInt64) {
    auto* state = Z7_CONTAINER_FROM_VTBL_SIMPLE(progress, LzmaProgress, interface);
    try {
        checkpoint(state->operation);
        return SZ_OK;
    } catch (...) {
        state->cancelled = true;
        return SZ_ERROR_PROGRESS;
    }
}

// The dictionary an LZMA2 chunk is encoded with: the requested (or level
// default) size, held to the chunk size and to the payload bound.
std::size_t lzma_dictionary_for(const CompressionOptions& options,
                                std::size_t dictionary_limit,
                                std::size_t dictionary_input_bound) {
    const int level = effective_level(CompressionMethod::lzma2, options);
    const std::size_t dictionary = options.lzma_dictionary_size == 0
        ? default_lzma_dictionary_size(level)
        : options.lzma_dictionary_size;
    const auto maximum_dictionary = std::min(
        std::max(dictionary_limit, kMinLzmaDictionarySize),
        kMaxLzmaDictionarySize);
    const auto bounded_dictionary = std::clamp(
        dictionary, kMinLzmaDictionarySize, maximum_dictionary);
    return std::min(bounded_dictionary,
                    std::max(dictionary_input_bound, kMinLzmaDictionarySize));
}

std::pair<ByteVector, std::uint8_t> encode_lzma2(
    std::span<const std::uint8_t> input,
    const CompressionOptions& options,
    std::size_t dictionary_limit,
    std::size_t dictionary_input_bound) {
    CLzma2EncHandle encoder = Lzma2Enc_Create(&kLzmaAllocator, &kLzmaAllocator);
    if (encoder == nullptr) {
        throw std::bad_alloc();
    }

    try {
        CLzma2EncProps properties;
        Lzma2EncProps_Init(&properties);
        const int level = effective_level(CompressionMethod::lzma2, options);
        properties.lzmaProps.level = level;
        if (options.lzma_dictionary_size != 0 &&
            static_cast<std::uint64_t>(options.lzma_dictionary_size) >
                (std::uint64_t{1} << 32)) {
            throw std::invalid_argument(
                "LZMA2 dictionary exceeds the 4 GiB limit");
        }
        // AXEC stores one LZMA2 property for the whole payload, so every frame
        // must use the same dictionary even when the final frame is short. The
        // caller supplies a stable payload/working-chunk bound to avoid a
        // multi-gigabyte SDK allocation for a genuinely small input.
        const auto effective_dictionary =
            lzma_dictionary_for(options, dictionary_limit, dictionary_input_bound);
        properties.lzmaProps.dictSize = static_cast<UInt32>(effective_dictionary);
        const std::size_t fast_bytes = options.lzma_fast_bytes == 0
            ? default_lzma_fast_bytes(level)
            : options.lzma_fast_bytes;
        properties.lzmaProps.fb =
            static_cast<int>(std::clamp<std::size_t>(fast_bytes, 5, 273));
        properties.lzmaProps.btMode = options.lzma_binary_tree ? 1 : 0;
        properties.lzmaProps.numHashBytes = 4;
        properties.lzmaProps.numThreads = 1;
        properties.blockSize = LZMA2_ENC_PROPS_BLOCK_SIZE_SOLID;
        properties.numBlockThreads_Reduced = 1;
        properties.numBlockThreads_Max = 1;
        properties.numTotalThreads = 1;
        properties.numThreadGroups = 0;
        if (Lzma2Enc_SetProps(encoder, &properties) != SZ_OK) {
            throw std::runtime_error("LZMA2 rejected the compression settings");
        }
        Lzma2Enc_SetDataSize(encoder, static_cast<UInt64>(input.size()));
        const std::uint8_t property = Lzma2Enc_WriteProperties(encoder);

        LzmaOutputStream output;
        output.interface.Write = lzma_write;
        LzmaProgress progress;
        progress.interface.Progress = lzma_progress;
        progress.operation = options.operation;
        const SRes result = Lzma2Enc_Encode2(
            encoder, &output.interface, nullptr, nullptr, nullptr,
            input.data(), input.size(), &progress.interface);
        if (progress.cancelled) {
            throw OperationCancelled();
        }
        if (result != SZ_OK) {
            throw std::runtime_error("LZMA2 compression failed");
        }
        Lzma2Enc_Destroy(encoder);
        return {std::move(output.bytes), property};
    } catch (...) {
        Lzma2Enc_Destroy(encoder);
        throw;
    }
}

void decode_lzma2_into(std::span<const std::uint8_t> input,
                       std::span<std::uint8_t> output,
                       std::uint8_t property) {
    const std::size_t expected_size = output.size();
    SizeT output_size = expected_size;
    SizeT input_size = input.size();
    ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
    const SRes result = Lzma2Decode(
        output.data(), &output_size, input.data(), &input_size, property,
        LZMA_FINISH_END, &status, &kLzmaAllocator);
    if (result != SZ_OK || output_size != expected_size || input_size != input.size() ||
        status != LZMA_STATUS_FINISHED_WITH_MARK) {
        throw FormatError("LZMA2 chunk does not match its declared size");
    }
}

ByteVector decode_lzma2(std::span<const std::uint8_t> input,
                       std::size_t expected_size,
                       std::uint8_t property) {
    ByteVector output(expected_size);
    decode_lzma2_into(input, output, property);
    return output;
}

struct MadeChunk {
    EncodedChunk chunk;
    // The LZMA2 stream property this chunk was encoded with. Chunks are encoded
    // independently (and possibly at once), so the caller checks they agree.
    std::optional<std::uint8_t> property;
};

MadeChunk make_chunk(std::span<const std::uint8_t> input,
                     CompressionMethod method,
                     const CompressionOptions& options,
                     std::size_t dictionary_limit,
                     std::size_t dictionary_input_bound) {
    if (input.size() > kMaxLzmaChunkSize) {
        throw std::runtime_error("external codec chunk exceeds the 4 GiB format limit");
    }
    MadeChunk made;
    EncodedChunk& chunk = made.chunk;
    chunk.raw_size = static_cast<std::uint32_t>(input.size());
    if (method == CompressionMethod::zstandard) {
        chunk.bytes = encode_zstandard(
            input, effective_level(CompressionMethod::zstandard, options));
    } else if (method == CompressionMethod::lzma2) {
        auto [bytes, property] =
            encode_lzma2(
                input, options, dictionary_limit, dictionary_input_bound);
        made.property = property;
        chunk.bytes = std::move(bytes);
    } else if (method == CompressionMethod::deflate) {
        chunk.bytes = encode_deflate(
            input, effective_level(CompressionMethod::deflate, options));
    } else {
        throw std::invalid_argument("unsupported external compression method");
    }
    if (chunk.bytes.size() >= input.size()) {
        chunk.stored = true;
        chunk.bytes.assign(input.begin(), input.end());
    }
    return made;
}

// How many chunks may be encoded at once. Chunks are independent, so this is a
// question of workers and of memory: an LZMA2 encoder holds about twelve times
// its dictionary, which for large chunks adds up quickly.
std::size_t chunk_encode_concurrency(CompressionMethod method,
                                     const CompressionOptions& options,
                                     std::size_t chunk_count,
                                     std::size_t chunk_size,
                                     std::size_t dictionary_input_bound) {
    if (chunk_count < 2) return 1;
    std::size_t workers = options.task_executor
        ? options.task_executor->worker_count()
        : (options.thread_count == 0 ? core::logical_processor_count() : options.thread_count);
    workers = std::min(std::max<std::size_t>(workers, 1), chunk_count);
    if (workers < 2) return 1;

    constexpr std::uint64_t kMemoryBudget = std::uint64_t{3} << 30;
    std::uint64_t per_chunk = 64ull << 20;  // Zstandard: window-dependent, bounded by the level
    if (method == CompressionMethod::lzma2) {
        per_chunk = std::uint64_t{lzma_dictionary_for(options, chunk_size, dictionary_input_bound)} * 12 +
                    2ull * chunk_size;
    } else if (method == CompressionMethod::deflate) {
        per_chunk = 1ull << 20;
    }
    const auto by_memory = std::max<std::uint64_t>(1, kMemoryBudget / std::max<std::uint64_t>(per_chunk, 1));
    return static_cast<std::size_t>(std::min<std::uint64_t>(workers, by_memory));
}

}  // namespace

ByteVector encode_external_codec_impl(
    std::span<const std::uint8_t> input,
    CompressionMethod method,
    const CompressionOptions& options,
    std::size_t dictionary_input_bound) {
    if (method != CompressionMethod::zstandard &&
        method != CompressionMethod::lzma2 &&
        method != CompressionMethod::deflate) {
        throw std::invalid_argument("method is not an external AXC codec");
    }
    if (method == CompressionMethod::lzma2 &&
        static_cast<std::uint64_t>(options.lzma_dictionary_size) >
            (std::uint64_t{1} << 32)) {
        throw std::invalid_argument("LZMA2 dictionary exceeds the 4 GiB limit");
    }

    const std::size_t maximum_chunk = method == CompressionMethod::lzma2
        ? kMaxLzmaChunkSize : kMaxFastCodecChunkSize;
    const std::size_t requested = options.external_codec_chunk_size != 0
        ? options.external_codec_chunk_size
        : (options.block_size == 0 ? maximum_chunk : options.block_size);
    const std::size_t chunk_size =
        std::clamp(requested, kMinChunkSize, maximum_chunk);
    const std::size_t chunk_count =
        input.empty() ? 0 : 1 + (input.size() - 1) / chunk_size;
    if (chunk_count > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("external codec chunk count exceeds the format limit");
    }

    std::vector<EncodedChunk> chunks;
    chunks.reserve(chunk_count);
    std::vector<std::uint8_t> properties;
    const auto chunk_end = [&](std::size_t index) {
        return std::min(input.size(), (index + 1) * chunk_size);
    };
    const auto encode_chunk = [&](std::size_t index) {
        checkpoint(options.operation);
        const std::size_t offset = index * chunk_size;
        return make_chunk(input.subspan(offset, chunk_end(index) - offset), method, options,
                          chunk_size, dictionary_input_bound);
    };
    // Chunks are accepted in order, so progress and the payload do not depend on
    // how they were scheduled.
    const auto accept = [&](MadeChunk made, std::size_t index) {
        if (made.property) {
            if (properties.empty()) {
                properties.push_back(*made.property);
            } else if (properties.front() != *made.property) {
                throw std::runtime_error("LZMA2 produced inconsistent stream properties");
            }
        }
        chunks.push_back(std::move(made.chunk));
        report_encoded(options, static_cast<std::uint64_t>(chunk_end(index)));
    };

    const std::size_t concurrency = chunk_encode_concurrency(
        method, options, chunk_count, chunk_size, dictionary_input_bound);
    if (concurrency <= 1) {
        for (std::size_t index = 0; index < chunk_count; ++index) {
            accept(encode_chunk(index), index);
        }
    } else {
        // The operation's executor when it has one, otherwise one for this
        // payload. Every chunk is queued and they are taken in order; the
        // semaphore keeps no more than `concurrency` encoders alive at once, which
        // is what bounds the memory (a finished chunk holds only its output).
        core::TaskExecutor* executor = options.task_executor.get();
        std::optional<core::TaskExecutor> local_executor;
        if (executor == nullptr) {
            local_executor.emplace(concurrency);
            executor = &*local_executor;
        }
        std::counting_semaphore<> permits(static_cast<std::ptrdiff_t>(concurrency));
        std::vector<std::future<MadeChunk>> pending;
        pending.reserve(chunk_count);
        try {
            for (std::size_t index = 0; index < chunk_count; ++index) {
                pending.push_back(executor->submit([&, index] {
                    permits.acquire();
                    struct Release {
                        std::counting_semaphore<>& permits;
                        ~Release() { permits.release(); }
                    } release{permits};
                    return encode_chunk(index);
                }));
            }
            for (std::size_t index = 0; index < chunk_count; ++index) {
                accept(executor->wait(pending[index]), index);
            }
        } catch (...) {
            // The tasks borrow `input`, `options` and `permits`: let each finish
            // before unwinding.
            for (auto& future : pending) {
                try {
                    if (future.valid()) executor->wait(future);
                } catch (...) {
                }
            }
            throw;
        }
    }

    if (properties.size() > kMaxPropertySize) {
        throw std::runtime_error("external codec properties exceed the format limit");
    }
    std::size_t total = kHeaderSize + properties.size();
    for (const auto& chunk : chunks) {
        if (chunk.bytes.size() > std::numeric_limits<std::uint32_t>::max() ||
            total > std::numeric_limits<std::size_t>::max() -
                        kRecordHeaderSize - chunk.bytes.size()) {
            throw std::runtime_error("external codec payload exceeds the platform limit");
        }
        total += kRecordHeaderSize + chunk.bytes.size();
    }

    ByteVector output;
    output.reserve(total);
    output.insert(output.end(), kMagic.begin(), kMagic.end());
    output.push_back(kPayloadVersion);
    output.push_back(static_cast<std::uint8_t>(properties.size()));
    append_u16(output, 0);
    append_u32(output, static_cast<std::uint32_t>(chunk_size));
    append_u32(output, static_cast<std::uint32_t>(chunk_count));
    output.insert(output.end(), properties.begin(), properties.end());
    for (const auto& chunk : chunks) {
        append_u32(output, chunk.raw_size);
        append_u32(output, static_cast<std::uint32_t>(chunk.bytes.size()));
        output.push_back(chunk.stored ? kStoredChunk : 0);
        output.insert(output.end(), 3, 0);
        output.insert(output.end(), chunk.bytes.begin(), chunk.bytes.end());
    }
    return output;
}

ByteVector encode_external_codec(std::span<const std::uint8_t> input,
                                 CompressionMethod method,
                                 const CompressionOptions& options) {
    return encode_external_codec_impl(input, method, options, input.size());
}

ByteVector encode_external_codec_with_dictionary_bound(
    std::span<const std::uint8_t> input,
    CompressionMethod method,
    const CompressionOptions& options,
    std::size_t dictionary_input_bound) {
    return encode_external_codec_impl(
        input, method, options,
        dictionary_input_bound == 0 ? input.size() : dictionary_input_bound);
}

ByteVector decode_external_codec(std::span<const std::uint8_t> payload,
                                 CompressionMethod method,
                                 std::size_t expected_size,
                                 const DecompressionOptions& options) {
    if (payload.size() < kHeaderSize ||
        !std::equal(kMagic.begin(), kMagic.end(), payload.begin())) {
        throw FormatError("external codec payload header is invalid");
    }
    std::size_t cursor = kMagic.size();
    if (payload[cursor++] != kPayloadVersion) {
        throw FormatError("unsupported external codec payload version");
    }
    const std::size_t property_size = payload[cursor++];
    if (property_size > kMaxPropertySize || read_u16(payload, cursor) != 0) {
        throw FormatError("external codec properties are invalid");
    }
    const std::size_t chunk_size = read_u32(payload, cursor);
    const std::size_t chunk_count = read_u32(payload, cursor);
    const std::size_t maximum_chunk = method == CompressionMethod::lzma2
        ? kMaxLzmaChunkSize : kMaxFastCodecChunkSize;
    if (chunk_size < kMinChunkSize || chunk_size > maximum_chunk ||
        cursor > payload.size() || property_size > payload.size() - cursor) {
        throw FormatError("external codec geometry is invalid");
    }
    const std::size_t expected_chunks =
        expected_size == 0 ? 0 : 1 + (expected_size - 1) / chunk_size;
    if (chunk_count != expected_chunks) {
        throw FormatError("external codec chunk count is invalid");
    }
    const auto properties = payload.subspan(cursor, property_size);
    cursor += property_size;
    if ((method == CompressionMethod::lzma2 && property_size != 1) ||
        (method != CompressionMethod::lzma2 && property_size != 0)) {
        throw FormatError("external codec properties do not match the codec");
    }
    if (method == CompressionMethod::lzma2) {
        validate_lzma2_property(properties.front(), chunk_size);
    }

    // Read every chunk record first: the geometry is validated before any chunk is
    // decoded, and the records say where each chunk's bytes go.
    struct ChunkJob {
        std::span<const std::uint8_t> encoded;
        std::size_t raw_size = 0;
        bool stored = false;
    };
    std::vector<ChunkJob> jobs;
    jobs.reserve(chunk_count);
    std::size_t planned = 0;
    for (std::size_t index = 0; index < chunk_count; ++index) {
        checkpoint(options.operation);
        const std::uint32_t raw_size = read_u32(payload, cursor);
        const std::uint32_t encoded_size = read_u32(payload, cursor);
        if (cursor > payload.size() || payload.size() - cursor < 4) {
            throw FormatError("external codec chunk header is truncated");
        }
        const std::uint8_t flags = payload[cursor++];
        if ((flags & ~kStoredChunk) != 0 ||
            payload[cursor++] != 0 || payload[cursor++] != 0 || payload[cursor++] != 0) {
            throw FormatError("external codec chunk flags are invalid");
        }
        const std::size_t remaining = expected_size - planned;
        const std::size_t required =
            std::min(chunk_size, remaining);
        if (raw_size != required || encoded_size > payload.size() - cursor) {
            throw FormatError("external codec chunk size is invalid");
        }
        const bool stored = (flags & kStoredChunk) != 0;
        if (stored && encoded_size != raw_size) {
            throw FormatError("stored external codec chunk has the wrong size");
        }
        jobs.push_back({payload.subspan(cursor, encoded_size), raw_size, stored});
        cursor += encoded_size;
        planned += raw_size;
    }
    if (cursor != payload.size() || planned != expected_size) {
        throw FormatError("external codec payload has trailing or missing data");
    }

    const auto decode_chunk = [&](const ChunkJob& job, std::uint8_t* destination) {
        checkpoint(options.operation);
        const std::span<std::uint8_t> output(destination, job.raw_size);
        if (job.stored) {
            std::copy(job.encoded.begin(), job.encoded.end(), output.begin());
        } else if (method == CompressionMethod::zstandard) {
            decode_zstandard_into(job.encoded, output);
        } else if (method == CompressionMethod::lzma2) {
            decode_lzma2_into(job.encoded, output, properties.front());
        } else if (method == CompressionMethod::deflate) {
            decode_deflate_into(job.encoded, output);
        } else {
            throw FormatError("unsupported external codec");
        }
    };

    // The output grows a few chunks at a time, never past the reserved capacity
    // (so the chunks being decoded do not move), and a payload that fails early
    // has only touched the memory it got to.
    ByteVector output;
    output.reserve(expected_size);
    std::size_t workers = options.thread_count == 0 ? core::logical_processor_count()
                                                    : options.thread_count;
    workers = std::min(std::max<std::size_t>(workers, 1), jobs.size());
    // Chunks of an LZMA2 payload each need a dictionary-sized working set.
    if (method == CompressionMethod::lzma2 && chunk_size != 0) {
        workers = std::min<std::size_t>(
            workers, std::max<std::size_t>(1, (std::size_t{3} << 30) / std::max<std::size_t>(chunk_size, 1)));
    }
    core::TaskExecutor* executor = core::TaskExecutor::current();
    std::optional<core::TaskExecutor> local_executor;
    if (workers > 1 && executor == nullptr) {
        local_executor.emplace(workers);
        executor = &*local_executor;
    }
    if (workers <= 1 || executor == nullptr) {
        for (const auto& job : jobs) {
            const std::size_t begin = output.size();
            output.resize(begin + job.raw_size);
            decode_chunk(job, output.data() + begin);
            if (options.decoded_bytes_progress) {
                options.decoded_bytes_progress(output.size(), expected_size);
            }
        }
    } else {
        for (std::size_t first = 0; first < jobs.size(); first += workers) {
            const std::size_t last = std::min(first + workers, jobs.size());
            const std::size_t base = output.size();
            std::size_t wave_bytes = 0;
            for (std::size_t i = first; i < last; ++i) wave_bytes += jobs[i].raw_size;
            output.resize(base + wave_bytes);
            std::vector<std::future<void>> tasks;
            tasks.reserve(last - first);
            try {
                std::size_t position = base + jobs[first].raw_size;
                for (std::size_t i = first + 1; i < last; ++i) {
                    const std::size_t at = position;
                    position += jobs[i].raw_size;
                    tasks.push_back(executor->submit(
                        [&, i, at] { decode_chunk(jobs[i], output.data() + at); }));
                }
                decode_chunk(jobs[first], output.data() + base);
                for (auto& task : tasks) executor->wait(task);
            } catch (...) {
                // The tasks write into `output`: let each finish before unwinding.
                for (auto& task : tasks) {
                    try {
                        if (task.valid()) executor->wait(task);
                    } catch (...) {
                    }
                }
                throw;
            }
            if (options.decoded_bytes_progress) {
                options.decoded_bytes_progress(output.size(), expected_size);
            }
        }
    }
    return output;
}

std::vector<ExternalCodecFrame> inspect_external_codec_frames(
    std::span<const std::uint8_t> payload,
    CompressionMethod method,
    std::size_t expected_size) {
    if (method != CompressionMethod::zstandard &&
        method != CompressionMethod::lzma2 &&
        method != CompressionMethod::deflate) {
        throw FormatError("method is not an external AXC codec");
    }
    if (payload.size() < kHeaderSize ||
        !std::equal(kMagic.begin(), kMagic.end(), payload.begin())) {
        throw FormatError("external codec payload header is invalid");
    }
    std::size_t cursor = kMagic.size();
    if (payload[cursor++] != kPayloadVersion) {
        throw FormatError("unsupported external codec payload version");
    }
    const auto property_size = static_cast<std::size_t>(payload[cursor++]);
    if (property_size > kMaxPropertySize || read_u16(payload, cursor) != 0) {
        throw FormatError("external codec properties are invalid");
    }
    const auto chunk_size = static_cast<std::size_t>(read_u32(payload, cursor));
    const auto chunk_count = static_cast<std::size_t>(read_u32(payload, cursor));
    const auto maximum_chunk = method == CompressionMethod::lzma2
        ? kMaxLzmaChunkSize : kMaxFastCodecChunkSize;
    if (chunk_size < kMinChunkSize || chunk_size > maximum_chunk ||
        property_size > payload.size() - cursor) {
        throw FormatError("external codec geometry is invalid");
    }
    const auto expected_chunks = expected_size == 0
        ? std::size_t{0} : 1 + (expected_size - 1) / chunk_size;
    if (chunk_count != expected_chunks) {
        throw FormatError("external codec chunk count is invalid");
    }
    const auto properties = payload.subspan(cursor, property_size);
    cursor += property_size;
    if ((method == CompressionMethod::lzma2 && property_size != 1) ||
        (method != CompressionMethod::lzma2 && property_size != 0)) {
        throw FormatError("external codec properties do not match the codec");
    }
    std::uint8_t lzma_property = 0;
    if (method == CompressionMethod::lzma2) {
        lzma_property = properties.front();
        validate_lzma2_property(lzma_property, chunk_size);
    }

    std::vector<ExternalCodecFrame> frames;
    frames.reserve(chunk_count);
    std::size_t uncompressed_offset = 0;
    for (std::size_t index = 0; index < chunk_count; ++index) {
        const auto frame_offset = cursor;
        const auto raw_size = static_cast<std::size_t>(read_u32(payload, cursor));
        const auto encoded_size = static_cast<std::size_t>(read_u32(payload, cursor));
        if (payload.size() - cursor < 4) {
            throw FormatError("external codec chunk header is truncated");
        }
        const auto flags = payload[cursor++];
        if ((flags & ~kStoredChunk) != 0 || payload[cursor++] != 0 ||
            payload[cursor++] != 0 || payload[cursor++] != 0) {
            throw FormatError("external codec chunk flags are invalid");
        }
        if (uncompressed_offset > expected_size ||
            raw_size != std::min(chunk_size, expected_size - uncompressed_offset) ||
            encoded_size > payload.size() - cursor) {
            throw FormatError("external codec chunk size is invalid");
        }
        const auto payload_offset = cursor;
        cursor += encoded_size;
        frames.push_back({uncompressed_offset,
                          raw_size,
                          frame_offset,
                          cursor - frame_offset,
                          payload_offset,
                          encoded_size,
                          (flags & kStoredChunk) != 0,
                          lzma_property});
        uncompressed_offset += raw_size;
    }
    if (cursor != payload.size() || uncompressed_offset != expected_size) {
        throw FormatError("external codec payload has trailing or missing data");
    }
    return frames;
}

ByteVector decode_external_codec_frame(std::span<const std::uint8_t> frame,
                                       CompressionMethod method,
                                       std::size_t expected_size,
                                       std::uint8_t lzma_property) {
    std::size_t cursor = 0;
    const auto raw_size = static_cast<std::size_t>(read_u32(frame, cursor));
    const auto encoded_size = static_cast<std::size_t>(read_u32(frame, cursor));
    if (frame.size() - cursor < 4) {
        throw FormatError("external codec chunk header is truncated");
    }
    const auto flags = frame[cursor++];
    if ((flags & ~kStoredChunk) != 0 || frame[cursor++] != 0 ||
        frame[cursor++] != 0 || frame[cursor++] != 0 ||
        encoded_size > frame.size() - cursor ||
        cursor + encoded_size != frame.size() || raw_size != expected_size) {
        throw FormatError("external codec chunk does not match its subframe map");
    }
    const auto encoded = frame.subspan(cursor, encoded_size);
    if ((flags & kStoredChunk) != 0) {
        if (encoded_size != expected_size) {
            throw FormatError("stored external codec chunk has the wrong size");
        }
        return ByteVector(encoded.begin(), encoded.end());
    }
    if (method == CompressionMethod::zstandard) {
        return decode_zstandard(encoded, expected_size);
    }
    if (method == CompressionMethod::lzma2) {
        // The enclosing AXEC header carries the independently decoded chunk
        // bound. A final short frame may legitimately use a dictionary larger
        // than its own raw size, so validate against the format ceiling here;
        // the complete-payload path validates the declared chunk geometry.
        validate_lzma2_property(lzma_property, kMaxLzmaChunkSize);
        return decode_lzma2(encoded, expected_size, lzma_property);
    }
    if (method == CompressionMethod::deflate) {
        return decode_deflate(encoded, expected_size);
    }
    throw FormatError("unsupported external codec");
}

}  // namespace axiom::codec
