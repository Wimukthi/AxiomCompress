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
                                  bool allow_context_split_codec = true,
                                  bool allow_contextual_footer_codec = true,
                                  bool allow_checkpoint_codec = true);

std::size_t effective_parallel_block_size(std::size_t input_size,
                                          const CompressionOptions& options);

std::size_t effective_thread_count(std::size_t requested_threads, std::size_t work_items);

// Worker budget for compression. Automatic mode exposes every logical
// processor to the shared executor; ratio-oriented block planning is computed
// separately, so SMT helpers can run nested parser/entropy tasks without
// forcing smaller block boundaries.
std::size_t effective_compression_thread_count(std::size_t requested_threads,
                                               std::size_t work_items);

}  // namespace axiom::codec
