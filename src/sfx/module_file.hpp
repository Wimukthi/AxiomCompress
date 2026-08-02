#pragma once

#include "axiom/archive.hpp"
#include "sfx/sfx_config.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace axiom::sfx {

// Package an archive with the non-executable SFX module installed beside the
// calling GUI or CLI. The module is read only when SFX creation is requested,
// so normal application startup does not map or copy its image.
void create_from_module_file(
    HINSTANCE application_module,
    const std::filesystem::path& archive_path,
    const std::filesystem::path& output_executable,
    const std::shared_ptr<OperationControl>& operation = nullptr,
    std::size_t io_buffer_size = 0,
    std::span<const std::uint8_t> config = {},
    SfxStubTier tier = SfxStubTier::full);

std::filesystem::path module_file_path(HINSTANCE application_module,
                                       SfxStubTier tier = SfxStubTier::full);

}  // namespace axiom::sfx
