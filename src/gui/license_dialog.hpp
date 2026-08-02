#pragma once

#include <windows.h>

#include <string>

namespace axiom::gui {

// Modal license gate shown by a self-extractor configured with require_accept.
// Returns true only when the user actively accepts: the accept control starts
// cleared and the continue button starts disabled, so dismissing the window in
// any other way declines.
//
// `dark` is the caller's resolved appearance, which may come from the package's
// `theme` setting rather than from the system.
bool show_license_dialog(HWND owner,
                         HINSTANCE instance,
                         const std::wstring& title,
                         const std::wstring& license_text,
                         bool dark);

}  // namespace axiom::gui
