#pragma once

// One wording, one set of units and one sampling configuration for every place
// the GUI shows a compression estimate (the create-archive preview and the
// Information dialog). Both render the lines produced here verbatim.

#include "axiom/archive.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace axiom::gui {

// The figures an estimate display needs, independent of which estimator entry
// point produced them.
struct EstimateFigures {
    std::uint64_t source_bytes = 0;
    std::uint64_t archive_bytes = 0;
    std::uint64_t low_bytes = 0;
    std::uint64_t high_bytes = 0;
    std::uint64_t seconds = 0;
    std::uint64_t sampled_bytes = 0;
    std::uint64_t planned_sample_bytes = 0;
    std::uint64_t duplicate_bytes = 0;
    std::size_t warnings = 0;
    std::uint64_t file_count = 0;
    std::uint64_t elapsed_ms = 0;
    std::uint64_t window_ms = 0;
    double margin_fraction = 0.0;
    double progress = 0.0;
    EstimateConfidence confidence = EstimateConfidence::low;
    EstimatePhase phase = EstimatePhase::scanning;
    // A guess from file types; nothing has been compressed yet.
    bool provisional = false;
    bool complete = false;
};

struct EstimatePresentation {
    // "24.6 MB  •  32.4% of original  •  67.6% saved"
    std::wstring headline;
    // "±3% (23.9 MB – 25.3 MB)  •  High confidence"
    std::wstring range;
    // "3.09x  •  about 12s  •  sampled 16.0 MB"
    std::wstring detail;
    // "Refining  •  4s of up to 12s" or "Done in 3s  •  High confidence"
    std::wstring status;
    // Archive size as a fraction of the source, clamped to [0, 1] for meters.
    double compressed_fraction = 1.0;
    bool grows = false;
};

EstimateFigures estimate_figures(const CompressionEstimateResult& result);
EstimateFigures estimate_figures(const CompressionEstimateSnapshot& snapshot);
EstimateFigures estimate_figures(const CompressionEstimateCurveResult& curve,
                                 const CompressionEstimateCurvePoint& point);

EstimatePresentation present_estimate(const EstimateFigures& figures);

// Progress to draw for a running estimate: the engine's own figure, advanced
// by the time since it was published so the bar moves between updates.
// Never reaches 1 before the run completes.
double displayed_estimate_progress(const EstimateFigures& figures,
                                   double seconds_since_update);

// The figures with the elapsed time advanced to now, so the status line's
// clock keeps moving between updates of a running estimate.
EstimateFigures live_estimate_figures(EstimateFigures figures,
                                      double seconds_since_update);

std::wstring format_estimate_bytes(std::uint64_t bytes);
std::wstring format_estimate_percent(double value);
std::wstring format_estimate_duration(std::uint64_t seconds);

// Sampling limits shared by every estimate display, so the same files and
// settings produce the same figures wherever they are shown. candidate_count
// is the number of codec levels measured in one run.
CompressionEstimateOptions gui_estimate_options(ArchiveFormat format,
                                                std::size_t candidate_count);

}  // namespace axiom::gui
