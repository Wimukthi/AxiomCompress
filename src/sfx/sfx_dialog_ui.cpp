#define NOMINMAX
#include "sfx/sfx_dialog_ui.hpp"

#include "gui/archive_feature_dialogs.hpp"
#include "gui/dialog_support.hpp"
#include "gui/license_dialog.hpp"
#include "gui/message_dialog.hpp"
#include "gui/operation_progress_window.hpp"
#include "gui/sfx_dialog.hpp"
#include "archive/sfx_image.hpp"
#include "sfx/runtime.hpp"

#include <shellapi.h>

#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>

namespace axiom::sfx {
namespace {

axiom::gui::OperationWindowTheme operation_theme(bool dark) {
    const auto colors = axiom::gui::dialog_colors(dark);
    return {dark,
            colors.background,
            colors.control_background,
            colors.text,
            colors.disabled_text,
            colors.border,
            colors.control_background,
            colors.selection_background,
            colors.border,
            colors.control_background,
            axiom::gui::dialog_accent_color()};
}

}  // namespace

bool DialogUi::ask_password(std::wstring& password) {
    return axiom::gui::show_archive_password_dialog(nullptr, password);
}

bool DialogUi::ask_license(const std::wstring& title, const std::wstring& text) {
    return axiom::gui::show_license_dialog(nullptr, instance_, title, text, dark_);
}

bool DialogUi::ask_options(const SfxSummary& summary, SfxChoices& choices) {
    axiom::gui::SfxArchiveSummary dialog_summary;
    dialog_summary.archive_name = summary.archive_name;
    dialog_summary.window_title = summary.window_title;
    dialog_summary.banner_text = summary.banner_text;
    dialog_summary.description = summary.description;
    dialog_summary.dark = dark_;
    dialog_summary.theme_forced = theme_forced_;
    dialog_summary.file_count = summary.file_count;
    dialog_summary.directory_count = summary.directory_count;
    dialog_summary.unpacked_size = summary.unpacked_size;
    dialog_summary.encrypted = summary.encrypted;
    dialog_summary.signature_present = summary.signature_present;
    dialog_summary.signature_valid = summary.signature_valid;
    dialog_summary.comment = summary.comment;

    axiom::gui::SfxExtractDialogOptions options;
    options.destination = choices.destination;
    options.overwrite = choices.overwrite;
    options.thread_count = choices.thread_count;
    options.restore_mtime = choices.restore_mtime;
    options.open_destination = choices.open_destination;
    options.allow_path_change = choices.allow_path_change;
    if (!axiom::gui::show_sfx_extract_dialog(nullptr, instance_, dialog_summary,
                                             options)) {
        return false;
    }
    choices.destination = options.destination;
    choices.overwrite = options.overwrite;
    choices.thread_count = options.thread_count;
    choices.restore_mtime = options.restore_mtime;
    choices.open_destination = options.open_destination;
    return true;
}

void DialogUi::run_with_progress(const std::shared_ptr<OperationControl>& operation,
                                 const std::function<void()>& work) {
    axiom::gui::OperationProgressWindow progress_window;
    if (!progress_window.create(
            nullptr, instance_, L"Extracting archive", {}, operation_theme(dark_),
            [operation](bool paused) { operation->set_paused(paused); },
            [operation] { operation->request_cancel(); })) {
        throw std::runtime_error("cannot create the extraction progress window");
    }
    progress_window.set_progress_source(operation);

    std::atomic_bool completed = false;
    std::jthread worker([&] {
        work();
        completed.store(true, std::memory_order_release);
    });
    while (!completed.load(std::memory_order_acquire)) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                operation->request_cancel();
                continue;
            }
            if (progress_window.hwnd() != nullptr &&
                IsDialogMessageW(progress_window.hwnd(), &message)) {
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 33, QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
    }
    worker.join();
    progress_window.close();
}

void DialogUi::message(const std::wstring& title, const std::wstring& text,
                       bool error) {
    // With a console attached the caller is a script, and a modal dialog would
    // block it. Text goes to the console instead.
    if (console_.attached()) {
        console_.write(text + L"\n");
        return;
    }
    axiom::gui::show_message_dialog(
        nullptr, instance_, GetDpiForSystem(), dark_, title, text,
        error ? axiom::gui::MessageDialogIcon::error
              : axiom::gui::MessageDialogIcon::information);
}

void DialogUi::reveal(const std::filesystem::path& destination) {
    ShellExecuteW(nullptr, L"open", destination.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
}

void DialogUi::write(const std::wstring& text) { console_.write(text); }

std::optional<int> run_embedded_if_present(
    HINSTANCE instance, const std::wstring& requested_destination) {
    std::wstring module(32768, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (length == 0 || length >= module.size()) return std::nullopt;
    module.resize(length);
    if (!axiom::sfx_locate_payload(std::filesystem::path(module))) {
        return std::nullopt;
    }

    HostConsole console(instance);
    DialogUi ui(instance, console, axiom::gui::dialog_system_prefers_dark_mode());
    std::vector<std::wstring> arguments;
    if (!requested_destination.empty()) arguments.push_back(requested_destination);
    return run_self_extractor(instance, arguments, ui);
}

void DialogUi::flush(const std::wstring& title, bool error) {
    const std::wstring buffered = console_.take_buffered();
    if (buffered.empty()) return;
    axiom::gui::show_message_dialog(
        nullptr, instance_, GetDpiForSystem(), dark_, title, buffered,
        error ? axiom::gui::MessageDialogIcon::error
              : axiom::gui::MessageDialogIcon::information);
}

}  // namespace axiom::sfx
