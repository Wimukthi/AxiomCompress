#pragma once

#include <windows.h>
#include <string>

int run_axiom_gui(HINSTANCE instance, int show_command, std::wstring initial_path = {});
