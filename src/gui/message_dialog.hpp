#pragma once

#include <windows.h>

#include <string_view>

namespace axiom::gui {

enum class MessageDialogIcon {
    none,
    information,
    warning,
    error,
    question,
};

enum class MessageDialogButtons {
    ok,
    ok_cancel,
    yes_no,
    yes_no_cancel,
};

int show_message_dialog(
    HWND owner,
    HINSTANCE instance,
    UINT dpi,
    bool dark,
    std::wstring_view title,
    std::wstring_view message,
    MessageDialogIcon icon = MessageDialogIcon::information,
    MessageDialogButtons buttons = MessageDialogButtons::ok,
    int default_result = IDOK);

}  // namespace axiom::gui

