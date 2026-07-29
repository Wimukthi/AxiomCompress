#include "codec/external_codecs.hpp"

#include "third_party/lzma-sdk/Lzma2Dec.h"
#include "third_party/lzma-sdk/Lzma2Enc.h"
#include "third_party/miniz/miniz.h"
#include "third_party/zstd/lib/zstd.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
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
constexpr std::size_t kMaxLzmaChunkSize = std::size_t{512} << 20;
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

ByteVector decode_zstandard(std::span<const std::uint8_t> input,
                           std::size_t expected_size) {
    const std::size_t frame_size =
        ZSTD_findFrameCompressedSize(input.data(), input.size());
    if (ZSTD_isError(frame_size) || frame_size != input.size()) {
        throw FormatError("Zstandard chunk is truncated or has trailing data");
    }
    ByteVector output(expected_size);
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

ByteVector decode_deflate(std::span<const std::uint8_t> input,
                         std::size_t expected_size) {
    if (input.size() > std::numeric_limits<mz_ulong>::max() ||
        expected_size > std::numeric_limits<mz_ulong>::max()) {
        throw FormatError("Deflate chunk exceeds the codec size limit");
    }
    ByteVector output(expected_size);
    mz_ulong output_size = static_cast<mz_ulong>(expected_size);
    mz_ulong input_size = static_cast<mz_ulong>(input.size());
    const int result = mz_uncompress2(output.data(), &output_size, input.data(), &input_size);
    if (result != MZ_OK || output_size != expected_size || input_size != input.size()) {
        throw FormatError("Deflate chunk does not match its declared size");
    }
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

std::pair<ByteVector, std::uint8_t> encode_lzma2(
    std::span<const std::uint8_t> input,
    const CompressionOptions& options,
    std::size_t dictionary_limit) {
    CLzma2EncHandle encoder = Lzma2Enc_Create(&kLzmaAllocator, &kLzmaAllocator);
    if (encoder == nullptr) {
        throw std::bad_alloc();
    }

    try {
        CLzma2EncProps properties;
        Lzma2EncProps_Init(&properties);
        const int level = effective_level(CompressionMethod::lzma2, options);
        properties.lzmaProps.level = level;
        const std::size_t dictionary = options.lzma_dictionary_size == 0
            ? default_lzma_dictionary_size(level)
            : options.lzma_dictionary_size;
        properties.lzmaProps.dictSize = static_cast<UInt32>(std::clamp<std::size_t>(
            dictionary, std::size_t{1} << 12,
            std::max(dictionary_limit, std::size_t{1} << 12)));
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

ByteVector decode_lzma2(std::span<const std::uint8_t> input,
                       std::size_t expected_size,
                       std::uint8_t property) {
    ByteVector output(expected_size);
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
    return output;
}

EncodedChunk make_chunk(std::span<const std::uint8_t> input,
                        CompressionMethod method,
                        const CompressionOptions& options,
                        std::size_t dictionary_limit,
                        std::vector<std::uint8_t>& properties) {
    EncodedChunk chunk;
    chunk.raw_size = static_cast<std::uint32_t>(input.size());
    if (method == CompressionMethod::zstandard) {
        chunk.bytes = encode_zstandard(
            input, effective_level(CompressionMethod::zstandard, options));
    } else if (method == CompressionMethod::lzma2) {
        auto [bytes, property] =
            encode_lzma2(input, options, dictionary_limit);
        if (properties.empty()) {
            properties.push_back(property);
        } else if (properties.front() != property) {
            throw std::runtime_error("LZMA2 produced inconsistent stream properties");
        }
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
    return chunk;
}

}  // namespace

ByteVector encode_external_codec(std::span<const std::uint8_t> input,
                                 CompressionMethod method,
                                 const CompressionOptions& options) {
    if (method != CompressionMethod::zstandard &&
        method != CompressionMethod::lzma2 &&
        method != CompressionMethod::deflate) {
        throw std::invalid_argument("method is not an external AXC codec");
    }

    const std::size_t maximum_chunk = method == CompressionMethod::lzma2
        ? kMaxLzmaChunkSize : kMaxFastCodecChunkSize;
    const std::size_t requested = options.block_size == 0
        ? maximum_chunk : options.block_size;
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
    for (std::size_t index = 0; index < chunk_count; ++index) {
        checkpoint(options.operation);
        const std::size_t offset = index * chunk_size;
        const std::size_t size = std::min(chunk_size, input.size() - offset);
        chunks.push_back(make_chunk(
            input.subspan(offset, size), method, options, chunk_size,
            properties));
        report_encoded(options, static_cast<std::uint64_t>(offset + size));
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
        const std::uint8_t property = properties.front();
        if (property > 40) {
            throw FormatError("LZMA2 dictionary property is invalid");
        }
        const std::uint64_t dictionary_size = property == 40
            ? std::numeric_limits<std::uint32_t>::max()
            : (static_cast<std::uint64_t>(2u | (property & 1u))
               << (property / 2u + 11u));
        if (dictionary_size > std::max(chunk_size, std::size_t{1} << 12)) {
            throw FormatError("LZMA2 dictionary exceeds the chunk memory bound");
        }
    }

    ByteVector output;
    output.reserve(expected_size);
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
        const std::size_t remaining = expected_size - output.size();
        const std::size_t required =
            std::min(chunk_size, remaining);
        if (raw_size != required || encoded_size > payload.size() - cursor) {
            throw FormatError("external codec chunk size is invalid");
        }
        const auto encoded = payload.subspan(cursor, encoded_size);
        cursor += encoded_size;

        ByteVector decoded;
        if ((flags & kStoredChunk) != 0) {
            if (encoded_size != raw_size) {
                throw FormatError("stored external codec chunk has the wrong size");
            }
            decoded.assign(encoded.begin(), encoded.end());
        } else if (method == CompressionMethod::zstandard) {
            decoded = decode_zstandard(encoded, raw_size);
        } else if (method == CompressionMethod::lzma2) {
            decoded = decode_lzma2(encoded, raw_size, properties.front());
        } else if (method == CompressionMethod::deflate) {
            decoded = decode_deflate(encoded, raw_size);
        } else {
            throw FormatError("unsupported external codec");
        }
        output.insert(output.end(), decoded.begin(), decoded.end());
        if (options.decoded_bytes_progress) {
            options.decoded_bytes_progress(output.size(), expected_size);
        }
    }
    if (cursor != payload.size() || output.size() != expected_size) {
        throw FormatError("external codec payload has trailing or missing data");
    }
    return output;
}

}  // namespace axiom::codec
