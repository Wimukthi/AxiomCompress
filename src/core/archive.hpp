#pragma once

#include "axiom/axiom.hpp"

namespace axiom::core {

enum class CodecId : std::uint8_t {
    store = 0,
    greedy_lz77 = 1,
    greedy_lz77_huffman = 2,
    parallel_blocks = 3,
    greedy_lz77_split = 4,
    greedy_lz77_split_slots = 5,
};

struct ArchiveHeader {
    CodecId codec = CodecId::store;
    std::uint64_t original_size = 0;
    std::uint64_t payload_size = 0;
    std::uint32_t crc32 = 0;
};

ByteVector write_archive(std::span<const std::uint8_t> payload,
                         const ArchiveHeader& header);

ArchiveHeader read_archive_header(std::span<const std::uint8_t> archive);

std::span<const std::uint8_t> archive_payload(std::span<const std::uint8_t> archive,
                                              const ArchiveHeader& header);

}  // namespace axiom::core
