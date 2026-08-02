#pragma once

// Read-only archive access for self-extracting runtimes. Keeping this boundary
// separate from ArchiveProvider prevents a small SFX stub from inheriting the
// archive creation/update/delete registry and mutation vtables.

#include "axiom/archive.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace axiom::sfx {

enum class SfxArchiveFormat {
    axar,
    zip,
};

class SfxArchiveReader final {
public:
    // Returns nullopt when the image has no valid SFX payload or the payload is
    // neither an AXAR nor a ZIP archive. Structural errors are deferred to the
    // selected reader so callers get the format-specific diagnostic.
    static std::optional<SfxArchiveReader> open(
        const std::filesystem::path& executable);

    SfxArchiveFormat format() const { return format_; }
    bool native() const { return format_ == SfxArchiveFormat::axar; }

    ArchiveCapabilities capabilities(const std::string& password = {}) const;
    std::vector<ArchiveEntry> list(const std::string& password = {}) const;
    void test(const DecompressionOptions& options = {}) const;
    void extract_all(const std::filesystem::path& destination,
                     const ExtractOptions& options = {}) const;
    void extract_selected(const std::vector<std::string>& entries,
                          const std::filesystem::path& destination,
                          const ExtractOptions& options = {}) const;

    ArchiveSignatureInfo signature(const std::string& password = {}) const;
    std::string comment(const std::string& password = {}) const;

private:
    SfxArchiveReader(std::filesystem::path executable,
                     SfxArchiveFormat format,
                     std::uint64_t payload_offset,
                     std::uint64_t payload_size)
        : executable_(std::move(executable)),
          format_(format),
          payload_offset_(payload_offset),
          payload_size_(payload_size) {}

    std::filesystem::path executable_;
    SfxArchiveFormat format_ = SfxArchiveFormat::axar;
    std::uint64_t payload_offset_ = 0;
    std::uint64_t payload_size_ = 0;
};

}  // namespace axiom::sfx
