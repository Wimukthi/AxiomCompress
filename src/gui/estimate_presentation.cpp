#include "gui/estimate_presentation.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace axiom::gui {
namespace {

constexpr wchar_t kSeparator[] = L"  •  ";

const wchar_t* confidence_label(EstimateConfidence confidence) {
    switch (confidence) {
        case EstimateConfidence::high: return L"High confidence";
        case EstimateConfidence::medium: return L"Medium confidence";
        case EstimateConfidence::low: return L"Low confidence";
    }
    return L"Low confidence";
}

std::wstring format_count(std::uint64_t value) {
    std::wstring digits = std::to_wstring(value);
    for (std::size_t position = digits.size(); position > 3; position -= 3) {
        digits.insert(position - 3, 1, L',');
    }
    return digits;
}

std::wstring format_fixed(double value, int precision) {
    std::wostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

}  // namespace

std::wstring format_estimate_bytes(std::uint64_t bytes) {
    // Same units and rounding as the file list: binary steps, short labels.
    static constexpr const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    return format_fixed(value, unit == 0 ? 0 : 1) + L' ' + units[unit];
}

std::wstring format_estimate_percent(double value) {
    return format_fixed(value, 1) + L'%';
}

std::wstring format_estimate_duration(std::uint64_t seconds) {
    const std::uint64_t hours = seconds / 3600;
    const std::uint64_t minutes = (seconds % 3600) / 60;
    const std::uint64_t remainder = seconds % 60;
    std::wostringstream stream;
    if (hours != 0) stream << hours << L"h ";
    if (hours != 0 || minutes != 0) stream << minutes << L"m ";
    stream << remainder << L's';
    return stream.str();
}

EstimateFigures estimate_figures(const CompressionEstimateResult& result) {
    EstimateFigures figures;
    figures.source_bytes = result.source_bytes;
    figures.archive_bytes = result.estimated_archive_bytes;
    figures.low_bytes = result.estimated_low_bytes;
    figures.high_bytes = result.estimated_high_bytes;
    figures.seconds = result.estimated_seconds;
    figures.sampled_bytes = result.sampled_bytes;
    figures.duplicate_bytes = result.duplicate_bytes;
    figures.warnings = result.warnings.size();
    figures.margin_fraction = result.estimated_margin_fraction;
    figures.confidence = result.confidence;
    figures.file_count = result.file_count;
    figures.phase = EstimatePhase::complete;
    figures.progress = 1.0;
    figures.complete = true;
    return figures;
}

EstimateFigures estimate_figures(const CompressionEstimateSnapshot& snapshot) {
    EstimateFigures figures;
    figures.source_bytes = snapshot.source_bytes;
    figures.archive_bytes = snapshot.estimated_archive_bytes;
    figures.low_bytes = snapshot.estimated_low_bytes;
    figures.high_bytes = snapshot.estimated_high_bytes;
    figures.seconds = snapshot.estimated_seconds;
    figures.sampled_bytes = snapshot.sampled_bytes;
    figures.planned_sample_bytes = snapshot.planned_sample_bytes;
    figures.margin_fraction = snapshot.estimated_margin_fraction;
    figures.confidence = snapshot.confidence;
    figures.file_count = snapshot.file_count;
    figures.elapsed_ms = snapshot.elapsed_ms;
    figures.window_ms = snapshot.window_ms;
    figures.progress = snapshot.progress;
    figures.phase = snapshot.phase;
    figures.provisional = snapshot.provisional;
    figures.complete = snapshot.phase == EstimatePhase::complete;
    return figures;
}

EstimateFigures estimate_figures(const CompressionEstimateCurveResult& curve,
                                 const CompressionEstimateCurvePoint& point) {
    EstimateFigures figures;
    figures.source_bytes = curve.source_bytes;
    figures.archive_bytes = point.estimated_archive_bytes;
    figures.low_bytes = point.estimated_low_bytes;
    figures.high_bytes = point.estimated_high_bytes;
    figures.seconds = point.estimated_seconds;
    figures.sampled_bytes = curve.sampled_bytes;
    figures.planned_sample_bytes = curve.planned_sample_bytes;
    figures.duplicate_bytes = curve.duplicate_bytes;
    figures.warnings = curve.warnings.size();
    figures.margin_fraction = point.estimated_margin_fraction;
    figures.confidence = point.confidence;
    figures.file_count = curve.file_count;
    figures.elapsed_ms = curve.elapsed_ms;
    figures.window_ms = curve.window_ms;
    figures.progress = curve.progress;
    figures.phase = curve.phase;
    figures.provisional = point.provisional;
    figures.complete = curve.complete;
    return figures;
}

EstimatePresentation present_estimate(const EstimateFigures& figures) {
    EstimatePresentation text;
    const double share = figures.source_bytes == 0
        ? 1.0
        : static_cast<double>(figures.archive_bytes) /
              static_cast<double>(figures.source_bytes);
    text.grows = share > 1.0;
    text.compressed_fraction = std::clamp(share, 0.0, 1.0);

    text.headline = format_estimate_bytes(figures.archive_bytes);
    if (figures.source_bytes != 0) {
        text.headline += kSeparator + format_estimate_percent(share * 100.0) +
                         L" of original" + kSeparator;
        text.headline += text.grows
            ? format_estimate_percent((share - 1.0) * 100.0) + L" larger"
            : format_estimate_percent((1.0 - share) * 100.0) + L" saved";
    }

    // Round the margin up so the stated range never looks tighter than it is.
    const double margin_percent = std::ceil(figures.margin_fraction * 100.0 - 1e-9);
    text.range = L"±" + format_fixed(std::max(0.0, margin_percent), 0) + L"% (" +
                 format_estimate_bytes(figures.low_bytes) + L" – " +
                 format_estimate_bytes(figures.high_bytes) + L")" + kSeparator +
                 (figures.provisional ? L"First guess from file types"
                                      : confidence_label(figures.confidence));

    if (figures.archive_bytes != 0 && figures.source_bytes != 0) {
        text.detail = format_fixed(1.0 / share, 2) + L"x";
    }
    const auto append = [&](const std::wstring& part) {
        if (!text.detail.empty()) text.detail += kSeparator;
        text.detail += part;
    };
    if (figures.seconds != 0) {
        append(L"about " + format_estimate_duration(figures.seconds));
    }
    if (figures.provisional) {
        append(L"nothing measured yet");
    } else {
        std::wstring sampled =
            L"sampled " + format_estimate_bytes(figures.sampled_bytes);
        if (!figures.complete &&
            figures.planned_sample_bytes > figures.sampled_bytes) {
            sampled += L" of " + format_estimate_bytes(figures.planned_sample_bytes);
        }
        append(sampled);
    }
    if (figures.duplicate_bytes != 0) {
        append(format_estimate_bytes(figures.duplicate_bytes) + L" duplicates");
    }
    if (figures.warnings != 0) {
        append(std::to_wstring(figures.warnings) +
               (figures.warnings == 1 ? L" warning" : L" warnings"));
    }

    const auto seconds_text = [](std::uint64_t milliseconds) {
        return format_estimate_duration((milliseconds + 500) / 1000);
    };
    switch (figures.phase) {
        case EstimatePhase::scanning:
            text.status = L"Reading the file list  •  " +
                          format_count(figures.file_count) +
                          (figures.file_count == 1 ? L" file" : L" files");
            break;
        case EstimatePhase::measuring:
            text.status = figures.provisional ? L"Measuring samples"
                                              : L"Refining";
            if (figures.window_ms != 0) {
                text.status += kSeparator + seconds_text(figures.elapsed_ms) +
                               L" of up to " + seconds_text(figures.window_ms);
            }
            break;
        case EstimatePhase::complete:
            text.status = figures.elapsed_ms >= 1000
                ? L"Done in " + seconds_text(figures.elapsed_ms)
                : std::wstring(L"Done");
            if (figures.window_ms != 0 &&
                figures.elapsed_ms + 250 < figures.window_ms &&
                figures.confidence == EstimateConfidence::high) {
                text.status += L", confident before the time limit";
            } else if (figures.window_ms != 0 &&
                       figures.elapsed_ms + 250 >= figures.window_ms) {
                text.status += L", time limit reached";
            }
            break;
    }
    return text;
}

EstimateFigures live_estimate_figures(EstimateFigures figures,
                                      double seconds_since_update) {
    if (figures.phase != EstimatePhase::measuring) return figures;
    figures.elapsed_ms += static_cast<std::uint64_t>(
        std::max(0.0, seconds_since_update) * 1000.0);
    if (figures.window_ms != 0) {
        figures.elapsed_ms = std::min(figures.elapsed_ms, figures.window_ms);
    }
    return figures;
}

double displayed_estimate_progress(const EstimateFigures& figures,
                                   double seconds_since_update) {
    if (figures.complete || figures.phase == EstimatePhase::complete) return 1.0;
    if (figures.phase == EstimatePhase::scanning) return 0.0;
    double progress = figures.progress;
    if (figures.window_ms != 0) {
        const double elapsed = static_cast<double>(figures.elapsed_ms) / 1000.0 +
                               std::max(0.0, seconds_since_update);
        progress = std::max(
            progress, elapsed / (static_cast<double>(figures.window_ms) / 1000.0));
    }
    return std::clamp(progress, 0.0, 0.99);
}

CompressionEstimateOptions gui_estimate_options(ArchiveFormat format,
                                                std::size_t candidate_count) {
    CompressionEstimateOptions options;
    options.format = format;
    // Adaptive ceilings, not fixed costs: sampling stops as soon as every
    // measured level reaches high confidence, or when the time window closes.
    // The byte ceiling is set so that time, not bytes, is what normally ends
    // sampling: fast levels read 100 MB of probes in seconds, and on Silesia a
    // 32 MB ceiling left them idle after 8 s with a +/-18% range, where 96 MB
    // gave +/-5% (actual error 1.3-1.5%) in the same window.
    options.sample_budget = std::size_t{128} << 20;
    options.sample_chunk_size = std::size_t{1} << 20;
    options.time_budget = std::chrono::seconds(std::clamp<long long>(
        4 * static_cast<long long>(std::max<std::size_t>(1, candidate_count)), 6, 30));
    options.stop_when_high_confidence = true;
    return options;
}

}  // namespace axiom::gui
