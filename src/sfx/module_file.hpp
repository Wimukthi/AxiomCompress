#pragma once

#include "axiom/archive.hpp"

#include <windows.h>

#include <cstddef>
#include <filesystem>
#include <memory>

namespace axiom::sfx {

// Package an archive with the non-executable AxiomSfx.bin module installed
// beside the calling GUI or CLI. The module is read only when SFX creation is
// requested, so normal application startup does not map or copy its image.
void create_from_module_file(
    HINSTANCE application_module,
    const std::filesystem::path& archive_path,
    const std::filesystem::path& output_executable,
    const std::shared_ptr<OperationControl>& operation = nullptr,
    std::size_t io_buffer_size = 0);

std::filesystem::path module_file_path(HINSTANCE application_module);

}  // namespace axiom::sfx
