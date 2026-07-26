#pragma once

#include "axiom/archive.hpp"

#ifdef _WIN32

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace axiom {

struct SevenZipCatalog {
    std::vector<ArchiveEntry> entries;
    bool encrypted = false;
    bool directory_encrypted = false;
};

bool seven_zip_library_available();

SevenZipCatalog seven_zip_library_list(
    const std::filesystem::path& archive_path,
    ArchiveFormat format,
    bool prefer_udf,
    const std::string& password);

void seven_zip_library_test(
    const std::filesystem::path& archive_path,
    ArchiveFormat format,
    bool prefer_udf,
    const std::string& password,
    const std::shared_ptr<OperationControl>& operation);

void seven_zip_library_extract(
    const std::filesystem::path& archive_path,
    ArchiveFormat format,
    bool prefer_udf,
    const std::vector<std::string>& selected_paths,
    const std::filesystem::path& destination,
    const std::string& password,
    const std::shared_ptr<OperationControl>& operation,
    std::uint64_t total_bytes,
    std::uint64_t total_items);

}  // namespace axiom

#endif
