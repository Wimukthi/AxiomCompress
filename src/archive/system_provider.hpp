#pragma once

#include "axiom/archive.hpp"

#include <filesystem>

namespace axiom {

const ArchiveProvider* system_archive_provider_for_path(const std::filesystem::path& path);

}  // namespace axiom
