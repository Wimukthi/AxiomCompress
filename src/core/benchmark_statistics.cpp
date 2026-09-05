#include "core/benchmark_statistics.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace axiom::core {
namespace {

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    if ((values.size() & 1u) != 0) return values[middle];
    return (values[middle - 1] + values[middle]) / 2.0;
}

double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) return 0.0;
    if (sorted.size() == 1) return sorted.front();
    const double index = fraction * static_cast<double>(sorted.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(index);
    const std::size_t upper = (std::min)(lower + 1, sorted.size() - 1);
    const double weight = index - static_cast<double>(lower);
    return sorted[lower] + (sorted[upper] - sorted[lower]) * weight;
}

}  // namespace

double benchmark_sample_throughput(const BenchmarkSample& sample) {
    if (sample.bytes == 0 || sample.wall_seconds <= 0.0 ||
        !std::isfinite(sample.wall_seconds)) {
        return 0.0;
    }
    return static_cast<double>(sample.bytes) / sample.wall_seconds;
}

BenchmarkSummary summarize_benchmark_samples(
    std::span<const BenchmarkSample> samples) {
    BenchmarkSummary result;
    std::vector<double> throughputs;
    std::vector<double> active_cores;
    std::vector<double> per_cpu_second;
    throughputs.reserve(samples.size());
    active_cores.reserve(samples.size());
    per_cpu_second.reserve(samples.size());
    for (const BenchmarkSample& sample : samples) {
        const double throughput = benchmark_sample_throughput(sample);
        if (throughput <= 0.0 || !std::isfinite(throughput)) continue;
        throughputs.push_back(throughput);
        if (sample.cpu_seconds > 0.0 && std::isfinite(sample.cpu_seconds)) {
            active_cores.push_back(sample.cpu_seconds / sample.wall_seconds);
            per_cpu_second.push_back(
                static_cast<double>(sample.bytes) / sample.cpu_seconds);
        }
    }
    result.sample_count = throughputs.size();
    if (throughputs.empty()) return result;

    std::sort(throughputs.begin(), throughputs.end());
    result.median_bytes_per_second = median(throughputs);
    result.low_bytes_per_second = percentile(throughputs, 0.10);
    result.high_bytes_per_second = percentile(throughputs, 0.90);
    result.median_active_cores = median(std::move(active_cores));
    result.median_bytes_per_cpu_second = median(std::move(per_cpu_second));

    std::vector<double> deviations;
    deviations.reserve(throughputs.size());
    for (const double throughput : throughputs) {
        deviations.push_back(std::abs(throughput - result.median_bytes_per_second));
    }
    const double absolute_deviation = median(std::move(deviations));
    if (result.median_bytes_per_second > 0.0) {
        // Scale MAD to the standard-deviation equivalent for normally
        // distributed noise while retaining resistance to a single bad pass.
        result.robust_spread_percent =
            1.4826 * absolute_deviation / result.median_bytes_per_second * 100.0;
    }
    return result;
}

bool benchmark_samples_stable(
    std::span<const BenchmarkSample> samples, std::size_t minimum_samples,
    double maximum_spread_percent) {
    const BenchmarkSummary summary = summarize_benchmark_samples(samples);
    return summary.sample_count >= minimum_samples &&
           summary.robust_spread_percent <= maximum_spread_percent;
}

std::size_t calibrated_benchmark_iterations(
    double single_iteration_seconds, double target_seconds,
    std::size_t maximum_iterations) {
    if (maximum_iterations == 0) return 0;
    if (single_iteration_seconds <= 0.0 || !std::isfinite(single_iteration_seconds) ||
        target_seconds <= 0.0 || !std::isfinite(target_seconds)) {
        return 1;
    }
    const double required = std::ceil(target_seconds / single_iteration_seconds);
    if (required <= 1.0) return 1;
    if (required >= static_cast<double>(maximum_iterations)) {
        return maximum_iterations;
    }
    return static_cast<std::size_t>(required);
}

}  // namespace axiom::core
