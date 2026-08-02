#pragma once

#include <windows.h>

#include <span>
#include <string>

namespace axiom::sfx {

class SfxUi;

// Runs the extractor embedded in the current executable. `arguments` excludes
// argv[0]. Returns one of the documented ExitCode values; a file with no
// embedded payload reports ExitCode::failure after explaining itself.
//
// All interaction goes through `ui`, so the same runtime drives the dialog stub
// and the console-only mini stub.
int run_self_extractor(HINSTANCE instance,
                       std::span<const std::wstring> arguments, SfxUi& ui);

}  // namespace axiom::sfx
