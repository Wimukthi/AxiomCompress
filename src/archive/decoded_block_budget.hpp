#pragma once

#include <cstdint>

namespace axiom::detail {

// Budget resident decoded bytes, including reservations for unfinished jobs.
// One block may exceed the limit only when it is the sole resident/in-flight
// block. Compressed input and codec workspace are separate allocations.
inline bool decoded_block_fits(std::uint64_t limit, std::uint64_t used,
                               std::uint64_t charge) noexcept {
    return used == 0 || (charge <= limit && used <= limit - charge);
}

}  // namespace axiom::detail
