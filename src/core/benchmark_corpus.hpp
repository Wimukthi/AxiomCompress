#pragma once

#include "axiom/axiom.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace axiom::core {

enum class BenchmarkCorpusKind {
    lz_synthetic,
    structured_text,
    random,
};

struct BenchmarkCorpusOptions {
    BenchmarkCorpusKind kind = BenchmarkCorpusKind::lz_synthetic;
    std::size_t size = 0;
    std::size_t window_size = 0;
    std::size_t segment_size = 0;
    std::uint64_t seed = 0xA710'B2C3'D4E5'F607ull;
};

using BenchmarkCorpusProgress = std::function<void(std::size_t completed)>;

inline constexpr unsigned kBenchmarkCorpusVersion = 2;
inline constexpr std::uint64_t kBenchmarkCorpusDefaultSeed =
    0xA710'B2C3'D4E5'F607ull;

// Generates a deterministic, versioned corpus. The optional callback runs at
// bounded intervals so GUI callers can report progress and observe cancellation.
ByteVector generate_benchmark_corpus(
    const BenchmarkCorpusOptions& options,
    const BenchmarkCorpusProgress& progress = {});

}  // namespace axiom::core
