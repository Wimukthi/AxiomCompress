#include "codec/external_codecs.hpp"

#include "third_party/lzma-sdk/Lzma2Dec.h"
#include "third_party/miniz/miniz.h"
#include "third_party/zstd/lib/zstd.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

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

void checkpoint(const std::shared_ptr<OperationControl>& operation) {
    if (operation) operation->checkpoint();
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
    if (context == nullptr) throw std::bad_alloc();

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

ByteVector decode_deflate(std::span<const std::uint8_t> input,
                          std::size_t expected_size) {
    if (input.size() > std::numeric_limits<mz_ulong>::max() ||
        expected_size > std::numeric_limits<mz_ulong>::max()) {
        throw FormatError("Deflate chunk exceeds the codec size limit");
    }
    ByteVector output(expected_size);
    mz_ulong output_size = static_cast<mz_ulong>(expected_size);
    mz_ulong input_size = static_cast<mz_ulong>(input.size());
    const int result = mz_uncompress2(output.data(), &output_size, input.data(),
                                      &input_size);
    if (result != MZ_OK || output_size != expected_size ||
        input_size != input.size()) {
        throw FormatError("Deflate chunk does not match its declared size");
    }
    return output;
}

void* lzma_alloc(ISzAllocPtr, size_t size) { return std::malloc(size); }
void lzma_free(ISzAllocPtr, void* address) { std::free(address); }

const ISzAlloc kLzmaAllocator{lzma_alloc, lzma_free};

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
    if (result != SZ_OK || output_size != expected_size ||
        input_size != input.size() || status != LZMA_STATUS_FINISHED_WITH_MARK) {
        throw FormatError("LZMA2 chunk does not match its declared size");
    }
    return output;
}

}  // namespace

// The shared AXC archive translation unit also contains the compression API.
// A decode-only link must still satisfy its unused reference, but reaching this
// path from an SFX runtime is a programming error rather than a fallback mode.
ByteVector encode_external_codec(std::span<const std::uint8_t>,
                                 CompressionMethod,
                                 const CompressionOptions&) {
    throw std::runtime_error("external compression is unavailable in the SFX decode runtime");
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
        if (property > 40) throw FormatError("LZMA2 dictionary property is invalid");
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
        if ((flags & ~kStoredChunk) != 0 || payload[cursor++] != 0 ||
            payload[cursor++] != 0 || payload[cursor++] != 0) {
            throw FormatError("external codec chunk flags are invalid");
        }
        const std::size_t remaining = expected_size - output.size();
        const std::size_t required = std::min(chunk_size, remaining);
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
        if (lzma_property > 40) {
            throw FormatError("LZMA2 dictionary property is invalid");
        }
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
        if (lzma_property > 40) {
            throw FormatError("LZMA2 dictionary property is invalid");
        }
        return decode_lzma2(encoded, expected_size, lzma_property);
    }
    if (method == CompressionMethod::deflate) {
        return decode_deflate(encoded, expected_size);
    }
    throw FormatError("unsupported external codec");
}

}  // namespace axiom::codec
