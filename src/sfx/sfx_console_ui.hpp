#pragma once

#include "sfx/sfx_host.hpp"
#include "sfx/sfx_ui.hpp"

namespace axiom::sfx {

// Console-backed interaction for the mini stub. It never prompts: anything the
// dialog tier would ask for has to be supplied on the command line or by the
// embedded configuration, and a missing answer is a clean error rather than a
// process that waits forever for input nobody is watching for.
class ConsoleUi final : public SfxUi {
public:
    explicit ConsoleUi(HostConsole& console) : console_(console) {}

    bool supports_prompts() const override { return false; }
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
    HostConsole& console_;
};

}  // namespace axiom::sfx
