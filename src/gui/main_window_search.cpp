// The Find Files dialog shown from the main window.

#include "gui/main_window_internal.hpp"

#include <deque>
#include <fstream>
#include <unordered_set>

namespace axiom::gui {

namespace {
constexpr DWORD kFileSearchStyle =
    WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
    WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
constexpr DWORD kFileSearchExStyle = WS_EX_WINDOWEDGE | WS_EX_CONTROLPARENT;

// Matches beyond this are not listed; the index itself is not capped.
constexpr std::size_t kFileSearchResultLimit = 1000000;

// Searches each field remembers.
constexpr std::size_t kHistoryLimit = 20;

// The result columns, in order, and the order each one sorts by.
constexpr std::array<axiom::search::SortKey, 7> kColumnSortKeys{
    axiom::search::SortKey::name,   axiom::search::SortKey::location,
    axiom::search::SortKey::type,   axiom::search::SortKey::size,
    axiom::search::SortKey::packed, axiom::search::SortKey::modified,
    axiom::search::SortKey::crc32,
};
constexpr int kColumnCount = static_cast<int>(kColumnSortKeys.size());

std::wstring trimmed(std::wstring text) {
    while (!text.empty() && std::iswspace(text.back())) text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && std::iswspace(text[start])) ++start;
    return text.substr(start);
}

std::wstring entry_type_text(const axiom::search::Entry& entry) {
    if (entry.kind == axiom::search::EntryKind::link) return L"Link";
    return axiom::gui::browser_type_text(fs::path(entry.name),
                                         entry.kind == axiom::search::EntryKind::folder);
}

std::wstring crc_text(std::uint32_t crc) {
    wchar_t text[16]{};
    swprintf_s(text, L"%08X", crc);
    return text;
}

// ---- export --------------------------------------------------------------------

enum class ExportFormat { csv, tsv, paths };

ExportFormat export_format_for(const fs::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (extension == L".tsv") return ExportFormat::tsv;
    if (extension == L".txt") return ExportFormat::paths;
    return ExportFormat::csv;
}

// A spreadsheet reads a cell that starts with one of these as a formula, so a
// file named "=cmd|..." could run something when the export is opened.
bool looks_like_formula(std::wstring_view text) {
    return !text.empty() && (text.front() == L'=' || text.front() == L'+' ||
                             text.front() == L'-' || text.front() == L'@' ||
                             text.front() == L'\t' || text.front() == L'\r');
}

std::wstring export_cell(std::wstring text, ExportFormat format) {
    if (looks_like_formula(text)) text.insert(text.begin(), L'\'');
    if (format == ExportFormat::tsv) {
        // Names inside archives may hold any character; a tab or line break
        // would split the row.
        for (wchar_t& c : text) {
            if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
        }
        return text;
    }
    if (text.find_first_of(L",\"\r\n") == std::wstring::npos) return text;
    std::wstring quoted = L"\"";
    for (wchar_t c : text) {
        if (c == L'"') quoted += L'"';
        quoted += c;
    }
    quoted += L'"';
    return quoted;
}

// Write the rows as UTF-8 with a byte order mark, so spreadsheets read names
// in any script correctly. Throws on a write error; removes the file when stopped.
void write_export(const axiom::search::Index& index, const std::vector<std::uint32_t>& rows,
                  const fs::path& path, ExportFormat format, const std::stop_token& stop) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("could not create the file");
    std::string buffer = "\xEF\xBB\xBF";
    const auto flush = [&] {
        out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        buffer.clear();
        if (!out) throw std::runtime_error("could not write the file");
    };
    const wchar_t separator = format == ExportFormat::csv ? L',' : L'\t';
    const auto write_row = [&](const std::vector<std::wstring>& cells) {
        std::wstring line;
        for (std::size_t k = 0; k < cells.size(); ++k) {
            if (k != 0) line += separator;
            line += export_cell(cells[k], format);
        }
        line += L"\r\n";
        buffer += utf8(line);
        if (buffer.size() >= (1u << 20)) flush();
    };
    if (format != ExportFormat::paths) {
        write_row({L"Name", L"Location", L"Path", L"Type", L"Size", L"Packed", L"Modified",
                   L"CRC-32"});
    }
    for (const std::uint32_t row : rows) {
        if (stop.stop_requested()) {
            out.close();
            std::error_code ignored;
            fs::remove(path, ignored);
            return;
        }
        const auto& entry = index.entries()[row];
        if (format == ExportFormat::paths) {
            buffer += utf8(index.display_path(entry) + L"\r\n");
            if (buffer.size() >= (1u << 20)) flush();
            continue;
        }
        const bool folder = entry.kind == axiom::search::EntryKind::folder;
        write_row({entry.name,
                   index.folder(entry.folder).display,
                   index.display_path(entry),
                   entry_type_text(entry),
                   folder ? std::wstring{} : std::to_wstring(entry.size),
                   entry.has_packed ? std::to_wstring(entry.packed) : std::wstring{},
                   axiom::gui::browser_time_text(entry.modified),
                   entry.has_crc32 ? crc_text(entry.crc32) : std::wstring{}});
    }
    flush();
    out.close();
    if (!out) throw std::runtime_error("could not write the file");
}

std::optional<fs::path> pick_export_file(HWND owner) {
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) {
        return std::nullopt;
    }
    static const COMDLG_FILTERSPEC filters[] = {
        {L"Comma-separated values (*.csv)", L"*.csv"},
        {L"Tab-separated values (*.tsv)", L"*.tsv"},
        {L"List of paths (*.txt)", L"*.txt"},
    };
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
    dialog->SetTitle(L"Export search results");
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetDefaultExtension(L"csv");
    dialog->SetFileName(L"Search results.csv");
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return std::nullopt;
    PWSTR name = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) || name == nullptr) {
        return std::nullopt;
    }
    fs::path path(name);
    CoTaskMemFree(name);
    return path;
}

}  // namespace

FileSearchDialog::FileSearchDialog(HINSTANCE instance,
                 ThemePalette theme,
                 UINT dpi,
                 axiom::gui::BrowserLocation location,
                 std::shared_ptr<const axiom::gui::ArchiveCatalog> archive_catalog,
                 bool include_hidden)
    : instance_(instance),
      theme_(theme),
      dpi_(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi),
      scope_(L"Scope: " + location.display_name()),
      location_(std::move(location)),
      archive_catalog_(std::move(archive_catalog)),
      include_hidden_(include_hidden),
      settings_(load_find_files_settings()) {
    if (settings_.sort_column < -1 || settings_.sort_column >= kColumnCount) {
        settings_.sort_column = -1;
    }
    sort_column_ = settings_.sort_column;
    sort_ascending_ = settings_.sort_ascending;
    if (settings_.name_history.size() > kHistoryLimit) settings_.name_history.resize(kHistoryLimit);
    if (settings_.content_history.size() > kHistoryLimit) {
        settings_.content_history.resize(kHistoryLimit);
    }
}

FileSearchDialogResult FileSearchDialog::show(HWND owner) {
    owner_ = owner;
    if (!register_class(instance_)) {
        return result_;
    }

    const SIZE window_size = dialog_window_size_for_client(
        940, 728, kFileSearchStyle, kFileSearchExStyle, dpi_);
    const int width = window_size.cx;
    const int height = window_size.cy;
    const POINT position = axiom::gui::centered_window_position(owner, width, height);
    hwnd_ = CreateWindowExW(
        kFileSearchExStyle,
        class_name(), L"Find files",
        kFileSearchStyle,
        position.x, position.y, width, height, owner, nullptr, instance_, this);
    if (hwnd_ == nullptr) return result_;

    axiom::gui::apply_axiom_window_icons(hwnd_, instance_);
    const int show_command = axiom::gui::restore_named_window_placement(
        hwnd_, owner, L"FileSearchDialog");
    const bool owner_was_enabled = axiom::gui::disable_dialog_owner(owner, hwnd_);
    ShowWindow(hwnd_, show_command);
    UpdateWindow(hwnd_);

    MSG message{};
    while (IsWindow(hwnd_)) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (axiom::gui::message_targets_window(hwnd_, message)) {
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
                destroy_modal_dialog(hwnd_);
                continue;
            }
            if (message.message == WM_KEYDOWN && handle_shortcut(message)) continue;
        }
        if (!IsDialogMessageW(hwnd_, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    axiom::gui::restore_dialog_owner(owner, owner_was_enabled);
    return result_;
}

const wchar_t* FileSearchDialog::class_name() {
    return L"AxiomFileSearchDialog";
}

bool FileSearchDialog::register_class(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    if (GetClassInfoExW(instance, class_name(), &wc)) return true;
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = &FileSearchDialog::window_proc;
    wc.lpszClassName = class_name();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_DBLCLKS;
    axiom::gui::assign_axiom_window_class_icons(wc, instance);
    return RegisterClassExW(&wc) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

int FileSearchDialog::scale(int value) const {
    return scale_for_dpi(dpi_, value);
}

HWND FileSearchDialog::make_control(const wchar_t* class_name,
                  const wchar_t* text,
                  DWORD style,
                  int id,
                  DWORD ex_style) {
    HWND control = CreateWindowExW(
        ex_style, class_name, text,
        style | WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 0, 0, hwnd_,
        id == 0 ? nullptr : reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        instance_, nullptr);
    if (control != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        axiom::gui::apply_dialog_control_theme(control, theme_.dark);
    }
    return control;
}

std::array<HWND, 29> FileSearchDialog::controls() const {
    return {title_, scope_label_, query_label_, search_edit_,
            content_label_, content_edit_, max_size_label_, max_size_edit_,
            match_case_, whole_name_, search_path_,
            include_files_, include_folders_, include_archives_,
            result_count_, search_button_, export_button_, go_to_button_, close_button_,
            where_label_, search_area_label_, search_area_,
            archive_types_label_, archive_types_, exclude_label_, exclude_edit_,
            include_subfolders_, find_in_archives_, skip_encrypted_};
}

bool FileSearchDialog::checkbox_checked(int id) const {
    switch (id) {
        case kMatchCase: return match_case_checked_;
        case kWholeName: return whole_name_checked_;
        case kSearchPath: return search_path_checked_;
        case kIncludeFiles: return include_files_checked_;
        case kIncludeFolders: return include_folders_checked_;
        case kIncludeArchives: return include_archives_checked_;
        case kIncludeSubfolders: return include_subfolders_checked_;
        case kFindInArchives: return find_in_archives_checked_;
        case kSkipEncrypted: return skip_encrypted_checked_;
        default: return false;
    }
}

void FileSearchDialog::set_checkbox(int id, bool checked) {
    switch (id) {
        case kMatchCase: match_case_checked_ = checked; break;
        case kWholeName: whole_name_checked_ = checked; break;
        case kSearchPath: search_path_checked_ = checked; break;
        case kIncludeFiles: include_files_checked_ = checked; break;
        case kIncludeFolders: include_folders_checked_ = checked; break;
        case kIncludeArchives: include_archives_checked_ = checked; break;
        case kIncludeSubfolders: include_subfolders_checked_ = checked; break;
        case kFindInArchives: find_in_archives_checked_ = checked; break;
        case kSkipEncrypted: skip_encrypted_checked_ = checked; break;
        default: return;
    }
    if (HWND control = GetDlgItem(hwnd_, id)) {
        InvalidateRect(control, nullptr, FALSE);
    }
}

void FileSearchDialog::toggle_checkbox(int id) {
    if ((id == kIncludeFiles || id == kIncludeFolders ||
         id == kIncludeArchives) &&
        checkbox_checked(id)) {
        const int enabled_types =
            static_cast<int>(include_files_checked_) +
            static_cast<int>(include_folders_checked_) +
            static_cast<int>(include_archives_checked_);
        if (enabled_types == 1) {
            MessageBeep(MB_ICONINFORMATION);
            set_text(result_count_,
                     L"At least one result type must remain selected.");
            return;
        }
    }
    set_checkbox(id, !checkbox_checked(id));
    // Options that change what is searched walk again (the index key changes);
    // the rest refine what was found.
    if (search_started_) run_search(false);
}

std::wstring FileSearchDialog::walk_key() const {
    int area = static_cast<int>(SendMessageW(search_area_, CB_GETCURSEL, 0, 0));
    if (area < 0) area = 0;
    std::wstring key = location_.display_name();
    key += L"|" + std::to_wstring(area);
    key += include_subfolders_checked_ ? L"|sub" : L"|top";
    key += include_hidden_ ? L"|hidden" : L"|visible";
    key += L"|exclude:" + trimmed(get_text(exclude_edit_));
    if (find_in_archives_checked_ && location_.kind != axiom::gui::BrowserLocationKind::archive) {
        key += L"|archives:" + get_text(archive_types_);
        key += skip_encrypted_checked_ ? L"|skip" : L"|open";
    }
    const std::wstring content = get_text(content_edit_);
    if (!content.empty()) {
        // Only files whose names match are read, so with a content search every
        // part of the query decides what is searched.
        key += L"|content:" + content + L"|max:" + get_text(max_size_edit_);
        key += L"|query:" + get_text(search_edit_);
        key += match_case_checked_ ? L"|case" : L"|nocase";
        key += whole_name_checked_ ? L"|whole" : L"|part";
        key += search_path_checked_ ? L"|path" : L"|name";
        key += include_files_checked_ ? L"f" : L"-";
        key += include_archives_checked_ ? L"a" : L"-";
    }
    return key;
}

void FileSearchDialog::run_search(bool rescan) {
    if (!include_files_checked_ && !include_folders_checked_ &&
        !include_archives_checked_) {
        MessageBeep(MB_ICONINFORMATION);
        set_text(result_count_,
                 L"Select at least one result type before searching.");
        EnableWindow(go_to_button_, FALSE);
        return;
    }
    search_started_ = true;
    if (rescan) remember_history();

    axiom::search::QueryOptions options;
    options.match_case = match_case_checked_;
    options.whole_name = whole_name_checked_;
    options.search_path = search_path_checked_;
    options.files = include_files_checked_;
    options.folders = include_folders_checked_;
    options.archives = include_archives_checked_;
    std::wstring error;
    auto query = axiom::search::Query::compile(get_text(search_edit_), options, &error);
    if (!query) {
        // Keep the last results; say what is wrong with the new query.
        query_error_ = std::move(error);
        update_status();
        return;
    }
    const std::wstring content_text = get_text(content_edit_);
    if (!content_text.empty() && !axiom::search::parse_size_text(get_text(max_size_edit_))) {
        query_error_ = L"Skip files over needs a size such as 64 MB or 1 GB";
        update_status();
        return;
    }
    query_error_.clear();
    export_status_.clear();
    query_ = std::move(query);
    content_active_ = !content_text.empty();
    matches_.clear();
    matched_upto_ = 0;
    sorted_count_ = 0;
    results_.set_virtual_row_count(0);
    EnableWindow(go_to_button_, FALSE);

    if (rescan || !index_ || index_key_ != walk_key()) {
        start_walk();
    }
    match_pending();
    if (walking_ || (index_ && matched_upto_ < index_->entries().size())) {
        SetTimer(hwnd_, kMatchTimer, 15, nullptr);
    }
    update_status();
}

void FileSearchDialog::start_walk() {
    // A walk still running is told to stop and left to finish on its own; its
    // batches carry an older generation and are dropped.
    const std::uint64_t generation = ++search_generation_;
    index_ = std::make_shared<axiom::search::Index>();
    index_key_ = walk_key();
    walk_started_ = std::chrono::steady_clock::now();
    walk_seconds_ = 0;
    stopped_ = false;

    int area = static_cast<int>(SendMessageW(search_area_, CB_GETCURSEL, 0, 0));
    if (area < 0) area = 0;
    const bool recurse = include_subfolders_checked_;

    axiom::search::WalkOptions options;
    options.recurse = recurse;
    options.include_hidden = include_hidden_;
    options.exclude_folders = trimmed(get_text(exclude_edit_));
    options.search_archives = find_in_archives_checked_;
    options.archive_patterns = get_text(archive_types_);
    options.skip_encrypted = skip_encrypted_checked_;
    // The name query decides which files are read for the content, so the walk
    // keeps its own copy.
    std::shared_ptr<const axiom::search::Query> content_filter;
    if (content_active_) {
        options.content = axiom::search::ContentPattern::compile(get_text(content_edit_),
                                                                 match_case_checked_);
        options.content_max_size =
            axiom::search::parse_size_text(get_text(max_size_edit_)).value_or(64ull << 20);
        content_filter = std::make_shared<const axiom::search::Query>(*query_);
    }
    const HWND target = hwnd_;

    if (location_.kind == axiom::gui::BrowserLocationKind::archive) {
        // The open archive's directory is already in memory: list it at once.
        axiom::search::Batch batch;
        if (archive_catalog_) {
            const axiom::search::FolderExcludes excludes(options.exclude_folders);
            axiom::search::add_archive_entries(
                batch, archive_catalog_->path(), archive_catalog_->entries(),
                area == 1 ? std::string_view{} : std::string_view(location_.archive_directory),
                recurse, &excludes);
        }
        if (!content_active_ || !archive_catalog_) {
            walking_ = false;
            index_->append(std::move(batch));
            walk_seconds_ = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - walk_started_).count();
            return;
        }
        // Reading its contents is not: that runs on the worker.
        walking_ = true;
        search_thread_.start([target, generation, batch = std::move(batch),
                              archive = archive_catalog_->path(), options = std::move(options),
                              content_filter](std::stop_token stop) mutable {
            options.content_filter = content_filter.get();
            axiom::search::search_archive_contents(batch, archive, 0, options, stop);
            // Stopped means its window may be gone: deliver nothing.
            if (stop.stop_requested()) return;
            auto* payload = new BatchPayload{generation, std::move(batch)};
            if (!PostMessageW(target, kSearchBatchMessage, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
            PostMessageW(target, kSearchDoneMessage, static_cast<WPARAM>(generation), 0);
        });
        return;
    }

    std::vector<fs::path> roots;
    if (location_.kind == axiom::gui::BrowserLocationKind::filesystem) {
        fs::path root = location_.filesystem_path;
        if (area == 1) {
            root = location_.filesystem_path.root_path();
            if (root.empty()) root = location_.filesystem_path;
        }
        roots.push_back(std::move(root));
    } else {
        const DWORD required = GetLogicalDriveStringsW(0, nullptr);
        std::wstring drives(static_cast<std::size_t>(required) + 1, L'\0');
        GetLogicalDriveStringsW(static_cast<DWORD>(drives.size()), drives.data());
        for (const wchar_t* drive = drives.c_str(); *drive != L'\0'; drive += wcslen(drive) + 1) {
            roots.emplace_back(drive);
        }
    }

    walking_ = true;
    search_thread_.start([target, generation, roots = std::move(roots),
                          options = std::move(options),
                          content_filter](std::stop_token stop) mutable {
        options.content_filter = content_filter.get();
        axiom::search::walk(roots, options, stop, [&](axiom::search::Batch&& batch) {
            // Stopped means its window may be gone: deliver nothing.
            if (stop.stop_requested()) return;
            auto* payload = new BatchPayload{generation, std::move(batch)};
            if (!PostMessageW(target, kSearchBatchMessage, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        });
        if (!stop.stop_requested()) {
            PostMessageW(target, kSearchDoneMessage, static_cast<WPARAM>(generation), 0);
        }
    });
}

void FileSearchDialog::on_batch(LPARAM value) {
    std::unique_ptr<BatchPayload> payload(reinterpret_cast<BatchPayload*>(value));
    if (!payload || payload->generation != search_generation_ || !index_) return;
    index_->append(std::move(payload->batch));
    SetTimer(hwnd_, kMatchTimer, 15, nullptr);
}

void FileSearchDialog::on_walk_done(WPARAM generation) {
    if (static_cast<std::uint64_t>(generation) != search_generation_) return;
    walking_ = false;
    walk_seconds_ = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - walk_started_).count();
    match_pending();
    update_status();
}

void FileSearchDialog::stop_search() {
    if (!walking_) return;
    // The threads finish on their own; whatever they still send carries an
    // older generation and is dropped.
    search_thread_.request_stop();
    ++search_generation_;
    walking_ = false;
    stopped_ = true;
    // What was found is kept, but it is not the whole area: the next search
    // reads the disk again rather than refining it.
    index_key_.clear();
    walk_seconds_ = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - walk_started_).count();
    match_pending();
    update_status();
}

void FileSearchDialog::match_pending() {
    if (!index_ || !query_) return;
    // Matched in slices so a large index never stalls the window: what is left
    // continues on the next timer tick.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(12);
    const auto& entries = index_->entries();
    const std::size_t before = matches_.size();
    while (matched_upto_ < entries.size() && matches_.size() < kFileSearchResultLimit) {
        const std::size_t end = std::min(entries.size(), matched_upto_ + 2048);
        for (; matched_upto_ < end; ++matched_upto_) {
            const auto& entry = entries[matched_upto_];
            if ((!content_active_ || entry.content == axiom::search::ContentState::found) &&
                query_->matches(*index_, entry) && matches_.size() < kFileSearchResultLimit) {
                matches_.push_back(static_cast<std::uint32_t>(matched_upto_));
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
    }
    const bool caught_up = matched_upto_ >= entries.size() ||
                           matches_.size() >= kFileSearchResultLimit;
    const bool finished = caught_up && !walking_;
    if (matches_.size() != before) {
        results_.set_virtual_row_count(matches_.size());
        sort_new_matches(finished);
        if (before == 0) {
            results_.select_index(0);
            EnableWindow(go_to_button_, TRUE);
        }
    } else if (finished) {
        sort_new_matches(true);
    }
    if (finished) KillTimer(hwnd_, kMatchTimer);
}

void FileSearchDialog::update_status() {
    std::wstring status;
    if (!query_error_.empty()) {
        status = L"Query: " + query_error_;
    } else {
        status = quote_count(matches_.size(), L"match", L"matches");
        if (index_) {
            status += L"  |  " + quote_count(index_->entries().size(),
                                             L"item scanned", L"items scanned");
            if (walking_) {
                status += L"  |  searching...";
            } else if (matched_upto_ < index_->entries().size() &&
                       matches_.size() < kFileSearchResultLimit) {
                status += L"  |  matching...";
            } else {
                wchar_t seconds[48]{};
                swprintf_s(seconds, stopped_ ? L"  |  stopped after %.1f s" : L" in %.1f s",
                           walk_seconds_);
                status += seconds;
            }
            if (index_->skipped_encrypted() != 0) {
                status += L"  |  " + quote_count(index_->skipped_encrypted(),
                                                  L"encrypted archive skipped",
                                                  L"encrypted archives skipped");
            }
            if (content_active_) {
                status += L"  |  " + quote_count(index_->content_read(),
                                                  L"file read", L"files read");
                if (index_->content_skipped() != 0) {
                    status += L" (" + quote_count(index_->content_skipped(),
                                                  L"skipped", L"skipped") + L")";
                }
            }
            if (index_->read_errors() != 0) {
                status += L"  |  " + quote_count(index_->read_errors(),
                                                  L"read warning", L"read warnings");
            }
        }
        if (matches_.size() >= kFileSearchResultLimit) status += L"  |  result limit reached";
        if (!export_status_.empty()) status += L"  |  " + export_status_;
    }
    set_text(result_count_, status);
    EnableWindow(export_button_, can_export());
    // While a search runs, its button stops it.
    const wchar_t* label = walking_ ? L"Stop" : L"Search";
    if (get_text(search_button_) != label) set_text(search_button_, label);
}

std::vector<std::wstring> FileSearchDialog::result_row(std::size_t row) const {
    if (!index_ || row >= matches_.size()) return {};
    const auto& entry = index_->entries()[matches_[row]];
    const auto& folder = index_->folder(entry.folder);
    const bool folder_kind = entry.kind == axiom::search::EntryKind::folder;
    return {entry.name,
            folder.display,
            entry_type_text(entry),
            folder_kind ? std::wstring{} : format_size(entry.size),
            entry.has_packed ? format_size(entry.packed) : std::wstring{},
            axiom::gui::browser_time_text(entry.modified),
            entry.has_crc32 ? crc_text(entry.crc32) : std::wstring{}};
}

FileSearchDialogResult FileSearchDialog::result_target(std::size_t row) const {
    FileSearchDialogResult result;
    if (!index_ || row >= matches_.size()) return result;
    const auto& entry = index_->entries()[matches_[row]];
    const auto& folder = index_->folder(entry.folder);
    if (folder.archive != axiom::search::kNoArchive) {
        result.target_location = axiom::gui::BrowserLocation::archive(
            index_->archive(folder.archive), folder.archive_directory);
        result.archive_entry_path = entry.archive_path;
    } else {
        result.target_location = axiom::gui::BrowserLocation::filesystem(folder.filesystem);
        result.filesystem_path = folder.filesystem / entry.name;
    }
    return result;
}

void FileSearchDialog::accept_selected() {
    const int row = results_.focused_index();
    if (row < 0 || row >= static_cast<int>(matches_.size())) return;
    result_ = result_target(static_cast<std::size_t>(row));
    result_.accepted = true;
    destroy_modal_dialog(hwnd_);
}

// ---- sorting -------------------------------------------------------------------

axiom::search::SortKey FileSearchDialog::sort_key() const {
    return kColumnSortKeys[static_cast<std::size_t>(std::clamp(sort_column_, 0, kColumnCount - 1))];
}

std::vector<std::uint32_t> FileSearchDialog::selected_entries() const {
    std::vector<std::uint32_t> entries;
    for (const int row : results_.selected_indices()) {
        if (row >= 0 && row < static_cast<int>(matches_.size())) {
            entries.push_back(matches_[static_cast<std::size_t>(row)]);
        }
    }
    return entries;
}

std::optional<std::uint32_t> FileSearchDialog::focused_entry() const {
    const int row = results_.focused_index();
    if (row < 0 || row >= static_cast<int>(matches_.size())) return std::nullopt;
    return matches_[static_cast<std::size_t>(row)];
}

void FileSearchDialog::restore_selection(const std::vector<std::uint32_t>& entries,
                                         std::optional<std::uint32_t> focused, bool reveal) {
    if (entries.empty() && !focused) return;
    const std::unordered_set<std::uint32_t> wanted(entries.begin(), entries.end());
    std::vector<int> rows;
    rows.reserve(entries.size());
    int focused_row = -1;
    for (std::size_t row = 0; row < matches_.size(); ++row) {
        const std::uint32_t entry = matches_[row];
        if (wanted.contains(entry)) rows.push_back(static_cast<int>(row));
        if (focused && entry == *focused) focused_row = static_cast<int>(row);
    }
    results_.set_selection_and_scroll(std::move(rows), focused_row,
                                      results_.horizontal_scroll_position(),
                                      results_.vertical_scroll_position());
    if (reveal) results_.reveal_focused();
}

void FileSearchDialog::on_sort(int column) {
    if (column < 0 || column >= kColumnCount) return;
    if (column == sort_column_) {
        sort_ascending_ = !sort_ascending_;
    } else {
        sort_column_ = column;
        sort_ascending_ = true;
    }
    results_.set_sort_indicator(sort_column_, sort_ascending_);
    if (!index_ || matches_.empty()) {
        sorted_count_ = matches_.size();
        return;
    }
    const auto selection = selected_entries();
    const auto focus = focused_entry();
    axiom::search::sort_matches(*index_, matches_, sort_key(), sort_ascending_);
    sorted_count_ = matches_.size();
    last_merge_ = std::chrono::steady_clock::now();
    restore_selection(selection, focus, true);
    InvalidateRect(results_.hwnd(), nullptr, FALSE);
}

void FileSearchDialog::sort_new_matches(bool force) {
    if (sort_column_ < 0 || !index_ || sorted_count_ >= matches_.size()) return;
    // A merge is a pass over every match; with many of them it waits for a
    // pause between batches rather than running on every tick.
    const auto now = std::chrono::steady_clock::now();
    if (!force && matches_.size() > 50000 && now - last_merge_ < std::chrono::milliseconds(300)) {
        return;
    }
    const auto selection = selected_entries();
    const auto focus = focused_entry();
    axiom::search::merge_matches(*index_, matches_, sorted_count_, sort_key(), sort_ascending_);
    sorted_count_ = matches_.size();
    last_merge_ = now;
    restore_selection(selection, focus, false);
    InvalidateRect(results_.hwnd(), nullptr, FALSE);
}

// ---- result actions ------------------------------------------------------------

void FileSearchDialog::show_results_menu(POINT point) {
    const auto focus = focused_entry();
    const bool has_selection = !results_.selected_indices().empty();
    bool in_archive = false;
    if (focus) {
        in_archive = index_->folder(index_->entries()[*focus].folder).archive !=
                     axiom::search::kNoArchive;
    }
    if (point.x == -1 && point.y == -1) {
        // From the keyboard: beside the focused row, or the table's corner.
        RECT table{};
        GetWindowRect(results_.hwnd(), &table);
        point = {table.left + scale(24), table.top + scale(24)};
        if (const auto cell = results_.cell_rect(results_.focused_index(), 0)) {
            POINT corner{cell->left + scale(24), cell->bottom};
            ClientToScreen(results_.hwnd(), &corner);
            point = corner;
        }
    }
    std::vector<CustomMenuItem> items{
        {static_cast<UINT>(kGoToButton), L"&Go to", L"Enter", focus.has_value()},
        {static_cast<UINT>(kResultOpen), L"&Open", L"", focus.has_value() && !in_archive},
        {static_cast<UINT>(kResultOpenFolder), L"Open containing &folder", L"", focus.has_value()},
        {0, L"", L"", false, true},
        {static_cast<UINT>(kResultCopyPath), L"&Copy path", L"Ctrl+C", has_selection},
        {static_cast<UINT>(kResultCopyName), L"Copy &name", L"", has_selection},
        {0, L"", L"", false, true},
        {static_cast<UINT>(kResultSelectAll), L"Select &all", L"Ctrl+A", !matches_.empty()},
        {static_cast<UINT>(kExportButton), L"&Export results...", L"Ctrl+S", can_export()},
    };
    const UINT command = show_custom_context_menu(hwnd_, instance_, dpi_, font_,
                                                  make_custom_menu_theme(theme_),
                                                  std::move(items), point);
    if (command != 0) SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
}

void FileSearchDialog::open_result(bool containing_folder) {
    const auto focus = focused_entry();
    if (!focus) return;
    const auto& entry = index_->entries()[*focus];
    const auto& folder = index_->folder(entry.folder);
    fs::path path;
    if (folder.archive != axiom::search::kNoArchive) {
        if (!containing_folder) {
            MessageBeep(MB_ICONINFORMATION);
            export_status_ = L"items inside archives open with Go to";
            update_status();
            return;
        }
        // The folder that holds the archive, with the archive selected.
        path = index_->archive(folder.archive);
    } else {
        path = folder.filesystem / entry.name;
    }
    HINSTANCE launched = nullptr;
    if (containing_folder) {
        const std::wstring parameters = L"/select,\"" + path.wstring() + L"\"";
        launched = ShellExecuteW(hwnd_, L"open", L"explorer.exe", parameters.c_str(), nullptr,
                                 SW_SHOWNORMAL);
    } else {
        launched = ShellExecuteW(hwnd_, nullptr, path.c_str(), nullptr,
                                 path.parent_path().c_str(), SW_SHOWNORMAL);
    }
    if (reinterpret_cast<INT_PTR>(launched) <= 32) {
        MessageBeep(MB_ICONWARNING);
        export_status_ = L"could not open " + path.filename().wstring();
        update_status();
    }
}

void FileSearchDialog::copy_results(bool names_only) {
    if (!index_) return;
    std::wstring text;
    for (const std::uint32_t entry_id : selected_entries()) {
        const auto& entry = index_->entries()[entry_id];
        if (!text.empty()) text += L"\r\n";
        text += names_only ? entry.name : index_->display_path(entry);
    }
    if (!set_clipboard_text(hwnd_, text)) MessageBeep(MB_ICONWARNING);
}

bool FileSearchDialog::can_export() const {
    return index_ && !exporting_ && !walking_ && !matches_.empty() &&
           (matched_upto_ >= index_->entries().size() ||
            matches_.size() >= kFileSearchResultLimit);
}

void FileSearchDialog::export_results() {
    if (!can_export()) {
        MessageBeep(MB_ICONINFORMATION);
        if (walking_ || (index_ && matched_upto_ < index_->entries().size())) {
            export_status_ = L"export when the search has finished";
            update_status();
        }
        return;
    }
    const auto path = pick_export_file(hwnd_);
    if (!path) return;
    // The thread reads this index and these rows only; a new search makes a
    // new index rather than changing this one.
    std::shared_ptr<const axiom::search::Index> index = index_;
    std::vector<std::uint32_t> rows = matches_;
    exporting_ = true;
    export_status_ = L"exporting...";
    update_status();
    const HWND target = hwnd_;
    export_thread_.start([target, index, rows = std::move(rows), path = *path,
                          format = export_format_for(*path)](std::stop_token stop) {
        auto* result = new ExportResult{path, rows.size(), {}};
        try {
            write_export(*index, rows, path, format, stop);
        } catch (const std::exception& error) {
            result->error = widen(error.what());
        }
        if (!PostMessageW(target, kExportDoneMessage, 0, reinterpret_cast<LPARAM>(result))) {
            delete result;
        }
    });
}

void FileSearchDialog::on_export_done(LPARAM value) {
    std::unique_ptr<ExportResult> result(reinterpret_cast<ExportResult*>(value));
    exporting_ = false;
    if (!result) return;
    if (result->error.empty()) {
        export_status_ = quote_count(result->rows, L"result", L"results") + L" exported to " +
                         result->path.filename().wstring();
    } else {
        MessageBeep(MB_ICONWARNING);
        export_status_ = L"export failed: " + result->error;
    }
    update_status();
}

bool FileSearchDialog::handle_shortcut(const MSG& message) {
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (!control || shift) return false;
    if (message.wParam == 'S') {
        export_results();
        return true;
    }
    if (message.wParam == 'C' && message.hwnd == results_.hwnd()) {
        copy_results(false);
        return true;
    }
    return false;
}

// ---- history and settings ------------------------------------------------------

void FileSearchDialog::remember_history() {
    const auto remember = [](std::vector<std::wstring>& history, HWND combo) {
        const std::wstring text = trimmed(get_text(combo));
        if (text.empty()) return;
        std::erase(history, text);
        history.insert(history.begin(), text);
        if (history.size() > kHistoryLimit) history.resize(kHistoryLimit);
        // Refilling the list clears the field, so keep what is being edited.
        const std::wstring current = get_text(combo);
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        for (const auto& item : history) {
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
        }
        set_text(combo, current);
    };
    remember(settings_.name_history, search_edit_);
    remember(settings_.content_history, content_edit_);
}

void FileSearchDialog::save_settings() {
    if (search_started_) remember_history();
    settings_.exclude_folders = trimmed(get_text(exclude_edit_));
    settings_.column_widths = results_.logical_column_widths();
    settings_.sort_column = sort_column_;
    settings_.sort_ascending = sort_ascending_;
    save_find_files_settings(settings_);
}

void FileSearchDialog::layout() {
    if (hwnd_ == nullptr) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int margin = scale(18);
    const int label_height = scale(22);
    const int edit_height = scale(30);
    const int button_width = scale(92);
    const int button_height = scale(30);
    const int gap = scale(10);
    const int bottom = client.bottom - margin;
    // A combo box's height includes its drop-down list.
    const int list_height = scale(260);

    MoveWindow(title_, margin, margin, client.right - margin * 2,
               label_height, TRUE);
    MoveWindow(scope_label_, margin, margin + scale(24),
               client.right - margin * 2, label_height, TRUE);

    const int query_top = margin + scale(58);
    MoveWindow(query_label_, margin, query_top + scale(5), scale(120),
               label_height, TRUE);
    MoveWindow(search_edit_, margin + scale(126), query_top,
               client.right - margin * 2 - scale(126) - button_width - gap,
               list_height, TRUE);
    MoveWindow(search_button_, client.right - margin - button_width, query_top,
               button_width, edit_height, TRUE);

    const int content_top = query_top + scale(40);
    const int size_edit_width = scale(96);
    const int size_label_width = scale(108);
    MoveWindow(content_label_, margin, content_top + scale(5), scale(120),
               label_height, TRUE);
    MoveWindow(content_edit_, margin + scale(126), content_top,
               client.right - margin * 2 - scale(126) - size_label_width - size_edit_width - gap,
               list_height, TRUE);
    MoveWindow(max_size_label_, client.right - margin - size_edit_width - size_label_width,
               content_top + scale(5), size_label_width, label_height, TRUE);
    MoveWindow(max_size_edit_, client.right - margin - size_edit_width, content_top,
               size_edit_width, edit_height, TRUE);

    const int options_top = content_top + scale(42);
    const int check_width = std::max(
        scale(128),
        (static_cast<int>(client.right) - margin * 2 - gap * 2) / 3);
    MoveWindow(match_case_, margin, options_top, check_width, label_height, TRUE);
    MoveWindow(whole_name_, margin + check_width + gap, options_top,
               check_width, label_height, TRUE);
    MoveWindow(search_path_, margin + (check_width + gap) * 2, options_top,
               check_width, label_height, TRUE);

    const int types_top = options_top + scale(28);
    MoveWindow(include_files_, margin, types_top, check_width, label_height, TRUE);
    MoveWindow(include_folders_, margin + check_width + gap, types_top,
               check_width, label_height, TRUE);
    MoveWindow(include_archives_, margin + (check_width + gap) * 2, types_top,
               check_width, label_height, TRUE);

    const int where_top = types_top + scale(34);
    MoveWindow(where_label_, margin, where_top,
               client.right - margin * 2, label_height, TRUE);
    const int scope_row_top = where_top + scale(27);
    const int field_label_width = scale(112);
    MoveWindow(search_area_label_, margin, scope_row_top + scale(5),
               field_label_width, label_height, TRUE);
    MoveWindow(search_area_, margin + field_label_width, scope_row_top,
               client.right - margin * 2 - field_label_width,
               scale(120), TRUE);
    const int archive_row_top = scope_row_top + scale(38);
    MoveWindow(archive_types_label_, margin, archive_row_top + scale(5),
               field_label_width, label_height, TRUE);
    MoveWindow(archive_types_, margin + field_label_width, archive_row_top,
               client.right - margin * 2 - field_label_width,
               scale(140), TRUE);
    const int exclude_row_top = archive_row_top + scale(38);
    MoveWindow(exclude_label_, margin, exclude_row_top + scale(5),
               field_label_width, label_height, TRUE);
    MoveWindow(exclude_edit_, margin + field_label_width, exclude_row_top,
               client.right - margin * 2 - field_label_width, edit_height, TRUE);

    const int find_options_top = exclude_row_top + scale(42);
    const int option_width = std::max(
        scale(150), (static_cast<int>(client.right) - margin * 2 - gap * 2) / 3);
    MoveWindow(include_subfolders_, margin, find_options_top,
               option_width, label_height, TRUE);
    MoveWindow(find_in_archives_, margin + option_width + gap, find_options_top,
               option_width, label_height, TRUE);
    MoveWindow(skip_encrypted_, margin + (option_width + gap) * 2, find_options_top,
               option_width, label_height, TRUE);

    const int button_top = bottom - button_height;
    MoveWindow(close_button_, client.right - margin - button_width, button_top,
               button_width, button_height, TRUE);
    MoveWindow(go_to_button_, client.right - margin - button_width * 2 - gap,
               button_top, button_width, button_height, TRUE);
    MoveWindow(export_button_, client.right - margin - button_width * 3 - gap * 2,
               button_top, button_width, button_height, TRUE);
    MoveWindow(result_count_, margin, button_top + scale(5),
               client.right - margin * 3 - button_width * 3 - gap * 2,
               label_height, TRUE);

    const int results_top = find_options_top + scale(36);
    MoveWindow(results_.hwnd(), margin, results_top,
               client.right - margin * 2,
               button_top - results_top - scale(12), TRUE);
    tooltip_.update_layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void FileSearchDialog::create_controls() {
    background_brush_ = CreateSolidBrush(theme_.window);
    edit_brush_ = CreateSolidBrush(theme_.edit);
    font_ = axiom::gui::create_dialog_font(dpi_);
    tooltip_.create(hwnd_, dpi_, theme_.dark);
    constexpr DWORD kHistoryStyle = WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL |
                                    CBS_OWNERDRAWFIXED | CBS_HASSTRINGS;
    title_ = make_control(L"STATIC", L"Find files and folders", SS_NOPREFIX);
    scope_label_ = make_control(L"STATIC", scope_.c_str(), SS_NOPREFIX);
    query_label_ = make_control(L"STATIC", L"Name/path text", SS_NOPREFIX);
    search_edit_ = make_control(L"COMBOBOX", L"", kHistoryStyle, kSearchText);
    SendMessageW(search_edit_, CB_LIMITTEXT, 1024, 0);
    content_label_ = make_control(L"STATIC", L"Containing text", SS_NOPREFIX);
    content_edit_ = make_control(L"COMBOBOX", L"", kHistoryStyle, kContentText);
    SendMessageW(content_edit_, CB_LIMITTEXT, 512, 0);
    max_size_label_ = make_control(L"STATIC", L"Skip files over", SS_NOPREFIX);
    max_size_edit_ = make_control(L"EDIT", L"64 MB", WS_TABSTOP | ES_AUTOHSCROLL,
                                  kContentMaxSize);
    SendMessageW(max_size_edit_, EM_SETLIMITTEXT, 32, 0);
    match_case_ = make_control(L"BUTTON", L"Match case",
                               WS_TABSTOP | BS_OWNERDRAW | BS_CHECKBOX,
                               kMatchCase);
    whole_name_ = make_control(L"BUTTON", L"Whole name",
                               WS_TABSTOP | BS_OWNERDRAW | BS_CHECKBOX,
                               kWholeName);
    search_path_ = make_control(L"BUTTON", L"Search path",
                                WS_TABSTOP | BS_OWNERDRAW | BS_CHECKBOX,
                                kSearchPath);
    include_files_ = make_control(L"BUTTON", L"Files (names)",
                                  WS_TABSTOP | BS_OWNERDRAW | BS_CHECKBOX,
                                  kIncludeFiles);
    include_folders_ = make_control(L"BUTTON", L"Folders",
                                    WS_TABSTOP | BS_OWNERDRAW | BS_CHECKBOX,
                                    kIncludeFolders);
    include_archives_ = make_control(L"BUTTON", L"Archive files",
                                     WS_TABSTOP | BS_OWNERDRAW | BS_CHECKBOX,
                                     kIncludeArchives);
    where_label_ = make_control(L"STATIC", L"Where to find", SS_NOPREFIX);
    search_area_label_ = make_control(L"STATIC", L"Search area", SS_NOPREFIX);
    search_area_ = make_control(L"COMBOBOX", L"",
                                WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
                                    CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                kSearchArea);
    archive_types_label_ = make_control(L"STATIC", L"Archive types", SS_NOPREFIX);
    archive_types_ = make_control(L"COMBOBOX", L"",
                                  WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN |
                                      CBS_AUTOHSCROLL | CBS_OWNERDRAWFIXED |
                                      CBS_HASSTRINGS,
                                  kArchiveTypes);
    SendMessageW(archive_types_, CB_LIMITTEXT, 512, 0);
    exclude_label_ = make_control(L"STATIC", L"Exclude folders", SS_NOPREFIX);
    exclude_edit_ = make_control(L"EDIT", settings_.exclude_folders.c_str(),
                                 WS_TABSTOP | ES_AUTOHSCROLL, kExcludeFolders);
    SendMessageW(exclude_edit_, EM_SETLIMITTEXT, 1024, 0);
    SendMessageW(exclude_edit_, EM_SETCUEBANNER, FALSE,
                 reinterpret_cast<LPARAM>(L"Folder names or paths, such as node_modules; .git"));
    include_subfolders_ = make_control(L"BUTTON", L"Find in subfolders",
                                       WS_TABSTOP | BS_OWNERDRAW | BS_CHECKBOX,
                                       kIncludeSubfolders);
    find_in_archives_ = make_control(L"BUTTON", L"Find in archives",
                                     WS_TABSTOP | BS_OWNERDRAW | BS_CHECKBOX,
                                     kFindInArchives);
    skip_encrypted_ = make_control(L"BUTTON", L"Skip encrypted",
                                   WS_TABSTOP | BS_OWNERDRAW | BS_CHECKBOX,
                                   kSkipEncrypted);
    result_count_ = make_control(L"STATIC", L"", SS_NOPREFIX);
    search_button_ = make_control(L"BUTTON", L"Search",
                                  WS_TABSTOP | BS_OWNERDRAW,
                                  kSearchButton);
    export_button_ = make_control(L"BUTTON", L"Export...",
                                  WS_TABSTOP | BS_OWNERDRAW,
                                  kExportButton);
    go_to_button_ = make_control(L"BUTTON", L"Go to",
                                 WS_TABSTOP | BS_OWNERDRAW,
                                 kGoToButton);
    close_button_ = make_control(L"BUTTON", L"Close",
                                 WS_TABSTOP | BS_OWNERDRAW,
                                 IDCANCEL);
    axiom::gui::add_dialog_tooltip(
        tooltip_, content_edit_,
        L"Text to find inside files whose names match, in folders and inside archives. "
        L"Found as UTF-8 or UTF-16 text; Match case applies. The list holds recent searches.");
    axiom::gui::add_dialog_tooltip(
        tooltip_, max_size_edit_,
        L"Files larger than this are not read for the containing text, such as 64 MB or 1 GB.");
    axiom::gui::add_dialog_tooltip(
        tooltip_, search_edit_,
        L"Unicode text matched against item names, and against paths when Search path is enabled. "
        L"The list holds recent searches.");
    axiom::gui::add_dialog_tooltip(
        tooltip_, exclude_edit_,
        L"Semicolon-separated folder names or paths to leave out with everything in them, "
        L"for example node_modules; .git; C:\\Windows. Wildcards * and ? work; inside archives too.");
    axiom::gui::add_dialog_tooltip(
        tooltip_, search_button_,
        L"Search the area again. While a search runs, stop it and keep what it has found.");
    axiom::gui::add_dialog_tooltip(
        tooltip_, export_button_,
        L"Save the results, in the order shown, as CSV, tab-separated values or a list of paths.");
    axiom::gui::add_dialog_tooltip(tooltip_, match_case_,
                                   L"Require uppercase and lowercase letters to match exactly.");
    axiom::gui::add_dialog_tooltip(tooltip_, whole_name_,
                                   L"Match the complete name or path instead of a substring.");
    axiom::gui::add_dialog_tooltip(tooltip_, search_path_,
                                   L"Include each item's relative path in the text search.");
    axiom::gui::add_dialog_tooltip(tooltip_, include_files_,
                                   L"Include regular files in search results.");
    axiom::gui::add_dialog_tooltip(tooltip_, include_folders_,
                                   L"Include folders in search results.");
    axiom::gui::add_dialog_tooltip(tooltip_, include_archives_,
                                   L"Include recognized archive files in search results.");
    axiom::gui::add_dialog_tooltip(
        tooltip_, search_area_,
        L"Choose whether to start at the current folder or at the containing drive/archive root.");
    axiom::gui::add_dialog_tooltip(
        tooltip_, archive_types_,
        L"Semicolon-separated archive filename patterns to search inside, for example *.axar;*.zip. Use * for all supported archive types.");
    axiom::gui::add_dialog_tooltip(
        tooltip_, include_subfolders_,
        L"Recursively search child folders. Reparse-point folders are not followed to avoid cycles.");
    axiom::gui::add_dialog_tooltip(
        tooltip_, find_in_archives_,
        L"Open matching supported archives encountered in the search area and search their directories.");
    axiom::gui::add_dialog_tooltip(
        tooltip_, skip_encrypted_,
        L"Do not inspect archives reported as encrypted. Unreadable encrypted directories are counted as warnings when this is off.");

    if (location_.kind == axiom::gui::BrowserLocationKind::filesystem) {
        search_area_labels_ = {L"Current folder", L"Current drive"};
    } else if (location_.kind == axiom::gui::BrowserLocationKind::archive) {
        search_area_labels_ = {L"Current archive folder", L"Entire archive"};
    } else {
        search_area_labels_ = {L"This PC"};
    }
    for (const auto& label : search_area_labels_) {
        SendMessageW(search_area_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(label.c_str()));
    }
    SendMessageW(search_area_, CB_SETCURSEL, 0, 0);
    constexpr std::array<const wchar_t*, 7> archive_patterns{
        L"*", L"*.axar", L"*.zip", L"*.7z", L"*.rar",
        L"*.tar;*.tgz;*.tar.*", L"*.cab;*.iso"};
    for (const wchar_t* pattern : archive_patterns) {
        SendMessageW(archive_types_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(pattern));
    }
    SendMessageW(archive_types_, CB_SETCURSEL, 0, 0);
    for (const auto& item : settings_.name_history) {
        SendMessageW(search_edit_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    }
    for (const auto& item : settings_.content_history) {
        SendMessageW(content_edit_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    }

    set_checkbox(kIncludeFiles, true);
    set_checkbox(kIncludeFolders, true);
    set_checkbox(kIncludeArchives, true);
    set_checkbox(kIncludeSubfolders, true);
    set_checkbox(kFindInArchives, true);
    set_checkbox(kSkipEncrypted, false);

    results_.create(hwnd_, instance_, kResultsTable);
    results_.set_theme(theme_);
    results_.set_font(font_);
    results_.set_dpi(dpi_);
    std::vector<TableColumn> columns{
        {L"Name", 210}, {L"Location", 250}, {L"Type", 100}, {L"Size", 80},
        {L"Packed", 80}, {L"Modified", 130}, {L"CRC-32", 80},
    };
    if (settings_.column_widths.size() == columns.size()) {
        for (std::size_t k = 0; k < columns.size(); ++k) {
            columns[k].logical_width = settings_.column_widths[k];
        }
    }
    results_.set_columns(std::move(columns));
    results_.set_sort_indicator(sort_column_, sort_ascending_);
    results_.set_virtual_rows(
        0,
        [this](std::size_t row) { return result_row(row); },
        [](std::size_t) { return -1; },
        [this](std::size_t row) -> std::wstring_view {
            if (!index_ || row >= matches_.size()) return {};
            return index_->entries()[matches_[row]].name;
        },
        nullptr);

    for (HWND control : controls()) {
        if (control != nullptr) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
    }
    EnableWindow(go_to_button_, FALSE);
    EnableWindow(export_button_, FALSE);
    axiom::gui::apply_dialog_dark_frame(hwnd_, theme_.dark);
    tooltip_.apply_theme(theme_.dark);
    layout();
    set_text(result_count_, L"Enter a name/path, then choose Search. An empty name lists everything.");
    SetFocus(search_edit_);
}

void FileSearchDialog::rebuild_font_for_dpi() {
    HFONT replacement = axiom::gui::create_dialog_font(dpi_);
    for (HWND control : controls()) {
        if (control != nullptr) {
            SendMessageW(control, WM_SETFONT,
                         reinterpret_cast<WPARAM>(replacement), TRUE);
        }
    }
    results_.set_font(replacement);
    results_.set_dpi(dpi_);
    axiom::gui::delete_dialog_font(font_);
    font_ = replacement;
}

LRESULT FileSearchDialog::control_color(WPARAM wparam, bool edit) {
    HDC dc = reinterpret_cast<HDC>(wparam);
    SetTextColor(dc, theme_.text);
    SetBkColor(dc, edit ? theme_.edit : theme_.window);
    SetBkMode(dc, edit ? OPAQUE : TRANSPARENT);
    return reinterpret_cast<LRESULT>(edit ? edit_brush_ : background_brush_);
}

LRESULT FileSearchDialog::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            create_controls();
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            const SIZE minimum = dialog_window_size_for_client(
                700, 618, kFileSearchStyle, kFileSearchExStyle, dpi_);
            info->ptMinTrackSize.x = minimum.cx;
            info->ptMinTrackSize.y = minimum.cy;
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wparam);
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            axiom::gui::apply_axiom_window_icons(hwnd_, instance_);
            rebuild_font_for_dpi();
            tooltip_.update_dpi(dpi_);
            layout();
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            const int notification = HIWORD(wparam);
            switch (id) {
                case kSearchText:
                    // Refining only re-matches the index in memory, so it can
                    // follow typing closely. A pick from the list arrives
                    // before the field shows it; the timer reads it after.
                    if ((notification == CBN_EDITCHANGE || notification == CBN_SELCHANGE) &&
                        search_started_) {
                        KillTimer(hwnd_, kSearchDebounceTimer);
                        SetTimer(hwnd_, kSearchDebounceTimer, 120, nullptr);
                    }
                    return 0;
                case kContentText:
                case kContentMaxSize:
                case kExcludeFolders:
                    // These read the disk again, so they wait for a pause.
                    if ((notification == EN_CHANGE || notification == CBN_EDITCHANGE ||
                         notification == CBN_SELCHANGE) &&
                        search_started_) {
                        KillTimer(hwnd_, kSearchDebounceTimer);
                        SetTimer(hwnd_, kSearchDebounceTimer, 450, nullptr);
                    }
                    return 0;
                case kMatchCase:
                case kWholeName:
                case kSearchPath:
                case kIncludeFiles:
                case kIncludeFolders:
                case kIncludeArchives:
                case kIncludeSubfolders:
                case kFindInArchives:
                case kSkipEncrypted:
                    if (notification == BN_CLICKED) toggle_checkbox(id);
                    if (id == kFindInArchives) {
                        EnableWindow(archive_types_, find_in_archives_checked_);
                    }
                    return 0;
                case kSearchArea:
                    if (notification == CBN_SELCHANGE && search_started_) run_search(false);
                    return 0;
                case kArchiveTypes:
                    if (search_started_ &&
                        (notification == CBN_SELCHANGE || notification == CBN_EDITCHANGE)) {
                        KillTimer(hwnd_, kSearchDebounceTimer);
                        SetTimer(hwnd_, kSearchDebounceTimer, 180, nullptr);
                    }
                    return 0;
                case kSearchButton:
                    if (walking_) {
                        stop_search();
                        return 0;
                    }
                    [[fallthrough]];
                case IDOK:
                    // An explicit search reads the disk again.
                    KillTimer(hwnd_, kSearchDebounceTimer);
                    run_search(true);
                    return 0;
                case kGoToButton:
                    accept_selected();
                    return 0;
                case kExportButton:
                    export_results();
                    return 0;
                case kResultOpen:
                    open_result(false);
                    return 0;
                case kResultOpenFolder:
                    open_result(true);
                    return 0;
                case kResultCopyPath:
                    copy_results(false);
                    return 0;
                case kResultCopyName:
                    copy_results(true);
                    return 0;
                case kResultSelectAll:
                    results_.select_all();
                    return 0;
                case IDCANCEL:
                    destroy_modal_dialog(hwnd_);
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_CONTEXTMENU:
            if (reinterpret_cast<HWND>(wparam) == results_.hwnd()) {
                show_results_menu({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
                return 0;
            }
            break;
        case WM_TIMER:
            if (wparam == kSearchDebounceTimer) {
                KillTimer(hwnd_, kSearchDebounceTimer);
                run_search(false);
                return 0;
            }
            if (wparam == kMatchTimer) {
                match_pending();
                update_status();
                return 0;
            }
            break;
        case kTableActivateMessage:
            accept_selected();
            return 0;
        case kTableSortMessage:
            on_sort(static_cast<int>(wparam));
            return 0;
        case kSearchBatchMessage:
            on_batch(lparam);
            return 0;
        case kSearchDoneMessage:
            on_walk_done(wparam);
            return 0;
        case kExportDoneMessage:
            on_export_done(lparam);
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return control_color(wparam, false);
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return control_color(wparam, true);
        case WM_DRAWITEM:
            if (lparam != 0) {
                const auto& draw = *reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
                if (draw.CtlType == ODT_COMBOBOX) {
                    axiom::gui::draw_dialog_combo_item(draw, theme_.dark);
                    return TRUE;
                }
                switch (draw.CtlID) {
                    case kMatchCase:
                    case kWholeName:
                    case kSearchPath:
                    case kIncludeFiles:
                    case kIncludeFolders:
                    case kIncludeArchives:
                    case kIncludeSubfolders:
                    case kFindInArchives:
                    case kSkipEncrypted: {
                        axiom::gui::draw_dialog_checkbox(
                            draw, theme_.dark,
                            checkbox_checked(static_cast<int>(draw.CtlID)));
                        break;
                    }
                    default:
                        axiom::gui::draw_dialog_button(draw, theme_.dark);
                        break;
                }
                return TRUE;
            }
            break;
        case WM_MEASUREITEM:
            if (lparam != 0) {
                auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
                if (measure->CtlType == ODT_COMBOBOX) {
                    measure->itemHeight = scale(22);
                    return TRUE;
                }
            }
            break;
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(hwnd_, &client);
            FillRect(reinterpret_cast<HDC>(wparam), &client, background_brush_);
            return 1;
        }
        case WM_CLOSE:
            destroy_modal_dialog(hwnd_);
            return 0;
        case WM_DESTROY:
            // The controls still exist here; by WM_NCDESTROY they are gone.
            save_settings();
            break;
        case WM_NCDESTROY: {
            KillTimer(hwnd_, kSearchDebounceTimer);
            KillTimer(hwnd_, kMatchTimer);
            // Closing never waits for the search: a thread can sit for a
            // long time opening a file an antivirus is scanning, and waiting
            // here froze the whole program with it.
            search_thread_.abandon();
            export_thread_.request_stop();
            export_thread_.join_all();
            MSG queued{};
            while (PeekMessageW(&queued, hwnd_, kSearchBatchMessage,
                                kSearchBatchMessage, PM_REMOVE)) {
                delete reinterpret_cast<BatchPayload*>(queued.lParam);
            }
            while (PeekMessageW(&queued, hwnd_, kExportDoneMessage,
                                kExportDoneMessage, PM_REMOVE)) {
                delete reinterpret_cast<ExportResult*>(queued.lParam);
            }
            axiom::gui::save_named_window_placement(L"FileSearchDialog", hwnd_);
            if (font_ != nullptr) axiom::gui::delete_dialog_font(font_);
            if (background_brush_ != nullptr) DeleteObject(background_brush_);
            if (edit_brush_ != nullptr) DeleteObject(edit_brush_);
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            hwnd_ = nullptr;
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

LRESULT CALLBACK FileSearchDialog::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    FileSearchDialog* dialog = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        dialog = create == nullptr
            ? nullptr
            : static_cast<FileSearchDialog*>(create->lpCreateParams);
        if (dialog == nullptr) return FALSE;
        dialog->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
    } else {
        dialog = reinterpret_cast<FileSearchDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (dialog != nullptr) return dialog->handle_message(message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace axiom::gui
