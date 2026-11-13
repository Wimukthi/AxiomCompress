#pragma once

#include <filesystem>
#include <system_error>
#include <unordered_set>

namespace axiom::core {

// fs::create_directories, at most once per directory over one extraction.
//
// Called on a parent that already exists, create_directories still tries each
// level from the root down, and on Windows that cost about 440 us per extracted
// file, more than writing a small file. A directory created or found here is
// only ever made a real directory by the extraction itself, so it is not asked
// again; a caller that removes or replaces directories must clear() first.
class EnsuredDirectories {
public:
    void ensure(const std::filesystem::path& directory, std::error_code& error) {
        error.clear();
        if (ensured_.contains(directory.native())) return;
        std::filesystem::create_directories(directory, error);
        if (!error) ensured_.insert(directory.native());
    }

    void clear() noexcept { ensured_.clear(); }

private:
    std::unordered_set<std::filesystem::path::string_type> ensured_;
};

}  // namespace axiom::core
