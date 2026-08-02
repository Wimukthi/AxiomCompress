#include "sfx/sfx_console_ui.hpp"

namespace axiom::sfx {

bool ConsoleUi::ask_password(std::wstring&) { return false; }

bool ConsoleUi::ask_license(const std::wstring&, const std::wstring& text) {
    // The terms are still shown, so an operator running this by hand can read
    // what they are being asked to accept with -y. Acceptance itself is never
    // implied here.
    console_.write(text + L"\n");
    return false;
}

bool ConsoleUi::ask_options(const SfxSummary&, SfxChoices&) { return false; }

void ConsoleUi::run_with_progress(const std::shared_ptr<OperationControl>&,
                                  const std::function<void()>& work) {
    work();
}

void ConsoleUi::message(const std::wstring&, const std::wstring& text, bool) {
    console_.write(text + L"\n");
}

void ConsoleUi::reveal(const std::filesystem::path&) {}

void ConsoleUi::write(const std::wstring& text) { console_.write(text); }

void ConsoleUi::flush(const std::wstring&, bool) {}

}  // namespace axiom::sfx
