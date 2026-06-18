#include "gui/app.hpp"

#include <shellapi.h>
#include <utility>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    int argument_count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    std::wstring initial_path;
    if (arguments != nullptr && argument_count > 1) initial_path = arguments[1];
    if (arguments != nullptr) LocalFree(arguments);
    return run_axiom_gui(instance, show_command, std::move(initial_path));
}
