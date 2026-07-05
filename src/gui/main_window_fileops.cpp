// MainWindow file and archive operations: compress/extract/test,
// archive editing commands, drag-and-drop staging, and operation lifecycle.

#include "gui/main_window_internal.hpp"

namespace axiom::gui {

std::string MainWindow::archive_name(std::string_view path) {
    const std::size_t separator = path.find_last_of('/');
    return std::string(separator == std::string_view::npos
                           ? path
                           : path.substr(separator + 1));
}

std::string MainWindow::join_archive_directory(std::string_view directory,
                                          std::string_view name) {
    if (directory.empty()) return std::string(name);
    return std::string(directory) + "/" + std::string(name);
}

bool MainWindow::same_filesystem_path(const fs::path& left, const fs::path& right) {
    std::error_code error;
    if (fs::equivalent(left, right, error)) return true;
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring MainWindow::sanitize_archive_stem(std::wstring stem) {
    for (wchar_t& ch : stem) {
        if (ch < L' ' || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' ||
            ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*') {
            ch = L'_';
        }
    }
    while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'.')) {
        stem.pop_back();
    }
    while (!stem.empty() && stem.front() == L' ') {
        stem.erase(stem.begin());
    }
    if (stem.empty()) return L"Archive";

    std::wstring folded = stem;
    std::transform(folded.begin(), folded.end(), folded.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towupper(ch)); });
    static constexpr std::array<std::wstring_view, 4> reserved{
        L"CON", L"PRN", L"AUX", L"NUL",
    };
    if (std::find(reserved.begin(), reserved.end(), folded) != reserved.end() ||
        (folded.size() == 4 &&
         (folded.rfind(L"COM", 0) == 0 || folded.rfind(L"LPT", 0) == 0) &&
         folded[3] >= L'1' && folded[3] <= L'9')) {
        stem += L"_";
    }
    return stem.empty() ? L"Archive" : stem;
}

bool MainWindow::path_is_directory(const fs::path& path) {
    std::error_code error;
    return fs::is_directory(path, error);
}

std::wstring MainWindow::archive_stem_for_single_input(const fs::path& path) {
    fs::path name = path.filename();
    if (name.empty()) {
        name = path.root_name();
    }
    std::wstring stem;
    if (path_is_directory(path)) {
        stem = name.wstring();
    } else {
        stem = name.stem().wstring();
        if (stem.empty()) stem = name.wstring();
    }
    return sanitize_archive_stem(std::move(stem));
}

std::wstring MainWindow::archive_stem_for_multiple_inputs(
    const std::vector<fs::path>& paths,
    const fs::path& current_folder) {
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

std::wstring MainWindow::suggested_archive_stem_for_inputs(
    const std::vector<fs::path>& paths,
    const fs::path& current_folder) {
    if (paths.size() == 1) return archive_stem_for_single_input(paths.front());
    return archive_stem_for_multiple_inputs(paths, current_folder);
}

bool MainWindow::output_collides_with_input(const fs::path& output,
                                       const std::vector<fs::path>& paths) {
    for (const auto& path : paths) {
        if (same_filesystem_path(output, path)) return true;
    }
    return false;
}

fs::path MainWindow::avoid_archive_input_collision(fs::path output,
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

std::string MainWindow::archive_drop_directory(POINT screen_point) const {
    if (history_.current().kind != axiom::gui::BrowserLocationKind::archive) return {};
    const int row = table_.row_at_screen_point(screen_point);
    if (row >= 0 && row < static_cast<int>(browser_items_.size())) {
        const auto& item = browser_items_[static_cast<std::size_t>(row)];
        if (item.kind == axiom::gui::BrowserItemKind::directory &&
            !item.archive_path.empty()) {
            return item.archive_path;
        }
    }
    return history_.current().archive_directory;
}

std::optional<std::string> MainWindow::password_for_archive_edit(const fs::path& archive) {
    try {
        const auto* provider = axiom::archive_provider_for_path(archive);
        if (provider == nullptr || !provider->capabilities(archive).encrypted) {
            return std::string{};
        }
    } catch (const std::exception& error) {
        show_app_message(widen(error.what()), axiom::gui::MessageDialogIcon::error,
                         L"Open archive");
        return std::nullopt;
    }
    const bool cache_password =
        application_options_.cache_passwords &&
        application_options_.password_prompt_mode == 0;
    if (cache_password && !archive_password_path_.empty() &&
        same_filesystem_path(archive_password_path_, archive)) {
        return archive_password_;
    }
    std::wstring password;
    if (!axiom::gui::show_archive_password_dialog(hwnd_, password)) return std::nullopt;
    std::string encoded = utf8(password);
    secure_clear(password);
    if (cache_password) {
        clear_archive_password();
        archive_password_path_ = archive;
        archive_password_ = encoded;
        return archive_password_;
    }
    return encoded;
}

void MainWindow::clear_archive_password() {
    secure_clear(archive_password_);
    secure_clear(pending_archive_password_);
    archive_password_path_.clear();
}

bool MainWindow::prepare_archive_password(const axiom::gui::BrowserLocation& location) {
    secure_clear(pending_archive_password_);
    if (location.kind != axiom::gui::BrowserLocationKind::archive) {
        clear_archive_password();
        return true;
    }
    const bool cache_password =
        application_options_.cache_passwords &&
        application_options_.password_prompt_mode == 0;
    if (cache_password && !archive_password_path_.empty() &&
        same_filesystem_path(archive_password_path_, location.archive_path)) {
        return true;
    }

    bool encrypted = false;
    try {
        const auto* provider = axiom::archive_provider_for_path(location.archive_path);
        if (provider == nullptr) {
            show_app_message(L"This archive format is not supported.",
                             axiom::gui::MessageDialogIcon::warning, L"Open archive");
            return false;
        }
        encrypted = provider->capabilities(location.archive_path).encrypted;
    } catch (const std::exception& error) {
        show_app_message(widen(error.what()), axiom::gui::MessageDialogIcon::error,
                         L"Open archive");
        return false;
    }
    if (!encrypted) {
        clear_archive_password();
        return true;
    }

    std::wstring entered;
    if (!axiom::gui::show_archive_password_dialog(hwnd_, entered)) return false;
    std::string encoded = utf8(entered);
    secure_clear(entered);
    if (cache_password) {
        clear_archive_password();
        archive_password_path_ = location.archive_path;
        archive_password_ = encoded;
    } else {
        clear_archive_password();
        pending_archive_password_ = std::move(encoded);
    }
    return true;
}

fs::path MainWindow::configured_temp_base() const {
    wchar_t temporary[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH, temporary);
    if (length == 0 || length > MAX_PATH) {
        throw std::runtime_error("could not locate the temporary directory");
    }
    fs::path base(temporary);
    if (application_options_.temp_folder_mode == 1) {
        if (const auto local = known_folder_path(FOLDERID_LocalAppData)) {
            base = *local / L"AxiomCompress" / L"Temp";
        }
    } else if (application_options_.temp_folder_mode == 2 &&
               !application_options_.temp_folder.empty()) {
        base = application_options_.temp_folder;
    }
    return base;
}

void MainWindow::cleanup_old_temp_directories() {
    fs::path base;
    try {
        base = configured_temp_base();
    } catch (...) {
        return;
    }
    const int days = std::clamp(application_options_.temp_cleanup_days, 0, 365);
    const auto now = fs::file_time_type::clock::now();
    const auto max_age = std::chrono::hours(24 * days);
    std::error_code iterate_error;
    for (fs::directory_iterator it(base, fs::directory_options::skip_permission_denied,
                                   iterate_error), end;
         !iterate_error && it != end; it.increment(iterate_error)) {
        const auto name = it->path().filename().wstring();
        if (name.rfind(L"AxiomDrag-", 0) != 0 && name.rfind(L"AxiomSfx-", 0) != 0) {
            continue;
        }
        std::error_code time_error;
        const auto modified = fs::last_write_time(it->path(), time_error);
        if (time_error) continue;
        if (days == 0 || now - modified >= max_age) {
            remove_temp_directory(it->path(), application_options_.wipe_encrypted_temp_files);
        }
    }
}

fs::path MainWindow::create_drag_staging_directory(bool sensitive) {
    fs::path base = configured_temp_base();
    std::error_code create_base_error;
    fs::create_directories(base, create_base_error);
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const fs::path candidate = base /
            (L"AxiomDrag-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
        std::error_code error;
        if (fs::create_directory(candidate, error)) {
            temp_directories_.push_back({candidate, sensitive});
            return candidate;
        }
    }
    throw std::runtime_error("could not create a temporary drag directory");
}

DWORD MainWindow::query_file_drop(IDataObject* object, POINT, DWORD, DWORD allowed) const {
    if (busy_ || history_.current().kind != axiom::gui::BrowserLocationKind::archive) {
        return DROPEFFECT_NONE;
    }
    const auto capabilities = active_archive_capabilities();
    if (capabilities.locked || capabilities.directory_encrypted) {
        return DROPEFFECT_NONE;
    }

    if (axiom::gui::data_object_has_archive_entries(object)) {
        axiom::gui::ArchiveDragPayload payload;
        if (!axiom::gui::read_archive_entries(object, payload)) return DROPEFFECT_NONE;
        if (same_filesystem_path(payload.archive_path,
                                 history_.current().archive_path)) {
            if (!capabilities.move_entries) return DROPEFFECT_NONE;
            return (allowed & DROPEFFECT_MOVE) != 0
                ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        }
    }
    return capabilities.update && axiom::gui::data_object_has_file_drop(object)
        ? DROPEFFECT_COPY
        : DROPEFFECT_NONE;
}

DWORD MainWindow::perform_file_drop(IDataObject* object, POINT point, DWORD, DWORD allowed) {
    if (busy_ || history_.current().kind != axiom::gui::BrowserLocationKind::archive) {
        return DROPEFFECT_NONE;
    }
    const fs::path archive = history_.current().archive_path;
    const auto capabilities = active_archive_capabilities();
    if (capabilities.locked || capabilities.directory_encrypted) {
        show_app_message(
            capabilities.locked
                ? L"This archive is locked and cannot be changed."
                : L"Editing archives with encrypted file names is not supported yet.",
                         axiom::gui::MessageDialogIcon::warning,
                         L"Drop into archive");
        return DROPEFFECT_NONE;
    }
    const std::string destination_directory = archive_drop_directory(point);

    axiom::gui::ArchiveDragPayload payload;
    const bool archive_drag = axiom::gui::data_object_has_archive_entries(object);
    if (archive_drag && !axiom::gui::read_archive_entries(object, payload)) {
        return DROPEFFECT_NONE;
    }
    if (archive_drag && same_filesystem_path(payload.archive_path, archive)) {
        if (!capabilities.move_entries) {
            show_app_message(L"This archive format does not support moving entries.",
                             axiom::gui::MessageDialogIcon::information,
                             L"Move in archive");
            return DROPEFFECT_NONE;
        }
        std::vector<axiom::ArchiveMove> moves;
        moves.reserve(payload.entry_paths.size());
        for (const auto& source : payload.entry_paths) {
            const std::string destination =
                join_archive_directory(destination_directory, archive_name(source));
            if (destination != source) moves.push_back({source, destination});
        }
        if (moves.empty()) return DROPEFFECT_NONE;
        auto password = password_for_archive_edit(archive);
        if (!password) return DROPEFFECT_NONE;
        auto options = compression_options();
        options.password = std::move(*password);
        operation_archive_output_ = archive;
        start_operation(
            L"Moving archive entries...", L"Archive entries moved.",
            [archive, moves = std::move(moves), options](
                std::shared_ptr<axiom::OperationControl> operation) mutable {
                auto run_options = options;
                run_options.operation = std::move(operation);
                const auto* provider = axiom::archive_provider_for_path(archive);
                if (provider == nullptr) {
                    throw std::runtime_error("unsupported archive format");
                }
                provider->move_entries(archive, moves, run_options);
            });
        return (allowed & DROPEFFECT_MOVE) != 0 ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    }
    if (!capabilities.update) {
        show_app_message(L"This archive format does not support adding entries.",
                         axiom::gui::MessageDialogIcon::information,
                         L"Drop into archive");
        return DROPEFFECT_NONE;
    }

    auto paths = axiom::gui::read_file_drop(object);
    if (paths.empty()) return DROPEFFECT_NONE;
    const std::wstring prompt =
        L"Add " + quote_count(paths.size(), L"item", L"items") +
        L" to this archive folder?\n\nFiles with matching names will be replaced.";
    if (show_app_message(prompt, axiom::gui::MessageDialogIcon::question,
                         L"Add to archive", axiom::gui::MessageDialogButtons::yes_no,
                         IDYES) != IDYES) {
        return DROPEFFECT_NONE;
    }
    auto password = password_for_archive_edit(archive);
    if (!password) return DROPEFFECT_NONE;
    std::vector<axiom::ArchiveInput> inputs;
    inputs.reserve(paths.size());
    for (const auto& path : paths) {
        inputs.push_back({path,
            join_archive_directory(destination_directory, utf8(path.filename().wstring()))});
    }
    auto options = compression_options();
    options.password = std::move(*password);
    operation_archive_output_ = archive;
    start_operation(
        L"Adding dropped items...", L"Dropped items were added to the archive.",
        [archive, inputs = std::move(inputs), options](
            std::shared_ptr<axiom::OperationControl> operation) mutable {
            auto run_options = options;
            run_options.operation = std::move(operation);
            const auto* provider = axiom::archive_provider_for_path(archive);
            if (provider == nullptr) {
                throw std::runtime_error("unsupported archive format");
            }
            provider->add_mapped(inputs, archive, run_options);
        });
    return DROPEFFECT_COPY;
}

StagedArchiveEntries MainWindow::extract_archive_entries_to_staging(
    const fs::path& archive, const std::vector<std::string>& entries,
    const std::string& password, bool for_drag, bool sensitive) {
    const auto* provider = axiom::archive_provider_for_path(archive);
    if (provider == nullptr || !provider->capabilities(archive, password).selective_extract) {
        throw std::runtime_error("this archive format does not support selective extraction");
    }
    const fs::path staging = create_drag_staging_directory(sensitive);
    const HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (completed == nullptr) throw std::runtime_error("could not start temporary extraction");
    auto control = std::make_shared<axiom::OperationControl>();
    std::exception_ptr failure;

    set_busy(true);
    set_status(for_drag ? L"Preparing archive entries for drag and drop..."
                        : L"Preparing archive file for viewing...");
    if (!operation_window_.create(
            hwnd_, instance_,
            for_drag ? L"Preparing dragged archive entries..."
                     : L"Opening archive file...",
            staging,
            make_operation_window_theme(theme_),
            [control](bool paused) { control->set_paused(paused); },
            [control] { control->request_cancel(); })) {
        CloseHandle(completed);
        set_busy(false);
        throw std::runtime_error("could not create temporary extraction progress window");
    }
    control->set_progress_callback([target = hwnd_](const axiom::OperationProgress& progress) {
        auto* copy = new axiom::OperationProgress(progress);
        if (!PostMessageW(target, kOperationProgressMessage, 0,
                          reinterpret_cast<LPARAM>(copy))) {
            delete copy;
        }
    });

    const std::size_t io_buffer_size = configured_io_buffer_size(application_options_);
    std::jthread worker([&, control, io_buffer_size] {
        try {
            axiom::ExtractOptions options;
            options.overwrite = axiom::ExtractOptions::Overwrite::overwrite;
            options.password = password;
            options.io_buffer_size = io_buffer_size;
            options.operation = control;
            provider->extract_selected(archive, entries, staging, options);
        } catch (...) {
            failure = std::current_exception();
        }
        SetEvent(completed);
    });

    bool quit_seen = false;
    while (WaitForSingleObject(completed, 0) != WAIT_OBJECT_0) {
        const DWORD wait = MsgWaitForMultipleObjects(1, &completed, FALSE, INFINITE,
                                                     QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) break;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                quit_seen = true;
                control->request_cancel();
                continue;
            }
            if (message.message == WM_CLOSE && message.hwnd == hwnd_) {
                quit_seen = true;
                control->request_cancel();
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    worker.join();
    CloseHandle(completed);
    operation_window_.close();
    set_busy(false);
    operation_paused_ = false;
    if (quit_seen) PostQuitMessage(0);
    if (failure) std::rethrow_exception(failure);

    StagedArchiveEntries staged;
    staged.directory = staging;
    staged.paths.reserve(entries.size());
    for (const auto& entry : entries) {
        staged.paths.push_back(staging / fs::path(widen(entry)));
    }
    set_status(for_drag ? L"Drag and drop ready." : L"Archive file is ready to open.");
    return staged;
}

void MainWindow::on_table_begin_drag() {
    if (busy_) return;
    auto filesystem_paths = selected_filesystem_paths();
    if (!filesystem_paths.empty()) {
        axiom::gui::FileDragSource source;
        source.files = [paths = std::move(filesystem_paths)] { return paths; };
        source.preferred_effect = DROPEFFECT_COPY;
        DWORD effect = DROPEFFECT_NONE;
        axiom::gui::do_file_drag(std::move(source),
                                 DROPEFFECT_COPY | DROPEFFECT_MOVE, effect);
        return;
    }

    if (history_.current().kind != axiom::gui::BrowserLocationKind::archive) return;
    auto entries = selected_archive_paths();
    if (entries.empty()) return;
    const fs::path archive = history_.current().archive_path;

    auto password = password_for_archive_edit(archive);
    if (!password) return;

    std::wstring drag_error;
    axiom::gui::FileDragSource source;
    source.archive_payload = {archive, entries};
    source.preferred_effect = DROPEFFECT_COPY;
    source.error_message = &drag_error;
    source.files = [this, archive, entries, password = std::move(*password)]() mutable {
        try {
            auto staged = extract_archive_entries_to_staging(
                archive, entries, password, true, !password.empty());
            secure_clear(password);
            return staged.paths;
        } catch (...) {
            secure_clear(password);
            throw;
        }
    };
    DWORD effect = DROPEFFECT_NONE;
    axiom::gui::do_file_drag(std::move(source),
                             DROPEFFECT_COPY | DROPEFFECT_MOVE, effect);
    if (!drag_error.empty()) {
        show_app_message(drag_error, axiom::gui::MessageDialogIcon::error,
                         L"Drag from archive");
    }
}

std::optional<fs::path> MainWindow::active_archive_path() const {
    if (history_.current().kind == axiom::gui::BrowserLocationKind::archive) {
        return history_.current().archive_path;
    }
    for (int index : selected_browser_indices()) {
        const auto& item = browser_items_[index];
        if (item.kind == axiom::gui::BrowserItemKind::archive) return item.filesystem_path;
    }
    return std::nullopt;
}

axiom::gui::ArchiveCapabilities MainWindow::active_archive_capabilities() const {
    const auto archive = active_archive_path();
    if (archive && archive_catalog_ &&
        same_filesystem_path(archive_catalog_->path(), *archive)) {
        return archive_catalog_->capabilities();
    }
    if (archive) {
        if (const auto* provider = axiom::archive_provider_for_path(*archive)) {
            try {
                return provider->capabilities(*archive);
            } catch (...) {
                return {};
            }
        }
    }
    return {};
}

const axiom::ArchiveProvider* MainWindow::active_archive_provider() const {
    const auto archive = active_archive_path();
    if (!archive) return nullptr;
    if (archive_catalog_ && same_filesystem_path(archive_catalog_->path(), *archive)) {
        return &archive_catalog_->provider();
    }
    return axiom::archive_provider_for_path(*archive);
}

axiom::gui::ArchiveFeatureAvailability MainWindow::implemented_feature_availability() {
    axiom::gui::ArchiveFeatureAvailability availability;
    // Metadata, ADS, and links are automatic; the dialog explains that rather
    // than exposing toggles which the archive API cannot honor independently.
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
    // Custom KDF presets are not exposed by the current API.
    availability.kdf_presets = false;
    return availability;
}

axiom::gui::ArchiveFeatureAvailability MainWindow::feature_availability_from_capabilities(
    const axiom::gui::ArchiveCapabilities& capabilities) {
    axiom::gui::ArchiveFeatureAvailability availability;
    availability.metadata = capabilities.metadata;
    availability.update = capabilities.update;
    availability.comments = capabilities.comments;
    availability.lock = capabilities.lock;
    availability.quick_open = capabilities.selective_extract;
    availability.encryption = capabilities.encryption || capabilities.encrypted;
    availability.header_encryption = capabilities.encryption;
    availability.kdf_presets = false;
    availability.volumes = capabilities.multi_volume;
    availability.recovery = capabilities.recovery_records;
    availability.authenticity = capabilities.authenticity;
    availability.sfx = capabilities.sfx;
    availability.posix_metadata = capabilities.metadata;
    return availability;
}

bool MainWindow::signature_key_is_trusted(const axiom::ArchiveSignatureInfo& info) const {
    if (!info.present || !info.valid) return false;
    if (application_options_.trusted_keys_folder.empty()) return true;
    std::error_code iterate_error;
    for (fs::directory_iterator it(application_options_.trusted_keys_folder,
                                   fs::directory_options::skip_permission_denied,
                                   iterate_error), end;
         !iterate_error && it != end; it.increment(iterate_error)) {
        std::error_code status_error;
        if (!it->is_regular_file(status_error)) continue;
        if (key_file_contains_public_key(it->path(), info.public_key)) {
            return true;
        }
    }
    return false;
}

void MainWindow::create_archive_from_paths(
    std::vector<fs::path> paths,
    std::optional<fs::path> target_archive,
    axiom::gui::ArchiveUpdateMode update_mode) {
    if (paths.empty()) return;

    axiom::gui::CreateArchiveDialogOptions dialog_options;
    dialog_options.level = application_options_.default_level;
    dialog_options.thread_count = application_options_.default_thread_count;
    dialog_options.dictionary_size = selected_dictionary_size_ != 0
        ? selected_dictionary_size_
        : application_options_.default_dictionary_size;
    dialog_options.word_size = selected_word_size_ != 0
        ? selected_word_size_
        : application_options_.default_word_size;
    dialog_options.solid_block_size = selected_solid_block_size_ != 0
        ? selected_solid_block_size_
        : application_options_.default_solid_block_size;
    dialog_options.feature_availability = implemented_feature_availability();
    dialog_options.features.update_mode =
        update_mode == axiom::gui::ArchiveUpdateMode::create_new
            ? static_cast<axiom::gui::ArchiveUpdateMode>(
                  std::clamp(application_options_.default_update_mode, 0, 4))
            : update_mode;
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
    const fs::path source_folder =
        history_.current().kind == axiom::gui::BrowserLocationKind::filesystem
            ? history_.current().filesystem_path
            : paths.front().parent_path();
    const std::wstring archive_file_name =
        suggested_archive_stem_for_inputs(paths, source_folder) + L".axar";
    const fs::path base = history_.current().kind == axiom::gui::BrowserLocationKind::filesystem
        ? history_.current().filesystem_path
        : paths.front().parent_path();
    fs::path default_archive = base / archive_file_name;
    if (application_options_.archive_output_mode == 1 &&
        !persisted_settings_.last_archive_output_folder.empty()) {
        default_archive =
            fs::path(persisted_settings_.last_archive_output_folder) / archive_file_name;
    } else if (application_options_.archive_output_mode == 2 &&
        !application_options_.archive_output_folder.empty()) {
        default_archive = fs::path(application_options_.archive_output_folder) / archive_file_name;
    }
    default_archive = avoid_archive_input_collision(std::move(default_archive), paths);
    dialog_options.archive_path = target_archive.value_or(default_archive);
    dialog_options.archive_format =
        axiom::archive_provider_for_path(dialog_options.archive_path)
            ? axiom::archive_provider_for_path(dialog_options.archive_path)->info().format
            : axiom::ArchiveFormat::axar;
    dialog_options.fixed_archive_format = target_archive.has_value();
    if (target_archive) {
        const auto* target_provider = axiom::archive_provider_for_path(*target_archive);
        if (target_provider != nullptr && !target_provider->info().native) {
            dialog_options.archive_format = target_provider->info().format;
            dialog_options.features.create_sfx = false;
            dialog_options.features.sign_archive = false;
            dialog_options.features.create_recovery_volumes = false;
            dialog_options.features.recovery_percent = 0;
            dialog_options.features.volume_size.clear();
        } else {
            try {
                const auto mode = axiom::archive_encryption_mode(*target_archive);
                if (mode == axiom::ArchiveEncryptionMode::data_and_directory) {
                    show_app_message(
                        L"Editing archives with encrypted file names is not supported yet.",
                        axiom::gui::MessageDialogIcon::information);
                    return;
                }
                // Existing plaintext archives cannot be converted to encrypted form
                // by append/update, and existing data-only archives cannot switch to
                // encrypted names without a complete format rewrite.
                dialog_options.feature_availability.header_encryption = false;
                if (mode == axiom::ArchiveEncryptionMode::none) {
                    dialog_options.feature_availability.encryption = false;
                }
                if (mode == axiom::ArchiveEncryptionMode::data_only) {
                    auto password = password_for_archive_edit(*target_archive);
                    if (!password) return;
                    dialog_options.features.encrypt_data = true;
                    dialog_options.features.password = widen(*password);
                    secure_clear(*password);
                }
                dialog_options.features.comment = widen(axiom::archive_comment(
                    *target_archive, dialog_options.features.encrypt_data
                        ? utf8(dialog_options.features.password) : std::string{}));
            } catch (...) {
                // The operation itself will report a precise archive error if necessary.
            }
        }
    }
    if (!axiom::gui::show_create_archive_dialog(hwnd_, paths.size(), dialog_options)) return;

    inputs_ = std::move(paths);
    selected_level_ = dialog_options.level;
    selected_thread_count_ = dialog_options.thread_count;
    selected_dictionary_size_ = dialog_options.dictionary_size;
    selected_word_size_ = dialog_options.word_size;
    selected_solid_block_size_ = dialog_options.solid_block_size;
    pending_archive_path_ = std::move(dialog_options.archive_path);
    if (pending_archive_path_.has_parent_path()) {
        persisted_settings_.last_archive_output_folder =
            pending_archive_path_.parent_path().wstring();
        save_current_settings();
    }
    pending_archive_features_ = std::move(dialog_options.features);
    on_compress();
}

void MainWindow::on_add_to_archive() {
    if (history_.current().kind == axiom::gui::BrowserLocationKind::archive) {
        const auto archive = active_archive_path();
        if (!archive || !active_archive_is_editable()) return;
        if (!active_archive_capabilities().update) {
            show_app_message(L"This archive format does not support adding entries.",
                             axiom::gui::MessageDialogIcon::information);
            return;
        }
        auto paths = pick_files(hwnd_);
        if (!paths.empty()) {
            create_archive_from_paths(std::move(paths), *archive,
                                      axiom::gui::ArchiveUpdateMode::add_or_replace);
        }
        return;
    }
    auto paths = selected_filesystem_paths();
    if (paths.empty()) paths = pick_files(hwnd_);
    create_archive_from_paths(std::move(paths));
}

void MainWindow::on_update_archive(axiom::gui::ArchiveUpdateMode mode) {
    const auto archive = active_archive_path();
    const auto capabilities = active_archive_capabilities();
    if (!archive) {
        show_app_message(L"Open an archive first.",
                         axiom::gui::MessageDialogIcon::information);
        return;
    }
    if (capabilities.locked || capabilities.directory_encrypted) {
        show_app_message(
            capabilities.locked
                ? L"This archive is locked and cannot be changed."
                : L"Editing archives with encrypted file names is not supported yet.",
            axiom::gui::MessageDialogIcon::information);
        return;
    }
    if (!capabilities.update) {
        show_app_message(L"This archive format does not support updating entries.",
                         axiom::gui::MessageDialogIcon::information);
        return;
    }
    if (mode == axiom::gui::ArchiveUpdateMode::synchronize &&
        show_app_message(
            L"Synchronize will mirror the selected source into the archive.\n\n"
            L"Archived entries missing from the source will be permanently removed. Continue?",
            axiom::gui::MessageDialogIcon::warning, L"Synchronize archive",
            axiom::gui::MessageDialogButtons::yes_no, IDNO) != IDYES) {
        return;
    }
    auto paths = pick_files(hwnd_);
    if (!paths.empty()) create_archive_from_paths(std::move(paths), *archive, mode);
}

bool MainWindow::active_archive_is_editable() {
    const auto capabilities = active_archive_capabilities();
    if (!capabilities.locked && !capabilities.directory_encrypted &&
        (capabilities.update || capabilities.delete_entries || capabilities.move_entries)) {
        return true;
    }
    show_app_message(
        capabilities.locked
            ? L"This archive is locked and cannot be changed."
            : capabilities.directory_encrypted
                ? L"Editing archives with encrypted file names is not supported yet."
                : L"This archive format cannot be changed.",
        axiom::gui::MessageDialogIcon::information);
    return false;
}

void MainWindow::on_view() {
    on_table_activate();
}

void MainWindow::on_delete_from_archive() {
    const auto archive = active_archive_path();
    if (!archive || history_.current().kind != axiom::gui::BrowserLocationKind::archive) {
        show_app_message(L"Open an archive and select entries to delete.",
                         axiom::gui::MessageDialogIcon::information);
        return;
    }
    if (!active_archive_is_editable()) return;
    const auto* provider = axiom::archive_provider_for_path(*archive);
    if (provider == nullptr || !active_archive_capabilities().delete_entries) {
        show_app_message(L"This archive format does not support deleting entries.",
                         axiom::gui::MessageDialogIcon::information);
        return;
    }
    std::vector<std::string> paths;
    for (const int index : selected_browser_indices()) {
        const auto& item = browser_items_[static_cast<std::size_t>(index)];
        if (!item.is_parent() && !item.archive_path.empty()) {
            paths.push_back(item.archive_path);
        }
    }
    if (paths.empty()) {
        show_app_message(L"Select one or more archive entries to delete.",
                         axiom::gui::MessageDialogIcon::information);
        return;
    }
    const std::wstring prompt = L"Permanently remove " +
        quote_count(paths.size(), L"selected archive entry", L"selected archive entries") +
        L"?\n\nThe archive will be rebuilt to reclaim their stored data.";
    if (show_app_message(prompt, axiom::gui::MessageDialogIcon::warning,
                         L"Delete from archive",
                         axiom::gui::MessageDialogButtons::yes_no, IDNO) != IDYES) {
        return;
    }
    const auto options = compression_options();
    operation_archive_output_ = *archive;
    start_operation(
        L"Deleting archive entries...", L"Selected entries were removed.",
        [archive = *archive, paths = std::move(paths), options](
            std::shared_ptr<axiom::OperationControl> operation) mutable {
            auto run_options = options;
            run_options.operation = std::move(operation);
            const auto* provider = axiom::archive_provider_for_path(archive);
            if (provider == nullptr) {
                throw std::runtime_error("unsupported archive format");
            }
            provider->delete_entries(archive, paths, run_options);
        });
}

void MainWindow::on_repack_archive() {
    const auto archive = active_archive_path();
    if (!archive) return;
    if (!active_archive_is_editable()) return;
    if (show_app_message(
            L"Rebuild this archive to reclaim dead space?\n\n"
            L"All live files will be recompressed into fresh solid blocks.",
            axiom::gui::MessageDialogIcon::question, L"Repack archive",
            axiom::gui::MessageDialogButtons::yes_no, IDNO) != IDYES) {
        return;
    }
    const auto options = compression_options();
    operation_archive_output_ = *archive;
    start_operation(
        L"Repacking archive...", L"Archive repacked successfully.",
        [archive = *archive, options](
            std::shared_ptr<axiom::OperationControl> operation) mutable {
            auto run_options = options;
            run_options.operation = std::move(operation);
            axiom::repack_archive(archive, run_options);
        });
}

void MainWindow::on_edit_archive_comment() {
    const auto archive = active_archive_path();
    if (!archive) return;
    if (!active_archive_is_editable()) return;
    std::wstring comment;
    try {
        comment = widen(axiom::archive_comment(*archive));
    } catch (const std::exception& error) {
        show_app_message(widen(error.what()), axiom::gui::MessageDialogIcon::error,
                         L"Archive comment");
        return;
    }
    if (!axiom::gui::show_archive_comment_dialog(hwnd_, comment)) return;
    const auto options = compression_options();
    const std::string encoded_comment = utf8(comment);
    operation_archive_output_ = *archive;
    start_operation(
        L"Updating archive comment...", L"Archive comment updated.",
        [archive = *archive, encoded_comment, options](
            std::shared_ptr<axiom::OperationControl> operation) mutable {
            auto run_options = options;
            run_options.operation = std::move(operation);
            axiom::set_archive_comment(archive, encoded_comment, run_options);
        });
}

void MainWindow::on_lock_archive() {
    const auto archive = active_archive_path();
    if (!archive) return;
    if (!active_archive_is_editable()) return;
    if (show_app_message(
            L"Lock this archive permanently?\n\n"
            L"A locked archive remains readable, but it cannot be updated, deleted from, "
            L"repacked, commented, or unlocked.",
            axiom::gui::MessageDialogIcon::warning, L"Lock archive",
            axiom::gui::MessageDialogButtons::yes_no, IDNO) != IDYES) {
        return;
    }
    const auto options = compression_options();
    operation_archive_output_ = *archive;
    start_operation(
        L"Locking archive...", L"Archive locked successfully.",
        [archive = *archive, options](
            std::shared_ptr<axiom::OperationControl> operation) mutable {
            auto run_options = options;
            run_options.operation = std::move(operation);
            axiom::lock_archive(archive, run_options);
        });
}

void MainWindow::on_delete_selected() {
    if (history_.current().kind == axiom::gui::BrowserLocationKind::archive) {
        on_delete_from_archive();
        return;
    }
    const auto paths = selected_filesystem_paths();
    if (paths.empty()) {
        show_app_message(L"Select one or more filesystem items to delete.",
                         axiom::gui::MessageDialogIcon::information);
        return;
    }
    const std::wstring prompt = L"Move " +
        quote_count(paths.size(), L"selected item", L"selected items") +
        L" to the Recycle Bin?";
    if (application_options_.confirm_delete &&
        show_app_message(prompt, axiom::gui::MessageDialogIcon::warning, L"Axiom",
                         axiom::gui::MessageDialogButtons::yes_no, IDNO) != IDYES) {
        return;
    }

    ComPtr<IFileOperation> operation;
    HRESULT result = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(operation.put()));
    if (SUCCEEDED(result)) {
        operation->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR |
                                     FOF_SILENT | FOFX_RECYCLEONDELETE | FOFX_EARLYFAILURE);
        for (const auto& path : paths) {
            ComPtr<IShellItem> item;
            result = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put()));
            if (FAILED(result)) break;
            result = operation->DeleteItem(item.get(), nullptr);
            if (FAILED(result)) break;
        }
        if (SUCCEEDED(result)) result = operation->PerformOperations();
    }
    if (FAILED(result)) {
        show_app_message(L"Windows could not move the selected items to the Recycle Bin.",
                         axiom::gui::MessageDialogIcon::error, L"Delete failed");
    }
    on_navigate_refresh();
}

void MainWindow::on_repair_archive() {
    const auto archive = active_archive_path();
    if (!archive || busy_) return;
    axiom::ArchiveRecoveryInfo info;
    try {
        info = axiom::archive_recovery_info(*archive);
    } catch (const std::exception& error) {
        show_app_message(widen(error.what()), axiom::gui::MessageDialogIcon::error,
                         L"Repair archive");
        return;
    }
    if (!info.present) {
        show_app_message(L"This archive does not contain a recovery record.",
                         axiom::gui::MessageDialogIcon::information,
                         L"Repair archive");
        return;
    }
    if (show_app_message(
            L"Check every recovery shard and reconstruct damaged archive data?\n\n"
            L"The archive is replaced atomically only after reconstruction succeeds.",
            axiom::gui::MessageDialogIcon::question, L"Repair archive",
            axiom::gui::MessageDialogButtons::yes_no, IDYES) != IDYES) {
        return;
    }
    operation_archive_output_ = *archive;
    start_operation(
        L"Repairing archive...", L"Archive recovery data was verified and rebuilt.",
        [archive = *archive](std::shared_ptr<axiom::OperationControl> operation) {
            if (!axiom::repair_archive(archive, operation)) {
                throw std::runtime_error("archive has no recovery record");
            }
        });
}

void MainWindow::on_create_recovery_volumes() {
    const auto archive = active_archive_path();
    if (!archive || busy_) return;
    axiom::gui::ArchiveFeatureOptions archive_options;
    axiom::gui::ExtractFeatureOptions extract_options;
    axiom::gui::ArchiveFeatureAvailability availability;
    availability.recovery = true;
    availability.volumes = true;
    try {
        const auto info = axiom::archive_recovery_info(*archive);
        archive_options.recovery_percent = info.present
            ? static_cast<int>(info.percent) : 10;
    } catch (...) {
        archive_options.recovery_percent = 10;
    }
    if (!axiom::gui::show_archive_feature_options_dialog(
            hwnd_, axiom::gui::ArchiveFeatureDialogContext::create_or_update,
            archive_options, extract_options, availability)) {
        return;
    }
    const auto size = parse_volume_size(archive_options.volume_size,
                                        archive_options.volume_unit);
    if (!size) {
        show_app_message(L"Enter a valid positive split-volume size, or leave it blank.",
                         axiom::gui::MessageDialogIcon::warning,
                         L"Recovery and volumes");
        return;
    }
    const unsigned percent = static_cast<unsigned>(
        std::clamp(archive_options.recovery_percent, 0, 100));
    const bool split = *size != 0;
    if (archive_options.create_recovery_volumes && !split) {
        show_app_message(L"Set a split-volume size before enabling recovery volumes.",
                         axiom::gui::MessageDialogIcon::warning,
                         L"Recovery and volumes");
        return;
    }
    operation_archive_output_ = *archive;
    start_operation(
        split ? L"Creating recovery and split volumes..."
              : L"Updating archive recovery record...",
        split ? L"Archive and recovery volumes created beside the source archive."
              : (percent == 0 ? L"Archive recovery record removed."
                              : L"Archive recovery record updated."),
        [archive = *archive, percent, split, volume_size = *size,
         make_recovery_volumes = archive_options.create_recovery_volumes](
            std::shared_ptr<axiom::OperationControl> operation) {
            axiom::set_archive_recovery(archive, percent, operation);
            if (!split) return;
            const std::uint64_t archive_bytes = fs::file_size(archive);
            const std::uint64_t data_count = std::max<std::uint64_t>(
                1, (archive_bytes + volume_size - 1) / volume_size);
            const unsigned recovery_count = make_recovery_volumes
                ? static_cast<unsigned>(std::max<std::uint64_t>(
                    1, (data_count * std::max(percent, 10u) + 99) / 100))
                : 0;
            axiom::create_archive_volumes(
                archive, volume_size, recovery_count, operation);
        });
}

void MainWindow::apply_operation_priority() {
    operation_priority_changed_ = false;
    previous_priority_class_ = 0;
    DWORD desired = 0;
    if (application_options_.worker_priority == 1) {
        desired = BELOW_NORMAL_PRIORITY_CLASS;
    } else if (application_options_.worker_priority == 2) {
        desired = IDLE_PRIORITY_CLASS;
    }
    if (desired == 0) return;
    HANDLE process = GetCurrentProcess();
    previous_priority_class_ = GetPriorityClass(process);
    if (previous_priority_class_ != 0 && previous_priority_class_ != desired &&
        SetPriorityClass(process, desired)) {
        operation_priority_changed_ = true;
    }
}

void MainWindow::restore_operation_priority() {
    if (operation_priority_changed_ && previous_priority_class_ != 0) {
        SetPriorityClass(GetCurrentProcess(), previous_priority_class_);
    }
    operation_priority_changed_ = false;
    previous_priority_class_ = 0;
}

void MainWindow::start_operation(std::wstring running,
                     std::wstring success,
                     std::function<void(std::shared_ptr<axiom::OperationControl>)> work) {
    if (busy_) {
        return;
    }

    // The archive engine writes its temporary output in the browsed directory.
    // Suppress any refresh already queued for those writes; completion performs
    // one authoritative reload after success, failure, or cancellation.
    KillTimer(hwnd_, kDirectoryRefreshTimer);
    set_busy(true);
    set_status(running);
    if (!operation_window_.create(
            hwnd_, instance_, running, operation_archive_output_,
            make_operation_window_theme(theme_),
            [this](bool paused) {
                if (!busy_ || !operation_runner_.running()) return;
                operation_paused_ = paused;
                operation_runner_.set_paused(paused);
                set_status(paused ? L"Operation paused." : L"Operation resumed.");
            },
            [this] {
                if (!busy_ || !operation_runner_.running()) return;
                operation_runner_.cancel();
                set_status(L"Cancelling operation...");
            })) {
        set_busy(false);
        set_status(L"Could not create the operation progress window.");
        return;
    }
    // The operation's temporary output can generate thousands of directory
    // notifications. Completion reloads the current location and restarts the
    // watcher, so keeping it active here only consumes I/O and UI resources.
    directory_watcher_.stop();
    append_log(L"Starting operation: " + running);
    apply_operation_priority();
    if (!operation_runner_.start(hwnd_, kOperationDoneMessage, kOperationProgressMessage,
                                 std::move(running), std::move(success), std::move(work))) {
        restore_operation_priority();
        append_log(L"Could not start operation: another operation is already running.");
        operation_window_.close();
        set_busy(false);
        set_status(L"Another operation is already running.");
        on_navigate_refresh();
    }
}

std::optional<std::uint64_t> MainWindow::parse_volume_size(
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

void MainWindow::on_compress() {
    if (inputs_.empty()) {
        show_app_message(L"Add at least one file or folder first.",
                         axiom::gui::MessageDialogIcon::information);
        return;
    }

    const auto archive = pending_archive_path_;
    if (archive.empty()) {
        show_app_message(L"Choose an output archive path.",
                         axiom::gui::MessageDialogIcon::information);
        return;
    }
    const auto* archive_provider = axiom::archive_provider_for_path(archive);
    if (archive_provider == nullptr) {
        show_app_message(L"Choose a supported archive format (.axar or .zip).",
                         axiom::gui::MessageDialogIcon::warning,
                         L"Archive format");
        return;
    }
    const auto provider_capabilities = archive_provider->capabilities(archive);
    if (!provider_capabilities.create &&
        pending_archive_features_.update_mode == axiom::gui::ArchiveUpdateMode::create_new) {
        show_app_message(
            L"This format is read-only in Axiom. Create .axar or .zip archives instead.",
            axiom::gui::MessageDialogIcon::warning,
            L"Archive format");
        return;
    }
    if (!provider_capabilities.update &&
        pending_archive_features_.update_mode != axiom::gui::ArchiveUpdateMode::create_new) {
        show_app_message(L"This archive format is read-only and cannot be updated.",
                         axiom::gui::MessageDialogIcon::warning,
                         L"Archive format");
        return;
    }

    const auto inputs = inputs_;
    auto options = compression_options();
    const auto mode = pending_archive_features_.update_mode;
    options.encrypt_header = pending_archive_features_.encrypt_names;
    options.recovery_percent = static_cast<unsigned>(
        std::clamp(pending_archive_features_.recovery_percent, 0, 100));
    options.password = pending_archive_features_.encrypt_data
        ? utf8(pending_archive_features_.password)
        : std::string{};
    const auto volume_size = parse_volume_size(
        pending_archive_features_.volume_size, pending_archive_features_.volume_unit);
    if (!volume_size) {
        show_app_message(L"Enter a valid positive split-volume size, or leave it blank.",
                         axiom::gui::MessageDialogIcon::warning,
                         L"Recovery and volumes");
        return;
    }
    const bool split_after = *volume_size != 0;
    const bool recovery_volumes = pending_archive_features_.create_recovery_volumes;
    if (recovery_volumes && !split_after) {
        show_app_message(L"Set a split-volume size before enabling recovery volumes.",
                         axiom::gui::MessageDialogIcon::warning,
                         L"Recovery and volumes");
        return;
    }
    const std::string comment = utf8(pending_archive_features_.comment);
    const bool set_comment = mode != axiom::gui::ArchiveUpdateMode::create_new ||
                             !comment.empty();
    const bool repack_after = pending_archive_features_.repack_after_update &&
                              mode != axiom::gui::ArchiveUpdateMode::create_new;
    const bool lock_after = pending_archive_features_.lock_archive;
    const bool sign_after = pending_archive_features_.sign_archive;
    const bool create_sfx_after = pending_archive_features_.create_sfx;
    const bool native_archive = archive_provider->info().native;
    const bool zip_archive = archive_provider->info().format == axiom::ArchiveFormat::zip;
    if (!native_archive &&
        ((!zip_archive && pending_archive_features_.encrypt_data) ||
         pending_archive_features_.encrypt_names ||
         !pending_archive_features_.comment.empty() ||
         pending_archive_features_.lock_archive ||
         pending_archive_features_.repack_after_update ||
         pending_archive_features_.recovery_percent != 0 ||
         !pending_archive_features_.volume_size.empty() ||
         pending_archive_features_.create_recovery_volumes ||
         pending_archive_features_.sign_archive ||
         pending_archive_features_.create_sfx)) {
        show_app_message(
            L"The selected archive format does not support one or more Axiom-only "
            L"options. ZIP supports file-data encryption only; choose Axiom archive "
            L"format for encrypted names, Recovery, SFX/signing, comment, or lock options.",
            axiom::gui::MessageDialogIcon::warning,
            L"Archive format");
        return;
    }
    if (create_sfx_after && split_after) {
        show_app_message(L"Self-extracting archives and split volumes are separate output "
                         L"modes. Disable one of them.",
                         axiom::gui::MessageDialogIcon::warning,
                         L"Archive output");
        return;
    }
    fs::path sfx_output = pending_archive_features_.sfx_destination;
    if (create_sfx_after && sfx_output.empty()) {
        sfx_output = archive;
        sfx_output.replace_extension(L".exe");
    }
    if (application_options_.confirm_overwrite &&
        mode == axiom::gui::ArchiveUpdateMode::create_new) {
        std::error_code exists_error;
        const fs::path output_path = create_sfx_after ? sfx_output : archive;
        if (!output_path.empty() && fs::exists(output_path, exists_error)) {
            if (show_app_message(
                    L"Replace the existing output file?\n\n" + output_path.wstring(),
                    axiom::gui::MessageDialogIcon::warning,
                    L"Overwrite output",
                    axiom::gui::MessageDialogButtons::yes_no,
                    IDNO) != IDYES) {
                return;
            }
        }
    }
    const fs::path sfx_stub = current_executable_path();
    axiom::ArchiveSigningKey signing_key;
    if (sign_after) {
        std::ifstream key_file(pending_archive_features_.signing_key, std::ios::binary);
        if (!key_file ||
            !key_file.read(reinterpret_cast<char*>(signing_key.secret_key.data()),
                           static_cast<std::streamsize>(signing_key.secret_key.size())) ||
            key_file.peek() != std::char_traits<char>::eof()) {
            show_app_message(L"Choose a valid 64-byte Axiom signing key.",
                             axiom::gui::MessageDialogIcon::error,
                             L"Sign archive");
            return;
        }
        std::copy_n(signing_key.secret_key.begin() + 32,
                    signing_key.public_key.size(), signing_key.public_key.begin());
    }
    secure_clear(pending_archive_features_.password);

    std::wstring running = L"Compressing...";
    std::wstring success = L"Archive created: " + archive.wstring();
    switch (mode) {
        case axiom::gui::ArchiveUpdateMode::add_or_replace:
            running = L"Adding to archive...";
            success = L"Archive updated: " + archive.wstring();
            break;
        case axiom::gui::ArchiveUpdateMode::update_newer:
            running = L"Updating archive...";
            success = L"Archive updated: " + archive.wstring();
            break;
        case axiom::gui::ArchiveUpdateMode::fresh_existing:
            running = L"Freshening archive...";
            success = L"Archive freshened: " + archive.wstring();
            break;
        case axiom::gui::ArchiveUpdateMode::synchronize:
            running = L"Synchronizing archive...";
            success = L"Archive synchronized: " + archive.wstring();
            break;
        default:
            break;
    }
    if (create_sfx_after) {
        if (mode == axiom::gui::ArchiveUpdateMode::create_new) {
            running = L"Creating self-extracting archive...";
        }
        success = L"Self-extracting archive created: " + sfx_output.wstring();
    } else if (split_after) {
        running = L"Creating split archive volumes...";
        success = L"Split archive volumes created beside: " + archive.wstring();
    }
    operation_archive_output_ = archive;
    start_operation(std::move(running), std::move(success),
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
                                                          operation,
                                                          options.io_buffer_size);
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
}

fs::path MainWindow::current_executable_path() const {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return fs::path(std::move(buffer));
}

void MainWindow::on_create_sfx() {
    const auto archive = active_archive_path();
    if (!archive || busy_) return;
    fs::path output = *archive;
    output.replace_extension(L".exe");
    if (show_app_message(L"Create this self-extracting archive?\n\n" + output.wstring(),
                         axiom::gui::MessageDialogIcon::question, L"Create SFX",
                         axiom::gui::MessageDialogButtons::yes_no, IDYES) != IDYES) {
        return;
    }
    const fs::path stub = current_executable_path();
    const std::size_t io_buffer_size = configured_io_buffer_size(application_options_);
    operation_archive_output_.clear();
    start_operation(L"Creating self-extracting archive...",
                    L"Self-extracting archive created: " + output.wstring(),
                    [archive = *archive, stub, output, io_buffer_size](
                        std::shared_ptr<axiom::OperationControl> operation) {
                        axiom::create_sfx_archive(archive, stub, output, operation,
                                                  io_buffer_size);
                    });
}

void MainWindow::on_verify_archive_signature() {
    const auto archive = active_archive_path();
    if (!archive) return;
    std::string password;
    try {
        if (axiom::archive_is_encrypted(*archive)) {
            std::wstring entered;
            if (!axiom::gui::show_archive_password_dialog(hwnd_, entered)) return;
            password = utf8(entered);
            secure_clear(entered);
        }
        const auto info = axiom::verify_archive_signature(*archive, password);
        if (!info.present) {
            show_app_message(L"This archive is not signed.",
                             axiom::gui::MessageDialogIcon::information,
                             L"Verify signature");
            return;
        }
        const bool trusted = signature_key_is_trusted(info);
        std::wstringstream fingerprint;
        fingerprint << std::hex << std::setfill(L'0');
        for (std::size_t index = 0; index < 8; ++index) {
            fingerprint << std::setw(2) << static_cast<unsigned>(info.public_key[index]);
            if (index == 3) fingerprint << L'-';
        }
        show_app_message(
            std::wstring(info.valid ? L"The archive signature is valid."
                                    : L"The archive signature is invalid.") +
                L"\n\nSigner fingerprint: " + fingerprint.str() +
                (info.valid && !application_options_.trusted_keys_folder.empty()
                     ? (trusted ? L"\nTrusted key: yes" : L"\nTrusted key: no")
                     : L""),
            info.valid && trusted ? axiom::gui::MessageDialogIcon::information
                                  : axiom::gui::MessageDialogIcon::error,
            L"Verify signature");
    } catch (const std::exception& error) {
        show_app_message(widen(error.what()), axiom::gui::MessageDialogIcon::error,
                         L"Verify signature");
    }
}

void MainWindow::on_extract() {
    const auto archive = active_archive_path();
    if (!archive) {
        show_app_message(L"Open or select a supported archive first.",
                         axiom::gui::MessageDialogIcon::information);
        return;
    }
    const auto* provider = active_archive_provider();
    if (provider == nullptr) {
        show_app_message(L"This archive format is not supported.",
                         axiom::gui::MessageDialogIcon::warning, L"Extract archive");
        return;
    }
    axiom::gui::ExtractArchiveDialogOptions dialog_options;
    dialog_options.thread_count = application_options_.default_thread_count;
    if (application_options_.extract_destination_mode == 1 &&
        !persisted_settings_.last_extract_destination_folder.empty()) {
        dialog_options.destination =
            fs::path(persisted_settings_.last_extract_destination_folder) / archive->stem();
    } else if (application_options_.extract_destination_mode == 2 &&
        !application_options_.extract_destination_folder.empty()) {
        dialog_options.destination =
            fs::path(application_options_.extract_destination_folder) / archive->stem();
    } else {
        dialog_options.destination = archive->parent_path() / archive->stem();
    }
    auto capabilities = active_archive_capabilities();
    if (!capabilities.list && !capabilities.extract && !capabilities.test) {
        try {
            capabilities = provider->capabilities(*archive);
        } catch (const std::exception& error) {
            show_app_message(widen(error.what()), axiom::gui::MessageDialogIcon::error,
                             L"Open archive");
            return;
        }
    }
    if (!capabilities.extract) {
        show_app_message(L"This archive format does not support extraction.",
                         axiom::gui::MessageDialogIcon::information,
                         L"Extract archive");
        return;
    }
    dialog_options.feature_availability =
        feature_availability_from_capabilities(capabilities);
    dialog_options.features.verify_signature = application_options_.verify_signatures;
    const bool encrypted = capabilities.encrypted;
    dialog_options.feature_availability.encryption = encrypted;
    if (!axiom::gui::show_extract_archive_dialog(hwnd_, *archive, dialog_options)) return;
    if (dialog_options.destination.has_parent_path()) {
        persisted_settings_.last_extract_destination_folder =
            dialog_options.destination.parent_path().wstring();
        save_current_settings();
    }

    if (encrypted && dialog_options.features.password.empty() &&
        !axiom::gui::show_archive_password_dialog(
            hwnd_, dialog_options.features.password)) {
        return;
    }

    axiom::ExtractOptions options;
    options.thread_count = dialog_options.thread_count;
    options.io_buffer_size = configured_io_buffer_size(application_options_);
    options.restore_mtime = dialog_options.restore_mtime;
    options.overwrite = dialog_options.overwrite
        ? axiom::ExtractOptions::Overwrite::overwrite
        : axiom::ExtractOptions::Overwrite::fail;
    options.password = utf8(dialog_options.features.password);
    secure_clear(dialog_options.features.password);
    const bool attempt_recovery = dialog_options.features.attempt_recovery;
    const bool verify_signature = dialog_options.features.verify_signature;
    const std::wstring trusted_keys_folder = application_options_.trusted_keys_folder;

    operation_archive_output_ = attempt_recovery ? *archive : fs::path{};
    start_operation(L"Extracting...",
                    L"Extracted to: " + dialog_options.destination.wstring(),
                    [archive = *archive, provider, output = dialog_options.destination, options,
                     attempt_recovery, verify_signature, trusted_keys_folder](
                        std::shared_ptr<axiom::OperationControl> operation) mutable {
                        auto run_options = options;
                        run_options.operation = operation;
                        const auto capabilities =
                            provider->capabilities(archive, run_options.password);
                        if (verify_signature && capabilities.authenticity) {
                            const auto signature = axiom::verify_archive_signature(
                                archive, run_options.password);
                            if (signature.present && !signature.valid) {
                                throw axiom::FormatError("archive signature is invalid");
                            }
                            if (signature.present && signature.valid &&
                                !trusted_keys_folder.empty()) {
                                bool trusted = false;
                                std::error_code iterate_error;
                                for (fs::directory_iterator it(
                                         trusted_keys_folder,
                                         fs::directory_options::skip_permission_denied,
                                         iterate_error), end;
                                     !iterate_error && it != end;
                                     it.increment(iterate_error)) {
                                    std::error_code status_error;
                                    if (!it->is_regular_file(status_error)) continue;
                                    if (key_file_contains_public_key(it->path(),
                                                                    signature.public_key)) {
                                        trusted = true;
                                        break;
                                    }
                                }
                                if (!trusted) {
                                    throw axiom::FormatError(
                                        "archive signature is valid but not trusted");
                                }
                            }
                        }
                        try {
                            provider->extract_all(archive, output, run_options);
                        } catch (const axiom::FormatError&) {
                            if (!attempt_recovery || !capabilities.recovery_records ||
                                !axiom::repair_archive(archive, operation)) {
                                throw;
                            }
                            auto retry_options = run_options;
                            retry_options.overwrite =
                                axiom::ExtractOptions::Overwrite::overwrite;
                            provider->extract_all(archive, output, retry_options);
                        }
                    });
}

void MainWindow::on_test() {
    const auto archive = active_archive_path();
    if (!archive) {
        show_app_message(L"Open or select a supported archive first.",
                         axiom::gui::MessageDialogIcon::information);
        return;
    }
    const auto* provider = active_archive_provider();
    if (provider == nullptr) {
        show_app_message(L"This archive format is not supported.",
                         axiom::gui::MessageDialogIcon::warning, L"Test archive");
        return;
    }

    axiom::DecompressionOptions options;
    options.thread_count = application_options_.default_thread_count;
    options.io_buffer_size = configured_io_buffer_size(application_options_);
    try {
        const auto capabilities = provider->capabilities(*archive);
        if (!capabilities.test) {
            show_app_message(L"This archive format does not support integrity testing.",
                             axiom::gui::MessageDialogIcon::information,
                             L"Test archive");
            return;
        }
        if (capabilities.encrypted) {
            std::wstring password;
            if (!axiom::gui::show_archive_password_dialog(hwnd_, password)) return;
            options.password = utf8(password);
            secure_clear(password);
        }
    } catch (const std::exception& error) {
        show_app_message(widen(error.what()), axiom::gui::MessageDialogIcon::error,
                         L"Open archive");
        return;
    }
    operation_archive_output_ = *archive;
    start_operation(L"Testing archive...",
                    L"Archive integrity test passed.",
                    [archive = *archive, provider, options](
                        std::shared_ptr<axiom::OperationControl> operation) mutable {
                        auto run_options = options;
                        run_options.operation = operation;
                        const auto capabilities =
                            provider->capabilities(archive, run_options.password);
                        try {
                            provider->test(archive, run_options);
                        } catch (const axiom::FormatError&) {
                            if (!capabilities.recovery_records ||
                                !axiom::repair_archive(archive, operation)) {
                                throw;
                            }
                            provider->test(archive, run_options);
                        }
                    });
}

void MainWindow::on_operation_progress(LPARAM lparam) {
    std::unique_ptr<axiom::OperationProgress> progress(
        reinterpret_cast<axiom::OperationProgress*>(lparam));
    if (!progress) {
        return;
    }

    // Keep only the newest worker update already waiting in the UI queue.
    // Every payload is heap-owned, so replacing the unique_ptr also releases
    // each stale update without changing the worker's progress cadence.
    MSG queued{};
    while (PeekMessageW(&queued, hwnd_, kOperationProgressMessage,
                        kOperationProgressMessage, PM_REMOVE)) {
        progress.reset(reinterpret_cast<axiom::OperationProgress*>(queued.lParam));
    }
    if (!busy_) {
        return;
    }
    operation_window_.set_progress(*progress);
}

void MainWindow::on_operation_done(LPARAM lparam) {
    std::unique_ptr<axiom::gui::OperationResult> result(
        reinterpret_cast<axiom::gui::OperationResult*>(lparam));
    operation_runner_.finish();
    restore_operation_priority();
    operation_window_.close();
    set_busy(false);
    const bool archive_changed = !operation_archive_output_.empty();
    fs::path open_after = std::move(operation_open_after_);
    operation_archive_output_.clear();
    if (archive_changed) archive_catalog_.reset();
    append_log(std::wstring(result->ok ? L"Operation succeeded: "
                                       : result->cancelled ? L"Operation cancelled: "
                                                           : L"Operation failed: ") +
               result->message);
    set_status(result->message);
    if (result->ok && !open_after.empty()) {
        navigate_to(axiom::gui::BrowserLocation::archive(std::move(open_after)));
    } else {
        on_navigate_refresh();
    }
    if (result->cancelled) {
        return;
    }
    show_app_message(
        result->message,
        result->ok ? axiom::gui::MessageDialogIcon::information
                   : axiom::gui::MessageDialogIcon::error,
        result->ok ? L"Axiom" : L"Axiom error");
}

}  // namespace axiom::gui
