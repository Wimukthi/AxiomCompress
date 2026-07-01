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
                                  std::uint32_t* crc32 = nullptr);

std::size_t effective_parallel_block_size(std::size_t input_size,
                                          const CompressionOptions& options);

std::size_t effective_thread_count(std::size_t requested_threads, std::size_t work_items);

}  // namespace axiom::codec
