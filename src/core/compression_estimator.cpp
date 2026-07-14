#include "axiom/archive.hpp"

#include "core/path_text.hpp"
#include "third_party/miniz/miniz.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>

namespace axiom {
namespace fs = std::filesystem;
namespace {

struct EstimateFile {
    fs::path path;
    std::string display_path;
    std::uint64_t size = 0;
};

struct SampleRegion {
    std::size_t file_index = 0;
    std::uint64_t offset = 0;
    std::size_t size = 0;
};

struct SampleObservation {
    double packed_ratio = 0.0;
    std::uint64_t bytes = 0;
};

struct SampleStatistics {
    double mean = 1.0;
    double margin = 0.35;
    double effective_samples = 0.0;
};

constexpr std::size_t kAdaptiveBatchProbes = 8;
constexpr std::size_t kMinimumRepresentativeProbes = 16;
constexpr double kHighConfidenceMargin = 0.025;
constexpr double kMediumConfidenceMargin = 0.05;
constexpr double kStableEstimateDelta = 0.01;

std::uint64_t saturated_add(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

std::uint64_t saturated_multiply(std::uint64_t left, std::uint64_t right) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left * right;
}

std::uint64_t clamp_u64(long double value) {
    if (!(value > 0.0L)) return 0;
    const auto maximum = static_cast<long double>(
        std::numeric_limits<std::uint64_t>::max());
    if (value >= maximum) return std::numeric_limits<std::uint64_t>::max();
    return static_cast<std::uint64_t>(value + 0.5L);
}

long double radical_inverse_base_two(std::size_t value) {
    long double result = 0.0L;
    long double place = 0.5L;
    while (value != 0) {
        if ((value & 1u) != 0) result += place;
        value >>= 1u;
        place *= 0.5L;
    }
    return result;
}

SampleStatistics calculate_sample_statistics(
    const std::vector<SampleObservation>& observations) {
    SampleStatistics result;
    long double total_weight = 0.0L;
    long double squared_weight = 0.0L;
    long double weighted_sum = 0.0L;
    for (const auto& observation : observations) {
        const long double weight = static_cast<long double>(observation.bytes);
        total_weight += weight;
        squared_weight += weight * weight;
        weighted_sum += weight * observation.packed_ratio;
    }
    if (!(total_weight > 0.0L)) return result;
    result.mean = static_cast<double>(weighted_sum / total_weight);

    long double variance = 0.0L;
    for (const auto& observation : observations) {
        const long double delta = observation.packed_ratio - result.mean;
        variance += observation.bytes * delta * delta;
    }
    variance /= total_weight;
    const double deviation = std::sqrt(static_cast<double>(variance));
    if (squared_weight > 0.0L) {
        result.effective_samples = static_cast<double>(
            total_weight * total_weight / squared_weight);
    }
    if (result.effective_samples > 1.0) {
        // A conservative 95% interval with a small model-error floor. The
        // extra inflation accounts for nearby regions not being independent.
        result.margin = (std::max)(
            0.015, 2.5 * deviation / std::sqrt(result.effective_samples));
    }
    return result;
}

std::uint64_t adaptive_minimum_sample_bytes(std::uint64_t source_bytes,
                                            std::uint64_t file_count,
                                            std::size_t chunk_size,
                                            std::size_t maximum_budget) {
    if (source_bytes <= maximum_budget) return source_bytes;
    std::uint64_t minimum = 4u << 20;
    if (source_bytes >= (1ull << 30)) minimum = 8u << 20;
    if (source_bytes >= (16ull << 30)) minimum = 12u << 20;
    if (file_count >= 10'000) minimum += 2u << 20;
    if (file_count >= 100'000) minimum += 2u << 20;
    minimum = (std::max)(minimum, saturated_multiply(
        kMinimumRepresentativeProbes, chunk_size));
    return (std::min<std::uint64_t>)(minimum, maximum_budget);
}

void report_estimate_progress(const std::shared_ptr<OperationControl>& operation,
                              OperationStage stage,
                              std::uint64_t completed_bytes,
                              std::uint64_t total_bytes,
                              std::uint64_t completed_items,
                              std::uint64_t total_items,
                              std::string current_path = {},
                              std::uint64_t current_completed = 0,
                              std::uint64_t current_total = 0) {
    if (!operation) return;
    operation->report(OperationProgress{
        stage, completed_bytes, total_bytes, completed_items, total_items,
        std::move(current_path), current_completed, current_total});
}

void add_estimate_warning(std::vector<OperationWarning>& warnings,
                          const std::shared_ptr<OperationControl>& operation,
                          const fs::path& path,
                          std::string message) {
    OperationWarning warning{core::path_to_utf8(path), std::move(message)};
    warnings.push_back(warning);
    if (operation) operation->add_warning(std::move(warning));
}

void add_regular_file(const fs::path& path,
                      std::vector<EstimateFile>& files,
                      std::set<fs::path>& seen,
                      std::uint64_t& source_bytes,
                      std::uint64_t& item_count,
                      std::vector<OperationWarning>& warnings,
                      const std::shared_ptr<OperationControl>& operation) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error) {
        add_estimate_warning(warnings, operation, path,
                             "could not read file size: " + error.message());
        return;
    }
    const fs::path key = path.lexically_normal();
    if (!seen.insert(key).second) return;
    files.push_back({path, core::path_to_utf8(path), size});
    source_bytes = saturated_add(source_bytes, size);
    item_count = saturated_add(item_count, 1);
}

void scan_estimate_input(const fs::path& input,
                         std::vector<EstimateFile>& files,
                         std::set<fs::path>& seen,
                         std::uint64_t& source_bytes,
                         std::uint64_t& item_count,
                         std::vector<OperationWarning>& warnings,
                         const std::shared_ptr<OperationControl>& operation) {
    if (operation) operation->checkpoint();
    std::error_code error;
    const fs::file_status status = fs::symlink_status(input, error);
    if (error) {
        add_estimate_warning(warnings, operation, input,
                             "could not inspect input: " + error.message());
        return;
    }
    if (fs::is_regular_file(status)) {
        add_regular_file(input, files, seen, source_bytes, item_count,
                         warnings, operation);
        return;
    }
    if (!fs::is_directory(status)) {
        ++item_count;  // Links and special entries still consume directory metadata.
        return;
    }

    ++item_count;
    fs::recursive_directory_iterator iterator(
        input, fs::directory_options::skip_permission_denied, error);
    const fs::recursive_directory_iterator end;
    if (error) {
        add_estimate_warning(warnings, operation, input,
                             "could not enumerate directory: " + error.message());
        return;
    }
    std::uint64_t scanned = 0;
    while (iterator != end) {
        if (operation) operation->checkpoint();
        const fs::directory_entry entry = *iterator;
        std::error_code status_error;
        const fs::file_status entry_status = entry.symlink_status(status_error);
        if (status_error) {
            add_estimate_warning(warnings, operation, entry.path(),
                                 "could not inspect entry: " + status_error.message());
        } else if (fs::is_regular_file(entry_status)) {
            add_regular_file(entry.path(), files, seen, source_bytes, item_count,
                             warnings, operation);
        } else {
            item_count = saturated_add(item_count, 1);
        }
        ++scanned;
        if ((scanned & 0xffu) == 0) {
            report_estimate_progress(operation, OperationStage::scanning,
                                     0, 0, scanned, 0,
                                     core::path_to_utf8(entry.path()));
        }
        iterator.increment(error);
        if (error) {
            add_estimate_warning(warnings, operation, input,
                                 "directory enumeration skipped an entry: " +
                                     error.message());
            error.clear();
        }
    }
}

std::vector<SampleRegion> plan_sample_regions(const std::vector<EstimateFile>& files,
                                               std::uint64_t source_bytes,
                                               std::size_t budget,
                                               std::size_t chunk_size) {
    std::vector<SampleRegion> regions;
    if (files.empty() || source_bytes == 0 || budget == 0 || chunk_size == 0) {
        return regions;
    }

    const std::uint64_t target = std::min<std::uint64_t>(source_bytes, budget);
    if (source_bytes <= target) {
        for (std::size_t file_index = 0; file_index < files.size(); ++file_index) {
            const auto& file = files[file_index];
            for (std::uint64_t offset = 0; offset < file.size;) {
                const std::size_t size = static_cast<std::size_t>(
                    std::min<std::uint64_t>(chunk_size, file.size - offset));
                regions.push_back({file_index, offset, size});
                offset += size;
            }
        }
        return regions;
    }

    const std::size_t count = std::max<std::size_t>(
        1, static_cast<std::size_t>((target + chunk_size - 1) / chunk_size));
    std::vector<std::uint64_t> ends;
    ends.reserve(files.size());
    std::uint64_t cumulative = 0;
    for (const auto& file : files) {
        cumulative = saturated_add(cumulative, file.size);
        ends.push_back(cumulative);
    }

    std::set<std::tuple<std::size_t, std::uint64_t, std::size_t>> unique;
    for (std::size_t index = 0; index < count; ++index) {
        // Low-discrepancy ordering keeps every prefix spread across the whole
        // byte range, allowing adaptive sampling to stop after any batch.
        const long double fraction = radical_inverse_base_two(index + 1);
        const std::uint64_t global = std::min<std::uint64_t>(
            source_bytes - 1, static_cast<std::uint64_t>(fraction * source_bytes));
        const auto found = std::upper_bound(ends.begin(), ends.end(), global);
        const std::size_t file_index = static_cast<std::size_t>(found - ends.begin());
        if (file_index >= files.size() || files[file_index].size == 0) continue;
        const std::uint64_t file_begin = file_index == 0 ? 0 : ends[file_index - 1];
        const std::uint64_t local = global - file_begin;
        const std::size_t size = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunk_size, files[file_index].size));
        const std::uint64_t half = size / 2;
        const std::uint64_t offset = std::min<std::uint64_t>(
            local > half ? local - half : 0, files[file_index].size - size);
        if (unique.emplace(file_index, offset, size).second) {
            regions.push_back({file_index, offset, size});
        }
    }
    return regions;
}

bool read_sample_region(const EstimateFile& file,
                        const SampleRegion& region,
                        unsigned retries,
                        ByteVector& output,
                        const std::shared_ptr<OperationControl>& operation) {
    for (unsigned attempt = 0; attempt <= retries; ++attempt) {
        if (operation) operation->checkpoint();
        std::ifstream input(file.path, std::ios::binary);
        if (input) {
            input.seekg(static_cast<std::streamoff>(region.offset), std::ios::beg);
            output.assign(region.size, 0);
            if (input && region.size != 0) {
                input.read(reinterpret_cast<char*>(output.data()),
                           static_cast<std::streamsize>(region.size));
            }
            if (input && static_cast<std::size_t>(input.gcount()) == region.size) {
                return true;
            }
            output.clear();
        }
        if (attempt != retries) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    return false;
}

std::uint64_t compress_zip_probe(std::span<const std::uint8_t> input,
                                 const CompressionOptions& options) {
    const int level = options.force_store ? MZ_NO_COMPRESSION
        : (options.use_fast_lz || options.max_chain_depth <= 16 || options.fast_entropy)
            ? MZ_BEST_SPEED
        : (options.use_tree_matcher || options.max_chain_depth >= 256 ||
           options.enable_optimal_parser)
            ? MZ_BEST_COMPRESSION
            : MZ_DEFAULT_COMPRESSION;
    mz_ulong length = mz_compressBound(static_cast<mz_ulong>(input.size()));
    ByteVector compressed(static_cast<std::size_t>(length));
    const int status = mz_compress2(
        compressed.data(), &length, input.data(), static_cast<mz_ulong>(input.size()), level);
    if (status != MZ_OK) throw std::runtime_error("ZIP estimate compression failed");
    // mz_compress2 emits a zlib wrapper; ZIP stores the raw Deflate payload.
    return length > 6 ? static_cast<std::uint64_t>(length - 6) : length;
}

std::uint64_t estimate_container_overhead(const std::vector<EstimateFile>& files,
                                          std::uint64_t item_count,
                                          std::uint64_t source_bytes,
                                          const CompressionEstimateOptions& options) {
    std::uint64_t path_bytes = 0;
    for (const auto& file : files) {
        path_bytes = saturated_add(path_bytes, file.display_path.size());
    }
    std::uint64_t overhead = options.format == ArchiveFormat::zip ? 64 : 96;
    const std::uint64_t per_item = options.format == ArchiveFormat::zip ? 110 : 96;
    overhead = saturated_add(overhead, saturated_multiply(item_count, per_item));
    overhead = saturated_add(overhead, saturated_multiply(
        path_bytes, options.format == ArchiveFormat::zip ? 2 : 1));

    const std::uint64_t block_size = std::max<std::uint64_t>(
        1, options.compression.block_size);
    const std::uint64_t blocks = source_bytes == 0
        ? 0 : 1 + (source_bytes - 1) / block_size;
    overhead = saturated_add(overhead, saturated_multiply(blocks, 48));
    if (!options.compression.password.empty()) {
        overhead = saturated_add(overhead,
                                 saturated_add(128, saturated_multiply(blocks, 40)));
    }
    return overhead;
}

CompressionEstimateSnapshot make_estimate_snapshot(
    std::uint64_t source_bytes,
    std::uint64_t sampled_bytes,
    std::uint64_t planned_sample_bytes,
    std::uint64_t packed_sample_bytes,
    std::uint64_t overhead,
    unsigned recovery_percent,
    std::uint64_t completed_probes,
    std::uint64_t total_probes) {
    CompressionEstimateSnapshot snapshot;
    snapshot.source_bytes = source_bytes;
    snapshot.sampled_bytes = sampled_bytes;
    snapshot.planned_sample_bytes = planned_sample_bytes;
    snapshot.completed_probes = completed_probes;
    snapshot.total_probes = total_probes;
    if (source_bytes == 0) {
        snapshot.estimated_archive_bytes = overhead;
        return snapshot;
    }
    if (sampled_bytes == 0) return snapshot;
    const long double payload_ratio = static_cast<long double>(packed_sample_bytes) /
                                      sampled_bytes;
    snapshot.estimated_archive_bytes = saturated_add(
        clamp_u64(payload_ratio * source_bytes), overhead);
    if (recovery_percent != 0) {
        const long double recovery =
            static_cast<long double>(snapshot.estimated_archive_bytes) *
            recovery_percent / 100.0L;
        snapshot.estimated_archive_bytes = saturated_add(
            snapshot.estimated_archive_bytes, clamp_u64(recovery));
    }
    if (snapshot.estimated_archive_bytes != 0) {
        snapshot.estimated_ratio = static_cast<double>(source_bytes) /
                                   snapshot.estimated_archive_bytes;
        snapshot.estimated_savings_percent = 100.0 *
            (1.0 - static_cast<double>(snapshot.estimated_archive_bytes) /
                       static_cast<double>(source_bytes));
    }
    return snapshot;
}

}  // namespace

CompressionEstimateResult estimate_compression(
    const std::vector<std::filesystem::path>& inputs,
    const CompressionEstimateOptions& options) {
    if (options.format != ArchiveFormat::axar && options.format != ArchiveFormat::zip) {
        throw std::invalid_argument("compression estimates support AXAR and ZIP only");
    }
    if (inputs.empty()) throw std::invalid_argument("no inputs selected for estimation");
    if (options.sample_budget == 0 || options.sample_chunk_size == 0) {
        throw std::invalid_argument("compression estimate sample sizes must be positive");
    }

    CompressionEstimateResult result;
    result.format = options.format;
    const auto operation = options.compression.operation;
    report_estimate_progress(operation, OperationStage::scanning,
                             0, 0, 0, inputs.size());

    std::vector<EstimateFile> files;
    std::set<fs::path> seen;
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        scan_estimate_input(inputs[index], files, seen, result.source_bytes,
                            result.item_count, result.warnings, operation);
        report_estimate_progress(operation, OperationStage::scanning,
                                 0, 0, index + 1, inputs.size(),
                                 core::path_to_utf8(inputs[index]));
    }
    std::sort(files.begin(), files.end(), [](const EstimateFile& left,
                                             const EstimateFile& right) {
        return left.path.native() < right.path.native();
    });
    result.file_count = files.size();

    const auto regions = plan_sample_regions(
        files, result.source_bytes, options.sample_budget, options.sample_chunk_size);
    std::uint64_t planned_bytes = 0;
    for (const auto& region : regions) planned_bytes += region.size;
    report_estimate_progress(operation, OperationStage::estimating,
                             0, planned_bytes, 0, regions.size());

    std::uint64_t packed_sample_bytes = 0;
    std::vector<SampleObservation> observations;
    observations.reserve(regions.size());
    const std::uint64_t overhead = estimate_container_overhead(
        files, result.item_count, result.source_bytes, options);
    const std::uint64_t minimum_sample_bytes = adaptive_minimum_sample_bytes(
        result.source_bytes, result.file_count, options.sample_chunk_size,
        options.sample_budget);
    const auto sampling_started = std::chrono::steady_clock::now();
    double codec_seconds = 0.0;
    std::optional<double> previous_batch_mean;
    std::size_t stable_batches = 0;
    std::set<std::size_t> warned_files;
    auto probe_options = options.compression;
    probe_options.encode_progress = [operation](double) {
        if (operation) operation->checkpoint();
    };
    probe_options.encoded_bytes_progress = {};

    for (std::size_t index = 0; index < regions.size(); ++index) {
        if (operation) operation->checkpoint();
        const auto& region = regions[index];
        ByteVector probe;
        if (!read_sample_region(files[region.file_index], region,
                                options.compression.input_open_retries,
                                probe, operation)) {
            if (warned_files.insert(region.file_index).second) {
                add_estimate_warning(result.warnings, operation,
                                     files[region.file_index].path,
                                     "could not read sample data; estimate confidence reduced");
            }
            if (options.time_budget.count() > 0 &&
                std::chrono::steady_clock::now() - sampling_started >=
                    options.time_budget) {
                break;
            }
            continue;
        }
        const auto codec_started = std::chrono::steady_clock::now();
        const std::uint64_t packed = options.format == ArchiveFormat::zip
            ? compress_zip_probe(probe, probe_options)
            : static_cast<std::uint64_t>(compress(probe, probe_options).size());
        codec_seconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - codec_started).count();
        packed_sample_bytes = saturated_add(packed_sample_bytes, packed);
        result.sampled_bytes = saturated_add(result.sampled_bytes, probe.size());
        if (!probe.empty()) {
            observations.push_back({static_cast<double>(packed) / probe.size(),
                                    probe.size()});
        }
        report_estimate_progress(
            operation, OperationStage::estimating,
            result.sampled_bytes, planned_bytes, index + 1, regions.size(),
            files[region.file_index].display_path, probe.size(), probe.size());
        if (options.progress_callback) {
            options.progress_callback(make_estimate_snapshot(
                result.source_bytes, result.sampled_bytes, planned_bytes,
                packed_sample_bytes, overhead, options.compression.recovery_percent,
                index + 1, regions.size()));
        }

        const bool batch_complete = observations.size() % kAdaptiveBatchProbes == 0;
        if (batch_complete) {
            const SampleStatistics statistics = calculate_sample_statistics(observations);
            if (previous_batch_mean) {
                if (std::abs(statistics.mean - *previous_batch_mean) <=
                    kStableEstimateDelta) {
                    ++stable_batches;
                } else {
                    stable_batches = 0;
                }
            }
            previous_batch_mean = statistics.mean;
            const bool representative =
                observations.size() >= kMinimumRepresentativeProbes &&
                result.sampled_bytes >= minimum_sample_bytes;
            if (representative && stable_batches != 0 &&
                statistics.margin <= kHighConfidenceMargin &&
                result.warnings.empty()) {
                break;
            }
        }
        if (options.time_budget.count() > 0 &&
            std::chrono::steady_clock::now() - sampling_started >= options.time_budget) {
            break;
        }
    }
    const SampleStatistics statistics = calculate_sample_statistics(observations);

    if (result.source_bytes == 0) {
        result.estimated_archive_bytes = overhead;
        result.estimated_low_bytes = overhead;
        result.estimated_high_bytes = overhead;
        result.confidence = result.warnings.empty()
            ? EstimateConfidence::high : EstimateConfidence::low;
    } else if (result.sampled_bytes == 0) {
        result.estimated_archive_bytes = saturated_add(result.source_bytes, overhead);
        result.estimated_low_bytes = overhead;
        result.estimated_high_bytes = result.estimated_archive_bytes;
        result.confidence = EstimateConfidence::low;
    } else {
        result.sample_coverage = static_cast<double>(result.sampled_bytes) /
                                 static_cast<double>(result.source_bytes);
        const long double payload_ratio = static_cast<long double>(packed_sample_bytes) /
                                          result.sampled_bytes;
        std::uint64_t projected_payload = clamp_u64(payload_ratio * result.source_bytes);
        result.estimated_archive_bytes = saturated_add(projected_payload, overhead);

        if (options.compression.recovery_percent != 0) {
            const long double recovery =
                static_cast<long double>(result.estimated_archive_bytes) *
                options.compression.recovery_percent / 100.0L;
            result.estimated_archive_bytes = saturated_add(
                result.estimated_archive_bytes, clamp_u64(recovery));
        }

        const bool fully_sampled = result.sample_coverage >= 0.999;
        result.confidence_margin_percent = fully_sampled
            ? 0.0 : statistics.margin * 100.0;
        double margin = 0.08;
        if (!fully_sampled) {
            const long double payload_margin =
                statistics.margin * static_cast<long double>(result.source_bytes);
            margin = result.estimated_archive_bytes == 0 ? 0.35
                : static_cast<double>(payload_margin / result.estimated_archive_bytes);
            margin = std::clamp(margin, 0.03, 0.35);
        }
        if (!result.warnings.empty()) margin = std::max(margin, 0.25);
        result.estimated_low_bytes = clamp_u64(
            static_cast<long double>(result.estimated_archive_bytes) * (1.0 - margin));
        result.estimated_high_bytes = clamp_u64(
            static_cast<long double>(result.estimated_archive_bytes) * (1.0 + margin));

        const bool representative =
            observations.size() >= kMinimumRepresentativeProbes &&
            result.sampled_bytes >= minimum_sample_bytes;
        if (result.warnings.empty() &&
            (fully_sampled ||
             (representative && stable_batches != 0 &&
              statistics.margin <= kHighConfidenceMargin))) {
            result.confidence = EstimateConfidence::high;
        } else if (observations.size() >= kAdaptiveBatchProbes &&
                   statistics.margin <= kMediumConfidenceMargin &&
                   result.warnings.size() <= 1) {
            result.confidence = EstimateConfidence::medium;
        } else {
            result.confidence = EstimateConfidence::low;
        }

        if (codec_seconds > 0.0) {
            const long double projected_seconds = codec_seconds *
                static_cast<long double>(result.source_bytes) / result.sampled_bytes;
            result.estimated_seconds = std::max<std::uint64_t>(
                1, clamp_u64(std::ceil(projected_seconds)));
        }
    }

    if (result.source_bytes != 0 && result.estimated_archive_bytes != 0) {
        result.estimated_ratio = static_cast<double>(result.source_bytes) /
                                 static_cast<double>(result.estimated_archive_bytes);
        result.estimated_savings_percent = 100.0 *
            (1.0 - static_cast<double>(result.estimated_archive_bytes) /
                       static_cast<double>(result.source_bytes));
    }
    if (options.volume_size != 0 && result.estimated_archive_bytes != 0) {
        result.volume_count = 1 +
            (result.estimated_archive_bytes - 1) / options.volume_size;
        result.final_volume_bytes = result.estimated_archive_bytes % options.volume_size;
        if (result.final_volume_bytes == 0) result.final_volume_bytes = options.volume_size;
    }
    if (options.progress_callback && result.sampled_bytes == 0) {
        options.progress_callback(make_estimate_snapshot(
            result.source_bytes, 0, planned_bytes, 0, overhead,
            options.compression.recovery_percent, 0, regions.size()));
    }
    report_estimate_progress(operation, OperationStage::estimating,
                             result.sampled_bytes, result.sampled_bytes,
                             observations.size(), observations.size());
    return result;
}

}  // namespace axiom
