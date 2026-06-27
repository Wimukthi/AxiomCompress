#include "gui/app.hpp"

#include <shellapi.h>
#include <cwchar>
#include <utility>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    int argument_count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    std::wstring initial_path;
    AxiomGuiStartupCommand startup;
    if (arguments != nullptr && argument_count > 1) {
        const std::wstring first = arguments[1];
        if (_wcsicmp(first.c_str(), L"--add") == 0) {
            startup.kind = AxiomGuiStartupCommand::Kind::add_to_archive;
            for (int index = 2; index < argument_count; ++index) {
                startup.paths.emplace_back(arguments[index]);
            }
        } else if (_wcsicmp(first.c_str(), L"--extract") == 0 && argument_count > 2) {
            startup.kind = AxiomGuiStartupCommand::Kind::extract_archive;
            startup.paths.emplace_back(arguments[2]);
            initial_path = arguments[2];
        } else if (_wcsicmp(first.c_str(), L"--test") == 0 && argument_count > 2) {
            startup.kind = AxiomGuiStartupCommand::Kind::test_archive;
            startup.paths.emplace_back(arguments[2]);
            initial_path = arguments[2];
        } else {
            initial_path = first;
        }
    }
    if (arguments != nullptr) LocalFree(arguments);
    return run_axiom_gui(instance, show_command, std::move(initial_path), std::move(startup));
}
