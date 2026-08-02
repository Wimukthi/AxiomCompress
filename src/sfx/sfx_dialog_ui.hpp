#pragma once

#include "sfx/sfx_host.hpp"
#include "sfx/sfx_ui.hpp"

#include <windows.h>

#include <optional>

namespace axiom::sfx {

// Extractor entry point for a binary that is normally something else. Returns
// nullopt when this executable carries no payload, so Axiom.exe can fall
// through to starting the file manager. Lives here because it constructs the
// dialog-backed UI.
std::optional<int> run_embedded_if_present(
    HINSTANCE instance, const std::wstring& requested_destination = {});

// Dialog-backed interaction for the full stub. This is the only translation
// unit in the extractor that reaches into axiom::gui, which is what lets the
// mini stub link the same runtime without the Win32 dialog stack.
class DialogUi final : public SfxUi {
public:
    DialogUi(HINSTANCE instance, HostConsole& console, bool dark)
        : instance_(instance), console_(console), dark_(dark) {}

    bool supports_prompts() const override { return true; }
    // A package that pinned an appearance keeps it; otherwise the extractor
    // follows the system, including across theme changes while it is open.
    void set_theme(bool dark) override {
        dark_ = dark;
        theme_forced_ = true;
    }
    bool ask_password(std::wstring& password) override;
    bool ask_license(const std::wstring& title, const std::wstring& text) override;
    bool ask_options(const SfxSummary& summary, SfxChoices& choices) override;
    void run_with_progress(const std::shared_ptr<OperationControl>& operation,
                           const std::function<void()>& work) override;
    void message(const std::wstring& title, const std::wstring& text,
                 bool error) override;
    void reveal(const std::filesystem::path& destination) override;
    void write(const std::wstring& text) override;
    void flush(const std::wstring& title, bool error) override;

private:
    HINSTANCE instance_{};
    HostConsole& console_;
    bool dark_ = false;
    bool theme_forced_ = false;
};

}  // namespace axiom::sfx
