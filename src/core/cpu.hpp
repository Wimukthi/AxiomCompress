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

// Physical core count (SMT siblings collapsed), detected once and cached.
// Compression workers scale with cores, not logical processors: the codec is
// memory-bound enough that hyperthread siblings add contention instead of
// throughput. Falls back to the logical count when the OS query fails.
std::size_t physical_core_count();

// Human-readable summary of the active features, e.g. "sse4.1 sse4.2 pclmul avx2"
// or "scalar" when none are present. Useful for diagnostics.
const char* cpu_features_string();

}  // namespace axiom::core
