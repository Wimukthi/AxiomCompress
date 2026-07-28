#define NOMINMAX
#include "sfx/runtime.hpp"

#include "axiom/archive.hpp"
#include "gui/archive_feature_dialogs.hpp"
#include "gui/dialog_support.hpp"
#include "gui/message_dialog.hpp"
#include "gui/operation_progress_window.hpp"
#include "gui/sfx_dialog.hpp"

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace axiom::sfx {
namespace fs = std::filesystem;
namespace {

constexpr std::array<char, 8> kMagic{
    'A', 'X', 'I', 'O', 'M', 'S', 'F', 'X'};

std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return L"Archive operation failed.";
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) throw std::runtime_error("password is not valid Unicode");
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), length,
                        nullptr, nullptr);
    return result;
}

void secure_clear(std::wstring& text) {
    if (!text.empty()) {
        SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
        text.clear();
    }
}

void secure_clear(std::string& text) {
    if (!text.empty()) {
        SecureZeroMemory(text.data(), text.size());
        text.clear();
    }
}

class TemporaryPayload {
public:
    TemporaryPayload() {
        wchar_t folder[32768]{};
        const DWORD length = GetTempPathW(
            static_cast<DWORD>(std::size(folder)), folder);
        if (length == 0 || length >= std::size(folder)) {
            throw std::runtime_error("cannot locate the temporary folder");
        }

        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            GUID id{};
            if (FAILED(CoCreateGuid(&id))) {
                throw std::runtime_error("cannot create a temporary payload name");
            }
            wchar_t name[64]{};
            if (StringFromGUID2(id, name, static_cast<int>(std::size(name))) <= 0) {
                continue;
            }
            path_ = fs::path(folder) / (std::wstring(L"AxiomSfx-") + name + L".payload");
            handle_ = CreateFileW(
                path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) return;
            if (GetLastError() != ERROR_FILE_EXISTS &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                break;
            }
        }
        throw std::runtime_error("cannot create the temporary archive");
    }

    ~TemporaryPayload() {
        close();
        if (!path_.empty()) {
            std::error_code ignored;
            fs::remove(path_, ignored);
        }
    }

    TemporaryPayload(const TemporaryPayload&) = delete;
    TemporaryPayload& operator=(const TemporaryPayload&) = delete;

    void write(const void* bytes, DWORD size) {
        const auto* cursor = static_cast<const std::uint8_t*>(bytes);
        DWORD remaining = size;
        while (remaining != 0) {
            DWORD written = 0;
            if (!WriteFile(handle_, cursor, remaining, &written, nullptr) ||
                written == 0) {
                throw std::runtime_error("cannot write the temporary archive");
            }
            cursor += written;
            remaining -= written;
        }
    }

    void close() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

axiom::gui::OperationWindowTheme operation_theme(bool dark) {
    const auto colors = axiom::gui::dialog_colors(dark);
    return {
        dark,
        colors.background,
        colors.control_background,
        colors.text,
        colors.disabled_text,
        colors.border,
        colors.control_background,
        colors.selection_background,
        colors.border,
        colors.control_background,
        axiom::gui::dialog_accent_color(),
    };
}

fs::path current_executable() {
    std::wstring module(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (length == 0 || length >= module.size()) {
        throw std::runtime_error("cannot locate the self-extractor");
    }
    module.resize(length);
    return fs::path(std::move(module));
}

}  // namespace

std::optional<int> run_embedded(
    HINSTANCE instance,
    const std::wstring& requested_destination) {
    const fs::path executable = current_executable();
    std::ifstream input(executable, std::ios::binary);
    if (!input) return std::nullopt;
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < static_cast<std::streamoff>(16)) return std::nullopt;
    input.seekg(end - static_cast<std::streamoff>(16));

    std::array<char, 8> found{};
    std::array<std::uint8_t, 8> encoded_size{};
    input.read(found.data(), static_cast<std::streamsize>(found.size()));
    input.read(reinterpret_cast<char*>(encoded_size.data()),
               static_cast<std::streamsize>(encoded_size.size()));
    if (!input || found != kMagic) return std::nullopt;

    std::uint64_t archive_size = 0;
    for (unsigned index = 0; index < encoded_size.size(); ++index) {
        archive_size |=
            static_cast<std::uint64_t>(encoded_size[index]) << (index * 8);
    }
    const std::uint64_t file_size = static_cast<std::uint64_t>(end);
    if (archive_size == 0 || archive_size > file_size - 16) return std::nullopt;
    const std::uint64_t archive_offset = file_size - 16 - archive_size;

    fs::path destination = requested_destination.empty()
        ? executable.parent_path() / executable.stem()
        : fs::path(requested_destination);
    const bool dark = axiom::gui::dialog_system_prefers_dark_mode();

    try {
        TemporaryPayload temporary;
        input.clear();
        input.seekg(static_cast<std::streamoff>(archive_offset));
        std::vector<char> buffer(std::size_t{1} << 20);
        std::uint64_t remaining = archive_size;
        while (remaining != 0) {
            const auto count = static_cast<std::streamsize>(
                std::min<std::uint64_t>(remaining, buffer.size()));
            input.read(buffer.data(), count);
            if (input.gcount() != count) {
                throw std::runtime_error("SFX payload is truncated");
            }
            temporary.write(buffer.data(), static_cast<DWORD>(count));
            remaining -= static_cast<std::uint64_t>(count);
        }
        temporary.close();

        const auto* provider =
            axiom::archive_provider_for_contents(temporary.path());
        if (provider == nullptr ||
            (provider->info().format != axiom::ArchiveFormat::axar &&
             provider->info().format != axiom::ArchiveFormat::zip)) {
            throw std::runtime_error(
                "SFX payload is not an Axiom or ZIP archive");
        }

        axiom::ExtractOptions options;
        options.overwrite = axiom::ExtractOptions::Overwrite::overwrite;
        auto capabilities = provider->capabilities(temporary.path());
        if (capabilities.encrypted) {
            std::wstring password;
            if (!axiom::gui::show_archive_password_dialog(nullptr, password)) {
                return 0;
            }
            options.password = utf8(password);
            secure_clear(password);
            capabilities =
                provider->capabilities(temporary.path(), options.password);
        }
        if (!capabilities.extract) {
            secure_clear(options.password);
            throw std::runtime_error(
                "SFX payload cannot be extracted with the supplied password");
        }

        axiom::ArchiveSignatureInfo signature;
        if (provider->info().native) {
            signature =
                axiom::verify_archive_signature(temporary.path(), options.password);
        }
        if (signature.present && !signature.valid) {
            secure_clear(options.password);
            throw std::runtime_error(
                "archive authenticity signature is invalid");
        }

        const auto entries =
            provider->list(temporary.path(), options.password);
        axiom::gui::SfxArchiveSummary summary;
        summary.archive_name = executable.filename().wstring();
        summary.encrypted = capabilities.encrypted;
        summary.signature_present = signature.present;
        summary.signature_valid = signature.valid;
        if (provider->info().native) {
            summary.comment =
                widen(axiom::archive_comment(temporary.path(), options.password));
        }
        for (const auto& entry : entries) {
            if (entry.is_directory) {
                ++summary.directory_count;
            } else {
                ++summary.file_count;
                summary.unpacked_size += entry.size;
            }
        }

        axiom::gui::SfxExtractDialogOptions dialog_options;
        dialog_options.destination = destination;
        dialog_options.overwrite =
            axiom::ExtractOptions::Overwrite::overwrite;
        if (!axiom::gui::show_sfx_extract_dialog(
                nullptr, instance, summary, dialog_options)) {
            secure_clear(options.password);
            return 0;
        }
        destination = dialog_options.destination;
        options.overwrite = dialog_options.overwrite;
        options.restore_mtime = dialog_options.restore_mtime;
        options.thread_count = dialog_options.thread_count;

        auto operation = std::make_shared<axiom::OperationControl>();
        options.operation = operation;

        std::atomic_bool completed = false;
        std::atomic_bool cancelled = false;
        std::exception_ptr failure;
        axiom::gui::OperationProgressWindow progress_window;
        if (!progress_window.create(
                nullptr, instance, L"Extracting archive", {},
                operation_theme(dark),
                [operation](bool paused) { operation->set_paused(paused); },
                [operation] { operation->request_cancel(); })) {
            secure_clear(options.password);
            throw std::runtime_error(
                "cannot create the extraction progress window");
        }
        progress_window.set_progress_source(operation);

        std::jthread worker([&] {
            try {
                provider->extract_all(temporary.path(), destination, options);
            } catch (const axiom::OperationCancelled&) {
                cancelled.store(true, std::memory_order_release);
            } catch (...) {
                failure = std::current_exception();
            }
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
            MsgWaitForMultipleObjectsEx(
                0, nullptr, 33, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        worker.join();
        progress_window.close();
        secure_clear(options.password);
        if (failure) std::rethrow_exception(failure);
        if (cancelled.load(std::memory_order_acquire)) return 0;

        if (dialog_options.open_destination) {
            ShellExecuteW(nullptr, L"open", destination.c_str(), nullptr,
                          nullptr, SW_SHOWNORMAL);
        }
        axiom::gui::show_message_dialog(
            nullptr, instance, GetDpiForSystem(), dark,
            L"Axiom Self-Extractor",
            L"Files were extracted to:\n\n" + destination.wstring(),
            axiom::gui::MessageDialogIcon::information);
        return 0;
    } catch (const std::exception& error) {
        axiom::gui::show_message_dialog(
            nullptr, instance, GetDpiForSystem(), dark,
            L"Axiom Self-Extractor",
            L"Extraction failed:\n\n" + widen(error.what()),
            axiom::gui::MessageDialogIcon::error);
        return 1;
    }
}

}  // namespace axiom::sfx
