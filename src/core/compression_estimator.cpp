#include "axiom/archive.hpp"
#include "archive/container_internal.hpp"
#include "codec/incompressible.hpp"
#include "codec/transform.hpp"

#include "core/cpu.hpp"
#include "core/file_meta.hpp"
#include "core/path_text.hpp"
// miniz declares its full static helper set in the header, so every
// translation unit that includes it leaves most of them unused. The
// vendored file is dependency-locked and cannot carry the suppression.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "third_party/miniz/miniz.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

// The estimator predicts an archive's size without writing it. It lays the
// selected files out the way the writer would (type-sorted solid stream with
// whole-file duplicates stored once, or independent ZIP entries), compresses
// probes spread across that stream with the real codec, and projects the
// result. Three corrections keep the projection honest:
//   * probes are cut from the solid stream, so small files are measured
//     together exactly as a solid block packs them;
//   * a codec that is too slow to cover enough probes is measured against a
//     fast runner on the probes they share, and the fast runner supplies the
//     spread across the rest of the data;
//   * when the writer's independent coding unit is larger than a probe, the
//     gain from the longer match history is measured and applied.
// Both public entry points run this one engine.

namespace axiom {
namespace fs = std::filesystem;
namespace {

using Clock = std::chrono::steady_clock;

struct EstimateFile {
    fs::path path;
    std::string display_path;
    std::uint64_t size = 0;
    std::size_t archive_path_bytes = 0;
    std::wstring extension;  // lower-case, the writer's grouping key
    int type_class = 4;
    std::size_t stratum = 0;
    bool duplicate = false;
};

// The bytes the archive would actually encode, in writer order.
struct StreamLayout {
    std::vector<std::size_t> order;          // indices into the file list
    std::vector<std::uint64_t> ends;         // cumulative end offset per entry
    std::vector<std::uint64_t> stratum_bytes;
    std::uint64_t bytes = 0;
};

struct ProbeSegment {
    std::size_t file_index = 0;
    std::uint64_t offset = 0;
    std::size_t size = 0;
};

struct ProbePlan {
    std::vector<std::uint64_t> starts;
    std::size_t probe_size = 0;
    std::uint64_t planned_bytes = 0;
    bool full_coverage = false;
};

struct LoadedProbe {
    ByteVector data;
    std::vector<ProbeSegment> segments;  // sizes reflect the bytes actually read
    std::vector<CompressionTransformRange> ranges;  // per-file filters, AXAR only
    std::size_t stratum = 0;
};

struct ProbeObservation {
    std::size_t probe = 0;
    std::size_t stratum = 0;
    std::uint64_t bytes = 0;
    std::uint64_t packed = 0;
};

// One codec configuration being measured: a requested candidate, or an
// internal fast stand-in that anchors slow candidates.
struct Runner {
    CompressionOptions options;
    int level = 0;
    bool internal = false;
    bool file_filters = false;
    bool active = true;
    bool parallel_path = false;   // the writer uses the parallel block codec
    double seconds = 0.0;         // summed per-probe codec time
    double wall_seconds = 0.0;    // elapsed time across its batches
    std::size_t batches = 0;
    std::uint64_t probed_bytes = 0;
    std::vector<ProbeObservation> observations;
    SolidWriterGeometry geometry;
    std::size_t probe_unit = 0;   // coding unit a probe is cut into
    std::size_t real_unit = 0;    // coding unit of the real archive
    double scale = 1.0;           // packed-size factor from the longer history
    double scale_extrapolated = 0.0;  // share of that factor not measured directly
    bool scale_stand_in = false;      // measured with a faster matcher
    bool scale_pending = false;
    std::uint64_t overhead = 0;
};

struct ScaleMeasurement {
    double factor = 1.0;   // region coded whole / the same region coded per probe
    double repeats = 0.0;  // exact-repeat share inside those regions
};

struct RatioEstimate {
    double ratio = 1.0;            // packed bytes per stream byte
    double relative_margin = 0.6;  // 95% half-width relative to ratio
    bool valid = false;
};

constexpr std::size_t kMinimumRepresentativeProbes = 16;
constexpr std::size_t kMinimumMediumProbes = 8;
// Each sampled AXC stream carries this fixed framing, while a real solid block
// carries it only once. Container overhead below accounts for the real block,
// so exclude the per-probe copy before projecting the payload ratio.
constexpr std::uint64_t kAxcFixedHeaderBytes = 36;
constexpr std::size_t kTransformPrefixBytes = std::size_t{64} << 10;
constexpr std::size_t kMaximumScaleRegion = std::size_t{16} << 20;
constexpr std::size_t kMaximumScaleRegions = 3;
constexpr std::size_t kMaximumDuplicateScan = std::size_t{32} << 20;
constexpr std::size_t kMaximumDuplicateScans = 3;
// Fixed directory bytes per AXAR entry besides its path and security
// descriptor: type, sizes, CRC-32, BLAKE3, timestamps and attributes.
constexpr std::uint64_t kAxarEntryBytes = 96;
// Share of a worker the writer's pipeline turns into throughput; reading,
// hashing and the ordered writer keep it from scaling one for one.
constexpr double kWorkerEfficiency = 0.8;
// Nearby regions of one file are not independent draws; inflate the interval.
constexpr double kDependenceInflation = 1.15;
// Residual error of a fully measured stream: block boundaries, framing and
// directory details the probes cannot reproduce byte for byte.
constexpr double kModelMargin = 0.02;
constexpr double kWarningMargin = 0.25;
constexpr double kMaximumMargin = 0.6;
constexpr double kHighRelativeMargin = 0.06;
constexpr double kHighSourceMargin = 0.01;
constexpr double kMediumRelativeMargin = 0.15;
constexpr double kMediumSourceMargin = 0.025;

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

// Two-sided 95% Student-t quantile. Small samples need the wide tails: three
// probes do not justify the interval thirty probes do.
double student_t_95(double degrees) {
    static constexpr std::array<double, 30> table{
        12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
        2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,
        2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042};
    if (!(degrees >= 1.0)) return table[0];
    if (degrees <= 30.0) {
        return table[static_cast<std::size_t>(degrees + 0.5) - 1];
    }
    return 1.96 + 2.5 / degrees;
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

// ---- scanning ----------------------------------------------------------------

void add_regular_file(const fs::path& path,
                      std::size_t base_bytes,
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
    EstimateFile file;
    file.path = path;
    file.display_path = core::path_to_utf8(path);
    file.archive_path_bytes = file.display_path.size() > base_bytes
        ? file.display_path.size() - base_bytes : file.display_path.size();
    file.size = size;
    files.push_back(std::move(file));
    source_bytes = saturated_add(source_bytes, size);
    item_count = saturated_add(item_count, 1);
}

void scan_estimate_input(const fs::path& input,
                         std::vector<EstimateFile>& files,
                         std::set<fs::path>& seen,
                         std::uint64_t& source_bytes,
                         std::uint64_t& item_count,
                         std::vector<OperationWarning>& warnings,
                         const std::shared_ptr<OperationControl>& operation,
                         const std::function<void()>& tick) {
    if (operation) operation->checkpoint();
    std::error_code error;
    const fs::file_status status = fs::symlink_status(input, error);
    if (error) {
        add_estimate_warning(warnings, operation, input,
                             "could not inspect input: " + error.message());
        return;
    }
    // Archive paths are stored relative to each input's parent folder.
    const std::string parent = core::path_to_utf8(input.parent_path());
    const std::size_t base_bytes = parent.empty() ? 0 : parent.size() + 1;
    if (fs::is_regular_file(status)) {
        add_regular_file(input, base_bytes, files, seen, source_bytes, item_count,
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
            add_regular_file(entry.path(), base_bytes, files, seen, source_bytes,
                             item_count, warnings, operation);
        } else {
            item_count = saturated_add(item_count, 1);
        }
        ++scanned;
        if ((scanned & 0xffu) == 0) {
            report_estimate_progress(operation, OperationStage::scanning,
                                     0, 0, scanned, 0,
                                     core::path_to_utf8(entry.path()));
            if (tick) tick();
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

bool read_file_range(const fs::path& path,
                     std::uint64_t offset,
                     std::size_t size,
                     unsigned retries,
                     std::uint8_t* output,
                     const std::shared_ptr<OperationControl>& operation) {
    for (unsigned attempt = 0; attempt <= retries; ++attempt) {
        if (operation) operation->checkpoint();
        std::ifstream input(path, std::ios::binary);
        if (input) {
            input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            if (input && size != 0) {
                input.read(reinterpret_cast<char*>(output),
                           static_cast<std::streamsize>(size));
            }
            if (input && static_cast<std::size_t>(input.gcount()) == size) {
                return true;
            }
        }
        if (attempt != retries) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    return false;
}

// ---- duplicate detection -------------------------------------------------------

struct ContentFingerprint {
    std::uint64_t first = 0xcbf29ce484222325ull;
    std::uint64_t second = 0x9e3779b97f4a7c15ull;
    auto operator<=>(const ContentFingerprint&) const = default;
};

void mix_fingerprint(ContentFingerprint& fingerprint,
                     std::span<const std::uint8_t> data) {
    std::uint64_t first = fingerprint.first;
    std::uint64_t second = fingerprint.second;
    for (const std::uint8_t byte : data) {
        first = (first ^ byte) * 0x100000001b3ull;
        second = (second + byte) * 0xff51afd7ed558ccdull;
        second ^= second >> 29;
    }
    fingerprint.first = first;
    fingerprint.second = second;
}

// The writer stores byte-identical files once. Hashing every same-sized file
// in full could cost more than the estimate itself, so large files are
// fingerprinted from their ends and evenly spaced interior windows.
std::optional<ContentFingerprint> fingerprint_file(
    const EstimateFile& file,
    unsigned retries,
    ByteVector& scratch,
    const std::shared_ptr<OperationControl>& operation) {
    constexpr std::size_t kWholeFileLimit = std::size_t{256} << 10;
    constexpr std::size_t kWindow = std::size_t{32} << 10;
    constexpr std::uint64_t kInteriorWindows = 6;
    ContentFingerprint fingerprint;
    const auto mix_range = [&](std::uint64_t offset, std::size_t size) {
        scratch.resize(size);
        if (!read_file_range(file.path, offset, size, retries,
                             scratch.data(), operation)) {
            return false;
        }
        mix_fingerprint(fingerprint, scratch);
        return true;
    };
    if (file.size <= kWholeFileLimit) {
        if (!mix_range(0, static_cast<std::size_t>(file.size))) return std::nullopt;
        return fingerprint;
    }
    if (!mix_range(0, kWindow)) return std::nullopt;
    for (std::uint64_t window = 1; window <= kInteriorWindows; ++window) {
        const std::uint64_t offset =
            (file.size - kWindow) / (kInteriorWindows + 1) * window;
        if (!mix_range(offset, kWindow)) return std::nullopt;
    }
    if (!mix_range(file.size - kWindow, kWindow)) return std::nullopt;
    return fingerprint;
}

std::uint64_t mark_duplicate_files(std::vector<EstimateFile>& files,
                                   unsigned retries,
                                   std::optional<Clock::time_point> deadline,
                                   const std::shared_ptr<OperationControl>& operation,
                                   bool& finished) {
    finished = true;
    std::vector<std::size_t> by_size;
    by_size.reserve(files.size());
    for (std::size_t index = 0; index < files.size(); ++index) {
        if (files[index].size != 0) by_size.push_back(index);
    }
    std::stable_sort(by_size.begin(), by_size.end(),
                     [&](std::size_t left, std::size_t right) {
                         return files[left].size < files[right].size;
                     });

    std::uint64_t duplicate_bytes = 0;
    ByteVector scratch;
    for (std::size_t begin = 0; begin < by_size.size();) {
        std::size_t end = begin + 1;
        while (end < by_size.size() &&
               files[by_size[end]].size == files[by_size[begin]].size) {
            ++end;
        }
        if (end - begin >= 2) {
            std::set<ContentFingerprint> seen;
            for (std::size_t position = begin; position < end; ++position) {
                if (deadline && Clock::now() >= *deadline) {
                    finished = false;
                    return duplicate_bytes;
                }
                auto& file = files[by_size[position]];
                const auto fingerprint =
                    fingerprint_file(file, retries, scratch, operation);
                if (!fingerprint) continue;
                if (!seen.insert(*fingerprint).second) {
                    file.duplicate = true;
                    duplicate_bytes = saturated_add(duplicate_bytes, file.size);
                }
            }
        }
        begin = end;
    }
    return duplicate_bytes;
}

// ---- stream layout and probe planning ------------------------------------------

// A file this large is its own stratum. Content varies far less inside one
// file than between files, and an extension says little when it is missing or
// generic: Silesia's twelve extensionless files (text, XML, x-ray images,
// binaries) used to share one stratum, and the spread between them was read as
// sampling noise, stating +/-24% for estimates that were within 3-5%.
constexpr std::uint64_t kOwnStratumBytes = std::uint64_t{4} << 20;

StreamLayout build_stream_layout(std::vector<EstimateFile>& files,
                                 ArchiveFormat format) {
    std::map<std::wstring, std::size_t> strata;
    for (std::size_t index = 0; index < files.size(); ++index) {
        auto& file = files[index];
        file.type_class = compression_type_class(file.path);
        file.extension = lower_ascii(file.path.extension().wstring());
        // Extensions never start with a control character, so the keys of
        // large files cannot collide with an extension group.
        const std::wstring key = file.size >= kOwnStratumBytes
            ? L"\x01" + std::to_wstring(index)
            : file.extension;
        file.stratum = strata.try_emplace(key, strata.size()).first->second;
    }

    StreamLayout layout;
    layout.stratum_bytes.assign(strata.size(), 0);
    for (std::size_t index = 0; index < files.size(); ++index) {
        if (files[index].size != 0 && !files[index].duplicate) {
            layout.order.push_back(index);
        }
    }
    if (format == ArchiveFormat::axar) {
        // Same grouping the AXAR writer applies before it fills solid blocks.
        std::stable_sort(
            layout.order.begin(), layout.order.end(),
            [&](std::size_t left, std::size_t right) {
                if (files[left].type_class != files[right].type_class) {
                    return files[left].type_class < files[right].type_class;
                }
                return files[left].extension < files[right].extension;
            });
    }
    layout.ends.reserve(layout.order.size());
    for (const std::size_t index : layout.order) {
        layout.bytes = saturated_add(layout.bytes, files[index].size);
        layout.ends.push_back(layout.bytes);
        layout.stratum_bytes[files[index].stratum] = saturated_add(
            layout.stratum_bytes[files[index].stratum], files[index].size);
    }
    return layout;
}

ProbePlan plan_probes(std::uint64_t stream_bytes,
                      std::size_t budget,
                      std::size_t probe_size) {
    ProbePlan plan;
    if (stream_bytes == 0 || budget == 0 || probe_size == 0) return plan;
    plan.probe_size = static_cast<std::size_t>(
        std::min<std::uint64_t>(probe_size, stream_bytes));
    const std::uint64_t slots = 1 + (stream_bytes - 1) / plan.probe_size;
    const std::uint64_t affordable = std::max<std::uint64_t>(
        1, (std::min<std::uint64_t>(budget, stream_bytes) + plan.probe_size - 1) /
               plan.probe_size);
    const std::uint64_t target = std::min(slots, affordable);
    plan.full_coverage = target == slots;

    // Low-discrepancy ordering keeps every prefix spread across the whole
    // stream, so sampling can stop after any probe and stay representative.
    std::set<std::uint64_t> chosen;
    const std::uint64_t attempts = slots * 8 + 64;
    for (std::uint64_t index = 1;
         chosen.size() < target && index <= attempts; ++index) {
        const std::uint64_t slot = std::min<std::uint64_t>(
            slots - 1,
            static_cast<std::uint64_t>(
                radical_inverse_base_two(static_cast<std::size_t>(index)) * slots));
        if (chosen.insert(slot).second) {
            plan.starts.push_back(slot * plan.probe_size);
        }
    }
    for (std::uint64_t slot = 0; chosen.size() < target && slot < slots; ++slot) {
        if (chosen.insert(slot).second) {
            plan.starts.push_back(slot * plan.probe_size);
        }
    }
    for (const std::uint64_t start : plan.starts) {
        plan.planned_bytes += std::min<std::uint64_t>(
            plan.probe_size, stream_bytes - start);
    }
    return plan;
}

std::vector<ProbeSegment> stream_segments(const StreamLayout& layout,
                                          std::uint64_t start,
                                          std::uint64_t size) {
    std::vector<ProbeSegment> segments;
    const std::uint64_t stop = std::min(layout.bytes, saturated_add(start, size));
    auto entry = static_cast<std::size_t>(
        std::upper_bound(layout.ends.begin(), layout.ends.end(), start) -
        layout.ends.begin());
    std::uint64_t cursor = start;
    while (cursor < stop && entry < layout.order.size()) {
        const std::uint64_t entry_begin = entry == 0 ? 0 : layout.ends[entry - 1];
        const std::uint64_t take = std::min(stop, layout.ends[entry]) - cursor;
        if (take != 0) {
            segments.push_back({layout.order[entry], cursor - entry_begin,
                                static_cast<std::size_t>(take)});
        }
        cursor += take;
        ++entry;
    }
    return segments;
}

// ---- probe compression ---------------------------------------------------------

std::uint64_t compress_zip_probe(std::span<const std::uint8_t> input,
                                 const CompressionOptions& options) {
    int level = MZ_DEFAULT_COMPRESSION;
    if (options.force_store || options.method == CompressionMethod::store) {
        level = MZ_NO_COMPRESSION;
    } else if (options.method == CompressionMethod::deflate) {
        level = std::clamp(
            options.codec_level == kAutomaticCodecLevel
                ? options.level : options.codec_level,
            0, 9);
    } else if (options.use_fast_lz || options.max_chain_depth <= 16 ||
               options.fast_entropy) {
        level = MZ_BEST_SPEED;
    } else if (options.use_tree_matcher || options.max_chain_depth >= 256 ||
               options.enable_optimal_parser) {
        level = MZ_BEST_COMPRESSION;
    }
    mz_ulong length = mz_compressBound(static_cast<mz_ulong>(input.size()));
    ByteVector compressed(static_cast<std::size_t>(length));
    const int status = mz_compress2(
        compressed.data(), &length, input.data(), static_cast<mz_ulong>(input.size()), level);
    if (status != MZ_OK) throw std::runtime_error("ZIP estimate compression failed");
    // mz_compress2 emits a zlib wrapper; ZIP stores the raw Deflate payload.
    return length > 6 ? static_cast<std::uint64_t>(length - 6) : length;
}

std::uint64_t compress_axar_probe(std::span<const std::uint8_t> input,
                                  const CompressionOptions& options) {
    const auto encoded = compress(input, options);
    return encoded.size() > kAxcFixedHeaderBytes
        ? static_cast<std::uint64_t>(encoded.size()) - kAxcFixedHeaderBytes
        : 0;
}

// Directory bytes the writer stores per entry beyond the fixed fields. On
// Windows that is dominated by the security descriptor, whose size depends on
// the folder's ACL, so it is read from a few of the selected files.
std::uint64_t sample_entry_metadata_bytes(const std::vector<EstimateFile>& files) {
    constexpr std::size_t kSamples = 8;
    if (files.empty()) return 0;
    std::uint64_t total = 0;
    std::uint64_t count = 0;
    const std::size_t stride = std::max<std::size_t>(1, files.size() / kSamples);
    for (std::size_t index = 0; index < files.size() && count < kSamples;
         index += stride) {
        try {
            const auto metadata = core::capture_metadata(files[index].path);
            total += metadata.windows_security_descriptor.size() +
                     metadata.reparse_data.size();
            ++count;
        } catch (...) {
            // Unreadable metadata is the writer's warning to raise, not ours.
        }
    }
    return count == 0 ? 0 : total / count;
}

std::uint64_t estimate_container_overhead(const std::vector<EstimateFile>& files,
                                          std::uint64_t item_count,
                                          std::uint64_t stream_bytes,
                                          ArchiveFormat format,
                                          const CompressionOptions& compression,
                                          std::uint64_t solid_block_size,
                                          std::uint64_t metadata_bytes) {
    std::uint64_t path_bytes = 0;
    for (const auto& file : files) {
        path_bytes = saturated_add(path_bytes, file.archive_path_bytes);
    }
    if (!files.empty() && item_count > files.size()) {
        // Folders and links carry paths about as long as the files do.
        path_bytes = clamp_u64(static_cast<long double>(path_bytes) *
                               item_count / files.size());
    }
    const bool zip = format == ArchiveFormat::zip;
    std::uint64_t overhead = zip ? 64 : 40;
    const std::uint64_t per_item =
        zip ? 110 : saturated_add(kAxarEntryBytes, metadata_bytes);
    overhead = saturated_add(overhead, saturated_multiply(item_count, per_item));
    overhead = saturated_add(overhead, saturated_multiply(path_bytes, zip ? 2 : 1));

    const std::uint64_t block_size = std::max<std::uint64_t>(1, solid_block_size);
    const std::uint64_t blocks = stream_bytes == 0
        ? 0 : 1 + (stream_bytes - 1) / block_size;
    overhead = saturated_add(overhead, saturated_multiply(blocks, 48));
    if (!compression.password.empty()) {
        overhead = saturated_add(overhead,
                                 saturated_add(128, saturated_multiply(blocks, 40)));
    }
    return overhead;
}

bool is_external_method(CompressionMethod method) {
    return method == CompressionMethod::zstandard ||
           method == CompressionMethod::lzma2 ||
           method == CompressionMethod::deflate;
}

// A cheap configuration that sees the same data geometry as a slow one. It
// supplies the spread of compressibility across probes the slow codec cannot
// afford to visit, and, for the Axiom codec, the gain of a longer history.
CompressionOptions make_fast_stand_in(const CompressionOptions& options) {
    CompressionOptions fast = options;
    if (is_external_method(options.method)) {
        fast.codec_level = 1;
        return fast;
    }
    fast.use_tree_matcher = false;
    fast.enable_optimal_parser = false;
    fast.use_fast_lz = false;
    fast.lazy_matching = true;
    fast.fast_entropy = true;
    fast.content_adaptive_blocks = false;
    fast.swarm_parse = false;
    fast.max_chain_depth = std::min<std::size_t>(fast.max_chain_depth, 16);
    fast.nice_length = std::min<std::size_t>(fast.nice_length, 64);
    return fast;
}

// The candidate's own matcher and window without the optimal parse: it finds
// the same long-range matches, which is what a longer unit adds, far sooner.
CompressionOptions make_scale_stand_in(const CompressionOptions& options) {
    CompressionOptions stand_in = options;
    stand_in.enable_optimal_parser = false;
    stand_in.lazy_matching = true;
    stand_in.content_adaptive_blocks = false;
    stand_in.swarm_parse = false;
    return stand_in;
}

// ---- statistics ----------------------------------------------------------------

// Ratio of a runner that visited every processed probe, with a stratified
// interval. Strata are file extensions: the writer groups by type, and
// compressibility varies far more between types than within one, so the
// interval comes from the spread inside each type rather than from the gap
// between types.
RatioEstimate estimate_full_ratio(const Runner& runner,
                                  const StreamLayout& layout,
                                  bool covered) {
    RatioEstimate estimate;
    const std::size_t strata = layout.stratum_bytes.size();
    struct Cell {
        long double bytes = 0.0L;
        long double packed = 0.0L;
        long double spread = 0.0L;
        std::size_t count = 0;
    };
    std::vector<Cell> cells(strata);
    long double total_bytes = 0.0L;
    long double total_packed = 0.0L;
    for (const auto& observation : runner.observations) {
        auto& cell = cells[observation.stratum];
        cell.bytes += observation.bytes;
        cell.packed += observation.packed;
        ++cell.count;
        total_bytes += observation.bytes;
        total_packed += observation.packed;
    }
    if (!(total_bytes > 0.0L) || layout.bytes == 0) return estimate;
    const long double overall = total_packed / total_bytes;

    long double overall_spread = 0.0L;
    for (const auto& observation : runner.observations) {
        const long double ratio =
            static_cast<long double>(observation.packed) / observation.bytes;
        auto& cell = cells[observation.stratum];
        const long double delta = ratio - cell.packed / cell.bytes;
        cell.spread += observation.bytes * delta * delta;
        overall_spread += observation.bytes * (ratio - overall) * (ratio - overall);
    }
    const std::size_t total_count = runner.observations.size();
    // Variance across everything, used where a type was not sampled at all.
    const long double overall_variance = total_count >= 2
        ? overall_spread / total_bytes * total_count / (total_count - 1)
        : 0.0625L;
    // Pooled within-type variance, used where a type has a single probe.
    long double pooled_spread = 0.0L;
    long double pooled_bytes = 0.0L;
    std::size_t pooled_count = 0;
    std::size_t pooled_strata = 0;
    for (const auto& cell : cells) {
        if (cell.count < 2) continue;
        pooled_spread += cell.spread;
        pooled_bytes += cell.bytes;
        pooled_count += cell.count;
        ++pooled_strata;
    }
    const long double pooled_variance = pooled_count > pooled_strata
        ? pooled_spread / pooled_bytes * pooled_count / (pooled_count - pooled_strata)
        : overall_variance;

    // The ratio itself is the plain packed/probed quotient: the probes are
    // spread in proportion to the stream, and reweighting whole probes by
    // type would misattribute the ones that straddle two types.
    const long double ratio = overall;
    long double variance = 0.0L;
    double degrees = 0.0;
    for (std::size_t stratum = 0; stratum < strata; ++stratum) {
        if (layout.stratum_bytes[stratum] == 0) continue;
        const long double weight =
            static_cast<long double>(layout.stratum_bytes[stratum]) / layout.bytes;
        const auto& cell = cells[stratum];
        if (cell.count == 0) {
            variance += weight * weight * overall_variance;
            continue;
        }
        const long double cell_variance = cell.count >= 2
            ? cell.spread / cell.bytes * cell.count / (cell.count - 1)
            : pooled_variance;
        const long double unsampled = std::clamp<long double>(
            1.0L - cell.bytes / layout.stratum_bytes[stratum], 0.0L, 1.0L);
        variance += weight * weight * cell_variance / cell.count * unsampled;
        if (cell.count >= 2) degrees += static_cast<double>(cell.count - 1);
    }
    estimate.ratio = static_cast<double>(ratio);
    estimate.valid = true;
    if (covered) {
        // Every byte was measured; only the model margin remains.
        estimate.relative_margin = 0.0;
    } else if (ratio > 0.0L) {
        estimate.relative_margin =
            student_t_95(degrees) * kDependenceInflation *
            std::sqrt(static_cast<double>(variance)) / static_cast<double>(ratio);
    } else {
        estimate.relative_margin = 0.0;
    }
    return estimate;
}

// A runner that stopped early is tied to a reference that kept going: the
// packed-size ratio between two codecs on the same bytes is far steadier than
// either codec's ratio across different data.
RatioEstimate estimate_linked_ratio(const Runner& runner,
                                    const Runner& reference,
                                    const RatioEstimate& reference_estimate) {
    RatioEstimate estimate;
    std::map<std::size_t, std::uint64_t> reference_packed;
    for (const auto& observation : reference.observations) {
        reference_packed.emplace(observation.probe, observation.packed);
    }
    long double packed = 0.0L;
    long double anchor = 0.0L;
    std::vector<std::pair<long double, long double>> pairs;
    for (const auto& observation : runner.observations) {
        const auto found = reference_packed.find(observation.probe);
        if (found == reference_packed.end() || found->second == 0) continue;
        packed += observation.packed;
        anchor += found->second;
        pairs.emplace_back(static_cast<long double>(observation.packed),
                           static_cast<long double>(found->second));
    }
    if (pairs.empty() || !(anchor > 0.0L) || !reference_estimate.valid) {
        return estimate;
    }
    const long double link = packed / anchor;
    double link_margin = 0.3;
    if (pairs.size() >= 2 && link > 0.0L) {
        long double spread = 0.0L;
        for (const auto& [own, other] : pairs) {
            const long double delta = own / other - link;
            spread += other * delta * delta;
        }
        const long double variance =
            spread / anchor * pairs.size() / (pairs.size() - 1);
        link_margin = student_t_95(static_cast<double>(pairs.size() - 1)) *
                      std::sqrt(static_cast<double>(variance / pairs.size())) /
                      static_cast<double>(link);
    }
    estimate.ratio = reference_estimate.ratio * static_cast<double>(link);
    estimate.relative_margin = std::sqrt(
        reference_estimate.relative_margin * reference_estimate.relative_margin +
        link_margin * link_margin);
    estimate.valid = true;
    return estimate;
}

std::uint64_t adaptive_minimum_sample_bytes(std::uint64_t stream_bytes,
                                            std::uint64_t file_count,
                                            std::size_t probe_size,
                                            std::size_t maximum_budget) {
    if (stream_bytes <= maximum_budget) return stream_bytes;
    std::uint64_t minimum = 4u << 20;
    if (stream_bytes >= (1ull << 30)) minimum = 8u << 20;
    if (stream_bytes >= (16ull << 30)) minimum = 12u << 20;
    if (file_count >= 10'000) minimum += 2u << 20;
    if (file_count >= 100'000) minimum += 2u << 20;
    minimum = (std::max)(minimum, saturated_multiply(
        kMinimumRepresentativeProbes, probe_size));
    return (std::min<std::uint64_t>)(minimum, maximum_budget);
}

// ---- first guess ---------------------------------------------------------------

// Before any byte is compressed the estimate is a guess from file types: a
// typical packed share per kind of content, scaled for method and effort. It
// is shown at once, with a wide range, and replaced as soon as probes land.
// As wide as the widest measured range, so the band only ever narrows.
constexpr double kPriorMargin = 0.6;

bool prior_is_compressed(const fs::path& path, int type_class) {
    if (type_class == 5) return true;
    if (type_class != 4) return false;
    // Packed formats the writer's ordering does not single out.
    static constexpr std::array<std::wstring_view, 24> packed{
        L".mov", L".m4a", L".m4v", L".ogg", L".opus", L".webm", L".heic",
        L".avif", L".jxl", L".cab", L".lz4", L".tgz", L".txz", L".lzma",
        L".apk", L".jar", L".whl", L".nupkg", L".odt", L".ods", L".odp",
        L".wma", L".wmv", L".axar"};
    const auto extension = lower_ascii(path.extension().wstring());
    return std::find(packed.begin(), packed.end(), extension) != packed.end();
}

// Typical packed share of one file's bytes at a balanced Axiom level.
double prior_file_ratio(const fs::path& path, int type_class) {
    if (prior_is_compressed(path, type_class)) return 0.99;
    switch (type_class) {
        case 1: return 0.25;
        case 2: return 0.42;
        case 3: return 0.65;
        default: break;
    }
    static constexpr std::array<std::wstring_view, 22> text_like{
        L".sql", L".yaml", L".yml", L".ini", L".toml", L".cfg", L".conf",
        L".cs", L".java", L".rs", L".go", L".sh", L".bat", L".ps1",
        L".vb", L".svg", L".tex", L".rtf", L".hxx", L".cxx", L".cc",
        L".inl"};
    const auto extension = lower_ascii(path.extension().wstring());
    if (std::find(text_like.begin(), text_like.end(), extension) !=
        text_like.end()) {
        return 0.25;
    }
    return 0.55;
}

// Packed-size factor for a method and effort, relative to Axiom level 5. It
// applies to compressible content only.
double prior_effort_factor(const CompressionOptions& options, int level) {
    double effort = 0.5;  // 0 fastest, 1 strongest
    double method = 1.0;
    switch (options.method) {
        case CompressionMethod::axiom:
            effort = (std::clamp(level, 1, 9) - 1) / 8.0;
            break;
        case CompressionMethod::zstandard:
            effort = (std::clamp(level, -5, 22) + 5) / 27.0;
            method = 1.06;
            break;
        case CompressionMethod::lzma2:
            effort = std::clamp(level, 0, 9) / 9.0;
            method = 0.97;
            break;
        case CompressionMethod::deflate:
            effort = std::clamp(level, 1, 9) / 9.0;
            method = 1.25;
            break;
        case CompressionMethod::store:
            break;
    }
    return method * (1.14 - 0.28 * effort);
}

bool stores_only(const CompressionOptions& options, int level) {
    return options.force_store || options.method == CompressionMethod::store ||
           (options.method == CompressionMethod::deflate && level <= 0);
}

// ---- the engine ----------------------------------------------------------------

// Runs work(0..count-1) on up to `count` threads and rethrows the first failure
// only after every task has finished; the tasks borrow the caller's buffers.
template <typename Work>
void run_parallel(std::size_t count, Work&& work) {
    std::vector<std::future<void>> tasks;
    tasks.reserve(count == 0 ? 0 : count - 1);
    std::exception_ptr failure;
    for (std::size_t index = 1; index < count; ++index) {
        tasks.push_back(std::async(std::launch::async, [&work, index] { work(index); }));
    }
    try {
        if (count != 0) work(0);
    } catch (...) {
        failure = std::current_exception();
    }
    for (auto& task : tasks) {
        try {
            task.get();
        } catch (...) {
            if (!failure) failure = std::current_exception();
        }
    }
    if (failure) std::rethrow_exception(failure);
}

// Doublings of match history between two unit sizes, discounted once the unit
// outgrows the codec window: past that point only the entropy model benefits,
// and that benefit roughly halves with every further doubling.
double history_doublings(double from, double to, double window) {
    double doublings = 0.0;
    double size = from;
    while (size < to) {
        const double next = std::min(size * 2.0, to);
        double weight = 1.0;
        if (window > 0.0 && size >= window) {
            weight = std::pow(0.5, std::floor(std::log2(size / window)) + 1.0);
        }
        doublings += std::log2(next / size) * weight;
        size = next;
    }
    return doublings;
}

// Share of a region's bytes that repeat content lying between `near` and `far`
// bytes back: exact long-range repeats (similar binaries, repeated records)
// that a unit of `far` bytes matches and a unit of `near` bytes cannot reach.
// Content-defined anchors are fingerprinted and verified byte for byte.
double long_range_duplicate_share(std::span<const std::uint8_t> data,
                                  double near, double far) {
    constexpr std::size_t kWindow = 48;
    constexpr std::size_t kMinimumGap = 32;
    constexpr std::size_t kMaximumRun = 512;
    if (data.size() < kWindow * 4 || !(far > near) ||
        data.size() > std::numeric_limits<std::uint32_t>::max()) {
        return 0.0;
    }
    static const std::array<std::uint64_t, 256> gear = [] {
        std::array<std::uint64_t, 256> table{};
        std::uint64_t state = 0x9e3779b97f4a7c15ull;
        for (auto& value : table) {
            state += 0x9e3779b97f4a7c15ull;
            std::uint64_t mixed = state;
            mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ull;
            mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebull;
            value = mixed ^ (mixed >> 31);
        }
        return table;
    }();
    std::size_t slots = 1024;
    while (slots < data.size() / 48) slots <<= 1;
    std::vector<std::uint32_t> last(slots, 0);  // anchor position + 1
    const double size = static_cast<double>(data.size());
    double repeated = 0.0;
    std::uint64_t rolling = 0;
    std::size_t previous_anchor = 0;
    double previous_distance = -1.0;
    const std::size_t stop = data.size() - kWindow;
    for (std::size_t position = 0; position < stop; ++position) {
        rolling = (rolling << 1) + gear[data[position]];
        if ((rolling >> 57) != 0 || position < previous_anchor + kMinimumGap) {
            continue;
        }
        const std::uint8_t* window = data.data() + position;
        std::uint64_t fingerprint = 0xcbf29ce484222325ull;
        for (std::size_t offset = 0; offset < kWindow; offset += 8) {
            std::uint64_t word = 0;
            std::memcpy(&word, window + offset, sizeof(word));
            fingerprint = (fingerprint ^ word) * 0xff51afd7ed558ccdull;
            fingerprint ^= fingerprint >> 32;
        }
        std::uint32_t& slot = last[fingerprint & (slots - 1)];
        double distance = -1.0;
        if (slot != 0 &&
            std::memcmp(data.data() + (slot - 1), window, kWindow) == 0) {
            distance = static_cast<double>(position - (slot - 1));
        }
        slot = static_cast<std::uint32_t>(position + 1);
        if (distance >= near && distance < far) {
            // A repeat is usable only where the unit already holds its
            // source; the scan itself sees fewer old sources near its start.
            const double usable = (1.0 - distance / far) * size /
                                  std::max(size - distance, size * 0.25);
            const double span = distance == previous_distance
                ? static_cast<double>(std::min(position - previous_anchor, kMaximumRun))
                : static_cast<double>(kWindow);
            repeated += span * usable;
        }
        previous_distance = distance;
        previous_anchor = position;
    }
    return std::clamp(repeated / size, 0.0, 0.9);
}

class EstimateEngine {
public:
    EstimateEngine(const std::vector<fs::path>& inputs,
                   const CompressionEstimateOptions& options,
                   std::span<const CompressionEstimateCurveCandidate> candidates,
                   const std::function<void(const CompressionEstimateCurveResult&)>&
                       progress)
        : inputs_(inputs),
          options_(options),
          candidates_(candidates),
          progress_(progress),
          operation_(options.compression.operation),
          retries_(options.compression.input_open_retries),
          batch_size_(std::clamp<std::size_t>(core::physical_core_count(), 1, 8)) {}

    CompressionEstimateCurveResult run() {
        started_ = Clock::now();
        result_.format = options_.format;
        scan();
        plan_ = plan_probes(layout_.bytes, options_.sample_budget,
                            preferred_probe_size());
        prepare_runners();
        result_.planned_sample_bytes = plan_.planned_bytes;
        result_.total_evaluations =
            saturated_multiply(plan_.starts.size(), candidates_.size());
        result_.points.clear();  // replaces the scan-time guess
        for (std::size_t index = 0; index < candidates_.size(); ++index) {
            CompressionEstimateCurvePoint point;
            point.level = candidates_[index].level;
            point.total_probes = plan_.starts.size();
            result_.points.push_back(point);
        }
        report_estimate_progress(
            operation_, OperationStage::estimating,
            0, result_.planned_sample_bytes, 0, result_.total_evaluations);
        // Every level shows the file-type guess until its first probes land.
        publish(false);

        sample();
        calibrate_scale();
        publish(true);
        report_estimate_progress(
            operation_, OperationStage::estimating,
            result_.sampled_bytes, result_.sampled_bytes,
            result_.completed_evaluations, result_.completed_evaluations);
        return std::move(result_);
    }

private:
    bool time_limited() const { return options_.time_budget.count() > 0; }

    double elapsed_seconds() const {
        return std::chrono::duration<double>(Clock::now() - started_).count();
    }

    double budget_seconds() const {
        return std::chrono::duration<double>(options_.time_budget).count();
    }

    // ---- scanning -------------------------------------------------------------

    void scan() {
        report_estimate_progress(operation_, OperationStage::scanning,
                                 0, 0, 0, inputs_.size());
        std::set<fs::path> seen;
        const std::function<void()> tick = [this] { publish_scan(false); };
        for (std::size_t index = 0; index < inputs_.size(); ++index) {
            scan_estimate_input(
                inputs_[index], files_, seen, result_.source_bytes,
                result_.item_count, result_.warnings, operation_, tick);
            report_estimate_progress(
                operation_, OperationStage::scanning,
                0, 0, index + 1, inputs_.size(),
                core::path_to_utf8(inputs_[index]));
            // The last input always publishes: the guess is on screen before
            // duplicate detection, which can take seconds on a large tree.
            publish_scan(index + 1 == inputs_.size());
        }
        std::sort(
            files_.begin(), files_.end(),
            [](const EstimateFile& left, const EstimateFile& right) {
                return left.path.native() < right.path.native();
            });
        result_.file_count = files_.size();
        if (options_.format == ArchiveFormat::axar) {
            std::optional<Clock::time_point> deadline;
            if (time_limited()) {
                deadline = Clock::now() + std::max<Clock::duration>(
                    std::chrono::seconds(3), options_.time_budget);
            }
            bool finished = true;
            result_.duplicate_bytes = mark_duplicate_files(
                files_, retries_, deadline, operation_, finished);
            if (!finished && !inputs_.empty()) {
                // Unfound duplicates would be counted twice; say so rather
                // than report a confident figure.
                add_estimate_warning(
                    result_.warnings, operation_, inputs_.front(),
                    "duplicate detection ran out of time; the archive may be "
                    "smaller than estimated");
            }
            metadata_bytes_ = sample_entry_metadata_bytes(files_);
        }
        layout_ = build_stream_layout(files_, options_.format);
        // Rebuild the guess over the bytes the archive would encode, now
        // that duplicates are known.
        prior_compressible_ = 0.0L;
        prior_incompressible_ = 0.0L;
        prior_entry_bytes_ = 0;
        classified_files_ = 0;
        classify_files();
        transform_hints_.assign(files_.size(), codec::TransformHint{});
        transform_hint_known_.assign(files_.size(), false);
        // Scanning and duplicate detection are not charged to the sampling
        // budget; a large tree would otherwise leave no time to measure.
        started_ = Clock::now();
    }

    // ---- first guess ----------------------------------------------------------

    void classify_files() {
        for (; classified_files_ < files_.size(); ++classified_files_) {
            const auto& file = files_[classified_files_];
            prior_entry_bytes_ = saturated_add(
                prior_entry_bytes_, kAxarEntryBytes + file.archive_path_bytes);
            if (file.duplicate || file.size == 0) continue;
            const int type_class = compression_type_class(file.path);
            const double share = prior_file_ratio(file.path, type_class);
            const long double bytes = static_cast<long double>(file.size);
            if (share >= 0.99) {
                prior_incompressible_ += bytes * share;
            } else {
                prior_compressible_ += bytes * share;
            }
        }
    }

    void fill_prior(CompressionEstimateCurvePoint& point,
                    const CompressionEstimateCurveCandidate& candidate,
                    std::uint64_t stream_bytes) const {
        const auto& compression = candidate.compression;
        const bool store = stores_only(compression, candidate.level);
        long double payload = store
            ? static_cast<long double>(stream_bytes)
            : prior_compressible_ *
                      prior_effort_factor(compression, candidate.level) *
                      (options_.format == ArchiveFormat::zip ? 1.1 : 1.0) +
                  prior_incompressible_;
        payload = std::min(payload, static_cast<long double>(stream_bytes));
        const std::uint64_t overhead = saturated_add(128, prior_entry_bytes_);
        std::uint64_t archive = saturated_add(clamp_u64(payload), overhead);
        if (compression.recovery_percent != 0) {
            archive = saturated_add(archive, clamp_u64(
                static_cast<long double>(archive) *
                compression.recovery_percent / 100.0L));
        }
        const double margin = store ? kModelMargin : kPriorMargin;
        point.provisional = true;
        point.confidence = EstimateConfidence::low;
        point.estimated_archive_bytes = archive;
        point.estimated_margin_fraction = margin;
        point.confidence_margin_percent = 0.0;
        point.estimated_seconds = 0;
        point.estimated_low_bytes =
            clamp_u64(static_cast<long double>(archive) * (1.0 - margin));
        // Stored data does not grow much past its own size and framing.
        const std::uint64_t ceiling = saturated_add(
            stream_bytes + stream_bytes / 100, overhead);
        point.estimated_high_bytes = std::max(archive, std::min<std::uint64_t>(
            ceiling,
            clamp_u64(static_cast<long double>(archive) * (1.0 + margin))));
        finish_point(point);
    }

    // Publishes the file-type guess while the file list is still being read.
    // Throttled so a huge tree does not flood the frontend.
    void publish_scan(bool force) {
        if (!progress_) return;
        const auto now = Clock::now();
        if (!force && last_scan_publish_ &&
            now - *last_scan_publish_ < std::chrono::milliseconds(100)) {
            return;
        }
        classify_files();
        last_scan_publish_ = now;
        result_.file_count = files_.size();
        result_.phase = EstimatePhase::scanning;
        result_.provisional = true;
        result_.progress = 0.0;
        result_.elapsed_ms = 0;
        result_.window_ms = window_milliseconds();
        // Duplicates are not known yet, so every byte counts.
        result_.points.resize(candidates_.size());
        for (std::size_t index = 0; index < candidates_.size(); ++index) {
            auto& point = result_.points[index];
            point = CompressionEstimateCurvePoint{};
            point.level = candidates_[index].level;
            fill_prior(point, candidates_[index], result_.source_bytes);
        }
        progress_(result_);
    }

    std::uint64_t window_milliseconds() const {
        return time_limited()
            ? static_cast<std::uint64_t>(options_.time_budget.count()) : 0;
    }

    // How far through its refinement window the run is. Time and finished
    // level-probe evaluations both count; reading a probe is not finishing it,
    // since slow levels and the longer-history calibration come after. The
    // run may still finish early once it is confident.
    double window_progress(bool complete) const {
        if (complete) return 1.0;
        double fraction = 0.0;
        if (time_limited() && budget_seconds() > 0.0) {
            fraction = elapsed_seconds() / budget_seconds();
        }
        if (result_.total_evaluations != 0) {
            fraction = std::max(
                fraction, 0.85 * static_cast<double>(result_.completed_evaluations) /
                              static_cast<double>(result_.total_evaluations));
        }
        return std::clamp(fraction, 0.0, 0.99);
    }

    // Once some levels are measured, the guess for the rest is rescaled by how
    // far it missed on the nearest measured level, so the curve does not jump
    // between guessed and measured points.
    void calibrate_guesses() {
        if (layout_.bytes == 0) return;
        for (std::size_t index = 0; index < result_.points.size(); ++index) {
            auto& point = result_.points[index];
            if (!point.provisional) continue;
            std::optional<std::size_t> nearest;
            for (std::size_t other = 0; other < result_.points.size(); ++other) {
                if (result_.points[other].provisional) continue;
                if (!nearest ||
                    std::abs(result_.points[other].level - point.level) <
                        std::abs(result_.points[*nearest].level - point.level)) {
                    nearest = other;
                }
            }
            if (!nearest) return;  // nothing measured yet
            CompressionEstimateCurvePoint guess;
            fill_prior(guess, candidates_[*nearest], layout_.bytes);
            if (guess.estimated_archive_bytes == 0) continue;
            const long double factor =
                static_cast<long double>(
                    result_.points[*nearest].estimated_archive_bytes) /
                static_cast<long double>(guess.estimated_archive_bytes);
            const auto scaled = [&](std::uint64_t bytes) {
                return clamp_u64(static_cast<long double>(bytes) * factor);
            };
            point.estimated_archive_bytes = scaled(point.estimated_archive_bytes);
            point.estimated_low_bytes = scaled(point.estimated_low_bytes);
            point.estimated_high_bytes = scaled(point.estimated_high_bytes);
            finish_point(point);
        }
    }

    // ---- runners --------------------------------------------------------------

    // Sizes of the solid blocks the writer would cut from the stream: it
    // flushes when the buffer reaches the block size, and early whenever the
    // file type changes after at least a small group has gathered.
    const std::vector<std::uint64_t>& solid_blocks(std::size_t block_size) const {
        const auto found = solid_block_cache_.find(block_size);
        if (found != solid_block_cache_.end()) return found->second;
        constexpr std::uint64_t kReadSlice = std::uint64_t{1} << 20;
        const std::uint64_t limit = std::max<std::size_t>(1, block_size);
        const std::uint64_t minimum_group =
            std::max<std::uint64_t>(1, std::min<std::uint64_t>(kReadSlice, limit / 4));
        std::vector<std::uint64_t> blocks;
        std::uint64_t buffered = 0;
        std::optional<int> previous_class;
        for (const std::size_t index : layout_.order) {
            const auto& file = files_[index];
            if (previous_class && *previous_class != file.type_class &&
                buffered >= minimum_group) {
                blocks.push_back(buffered);
                buffered = 0;
            }
            previous_class = file.type_class;
            std::uint64_t remaining = file.size;
            if (buffered + remaining < limit) {
                buffered += remaining;
                continue;
            }
            while (remaining != 0) {
                const std::uint64_t take = std::min(kReadSlice, remaining);
                buffered += take;
                remaining -= take;
                if (buffered >= limit) {
                    blocks.push_back(buffered);
                    buffered = 0;
                }
            }
        }
        if (buffered != 0) blocks.push_back(buffered);
        return solid_block_cache_.emplace(block_size, std::move(blocks)).first->second;
    }

    // The span the writer codes with one match history for these options
    // (the byte-weighted typical span across its solid blocks), and whether
    // it reaches it through the parallel block codec.
    std::size_t writer_unit(const CompressionOptions& compression,
                            SolidWriterGeometry& geometry,
                            bool& parallel) const {
        geometry = solid_writer_geometry(compression);
        auto block_options = compression;
        block_options.thread_count = geometry.inner_threads;
        const bool codec_blocks =
            !is_external_method(compression.method) &&
            !compression.force_store &&
            compression.method != CompressionMethod::store;
        // Mirror compress(): several blocks on several workers take the
        // parallel block codec; one block or one worker takes the serial
        // whole-input analysis; small thorough inputs try both and the
        // whole-input window wins.
        constexpr std::size_t kSerialThoroughLimit = std::size_t{16} << 20;
        long double weighted_log = 0.0L;
        long double parallel_bytes = 0.0L;
        long double total = 0.0L;
        for (const std::uint64_t block : solid_blocks(geometry.solid_block_size)) {
            const std::size_t block_bytes = static_cast<std::size_t>(block);
            std::size_t unit = codec_unit_size(block_bytes, block_options);
            bool block_parallel = codec_blocks && geometry.inner_threads > 1 &&
                                  (block_bytes + unit - 1) / unit > 1;
            const bool thorough = compression.enable_optimal_parser &&
                (!block_parallel ||
                 (block_bytes <= kSerialThoroughLimit &&
                  block_bytes <= compression.optimal_parse_limit));
            if (compression.enable_content_dedup) {
                // Content-defined chunks are compressed one by one.
                unit = std::clamp<std::size_t>(
                    compression.snapshot_average_chunk_size, 1, block_bytes);
                block_parallel = false;
            } else if (!block_parallel || thorough) {
                unit = block_bytes;
                block_parallel = false;
            }
            weighted_log += static_cast<long double>(block) *
                            std::log(static_cast<long double>(unit));
            if (block_parallel) parallel_bytes += block;
            total += block;
        }
        if (!(total > 0.0L)) {
            parallel = false;
            return 1;
        }
        parallel = parallel_bytes * 2.0L >= total;
        return std::max<std::size_t>(
            1, static_cast<std::size_t>(std::exp(weighted_log / total) + 0.5L));
    }

    // A probe the size of the writer's own unit measures that unit exactly,
    // so follow it whenever it is close to the requested probe size.
    std::size_t preferred_probe_size() const {
        const std::size_t requested = options_.sample_chunk_size;
        if (options_.format != ArchiveFormat::axar || layout_.bytes == 0) {
            return requested;
        }
        std::size_t smallest = std::numeric_limits<std::size_t>::max();
        for (const auto& candidate : candidates_) {
            SolidWriterGeometry geometry;
            bool parallel = false;
            smallest = std::min(
                smallest, writer_unit(candidate.compression, geometry, parallel));
        }
        return smallest >= requested / 2 && smallest <= requested * 2
            ? smallest : requested;
    }

    Runner make_runner(const CompressionOptions& compression, int level,
                       bool internal) const {
        Runner runner;
        runner.level = level;
        runner.internal = internal;
        runner.options = compression;
        runner.options.operation = operation_;
        runner.options.encode_progress = [operation = operation_](double) {
            if (operation) operation->checkpoint();
        };
        runner.options.encoded_bytes_progress = {};
        runner.options.transform_ranges.clear();
        runner.file_filters = options_.format == ArchiveFormat::axar &&
                              compression.enable_file_filters &&
                              !compression.enable_content_dedup;
        const std::size_t probe = std::max<std::size_t>(1, plan_.probe_size);
        if (options_.format == ArchiveFormat::axar) {
            bool parallel = false;
            const std::size_t unit =
                writer_unit(compression, runner.geometry, parallel);
            // Hand each probe the thread count a real solid block receives, so
            // the codec cuts it into the same independent units.
            runner.options.thread_count = runner.geometry.inner_threads;
            runner.real_unit = unit;
            runner.parallel_path = parallel;
            runner.probe_unit =
                std::min(unit, codec_unit_size(probe, runner.options));
            runner.scale_pending =
                runner.real_unit > runner.probe_unit + runner.probe_unit / 20;
        } else {
            runner.geometry.solid_block_size =
                std::max<std::size_t>(1, compression.block_size);
            runner.probe_unit = runner.real_unit = probe;
        }
        runner.overhead = estimate_container_overhead(
            files_, result_.item_count, layout_.bytes, options_.format,
            compression, runner.geometry.solid_block_size, metadata_bytes_);
        return runner;
    }

    void prepare_runners() {
        runners_.reserve(candidates_.size() + 1);
        for (const auto& candidate : candidates_) {
            runners_.push_back(
                make_runner(candidate.compression, candidate.level, false));
        }
    }

    // ---- probes ---------------------------------------------------------------

    codec::TransformHint transform_hint(const ProbeSegment& segment,
                                        std::span<const std::uint8_t> bytes) {
        const std::size_t file_index = segment.file_index;
        if (transform_hint_known_[file_index]) return transform_hints_[file_index];
        transform_hint_known_[file_index] = true;
        const auto& file = files_[file_index];
        const std::size_t prefix_size = static_cast<std::size_t>(
            std::min<std::uint64_t>(file.size, kTransformPrefixBytes));
        if (segment.offset == 0 && bytes.size() >= prefix_size) {
            transform_hints_[file_index] = codec::detect_transform_hint(bytes);
        } else {
            ByteVector prefix(prefix_size);
            if (read_file_range(file.path, 0, prefix_size, retries_,
                                prefix.data(), operation_)) {
                transform_hints_[file_index] = codec::detect_transform_hint(prefix);
            }
        }
        return transform_hints_[file_index];
    }

    LoadedProbe load_probe(std::uint64_t start, std::uint64_t size,
                           bool with_filters) {
        LoadedProbe probe;
        const auto segments = stream_segments(layout_, start, size);
        std::uint64_t total = 0;
        for (const auto& segment : segments) total += segment.size;
        probe.data.resize(static_cast<std::size_t>(total));
        std::size_t cursor = 0;
        const std::uint64_t middle = start + size / 2;
        std::uint64_t stream_cursor = start;
        for (const auto& segment : segments) {
            if (middle >= stream_cursor) probe.stratum = files_[segment.file_index].stratum;
            stream_cursor += segment.size;
            if (!read_file_range(files_[segment.file_index].path, segment.offset,
                                 segment.size, retries_,
                                 probe.data.data() + cursor, operation_)) {
                if (warned_files_.insert(segment.file_index).second) {
                    add_estimate_warning(
                        result_.warnings, operation_,
                        files_[segment.file_index].path,
                        "could not read sample data; estimate confidence reduced");
                }
                continue;
            }
            if (with_filters) {
                // The writer classifies filters per file from its first bytes.
                const auto hint = transform_hint(
                    segment, std::span<const std::uint8_t>(
                                 probe.data.data() + cursor, segment.size));
                if (hint.transform != CompressionTransform::none) {
                    probe.ranges.push_back({
                        hint.transform, static_cast<std::uint64_t>(cursor),
                        static_cast<std::uint64_t>(segment.size),
                        segment.offset, hint.parameter});
                }
            }
            probe.segments.push_back(segment);
            cursor += segment.size;
        }
        probe.data.resize(cursor);
        return probe;
    }

    // The filter ranges of a probe that fall inside one of its slices.
    static std::vector<CompressionTransformRange> clip_ranges(
        std::span<const CompressionTransformRange> ranges,
        std::uint64_t offset, std::uint64_t size) {
        std::vector<CompressionTransformRange> clipped;
        for (const auto& range : ranges) {
            const std::uint64_t begin = std::max(range.offset, offset);
            const std::uint64_t end =
                std::min(range.offset + range.size, offset + size);
            if (begin >= end) continue;
            clipped.push_back({range.transform, begin - offset, end - begin,
                               range.source_offset + (begin - range.offset),
                               range.parameter});
        }
        return clipped;
    }

    // One AXC stream the way the writer would produce it for this runner.
    static std::uint64_t encode_axar(std::span<const std::uint8_t> data,
                                     const CompressionOptions& base,
                                     std::span<const CompressionTransformRange> ranges,
                                     bool parallel_path) {
        auto unit_options = base;
        unit_options.transform_ranges.assign(ranges.begin(), ranges.end());
        // An empty list means no file qualified; do not let the codec detect
        // a filter across file boundaries.
        unit_options.enable_file_filters = !ranges.empty();
        if (parallel_path) {
            // A single-unit probe would otherwise take the serial analysis the
            // writer only uses for single-block inputs.
            if (!unit_options.bypass_incompressible_heuristic &&
                codec::likely_incompressible(data)) {
                return data.size();
            }
            unit_options.force_parallel_blocks = true;
        }
        return std::min<std::uint64_t>(
            compress_axar_probe(data, unit_options), data.size());
    }

    std::uint64_t compress_probe(const Runner& runner,
                                 const LoadedProbe& probe) const {
        if (options_.format == ArchiveFormat::zip) {
            // ZIP entries are compressed one by one; so are the probe's files.
            std::uint64_t packed = 0;
            std::size_t cursor = 0;
            for (const auto& segment : probe.segments) {
                packed = saturated_add(packed, compress_zip_probe(
                    std::span<const std::uint8_t>(
                        probe.data.data() + cursor, segment.size),
                    runner.options));
                cursor += segment.size;
            }
            return packed;
        }
        return encode_axar(
            probe.data, runner.options,
            runner.file_filters
                ? std::span<const CompressionTransformRange>(probe.ranges)
                : std::span<const CompressionTransformRange>{},
            runner.parallel_path);
    }

    // The writer encodes one unit per worker; so does a batch of probes.
    void run_batch(Runner& runner, const std::vector<LoadedProbe>& batch,
                   std::size_t first_probe_index) {
        std::vector<std::uint64_t> packed(batch.size());
        std::vector<double> seconds(batch.size());
        const auto wall_started = Clock::now();
        run_parallel(batch.size(), [&](std::size_t index) {
            const auto codec_started = Clock::now();
            packed[index] = compress_probe(runner, batch[index]);
            seconds[index] = std::chrono::duration<double>(
                Clock::now() - codec_started).count();
        });
        runner.wall_seconds += std::chrono::duration<double>(
            Clock::now() - wall_started).count();
        ++runner.batches;
        for (std::size_t index = 0; index < batch.size(); ++index) {
            runner.seconds += seconds[index];
            runner.probed_bytes += batch[index].data.size();
            runner.observations.push_back(
                {first_probe_index + index, batch[index].stratum,
                 batch[index].data.size(), packed[index]});
        }
    }

    // ---- sampling -------------------------------------------------------------

    bool pending_scale() const {
        return std::any_of(runners_.begin(), runners_.end(),
                           [](const Runner& runner) {
                               return !runner.internal && runner.scale_pending;
                           });
    }

    double sampling_budget_seconds() const {
        return budget_seconds() * (pending_scale() ? 0.65 : 1.0);
    }

    // Wall-clock seconds each requested candidate may spend before it stops
    // visiting new probes and leans on the anchor for the rest.
    double runner_slice_seconds() const {
        if (!time_limited()) return std::numeric_limits<double>::infinity();
        const bool anchor_is_internal = anchor_ && runners_[*anchor_].internal;
        return sampling_budget_seconds() * (anchor_is_internal ? 0.75 : 0.9) /
               static_cast<double>(std::max<std::size_t>(1, candidates_.size()));
    }

    void choose_anchor(const std::vector<LoadedProbe>& batch,
                       std::size_t first_probe_index) {
        std::size_t fastest = 0;
        for (std::size_t index = 1; index < runners_.size(); ++index) {
            if (runners_[index].wall_seconds < runners_[fastest].wall_seconds) {
                fastest = index;
            }
        }
        anchor_ = fastest;
        if (!time_limited() || plan_.starts.size() <= batch.size()) return;
        // Even the cheapest candidate cannot reach a representative sample in
        // its share of the time: add a fast stand-in to carry the spread.
        const double wanted = static_cast<double>(
            std::min(plan_.starts.size(), kMinimumRepresentativeProbes));
        const double projected = runners_[fastest].wall_seconds *
                                 std::ceil(wanted / static_cast<double>(batch.size()));
        if (projected <= runner_slice_seconds() ||
            options_.format != ArchiveFormat::axar ||
            runners_[fastest].options.force_store ||
            runners_[fastest].options.method == CompressionMethod::store) {
            return;
        }
        Runner stand_in = make_runner(
            make_fast_stand_in(candidates_[fastest].compression),
            runners_[fastest].level, true);
        run_batch(stand_in, batch, first_probe_index);
        if (stand_in.wall_seconds * 3.0 > runners_[fastest].wall_seconds) return;
        runners_.push_back(std::move(stand_in));
        anchor_ = runners_.size() - 1;
    }

    void sample() {
        const std::uint64_t minimum_bytes = adaptive_minimum_sample_bytes(
            layout_.bytes, result_.file_count, plan_.probe_size,
            options_.sample_budget);
        const bool with_filters = std::any_of(
            runners_.begin(), runners_.end(),
            [](const Runner& runner) { return runner.file_filters; });
        std::vector<LoadedProbe> batch;
        std::size_t next = 0;
        while (next < plan_.starts.size()) {
            if (operation_) operation_->checkpoint();
            batch.clear();
            while (batch.size() < batch_size_ && next < plan_.starts.size()) {
                const std::uint64_t start = plan_.starts[next++];
                LoadedProbe probe = load_probe(
                    start,
                    std::min<std::uint64_t>(plan_.probe_size, layout_.bytes - start),
                    with_filters);
                if (!probe.data.empty()) batch.push_back(std::move(probe));
            }
            const bool out_of_time =
                time_limited() && elapsed_seconds() >= sampling_budget_seconds();
            if (batch.empty()) {
                if (out_of_time) break;
                continue;
            }
            const std::size_t first_probe_index = processed_probes_;
            for (const auto& probe : batch) {
                result_.sampled_bytes =
                    saturated_add(result_.sampled_bytes, probe.data.size());
            }
            processed_probes_ += batch.size();

            const double slice = runner_slice_seconds();
            for (std::size_t index = 0; index < runners_.size(); ++index) {
                if (operation_) operation_->checkpoint();
                auto& runner = runners_[index];
                if (!runner.active) continue;
                const bool is_anchor = anchor_ && *anchor_ == index;
                if (!is_anchor && anchor_ && runner.batches != 0) {
                    const double average =
                        runner.wall_seconds / static_cast<double>(runner.batches);
                    if (runner.wall_seconds + average > slice) {
                        runner.active = false;
                        continue;
                    }
                }
                run_batch(runner, batch, first_probe_index);
                if (!runner.internal) {
                    result_.completed_evaluations += batch.size();
                    // Populate the first visible curve one point at a time.
                    // Later batches update the complete curve together,
                    // avoiding excessive cross-thread UI notifications.
                    if (first_probe_index == 0) publish(false);
                }
            }
            if (!anchor_) choose_anchor(batch, first_probe_index);
            publish(false);

            const auto& segment = batch.back().segments.back();
            report_estimate_progress(
                operation_, OperationStage::estimating,
                result_.sampled_bytes, result_.planned_sample_bytes,
                result_.completed_evaluations, result_.total_evaluations,
                files_[segment.file_index].display_path,
                batch.back().data.size(), batch.back().data.size());

            const bool representative =
                processed_probes_ >= kMinimumRepresentativeProbes &&
                result_.sampled_bytes >= minimum_bytes;
            if (options_.stop_when_high_confidence && representative &&
                sampling_converged_) {
                break;
            }
            if (time_limited() && elapsed_seconds() >= sampling_budget_seconds()) {
                break;
            }
        }
    }

    // ---- longer-history calibration -------------------------------------------

    // When the writer codes longer independent units than a probe holds, the
    // probes miss the matches a longer history finds. Measure that gain on
    // real stream regions: a region coded as one unit against the same region
    // coded probe by probe.
    void calibrate_scale() {
        if (options_.format != ArchiveFormat::axar || layout_.bytes == 0) return;
        std::size_t pending = 0;
        for (std::size_t index = 0; index < candidates_.size(); ++index) {
            if (runners_[index].scale_pending &&
                !runners_[index].observations.empty()) {
                ++pending;
            }
        }
        if (pending == 0) return;
        const double total_allowance = time_limited()
            ? std::max(1.5, budget_seconds() - elapsed_seconds())
            : 30.0;
        const double allowance = total_allowance / static_cast<double>(pending);

        std::map<std::string, ScaleMeasurement> measured;
        std::map<std::string, double> scanned;
        std::map<std::string, double> amplitudes;
        for (std::size_t index = 0; index < candidates_.size(); ++index) {
            auto& runner = runners_[index];
            if (!runner.scale_pending || runner.observations.empty()) continue;
            const std::size_t unit = runner.probe_unit;
            const std::size_t target = runner.real_unit;
            const std::size_t ceiling = std::min(target, kMaximumScaleRegion);
            // Serial seconds per byte under load, from the probes just run.
            const double speed = runner.seconds > 0.0
                ? static_cast<double>(runner.probed_bytes) / runner.seconds
                : std::numeric_limits<double>::infinity();
            const auto affordable = [&](double bytes_per_second) {
                // One region is coded whole on one thread, then in pieces
                // across the batch workers.
                std::size_t region = std::min(ceiling, unit * 2);
                while (region < ceiling) {
                    const std::size_t larger = std::min(ceiling, region * 2);
                    const double cost = static_cast<double>(larger) *
                        (1.0 + 1.0 / static_cast<double>(batch_size_)) /
                        bytes_per_second;
                    if (cost > allowance) break;
                    region = larger;
                }
                return region;
            };
            // A candidate that cannot afford a long region lends its window to
            // a matcher of the same strength without the optimal parse, which
            // finds the same long-range matches several times faster.
            std::size_t region = affordable(speed);
            bool own = true;
            if (region < ceiling && runner.options.enable_optimal_parser) {
                const std::size_t stand_in_region = affordable(speed * 4.0);
                if (stand_in_region > region) {
                    region = stand_in_region;
                    own = false;
                }
            }
            CompressionOptions unit_options =
                own ? runner.options : make_scale_stand_in(runner.options);
            unit_options.auto_block_size_for_threads = false;
            unit_options.content_adaptive_blocks = false;

            const std::string key =
                std::to_string(static_cast<int>(unit_options.method)) + ':' +
                std::to_string(own ? index : candidates_.size()) + ':' +
                std::to_string(unit_options.window_size) + ':' +
                std::to_string(unit_options.max_match) + ':' +
                std::to_string(unit_options.codec_level) + ':' +
                std::to_string(runner.parallel_path) + ':' +
                std::to_string(runner.file_filters) + ':' +
                std::to_string(unit) + ':' + std::to_string(region);
            std::optional<ScaleMeasurement> measurement;
            if (const auto found = measured.find(key); found != measured.end()) {
                measurement = found->second;
            } else {
                measurement = measure_scale(unit_options, runner.parallel_path,
                                            runner.file_filters, unit, region,
                                            allowance);
                if (measurement) measured.emplace(key, *measurement);
            }

            // More history helps in two ways. Steadily: every match has more
            // candidates, a gain that follows the history curve and can be
            // extended past the measured region. And in steps: exact repeats
            // of distant content, which depend on where the copies lie and
            // are counted directly across the whole unit instead.
            double scale = 1.0;
            if (measurement) {
                double steady = std::min(
                    1.0, measurement->factor / (1.0 - 0.9 * measurement->repeats));
                const double window = is_external_method(runner.options.method)
                    ? static_cast<double>(runner.options.lzma_dictionary_size)
                    : static_cast<double>(runner.options.window_size);
                const double measured_doublings = history_doublings(
                    static_cast<double>(unit), static_cast<double>(region), window);
                const double total_doublings = history_doublings(
                    static_cast<double>(unit), static_cast<double>(target), window);
                if (measured_doublings > 0.0 &&
                    total_doublings > measured_doublings) {
                    // Bounded: a gain seen over one doubling is not evidence
                    // of the same gain over five.
                    steady = std::pow(steady, std::min(
                        total_doublings / measured_doublings, 2.5));
                    runner.scale_extrapolated =
                        1.0 - measured_doublings / total_doublings;
                }
                if (!own) {
                    // The stand-in sees the same history but parses it less
                    // thoroughly; scale its gain by how the real codec's gain
                    // compares on the probes themselves.
                    const std::string amplitude_key = std::to_string(index);
                    double amplitude = 1.0;
                    if (const auto found = amplitudes.find(amplitude_key);
                        found != amplitudes.end()) {
                        amplitude = found->second;
                    } else {
                        amplitude = measure_stand_in_amplitude(runner);
                        amplitudes.emplace(amplitude_key, amplitude);
                    }
                    steady = std::pow(steady, amplitude);
                    runner.scale_stand_in = true;
                }
                scale = steady;
            }
            std::optional<double> repeats;
            if (target > unit + unit / 20) {
                const std::string scan_key =
                    std::to_string(runner.file_filters) + ':' +
                    std::to_string(unit) + ':' + std::to_string(target);
                if (const auto found = scanned.find(scan_key);
                    found != scanned.end()) {
                    repeats = found->second;
                } else {
                    repeats = scan_long_range_duplicates(
                        unit, target, runner.file_filters);
                    if (repeats) scanned.emplace(scan_key, *repeats);
                }
            }
            if (!measurement && !repeats) continue;
            if (repeats) {
                scale *= 1.0 - 0.9 * *repeats;
            } else {
                scale *= 1.0 - 0.9 * measurement->repeats;
            }
            if (!measurement) {
                runner.scale_extrapolated = 1.0;
            }
            runner.scale = std::clamp(scale, 0.3, 1.0);
            runner.scale_pending = false;
            publish(false);
        }
    }

    // How much more (or less) the real codec gains from doubled history than
    // its stand-in does, measured where the real codec has already been run:
    // each probe coded whole against the same probe coded as two halves.
    double measure_stand_in_amplitude(const Runner& runner) {
        constexpr std::size_t kProbes = 4;
        const CompressionOptions stand_in = [&] {
            auto options = make_scale_stand_in(runner.options);
            options.operation = operation_;
            return options;
        }();
        std::vector<LoadedProbe> probes;
        std::vector<std::uint64_t> own_whole;
        for (const auto& observation : runner.observations) {
            if (probes.size() >= kProbes || observation.probe >= plan_.starts.size()) {
                break;
            }
            const std::uint64_t start = plan_.starts[observation.probe];
            LoadedProbe probe = load_probe(
                start, std::min<std::uint64_t>(plan_.probe_size, layout_.bytes - start),
                runner.file_filters);
            if (probe.data.size() != observation.bytes || probe.data.size() < 4096) {
                continue;
            }
            probes.push_back(std::move(probe));
            own_whole.push_back(observation.packed);
        }
        if (probes.empty()) return 1.0;

        // Per probe: two halves with the real codec, the whole and two halves
        // with the stand-in.
        const std::size_t tasks = probes.size() * 5;
        std::vector<std::uint64_t> packed(tasks);
        const std::size_t lanes = std::min(tasks, batch_size_);
        run_parallel(lanes, [&](std::size_t lane) {
            for (std::size_t task = lane; task < tasks; task += lanes) {
                // Real-codec halves come first so they spread across lanes.
                const bool own = task < probes.size() * 2;
                const std::size_t local = own ? task : task - probes.size() * 2;
                const std::size_t probe_index = own ? local / 2 : local / 3;
                const std::size_t part = own ? local % 2 : local % 3;
                const auto& probe = probes[probe_index];
                const std::size_t half = probe.data.size() / 2;
                std::size_t offset = 0;
                std::size_t size = probe.data.size();
                if (own || part != 2) {
                    offset = part == 0 ? 0 : half;
                    size = part == 0 ? half : probe.data.size() - half;
                }
                const auto ranges = clip_ranges(probe.ranges, offset, size);
                packed[task] = encode_axar(
                    std::span<const std::uint8_t>(probe.data.data() + offset, size),
                    own ? runner.options : stand_in, ranges, runner.parallel_path);
            }
        });
        long double own_halves = 0.0L;
        long double own_wholes = 0.0L;
        long double stand_halves = 0.0L;
        long double stand_wholes = 0.0L;
        for (std::size_t index = 0; index < probes.size(); ++index) {
            own_wholes += own_whole[index];
            own_halves += packed[index * 2] + packed[index * 2 + 1];
            const std::size_t base = probes.size() * 2 + index * 3;
            stand_halves += packed[base] + packed[base + 1];
            stand_wholes += packed[base + 2];
        }
        if (!(own_halves > 0.0L) || !(stand_halves > 0.0L)) return 1.0;
        const double own_gain = static_cast<double>(own_wholes / own_halves);
        const double stand_gain = static_cast<double>(stand_wholes / stand_halves);
        // A stand-in gain too small to measure would turn noise into a large
        // exponent; so would an unbounded ratio of two small numbers.
        if (!(stand_gain < 0.99) || !(own_gain < 1.0)) return 1.0;
        return std::clamp(std::log(own_gain) / std::log(stand_gain), 0.7, 2.0);
    }

    std::optional<double> scan_long_range_duplicates(std::size_t near,
                                                     std::size_t far,
                                                     bool filters) {
        const std::size_t region = static_cast<std::size_t>(
            std::min<std::uint64_t>(
                std::min(far * 2, kMaximumDuplicateScan), layout_.bytes));
        if (region <= near) return std::nullopt;
        const std::uint64_t slots = std::max<std::uint64_t>(1, layout_.bytes / region);
        double share = 0.0;
        std::size_t scans = 0;
        std::set<std::uint64_t> used;
        for (std::size_t attempt = 1; attempt <= kMaximumDuplicateScans; ++attempt) {
            if (operation_) operation_->checkpoint();
            const std::uint64_t slot = std::min<std::uint64_t>(
                slots - 1, static_cast<std::uint64_t>(
                    radical_inverse_base_two(attempt) * slots));
            if (!used.insert(slot).second) continue;
            const LoadedProbe probe = load_probe(slot * region, region, filters);
            if (probe.data.size() <= near) continue;
            share += duplicate_share(probe, near, far);
            ++scans;
        }
        if (scans == 0) return std::nullopt;
        return share / static_cast<double>(scans);
    }

    std::optional<ScaleMeasurement> measure_scale(const CompressionOptions& unit_options,
                                        bool parallel_path,
                                        bool filters,
                                        std::size_t unit,
                                        std::size_t region,
                                        double allowance) {
        const auto began = Clock::now();
        auto whole_options = unit_options;
        whole_options.block_size = std::max(whole_options.block_size, region);
        auto piece_options = unit_options;
        piece_options.block_size = std::max(piece_options.block_size, unit);

        long double whole = 0.0L;
        long double pieces = 0.0L;
        long double repeated = 0.0L;
        long double scanned_bytes = 0.0L;
        const std::uint64_t slots = std::max<std::uint64_t>(1, layout_.bytes / region);
        // Centre a lone region when the stream holds little more than one.
        const std::uint64_t slack = layout_.bytes - std::min<std::uint64_t>(
            layout_.bytes, slots * region);
        std::set<std::uint64_t> used;
        for (std::size_t attempt = 1; attempt <= kMaximumScaleRegions; ++attempt) {
            if (operation_) operation_->checkpoint();
            const std::uint64_t slot = std::min<std::uint64_t>(
                slots - 1, static_cast<std::uint64_t>(
                    radical_inverse_base_two(attempt) * slots));
            if (!used.insert(slot).second) continue;
            const std::uint64_t start = slot * region + (slots == 1 ? slack / 2 : 0);
            const LoadedProbe probe = load_probe(start, region, filters);
            if (probe.data.size() <= unit) continue;
            const std::size_t piece_count = (probe.data.size() + unit - 1) / unit;
            std::vector<std::uint64_t> piece_packed(piece_count);
            std::uint64_t whole_packed = 0;
            // The whole region occupies one worker; the pieces share the rest.
            run_parallel(std::min(piece_count, batch_size_) + 1,
                         [&](std::size_t worker) {
                if (worker == 0) {
                    whole_packed = encode_axar(
                        probe.data, whole_options, probe.ranges, parallel_path);
                    return;
                }
                const std::size_t lanes = std::min(piece_count, batch_size_);
                for (std::size_t piece = worker - 1; piece < piece_count;
                     piece += lanes) {
                    const std::size_t offset = piece * unit;
                    const std::size_t size =
                        std::min(unit, probe.data.size() - offset);
                    const auto ranges = clip_ranges(probe.ranges, offset, size);
                    piece_packed[piece] = encode_axar(
                        std::span<const std::uint8_t>(
                            probe.data.data() + offset, size),
                        piece_options, ranges, parallel_path);
                }
            });
            whole += static_cast<long double>(whole_packed);
            for (const std::uint64_t value : piece_packed) pieces += value;
            const auto region_bytes = static_cast<long double>(probe.data.size());
            repeated += duplicate_share(probe, unit, probe.data.size()) * region_bytes;
            scanned_bytes += region_bytes;
            const double spent =
                std::chrono::duration<double>(Clock::now() - began).count();
            if (spent * static_cast<double>(attempt + 1) /
                    static_cast<double>(attempt) > allowance) {
                break;
            }
        }
        if (!(pieces > 0.0L)) return std::nullopt;
        return ScaleMeasurement{
            std::clamp(static_cast<double>(whole / pieces), 0.3, 1.0),
            scanned_bytes > 0.0L ? static_cast<double>(repeated / scanned_bytes) : 0.0};
    }

    // Exact-repeat share of one loaded region, after the executable filters
    // that turn relative branch targets into absolute ones: that is what lets
    // distant copies of code match.
    static double duplicate_share(const LoadedProbe& probe, std::size_t near,
                                  std::size_t far) {
        ByteVector filtered;
        if (!probe.ranges.empty()) {
            filtered = codec::apply_transform_ranges(
                probe.data, codec::normalize_transform_ranges(
                                probe.ranges, probe.data.size()));
        }
        return long_range_duplicate_share(
            filtered.size() == probe.data.size()
                ? std::span<const std::uint8_t>(filtered)
                : std::span<const std::uint8_t>(probe.data),
            static_cast<double>(near), static_cast<double>(far));
    }

    // ---- projection -----------------------------------------------------------

    const Runner* reference_for(const Runner& runner) const {
        // The slowest runner that visited every probe behaves most like a slow
        // candidate; prefer one using the same method.
        const Runner* best = nullptr;
        double best_cost = -1.0;
        bool best_same_method = false;
        for (const auto& other : runners_) {
            if (&other == &runner ||
                other.observations.size() != processed_probes_ ||
                other.probed_bytes == 0) {
                continue;
            }
            const bool same_method = other.options.method == runner.options.method;
            const double cost =
                other.seconds / static_cast<double>(other.probed_bytes);
            if (best == nullptr || (same_method && !best_same_method) ||
                (same_method == best_same_method && cost > best_cost)) {
                best = &other;
                best_cost = cost;
                best_same_method = same_method;
            }
        }
        return best;
    }

    RatioEstimate ratio_for(const Runner& runner) const {
        const bool covered = plan_.full_coverage &&
                             processed_probes_ == plan_.starts.size();
        if (runner.observations.size() == processed_probes_) {
            return estimate_full_ratio(runner, layout_, covered);
        }
        if (const Runner* reference = reference_for(runner)) {
            const auto linked = estimate_linked_ratio(
                runner, *reference,
                estimate_full_ratio(*reference, layout_, covered));
            if (linked.valid) return linked;
        }
        return estimate_full_ratio(runner, layout_, false);
    }

    void update_point(CompressionEstimateCurvePoint& point, const Runner& runner,
                      bool complete, bool& sampling_high) const {
        const bool warnings_present = !result_.warnings.empty();
        const unsigned recovery_percent = runner.options.recovery_percent;
        point.complete = complete;
        point.completed_probes = runner.observations.size();
        point.total_probes = plan_.starts.size();
        point.sampled_bytes = runner.probed_bytes;

        const auto with_recovery = [&](std::uint64_t bytes) {
            if (recovery_percent == 0) return bytes;
            return saturated_add(bytes, clamp_u64(
                static_cast<long double>(bytes) * recovery_percent / 100.0L));
        };
        point.provisional = false;
        if (layout_.bytes != 0 && runner.observations.empty()) {
            // Not measured yet: keep showing the file-type guess.
            const std::size_t index = static_cast<std::size_t>(&runner - runners_.data());
            fill_prior(point, candidates_[std::min(index, candidates_.size() - 1)],
                       layout_.bytes);
            point.complete = complete;
            point.total_probes = plan_.starts.size();
            sampling_high = false;
            return;
        }
        if (layout_.bytes == 0) {
            point.estimated_archive_bytes = with_recovery(runner.overhead);
            point.estimated_low_bytes = point.estimated_archive_bytes;
            point.estimated_high_bytes = point.estimated_archive_bytes;
            point.confidence = warnings_present
                ? EstimateConfidence::low : EstimateConfidence::high;
            finish_point(point);
            return;
        }
        const RatioEstimate measured = ratio_for(runner);
        if (!measured.valid) {
            point.estimated_archive_bytes =
                with_recovery(saturated_add(layout_.bytes, runner.overhead));
            point.estimated_low_bytes = with_recovery(runner.overhead);
            point.estimated_high_bytes = point.estimated_archive_bytes;
            point.estimated_margin_fraction = 1.0;
            point.confidence = EstimateConfidence::low;
            sampling_high = false;
            finish_point(point);
            return;
        }

        const double ratio = measured.ratio * runner.scale;
        // The longer-history correction is itself an estimate from a few
        // regions, less certain the further it was extended; an unmeasured
        // one is an unknown gain in the archive's favour.
        const double scale_margin = runner.scale_pending
            ? 0.10
            : (1.0 - runner.scale) * ((runner.scale_stand_in ? 0.6 : 0.3) +
                                      0.4 * runner.scale_extrapolated);
        const double payload_margin = std::sqrt(
            measured.relative_margin * measured.relative_margin +
            scale_margin * scale_margin);
        const long double payload =
            static_cast<long double>(ratio) * layout_.bytes;
        point.estimated_archive_bytes = with_recovery(
            saturated_add(clamp_u64(payload), runner.overhead));

        const double payload_share = point.estimated_archive_bytes == 0
            ? 1.0
            : std::min(1.0, static_cast<double>(
                  payload * (100.0L + recovery_percent) / 100.0L /
                  point.estimated_archive_bytes));
        const double sampling_margin = payload_margin * payload_share;
        double margin = std::sqrt(sampling_margin * sampling_margin +
                                  kModelMargin * kModelMargin);
        if (warnings_present) margin = std::max(margin, kWarningMargin);
        margin = std::min(margin, kMaximumMargin);
        point.estimated_margin_fraction = margin;
        point.confidence_margin_percent = payload_margin * ratio * 100.0;
        point.estimated_low_bytes = clamp_u64(
            static_cast<long double>(point.estimated_archive_bytes) * (1.0 - margin));
        point.estimated_high_bytes = clamp_u64(
            static_cast<long double>(point.estimated_archive_bytes) * (1.0 + margin));

        // A range is "tight" when it is narrow relative to the archive, or
        // when it is a sliver of the source: 3% +/- 1% of the original is a
        // confident answer even though the archive itself may vary by a third.
        const double source_margin = result_.source_bytes == 0
            ? margin
            : margin * static_cast<double>(point.estimated_archive_bytes) /
                  static_cast<double>(result_.source_bytes);
        const bool covered = plan_.full_coverage &&
                             processed_probes_ == plan_.starts.size();
        const bool tight =
            margin <= kHighRelativeMargin || source_margin <= kHighSourceMargin;
        const bool fair =
            margin <= kMediumRelativeMargin || source_margin <= kMediumSourceMargin;
        if (warnings_present) {
            point.confidence = EstimateConfidence::low;
        } else if (tight &&
                   (covered || processed_probes_ >= kMinimumRepresentativeProbes)) {
            point.confidence = EstimateConfidence::high;
        } else if (fair &&
                   (covered || processed_probes_ >= kMinimumMediumProbes)) {
            point.confidence = EstimateConfidence::medium;
        } else {
            point.confidence = EstimateConfidence::low;
        }
        // More probes only help a runner that is still visiting them.
        if (runner.active && point.confidence != EstimateConfidence::high) {
            sampling_high = false;
        }

        if (runner.seconds > 0.0 && runner.probed_bytes != 0) {
            point.estimated_seconds = std::max<std::uint64_t>(
                1, clamp_u64(std::ceil(projected_seconds(runner))));
        }
        finish_point(point);
    }

    // Probe timings are per coding unit, taken while a batch keeps the cores
    // as busy as the writer would; the writer runs one unit per worker.
    long double projected_seconds(const Runner& runner) const {
        double workers = 1.0;
        if (options_.format == ArchiveFormat::axar && runner.parallel_path) {
            const double block = static_cast<double>(std::min<std::uint64_t>(
                runner.geometry.solid_block_size, layout_.bytes));
            const double units_per_block = std::max(1.0, std::ceil(
                block / static_cast<double>(std::max<std::size_t>(1, runner.real_unit))));
            const double blocks = std::max(1.0, std::ceil(
                static_cast<double>(layout_.bytes) / std::max(1.0, block)));
            workers = std::min(static_cast<double>(runner.geometry.outer_workers),
                               blocks) *
                      std::min(static_cast<double>(runner.geometry.inner_threads),
                               units_per_block);
        }
        // Longer units search a deeper history; the optimal parse feels it most.
        const double growth = runner.real_unit > runner.probe_unit
            ? std::pow(static_cast<double>(runner.real_unit) /
                           static_cast<double>(std::max<std::size_t>(1, runner.probe_unit)),
                       runner.options.enable_optimal_parser ? 0.45 : 0.15)
            : 1.0;
        return runner.seconds * growth * static_cast<long double>(layout_.bytes) /
               runner.probed_bytes / std::max(1.0, workers * kWorkerEfficiency);
    }

    void finish_point(CompressionEstimateCurvePoint& point) const {
        if (result_.source_bytes != 0 && point.estimated_archive_bytes != 0) {
            point.estimated_ratio =
                static_cast<double>(result_.source_bytes) /
                static_cast<double>(point.estimated_archive_bytes);
            point.estimated_savings_percent =
                100.0 *
                (1.0 - static_cast<double>(point.estimated_archive_bytes) /
                           static_cast<double>(result_.source_bytes));
        }
    }

    void publish(bool complete) {
        result_.complete = complete;
        result_.phase = complete ? EstimatePhase::complete : EstimatePhase::measuring;
        result_.progress = window_progress(complete);
        result_.elapsed_ms = static_cast<std::uint64_t>(
            std::max(0.0, elapsed_seconds()) * 1000.0);
        result_.window_ms = window_milliseconds();
        result_.sample_coverage = layout_.bytes == 0
            ? 0.0
            : result_.sampled_bytes >= layout_.bytes
                ? 1.0
                : static_cast<double>(result_.sampled_bytes) /
                      static_cast<double>(layout_.bytes);
        bool sampling_high = true;
        for (std::size_t index = 0; index < candidates_.size(); ++index) {
            update_point(result_.points[index], runners_[index],
                         complete, sampling_high);
        }
        sampling_converged_ = sampling_high;
        calibrate_guesses();
        result_.provisional = std::all_of(
            result_.points.begin(), result_.points.end(),
            [](const CompressionEstimateCurvePoint& point) {
                return point.provisional;
            });
        result_.reached_high_confidence =
            !result_.points.empty() &&
            std::all_of(
                result_.points.begin(), result_.points.end(),
                [](const CompressionEstimateCurvePoint& point) {
                    return point.confidence == EstimateConfidence::high;
                });
        if (progress_) progress_(result_);
    }

    const std::vector<fs::path>& inputs_;
    const CompressionEstimateOptions& options_;
    std::span<const CompressionEstimateCurveCandidate> candidates_;
    const std::function<void(const CompressionEstimateCurveResult&)>& progress_;
    std::shared_ptr<OperationControl> operation_;
    unsigned retries_ = 0;
    std::size_t batch_size_ = 1;

    CompressionEstimateCurveResult result_;
    std::vector<EstimateFile> files_;
    StreamLayout layout_;
    ProbePlan plan_;
    std::vector<Runner> runners_;
    mutable std::map<std::size_t, std::vector<std::uint64_t>> solid_block_cache_;
    std::optional<std::size_t> anchor_;
    std::size_t processed_probes_ = 0;
    std::uint64_t metadata_bytes_ = 0;
    bool sampling_converged_ = false;
    std::size_t classified_files_ = 0;
    long double prior_compressible_ = 0.0L;    // guessed packed bytes
    long double prior_incompressible_ = 0.0L;  // already-packed bytes
    std::uint64_t prior_entry_bytes_ = 0;
    std::optional<Clock::time_point> last_scan_publish_;
    std::vector<codec::TransformHint> transform_hints_;
    std::vector<bool> transform_hint_known_;
    std::set<std::size_t> warned_files_;
    Clock::time_point started_;
};

void validate_estimate_request(const std::vector<fs::path>& inputs,
                               const CompressionEstimateOptions& options) {
    if (options.format != ArchiveFormat::axar && options.format != ArchiveFormat::zip) {
        throw std::invalid_argument("compression estimates support AXAR and ZIP only");
    }
    if (inputs.empty()) throw std::invalid_argument("no inputs selected for estimation");
    if (options.sample_budget == 0 || options.sample_chunk_size == 0) {
        throw std::invalid_argument("compression estimate sample sizes must be positive");
    }
}

}  // namespace

CompressionEstimateResult estimate_compression(
    const std::vector<std::filesystem::path>& inputs,
    const CompressionEstimateOptions& options) {
    validate_estimate_request(inputs, options);

    // A single prognosis is a one-point curve: both entry points share the
    // sampling plan, the statistics and therefore the numbers.
    auto engine_options = options;
    engine_options.stop_when_high_confidence = true;
    const std::array<CompressionEstimateCurveCandidate, 1> candidates{{
        {options.compression.level, options.compression}}};
    const std::function<void(const CompressionEstimateCurveResult&)> progress =
        [&](const CompressionEstimateCurveResult& curve) {
            if (!options.progress_callback || curve.points.empty()) return;
            const auto& point = curve.points.front();
            CompressionEstimateSnapshot snapshot;
            snapshot.source_bytes = curve.source_bytes;
            snapshot.phase = curve.phase;
            snapshot.provisional = point.provisional;
            snapshot.progress = curve.progress;
            snapshot.elapsed_ms = curve.elapsed_ms;
            snapshot.window_ms = curve.window_ms;
            snapshot.file_count = curve.file_count;
            snapshot.sampled_bytes = curve.sampled_bytes;
            snapshot.planned_sample_bytes = curve.planned_sample_bytes;
            snapshot.confidence = point.confidence;
            snapshot.estimated_archive_bytes = point.estimated_archive_bytes;
            snapshot.estimated_low_bytes = point.estimated_low_bytes;
            snapshot.estimated_high_bytes = point.estimated_high_bytes;
            snapshot.estimated_seconds = point.estimated_seconds;
            snapshot.estimated_margin_fraction = point.estimated_margin_fraction;
            snapshot.completed_probes = point.completed_probes;
            snapshot.total_probes = point.total_probes;
            snapshot.estimated_ratio = point.estimated_ratio;
            snapshot.estimated_savings_percent = point.estimated_savings_percent;
            options.progress_callback(snapshot);
        };
    auto curve = EstimateEngine(inputs, engine_options, candidates, progress).run();

    CompressionEstimateResult result;
    const auto& point = curve.points.front();
    result.format = curve.format;
    result.confidence = point.confidence;
    result.source_bytes = curve.source_bytes;
    result.sampled_bytes = curve.sampled_bytes;
    result.duplicate_bytes = curve.duplicate_bytes;
    result.estimated_archive_bytes = point.estimated_archive_bytes;
    result.estimated_low_bytes = point.estimated_low_bytes;
    result.estimated_high_bytes = point.estimated_high_bytes;
    result.estimated_seconds = point.estimated_seconds;
    result.file_count = curve.file_count;
    result.item_count = curve.item_count;
    result.estimated_ratio = point.estimated_ratio;
    result.estimated_savings_percent = point.estimated_savings_percent;
    result.estimated_margin_fraction = point.estimated_margin_fraction;
    result.confidence_margin_percent = point.confidence_margin_percent;
    result.sample_coverage = curve.sample_coverage;
    result.warnings = std::move(curve.warnings);
    if (options.volume_size != 0 && result.estimated_archive_bytes != 0) {
        result.volume_count = 1 +
            (result.estimated_archive_bytes - 1) / options.volume_size;
        result.final_volume_bytes = result.estimated_archive_bytes % options.volume_size;
        if (result.final_volume_bytes == 0) result.final_volume_bytes = options.volume_size;
    }
    return result;
}

CompressionEstimateCurveResult estimate_compression_curve(
    const std::vector<std::filesystem::path>& inputs,
    const CompressionEstimateOptions& options,
    std::span<const CompressionEstimateCurveCandidate> candidates,
    const std::function<void(const CompressionEstimateCurveResult&)>&
        progress_callback) {
    validate_estimate_request(inputs, options);
    if (candidates.empty()) {
        throw std::invalid_argument(
            "no compression levels selected for estimation");
    }
    return EstimateEngine(inputs, options, candidates, progress_callback).run();
}

}  // namespace axiom
