#pragma once

#include "axiom/archive.hpp"

#include <windows.h>

#include <filesystem>
#include <vector>

namespace axiom::gui {

void show_filesystem_information_dialog(
    HWND owner,
    std::vector<std::filesystem::path> inputs,
    CompressionEstimateOptions estimate_options,
    int compression_level);

}  // namespace axiom::gui
