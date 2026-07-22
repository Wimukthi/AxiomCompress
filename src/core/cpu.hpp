#pragma once

#include <cstddef>

namespace axiom::core {

// Runtime CPU instruction-set features, detected once via CPUID on x86. On
// non-x86 targets every field is false, so callers fall back to the portable
// scalar path. AVX2 implies the OS also saves the wide vector state (verified
// through OSXSAVE + XGETBV), so a true here is safe to act on.
struct CpuFeatures {
    bool sse41 = false;
    bool sse42 = false;
    bool pclmul = false;
    bool popcnt = false;
    bool avx2 = false;
};

// Detected once and cached; cheap to call repeatedly.
const CpuFeatures& cpu_features();

// Logical processors visible across the machine. On Windows this sums every
// processor group instead of relying on hardware_concurrency's group-local
// result. Falls back to one when topology discovery is unavailable.
std::size_t logical_processor_count();

// Spread a long-lived worker across Windows processor groups when the machine
// has more logical processors than one group can address. It is a no-op on
// single-group Windows systems and other platforms.
void bind_current_thread_to_processor_group(std::size_t worker_index);

// Physical core count (SMT siblings collapsed), detected once and cached.
// Compression block geometry scales with cores while the executor retains the
// full logical count for nested work. Falls back to the logical count when the
// OS query fails.
std::size_t physical_core_count();

// Human-readable summary of the active features, e.g. "sse4.1 sse4.2 pclmul avx2"
// or "scalar" when none are present. Useful for diagnostics.
const char* cpu_features_string();

}  // namespace axiom::core
