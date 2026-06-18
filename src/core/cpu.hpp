#pragma once

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

// Human-readable summary of the active features, e.g. "sse4.1 sse4.2 pclmul avx2"
// or "scalar" when none are present. Useful for diagnostics.
const char* cpu_features_string();

}  // namespace axiom::core
