#define NOMINMAX
#include "sfx/runtime.hpp"

#include "gui/dialog_support.hpp"
#include "gui/message_dialog.hpp"
#include "sfx/sfx_dialog_ui.hpp"
#include "sfx/sfx_host.hpp"
#include "sfx/sfx_options.hpp"

#include <commctrl.h>
#include <shellapi.h>

#include <string>
#include <vector>

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
        return static_cast<int>(axiom::sfx::ExitCode::failure);
    }

    std::vector<std::wstring> arguments;
    int argument_count = 0;
    if (LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &argument_count)) {
        for (int index = 1; index < argument_count; ++index) {
            arguments.emplace_back(raw[index]);
        }
        LocalFree(raw);
    }

    axiom::sfx::HostConsole console(instance);
    axiom::sfx::DialogUi ui(instance, console,
                            axiom::gui::dialog_system_prefers_dark_mode());
    const int result = axiom::sfx::run_self_extractor(instance, arguments, ui);
    OleUninitialize();
    return result;
}
