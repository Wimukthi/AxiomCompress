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
    // Caption for the extractor window. Empty uses the Axiom default; a
    // package that configured a title expects to see it here, not only on
    // the message boxes.
    std::wstring window_title;
    // Replaces the "Extract <file>" heading when set.
    std::wstring banner_text;
    // The package author's own description, shown in place of the archive
    // comment when both exist.
    std::wstring description;
    // Appearance resolved by the caller. When theme_forced is false the dialog
    // follows the system and keeps following it across theme changes.
    bool dark = false;
    bool theme_forced = false;
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
    // Cleared by a package whose author fixed the destination; the path edit
    // and the browse button are then shown but not editable.
    bool allow_path_change = true;
};

bool show_sfx_extract_dialog(HWND owner,
                             HINSTANCE instance,
                             const SfxArchiveSummary& summary,
                             SfxExtractDialogOptions& options);

}  // namespace axiom::gui
