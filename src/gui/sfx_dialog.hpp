#pragma once

#include "axiom/archive.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace axiom::gui {

struct SfxArchiveSummary {
    std::wstring archive_name;
    std::size_t file_count = 0;
    std::size_t directory_count = 0;
    std::uint64_t unpacked_size = 0;
    bool encrypted = false;
    bool signature_present = false;
    bool signature_valid = false;
    std::wstring comment;
};

struct SfxExtractDialogOptions {
    std::filesystem::path destination;
    ExtractOptions::Overwrite overwrite = ExtractOptions::Overwrite::overwrite;
    std::size_t thread_count = 0;
    bool restore_mtime = true;
    bool open_destination = true;
};

bool show_sfx_extract_dialog(HWND owner,
                             HINSTANCE instance,
                             const SfxArchiveSummary& summary,
                             SfxExtractDialogOptions& options);

}  // namespace axiom::gui
