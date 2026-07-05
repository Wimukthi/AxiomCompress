// The quick "Add to archive" flow used by shell integration and
// the embedded self-extractor startup path.

#include "gui/main_window_internal.hpp"

namespace axiom::gui {

namespace {

class QuickAddController {
public:
    int run(HINSTANCE instance, std::vector<fs::path> inputs) {
        if (inputs.empty()) return 0;
        instance_ = instance;
        inputs_ = std::move(inputs);
        persisted_settings_ = axiom::gui::load_gui_settings();
        application_options_ = persisted_settings_.application;
        configure_dialog_appearance(application_options_);
        theme_ = make_theme(application_options_.theme_mode,
                            application_options_.accent_color_mode,
                            application_options_.custom_accent_color);
        if (!create_owner()) return 1;

        axiom::gui::CreateArchiveDialogOptions dialog_options;
        dialog_options.level = application_options_.default_level;
        dialog_options.thread_count = application_options_.default_thread_count;
        dialog_options.dictionary_size = application_options_.default_dictionary_size;
        dialog_options.word_size = application_options_.default_word_size;
        dialog_options.solid_block_size = application_options_.default_solid_block_size;
        dialog_options.feature_availability = implemented_feature_availability();
        dialog_options.features.update_mode = static_cast<axiom::gui::ArchiveUpdateMode>(
            std::clamp(application_options_.default_update_mode, 0, 4));
        dialog_options.features.volume_size = application_options_.default_volume_size;
        dialog_options.features.volume_unit =
            std::clamp(application_options_.default_volume_unit, 0, 3);
        dialog_options.features.recovery_percent =
            std::clamp(application_options_.default_recovery_percent, 0, 100);
        dialog_options.features.create_recovery_volumes =
            application_options_.default_recovery_volumes;
        dialog_options.features.create_sfx = application_options_.default_create_sfx;
        dialog_options.features.sign_archive = application_options_.default_sign_archive;
        dialog_options.features.signing_key = application_options_.default_signing_key;
        dialog_options.archive_path = default_archive_path();
        dialog_options.archive_format =
            axiom::archive_provider_for_path(dialog_options.archive_path)
                ? axiom::archive_provider_for_path(dialog_options.archive_path)->info().format
                : axiom::ArchiveFormat::axar;

        if (!axiom::gui::show_create_archive_dialog(
                hwnd_, inputs_.size(), dialog_options)) {
            DestroyWindow(hwnd_);
            return 0;
        }

        if (dialog_options.archive_path.has_parent_path()) {
            persisted_settings_.last_archive_output_folder =
                dialog_options.archive_path.parent_path().wstring();
            axiom::gui::save_gui_settings(persisted_settings_);
        }

        const bool started = start_create_operation(std::move(dialog_options));
        if (!started) {
            DestroyWindow(hwnd_);
            return 1;
        }

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return exit_code_;
    }

private:
    static const wchar_t* class_name() {
        return L"AxiomQuickAddOwner";
    }

    static bool register_class(HINSTANCE instance) {
        static ATOM atom = 0;
        if (atom != 0) return true;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = instance;
        wc.lpfnWndProc = &QuickAddController::window_proc;
        wc.lpszClassName = class_name();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        atom = RegisterClassExW(&wc);
        return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    bool create_owner() {
        if (!register_class(instance_)) return false;
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const UINT dpi = GetDpiForSystem();
        const int width = MulDiv(840, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
        const int height = MulDiv(650, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
        const int x = work.left + ((work.right - work.left) - width) / 2;
        const int y = work.top + ((work.bottom - work.top) - height) / 2;
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, class_name(), L"Axiom",
                                WS_OVERLAPPED,
                                x, y, width, height,
                                nullptr, nullptr, instance_, this);
        if (hwnd_ == nullptr) return false;
        set_dark_title_bar(hwnd_, theme_.dark);
        return true;
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        QuickAddController* controller = nullptr;
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            controller = static_cast<QuickAddController*>(create->lpCreateParams);
            controller->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(controller));
        } else {
            controller = reinterpret_cast<QuickAddController*>(
                GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (controller != nullptr) {
            return controller->handle_message(message, wparam, lparam);
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case kOperationProgressMessage:
                on_operation_progress(lparam);
                return 0;
            case kOperationDoneMessage:
                on_operation_done(lparam);
                return 0;
            case WM_CLOSE:
                operation_runner_.cancel();
                return 0;
            case WM_DESTROY:
                operation_runner_.cancel();
                operation_runner_.finish();
                if (!done_) {
                    done_ = true;
                    exit_code_ = 1;
                }
                return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    int show_message(std::wstring_view message,
                     axiom::gui::MessageDialogIcon icon,
                     std::wstring_view title = L"Axiom",
                     axiom::gui::MessageDialogButtons buttons =
                         axiom::gui::MessageDialogButtons::ok,
                     int default_result = IDOK) const {
        return axiom::gui::show_message_dialog(
            hwnd_, instance_, GetDpiForWindow(hwnd_), theme_.dark,
            title, message, icon, buttons, default_result);
    }

    static axiom::gui::ArchiveFeatureAvailability implemented_feature_availability() {
        axiom::gui::ArchiveFeatureAvailability availability;
        availability.metadata = false;
        availability.update = true;
        availability.comments = true;
        availability.lock = true;
        availability.encryption = true;
        availability.recovery = true;
        availability.volumes = true;
        availability.authenticity = true;
        availability.sfx = true;
        availability.posix_metadata = true;
        availability.header_encryption = true;
        availability.kdf_presets = false;
        return availability;
    }

    static std::optional<std::uint64_t> parse_volume_size(
        const std::wstring& text, int unit) {
        if (text.empty()) return std::uint64_t{0};
        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long long value = _wcstoui64(text.c_str(), &end, 10);
        if (errno == ERANGE || end == text.c_str() || *end != L'\0' || value == 0 ||
            unit < 0 || unit > 3) {
            return std::nullopt;
        }
        std::uint64_t multiplier = 1024;
        for (int index = 0; index < unit; ++index) {
            if (multiplier > std::numeric_limits<std::uint64_t>::max() / 1024) {
                return std::nullopt;
            }
            multiplier *= 1024;
        }
        if (value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(value) * multiplier;
    }

    static bool same_filesystem_path(const fs::path& left, const fs::path& right) {
        std::error_code error;
        if (fs::equivalent(left, right, error)) return true;
        return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
    }

    static std::wstring sanitize_archive_stem(std::wstring stem) {
        for (wchar_t& ch : stem) {
            if (ch < L' ' || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' ||
                ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*') {
                ch = L'_';
            }
        }
        while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'.')) {
            stem.pop_back();
        }
        while (!stem.empty() && stem.front() == L' ') stem.erase(stem.begin());
        if (stem.empty()) return L"Archive";
        return stem;
    }

    static bool path_is_directory(const fs::path& path) {
        std::error_code error;
        return fs::is_directory(path, error);
    }

    static std::wstring archive_stem_for_inputs(const std::vector<fs::path>& paths,
                                                const fs::path& current_folder) {
        if (paths.size() == 1) {
            fs::path name = paths.front().filename();
            if (name.empty()) name = paths.front().root_name();
            std::wstring stem = path_is_directory(paths.front())
                ? name.wstring()
                : name.stem().wstring();
            if (stem.empty()) stem = name.wstring();
            return sanitize_archive_stem(std::move(stem));
        }
        fs::path common_parent = paths.empty() ? fs::path{} : paths.front().parent_path();
        bool same_parent = !common_parent.empty();
        for (const auto& path : paths) {
            if (path.parent_path().empty() ||
                !same_filesystem_path(common_parent, path.parent_path())) {
                same_parent = false;
                break;
            }
        }
        fs::path name = same_parent ? common_parent.filename() : current_folder.filename();
        if (name.empty() && same_parent) name = common_parent.root_name();
        if (name.empty()) name = L"Selected items";
        return sanitize_archive_stem(name.wstring());
    }

    static bool output_collides_with_input(const fs::path& output,
                                           const std::vector<fs::path>& paths) {
        for (const auto& path : paths) {
            if (same_filesystem_path(output, path)) return true;
        }
        return false;
    }

    static fs::path avoid_archive_input_collision(fs::path output,
                                                  const std::vector<fs::path>& paths) {
        if (!output_collides_with_input(output, paths)) return output;
        const fs::path parent = output.parent_path();
        const std::wstring stem = output.stem().wstring();
        const std::wstring extension = output.extension().wstring();
        fs::path candidate = parent / (stem + L" archive" + extension);
        for (int index = 2; output_collides_with_input(candidate, paths) && index < 100;
             ++index) {
            candidate = parent / (stem + L" archive " + std::to_wstring(index) + extension);
        }
        return candidate;
    }

    fs::path default_archive_path() const {
        const fs::path source_folder = inputs_.front().parent_path();
        const std::wstring file_name = archive_stem_for_inputs(inputs_, source_folder) + L".axar";
        fs::path folder = source_folder;
        if (application_options_.archive_output_mode == 1 &&
            !persisted_settings_.last_archive_output_folder.empty()) {
            folder = persisted_settings_.last_archive_output_folder;
        } else if (application_options_.archive_output_mode == 2 &&
                   !application_options_.archive_output_folder.empty()) {
            folder = application_options_.archive_output_folder;
        }
        return avoid_archive_input_collision(folder / file_name, inputs_);
    }

    axiom::CompressionOptions compression_options(
        const axiom::gui::CreateArchiveDialogOptions& dialog_options) const {
        axiom::CompressionOptions options;
        axiom::apply_compression_level(options, dialog_options.level);
        options.thread_count = dialog_options.thread_count;
        options.io_buffer_size = configured_io_buffer_size(application_options_);
        if (dialog_options.dictionary_size != 0) {
            options.window_size = dialog_options.dictionary_size;
        }
        if (dialog_options.word_size != 0) {
            options.nice_length = dialog_options.word_size;
        }
        if (dialog_options.solid_block_size != 0) {
            options.block_size = dialog_options.solid_block_size;
            options.auto_block_size_for_threads = false;
        }
        if (application_options_.memory_limit_mode == 1) {
            if (const auto limit = parse_size_setting(application_options_.memory_limit)) {
                const std::size_t capped = static_cast<std::size_t>(
                    std::min<std::uint64_t>(*limit, std::numeric_limits<std::size_t>::max()));
                const std::size_t practical_cap = std::max<std::size_t>(capped, 64u << 10);
                options.window_size = std::min(options.window_size, practical_cap);
                options.block_size = std::min(options.block_size, practical_cap);
                options.auto_block_size_for_threads = false;
            }
        }
        return options;
    }

    static fs::path current_executable_path() {
        std::wstring buffer(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size()) return {};
        buffer.resize(length);
        return fs::path(std::move(buffer));
    }

    void apply_operation_priority() {
        operation_priority_changed_ = false;
        previous_priority_class_ = 0;
        DWORD desired = 0;
        if (application_options_.worker_priority == 1) {
            desired = BELOW_NORMAL_PRIORITY_CLASS;
        } else if (application_options_.worker_priority == 2) {
            desired = PROCESS_MODE_BACKGROUND_BEGIN;
        } else {
            return;
        }
        HANDLE process = GetCurrentProcess();
        previous_priority_class_ = GetPriorityClass(process);
        if (previous_priority_class_ != 0 && previous_priority_class_ != desired &&
            SetPriorityClass(process, desired)) {
            operation_priority_changed_ = true;
        }
    }

    void restore_operation_priority() {
        if (operation_priority_changed_ && previous_priority_class_ != 0) {
            SetPriorityClass(GetCurrentProcess(), previous_priority_class_);
        }
        operation_priority_changed_ = false;
        previous_priority_class_ = 0;
    }

    bool start_create_operation(axiom::gui::CreateArchiveDialogOptions dialog_options) {
        const fs::path archive = dialog_options.archive_path;
        if (archive.empty()) {
            show_message(L"Choose an output archive path.",
                         axiom::gui::MessageDialogIcon::information);
            return false;
        }
        const auto* archive_provider = axiom::archive_provider_for_path(archive);
        if (archive_provider == nullptr) {
            show_message(L"Choose a supported archive format (.axar or .zip).",
                         axiom::gui::MessageDialogIcon::warning, L"Archive format");
            return false;
        }
        const auto provider_capabilities = archive_provider->capabilities(archive);
        if (!provider_capabilities.create &&
            dialog_options.features.update_mode == axiom::gui::ArchiveUpdateMode::create_new) {
            show_message(
                L"This format is read-only in Axiom. Create .axar or .zip archives instead.",
                axiom::gui::MessageDialogIcon::warning,
                L"Archive format");
            return false;
        }
        if (!provider_capabilities.update &&
            dialog_options.features.update_mode != axiom::gui::ArchiveUpdateMode::create_new) {
            show_message(L"This archive format is read-only and cannot be updated.",
                         axiom::gui::MessageDialogIcon::warning,
                         L"Archive format");
            return false;
        }

        auto options = compression_options(dialog_options);
        const auto mode = dialog_options.features.update_mode;
        options.encrypt_header = dialog_options.features.encrypt_names;
        options.recovery_percent = static_cast<unsigned>(
            std::clamp(dialog_options.features.recovery_percent, 0, 100));
        options.password = dialog_options.features.encrypt_data
            ? utf8(dialog_options.features.password)
            : std::string{};
        const auto volume_size = parse_volume_size(
            dialog_options.features.volume_size, dialog_options.features.volume_unit);
        if (!volume_size) {
            show_message(L"Enter a valid positive split-volume size, or leave it blank.",
                         axiom::gui::MessageDialogIcon::warning,
                         L"Recovery and volumes");
            return false;
        }
        const bool split_after = *volume_size != 0;
        const bool recovery_volumes = dialog_options.features.create_recovery_volumes;
        if (recovery_volumes && !split_after) {
            show_message(L"Set a split-volume size before enabling recovery volumes.",
                         axiom::gui::MessageDialogIcon::warning,
                         L"Recovery and volumes");
            return false;
        }

        const std::string comment = utf8(dialog_options.features.comment);
        const bool set_comment = mode != axiom::gui::ArchiveUpdateMode::create_new ||
                                 !comment.empty();
        const bool repack_after = dialog_options.features.repack_after_update &&
                                  mode != axiom::gui::ArchiveUpdateMode::create_new;
        const bool lock_after = dialog_options.features.lock_archive;
        const bool sign_after = dialog_options.features.sign_archive;
        const bool create_sfx_after = dialog_options.features.create_sfx;
        const bool native_archive = archive_provider->info().native;
        const bool zip_archive = archive_provider->info().format == axiom::ArchiveFormat::zip;
        if (!native_archive &&
            ((!zip_archive && dialog_options.features.encrypt_data) ||
             dialog_options.features.encrypt_names ||
             !dialog_options.features.comment.empty() ||
             dialog_options.features.lock_archive ||
             dialog_options.features.repack_after_update ||
             dialog_options.features.recovery_percent != 0 ||
             !dialog_options.features.volume_size.empty() ||
             dialog_options.features.create_recovery_volumes ||
             dialog_options.features.sign_archive ||
             dialog_options.features.create_sfx)) {
            show_message(
                L"The selected archive format does not support one or more Axiom-only "
                L"options. ZIP supports file-data encryption only; choose Axiom archive "
                L"format for encrypted names, Recovery, SFX/signing, comment, or lock options.",
                axiom::gui::MessageDialogIcon::warning,
                L"Archive format");
            return false;
        }
        if (create_sfx_after && split_after) {
            show_message(L"Self-extracting archives and split volumes are separate output "
                         L"modes. Disable one of them.",
                         axiom::gui::MessageDialogIcon::warning,
                         L"Archive output");
            return false;
        }
        fs::path sfx_output = dialog_options.features.sfx_destination;
        if (create_sfx_after && sfx_output.empty()) {
            sfx_output = archive;
            sfx_output.replace_extension(L".exe");
        }
        if (application_options_.confirm_overwrite &&
            mode == axiom::gui::ArchiveUpdateMode::create_new) {
            std::error_code exists_error;
            const fs::path output_path = create_sfx_after ? sfx_output : archive;
            if (!output_path.empty() && fs::exists(output_path, exists_error)) {
                if (show_message(
                        L"Replace the existing output file?\n\n" + output_path.wstring(),
                        axiom::gui::MessageDialogIcon::warning,
                        L"Overwrite output",
                        axiom::gui::MessageDialogButtons::yes_no,
                        IDNO) != IDYES) {
                    return false;
                }
            }
        }

        const fs::path sfx_stub = current_executable_path();
        axiom::ArchiveSigningKey signing_key;
        if (sign_after) {
            std::ifstream key_file(dialog_options.features.signing_key, std::ios::binary);
            if (!key_file ||
                !key_file.read(reinterpret_cast<char*>(signing_key.secret_key.data()),
                               static_cast<std::streamsize>(signing_key.secret_key.size())) ||
                key_file.peek() != std::char_traits<char>::eof()) {
                show_message(L"Choose a valid 64-byte Axiom signing key.",
                             axiom::gui::MessageDialogIcon::error,
                             L"Sign archive");
                return false;
            }
            std::copy_n(signing_key.secret_key.begin() + 32,
                        signing_key.public_key.size(), signing_key.public_key.begin());
        }
        secure_clear(dialog_options.features.password);

        std::wstring running = L"Compressing...";
        std::wstring success = L"Archive created: " + archive.wstring();
        if (create_sfx_after) {
            running = L"Creating self-extracting archive...";
            success = L"Self-extracting archive created: " + sfx_output.wstring();
        } else if (split_after) {
            running = L"Creating split archive volumes...";
            success = L"Split archive volumes created beside: " + archive.wstring();
        }

        operation_archive_output_ = archive;
        if (!operation_window_.create(
                hwnd_, instance_, running, create_sfx_after ? sfx_output : archive,
                make_operation_window_theme(theme_),
                [this](bool paused) {
                    if (!operation_runner_.running()) return;
                    operation_runner_.set_paused(paused);
                },
                [this] {
                    if (!operation_runner_.running()) return;
                    operation_runner_.cancel();
                    operation_window_.set_cancelling();
                })) {
            show_message(L"Could not create the operation progress window.",
                         axiom::gui::MessageDialogIcon::error);
            return false;
        }

        apply_operation_priority();
        const std::vector<fs::path> inputs = inputs_;
        const bool started = operation_runner_.start(
            hwnd_, kOperationDoneMessage, kOperationProgressMessage,
            running, success,
            [inputs, archive, options, mode, comment, set_comment,
             repack_after, lock_after, sign_after, signing_key,
             create_sfx_after, sfx_output, sfx_stub, split_after,
             volume_size = *volume_size, recovery_volumes](
                std::shared_ptr<axiom::OperationControl> operation) mutable {
                auto run_options = options;
                run_options.operation = operation;
                const auto* provider = axiom::archive_provider_for_path(archive);
                if (provider == nullptr) {
                    throw std::runtime_error("unsupported archive format");
                }
                try {
                    switch (mode) {
                        case axiom::gui::ArchiveUpdateMode::add_or_replace:
                            provider->add(inputs, archive, run_options);
                            break;
                        case axiom::gui::ArchiveUpdateMode::update_newer:
                            provider->update(inputs, archive, run_options, false);
                            break;
                        case axiom::gui::ArchiveUpdateMode::fresh_existing:
                            provider->update(inputs, archive, run_options, true);
                            break;
                        case axiom::gui::ArchiveUpdateMode::synchronize:
                            provider->sync(inputs, archive, run_options);
                            break;
                        default:
                            provider->create(inputs, archive, run_options);
                            break;
                    }
                    if (provider->info().native && set_comment) {
                        axiom::set_archive_comment(archive, comment, run_options);
                    }
                    if (provider->info().native && repack_after) {
                        axiom::repack_archive(archive, run_options);
                    }
                    if (provider->info().native && sign_after) {
                        axiom::sign_archive(archive, signing_key, run_options);
                        SecureZeroMemory(signing_key.secret_key.data(),
                                         signing_key.secret_key.size());
                    }
                    if (provider->info().native && lock_after) {
                        axiom::lock_archive(archive, run_options);
                    }
                    if (provider->info().native && split_after) {
                        const std::uint64_t archive_bytes = fs::file_size(archive);
                        const std::uint64_t data_volumes = std::max<std::uint64_t>(
                            1, (archive_bytes + volume_size - 1) / volume_size);
                        const unsigned recovery_count = recovery_volumes
                            ? static_cast<unsigned>(std::max<std::uint64_t>(
                                1, (data_volumes *
                                    std::max<unsigned>(options.recovery_percent, 10) +
                                    99) / 100))
                            : 0;
                        axiom::create_archive_volumes(
                            archive, volume_size, recovery_count, operation);
                        std::error_code remove_error;
                        if (!fs::remove(archive, remove_error) && remove_error) {
                            throw fs::filesystem_error(
                                "could not remove the unsplit archive", archive,
                                remove_error);
                        }
                    }
                    if (provider->info().native && create_sfx_after) {
                        axiom::create_sfx_archive(archive, sfx_stub, sfx_output,
                                                  operation, options.io_buffer_size);
                        std::error_code remove_error;
                        if (!fs::remove(archive, remove_error) && remove_error) {
                            std::error_code cleanup_error;
                            fs::remove(sfx_output, cleanup_error);
                            throw fs::filesystem_error(
                                "could not remove the intermediate SFX archive",
                                archive, remove_error);
                        }
                    }
                } catch (...) {
                    if (create_sfx_after &&
                        mode == axiom::gui::ArchiveUpdateMode::create_new) {
                        std::error_code cleanup_error;
                        fs::remove(archive, cleanup_error);
                    }
                    throw;
                }
            });
        if (!started) {
            restore_operation_priority();
            operation_window_.close();
            show_message(L"Another operation is already running.",
                         axiom::gui::MessageDialogIcon::information);
            return false;
        }
        return true;
    }

    void on_operation_progress(LPARAM lparam) {
        std::unique_ptr<axiom::OperationProgress> progress(
            reinterpret_cast<axiom::OperationProgress*>(lparam));
        if (!progress) return;
        MSG queued{};
        while (PeekMessageW(&queued, hwnd_, kOperationProgressMessage,
                            kOperationProgressMessage, PM_REMOVE)) {
            progress.reset(reinterpret_cast<axiom::OperationProgress*>(queued.lParam));
        }
        operation_window_.set_progress(*progress);
    }

    void on_operation_done(LPARAM lparam) {
        std::unique_ptr<axiom::gui::OperationResult> result(
            reinterpret_cast<axiom::gui::OperationResult*>(lparam));
        operation_runner_.finish();
        restore_operation_priority();
        operation_window_.close();
        exit_code_ = (!result || result->ok || result->cancelled) ? 0 : 1;
        done_ = true;
        if (result && !result->cancelled) {
            show_message(result->message,
                         result->ok ? axiom::gui::MessageDialogIcon::information
                                    : axiom::gui::MessageDialogIcon::error,
                         result->ok ? L"Axiom" : L"Axiom error");
        }
        DestroyWindow(hwnd_);
    }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    ThemePalette theme_;
    axiom::gui::PersistedGuiSettings persisted_settings_;
    axiom::gui::ApplicationDialogOptions application_options_;
    std::vector<fs::path> inputs_;
    axiom::gui::OperationRunner operation_runner_;
    axiom::gui::OperationProgressWindow operation_window_;
    fs::path operation_archive_output_;
    DWORD previous_priority_class_ = 0;
    bool operation_priority_changed_ = false;
    bool done_ = false;
    int exit_code_ = 0;
};

}  // namespace

int run_quick_add_to_archive(HINSTANCE instance, const std::vector<std::wstring>& paths) {
std::vector<fs::path> inputs;
inputs.reserve(paths.size());
for (const auto& path : paths) {
    if (!path.empty()) inputs.emplace_back(path);
}
QuickAddController controller;
return controller.run(instance, std::move(inputs));
}

std::optional<int> run_embedded_sfx(HINSTANCE instance, const std::wstring& requested_destination) {
constexpr std::array<char, 8> magic = {'A', 'X', 'I', 'O', 'M', 'S', 'F', 'X'};
std::wstring module(32768, L'\0');
const DWORD module_length = GetModuleFileNameW(nullptr, module.data(),
                                               static_cast<DWORD>(module.size()));
if (module_length == 0 || module_length >= module.size()) return std::nullopt;
module.resize(module_length);
const fs::path executable(module);

std::ifstream input(executable, std::ios::binary);
if (!input) return std::nullopt;
input.seekg(0, std::ios::end);
const auto end = input.tellg();
if (end < static_cast<std::streamoff>(16)) return std::nullopt;
input.seekg(end - static_cast<std::streamoff>(16));
std::array<char, 8> found{};
input.read(found.data(), static_cast<std::streamsize>(found.size()));
std::array<std::uint8_t, 8> encoded_size{};
input.read(reinterpret_cast<char*>(encoded_size.data()),
           static_cast<std::streamsize>(encoded_size.size()));
if (!input || found != magic) return std::nullopt;
std::uint64_t archive_size = 0;
for (unsigned index = 0; index < 8; ++index) {
    archive_size |= static_cast<std::uint64_t>(encoded_size[index]) << (index * 8);
}
const std::uint64_t file_size = static_cast<std::uint64_t>(end);
if (archive_size == 0 || archive_size > file_size - 16) return std::nullopt;
const std::uint64_t archive_offset = file_size - 16 - archive_size;

fs::path destination = requested_destination.empty()
    ? executable.parent_path() / executable.stem()
    : fs::path(requested_destination);
const bool dark = system_prefers_dark_mode();

wchar_t temp_path[MAX_PATH + 1]{};
if (GetTempPathW(MAX_PATH, temp_path) == 0) return 1;
const fs::path temporary = fs::path(temp_path) /
    (L"AxiomSfx-" + std::to_wstring(GetCurrentProcessId()) + L".axar");
try {
    const auto persisted = axiom::gui::load_gui_settings();
    const std::size_t io_buffer_size = configured_io_buffer_size(persisted.application);
    input.clear();
    input.seekg(static_cast<std::streamoff>(archive_offset));
    std::ofstream archive(temporary, std::ios::binary | std::ios::trunc);
    if (!archive) throw std::runtime_error("cannot create the temporary archive");
    const std::size_t copy_buffer_size = io_buffer_size == 0
        ? (std::size_t{1} << 20)
        : std::clamp<std::size_t>(io_buffer_size, 64u << 10, 64u << 20);
    std::vector<char> buffer(copy_buffer_size);
    std::uint64_t remaining = archive_size;
    while (remaining > 0) {
        const auto count = static_cast<std::streamsize>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), count);
        if (input.gcount() != count) throw std::runtime_error("SFX payload is truncated");
        archive.write(buffer.data(), count);
        remaining -= static_cast<std::uint64_t>(count);
    }
    archive.close();
    axiom::ExtractOptions options;
    options.overwrite = axiom::ExtractOptions::Overwrite::overwrite;
    if (axiom::archive_is_encrypted(temporary)) {
        std::wstring password;
        if (!axiom::gui::show_archive_password_dialog(nullptr, password)) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return 0;
        }
        options.password = utf8(password);
        secure_clear(password);
    }
    const auto signature = axiom::verify_archive_signature(temporary, options.password);
    if (signature.present && !signature.valid) {
        throw std::runtime_error("archive authenticity signature is invalid");
    }

    const auto entries = axiom::list_archive(temporary, options.password);
    axiom::gui::SfxArchiveSummary summary;
    summary.archive_name = executable.filename().wstring();
    summary.encrypted = axiom::archive_is_encrypted(temporary);
    summary.signature_present = signature.present;
    summary.signature_valid = signature.valid;
    summary.comment = widen(axiom::archive_comment(temporary, options.password));
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
    dialog_options.overwrite = axiom::ExtractOptions::Overwrite::overwrite;
    if (!axiom::gui::show_sfx_extract_dialog(nullptr, instance, summary, dialog_options)) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return 0;
    }
    destination = dialog_options.destination;
    options.overwrite = dialog_options.overwrite;
    options.restore_mtime = dialog_options.restore_mtime;
    options.thread_count = dialog_options.thread_count;
    options.io_buffer_size = io_buffer_size;

    auto operation = std::make_shared<axiom::OperationControl>();
    options.operation = operation;
    std::mutex progress_mutex;
    std::optional<axiom::OperationProgress> latest_progress;
    operation->set_progress_callback([&](const axiom::OperationProgress& progress) {
        std::lock_guard lock(progress_mutex);
        latest_progress = progress;
    });

    std::atomic_bool completed = false;
    std::atomic_bool cancelled = false;
    std::exception_ptr failure;
    axiom::gui::OperationProgressWindow progress_window;
    configure_dialog_appearance(persisted.application);
    const auto operation_theme = make_operation_window_theme(
        make_theme(persisted.application.theme_mode,
                   persisted.application.accent_color_mode,
                   persisted.application.custom_accent_color));
    progress_window.create(
        nullptr, instance, L"Extracting archive", {}, operation_theme,
        [operation](bool paused) { operation->set_paused(paused); },
        [operation] { operation->request_cancel(); });

    std::jthread worker([&] {
        try {
            axiom::extract_archive(temporary, destination, options);
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
        std::optional<axiom::OperationProgress> progress;
        {
            std::lock_guard lock(progress_mutex);
            progress.swap(latest_progress);
        }
        if (progress && progress_window.hwnd() != nullptr) {
            progress_window.set_progress(*progress);
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 33, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    worker.join();
    progress_window.close();
    if (failure) std::rethrow_exception(failure);

    std::error_code ignored;
    fs::remove(temporary, ignored);
    if (cancelled.load(std::memory_order_acquire)) return 0;
    if (dialog_options.open_destination) {
        ShellExecuteW(nullptr, L"open", destination.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    axiom::gui::show_message_dialog(
        nullptr, instance, GetDpiForSystem(), dark, L"Axiom Self-Extractor",
        L"Files were extracted to:\n\n" + destination.wstring(),
        axiom::gui::MessageDialogIcon::information);
    return 0;
} catch (const std::exception& error) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    axiom::gui::show_message_dialog(
        nullptr, instance, GetDpiForSystem(), dark, L"Axiom Self-Extractor",
        L"Extraction failed:\n\n" + widen(error.what()),
        axiom::gui::MessageDialogIcon::error);
    return 1;
}
}

}  // namespace axiom::gui
