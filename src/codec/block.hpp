#pragma once

#include "axiom/axiom.hpp"

namespace axiom::codec {

ByteVector encode_parallel_blocks(std::span<const std::uint8_t> input,
                                  const CompressionOptions& options,
                                  std::uint32_t* crc32 = nullptr);

ByteVector decode_parallel_blocks(std::span<const std::uint8_t> encoded,
                                  std::size_t output_size,
                                  std::size_t thread_count,
                                  const std::shared_ptr<OperationControl>& operation = nullptr,
                                  const std::function<void(std::uint64_t)>& decoded_bytes_progress = {},
                                  std::uint32_t* crc32 = nullptr,
                                  bool allow_sequence_codec = true,
                                  bool allow_context_split_codec = true);

std::size_t effective_parallel_block_size(std::size_t input_size,
                                          const CompressionOptions& options);

std::size_t effective_thread_count(std::size_t requested_threads, std::size_t work_items);

// Worker count for compression: like effective_thread_count, but an
// unspecified request (0) maps to the physical core count instead of the
// logical processor count. Compression is memory-bound enough that SMT
// siblings add contention, not throughput (measured flat-to-negative
// scaling past the physical core count). Decode keeps the logical count.
std::size_t effective_compression_thread_count(std::size_t requested_threads,
                                               std::size_t work_items);

}  // namespace axiom::codec
