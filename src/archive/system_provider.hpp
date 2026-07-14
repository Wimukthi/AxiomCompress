#pragma once

#include "axiom/archive.hpp"

#include <filesystem>

namespace axiom {

const ArchiveProvider* system_archive_provider_for_contents(
    const std::filesystem::path& path);
const ArchiveProvider* system_archive_provider_for_extension(
    const std::filesystem::path& path);

}  // namespace axiom
