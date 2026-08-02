#define NOMINMAX
#include "sfx/runtime.hpp"

#include "sfx/sfx_console_ui.hpp"
#include "sfx/sfx_host.hpp"
#include "sfx/sfx_options.hpp"

#include <objbase.h>
#include <shellapi.h>

#include <string>
#include <vector>

// The mini self-extractor: the same runtime, driven from the console instead of
// from dialogs. It links none of the Win32 dialog stack, which is where the
// size difference against the full stub comes from. Everything the full stub
// would prompt for must arrive on the command line or in the embedded config.
int wmain(int argument_count, wchar_t** argument_values) {
    // COM is still needed: SHGetKnownFolderPath backs the destination
    // templates, and ShellExecuteEx backs run-after-extract and elevation.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) {
        return static_cast<int>(axiom::sfx::ExitCode::failure);
    }

    std::vector<std::wstring> arguments;
    for (int index = 1; index < argument_count; ++index) {
        arguments.emplace_back(argument_values[index]);
    }

    axiom::sfx::HostConsole console(nullptr);
    axiom::sfx::ConsoleUi ui(console);
    const int result =
        axiom::sfx::run_self_extractor(GetModuleHandleW(nullptr), arguments, ui);
    CoUninitialize();
    return result;
}
