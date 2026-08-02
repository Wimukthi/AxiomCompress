#pragma once

#include "axiom/axiom.hpp"

#include <chrono>

namespace axiom::codec {

// Phase scopes keep timing out of the codec's inner loops. The callback is
// copied through CompressionOptions into worker tasks, so the scope only keeps
// a pointer to the live option field and cannot extend its lifetime.
class CompressionTelemetryScope {
public:
    CompressionTelemetryScope(const CompressionOptions& options,
                              CompressionTelemetryPhase phase,
                              std::size_t input_bytes)
        : callback_(&options.compression_telemetry), phase_(phase),
          input_bytes_(input_bytes) {
        // Keep the normal path to a callback check; clock reads are only
        // needed when a caller explicitly enables profiling.
        if (*callback_) {
            started_ = clock::now();
        }
    }

    ~CompressionTelemetryScope() noexcept {
        if (callback_ == nullptr || !*callback_) {
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock::now() - started_);
        try {
            (*callback_)({phase_, static_cast<std::uint64_t>(elapsed.count()),
                          static_cast<std::uint64_t>(input_bytes_)});
        } catch (...) {
            // Diagnostics must never turn a successful compression into a
            // failed operation, including while unwinding another exception.
        }
    }

    CompressionTelemetryScope(const CompressionTelemetryScope&) = delete;
    CompressionTelemetryScope& operator=(const CompressionTelemetryScope&) = delete;

private:
    using clock = std::chrono::steady_clock;

    const CompressionTelemetryCallback* callback_ = nullptr;
    CompressionTelemetryPhase phase_ = CompressionTelemetryPhase::block_total;
    std::size_t input_bytes_ = 0;
    clock::time_point started_;
};

}  // namespace axiom::codec
