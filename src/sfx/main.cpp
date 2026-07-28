#define NOMINMAX
#include "sfx/runtime.hpp"

#include "gui/dialog_support.hpp"
#include "gui/message_dialog.hpp"

#include <commctrl.h>
#include <shellapi.h>

#include <string>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    if (!SetProcessDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        SetProcessDPIAware();
    }

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    const HRESULT com = OleInitialize(nullptr);
    if (FAILED(com)) {
        axiom::gui::show_message_dialog(
            nullptr, instance, GetDpiForSystem(),
            axiom::gui::dialog_system_prefers_dark_mode(),
            L"Axiom Self-Extractor", L"Failed to initialize Windows COM.",
            axiom::gui::MessageDialogIcon::error);
        return 1;
    }

    int argument_count = 0;
    LPWSTR* arguments =
        CommandLineToArgvW(GetCommandLineW(), &argument_count);
    const std::wstring destination =
        arguments != nullptr && argument_count > 1 ? arguments[1] : L"";
    if (arguments != nullptr) LocalFree(arguments);

    const auto result = axiom::sfx::run_embedded(instance, destination);
    if (!result.has_value()) {
        axiom::gui::show_message_dialog(
            nullptr, instance, GetDpiForSystem(),
            axiom::gui::dialog_system_prefers_dark_mode(),
            L"Axiom Self-Extractor",
            L"This file does not contain a valid embedded archive.",
            axiom::gui::MessageDialogIcon::error);
    }
    OleUninitialize();
    return result.value_or(1);
}
