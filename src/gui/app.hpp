#pragma once

#include <windows.h>
#include <string>
#include <vector>

struct AxiomGuiStartupCommand {
    enum class Kind {
        normal,
        add_to_archive,
        extract_archive,
        test_archive,
    };

    Kind kind = Kind::normal;
    std::vector<std::wstring> paths;
};

int run_axiom_gui(HINSTANCE instance,
                  int show_command,
                  std::wstring initial_path = {},
                  AxiomGuiStartupCommand startup_command = {});
