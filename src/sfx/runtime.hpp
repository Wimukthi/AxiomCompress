#pragma once

#include <windows.h>

#include <optional>
#include <string>

namespace axiom::sfx {

// Runs only when the current executable contains the fixed AXIOMSFX trailer.
// A missing trailer returns nullopt so Axiom.exe can continue normal startup.
std::optional<int> run_embedded(
    HINSTANCE instance,
    const std::wstring& requested_destination = {});

}  // namespace axiom::sfx
