#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace axiom::core {

inline constexpr unsigned kBenchmarkProtocolVersion = 3;

struct BenchmarkSample {
    std::uint64_t bytes = 0;
    double wall_seconds = 0.0;
    double cpu_seconds = 0.0;
};

struct BenchmarkSummary {
    std::size_t sample_count = 0;
    double median_bytes_per_second = 0.0;
    double low_bytes_per_second = 0.0;
    double high_bytes_per_second = 0.0;
    double robust_spread_percent = 0.0;
    double median_active_cores = 0.0;
    double median_bytes_per_cpu_second = 0.0;
};

double benchmark_sample_throughput(const BenchmarkSample& sample);
BenchmarkSummary summarize_benchmark_samples(
    std::span<const BenchmarkSample> samples);
bool benchmark_samples_stable(
    std::span<const BenchmarkSample> samples,
    std::size_t minimum_samples = 5,
    double maximum_spread_percent = 2.0);
std::size_t calibrated_benchmark_iterations(
    double single_iteration_seconds, double target_seconds,
    std::size_t maximum_iterations);

}  // namespace axiom::core
