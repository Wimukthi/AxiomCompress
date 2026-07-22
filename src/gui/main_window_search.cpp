// The Find Files dialog shown from the main window.

#include "gui/main_window_internal.hpp"

#include <deque>
#include <unordered_set>

namespace axiom::gui {

namespace {
constexpr DWORD kFileSearchStyle =
    WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
    WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
constexpr DWORD kFileSearchExStyle = WS_EX_WINDOWEDGE | WS_EX_CONTROLPARENT;

constexpr std::size_t kFileSearchResultLimit = 100000;

bool wildcard_match_folded(std::wstring_view pattern, std::wstring_view text) {
    std::size_t pattern_index = 0;
    std::size_t text_index = 0;
    std::size_t star_index = std::wstring_view::npos;
    std::size_t star_text_index = 0;
    while (text_index < text.size()) {
        if (pattern_index < pattern.size() &&
            (pattern[pattern_index] == L'?' ||
             std::towlower(pattern[pattern_index]) == std::towlower(text[text_index]))) {
            ++pattern_index;
            ++text_index;
        } else if (pattern_index < pattern.size() && pattern[pattern_index] == L'*') {
            star_index = pattern_index++;
            star_text_index = text_index;
        } else if (star_index != std::wstring_view::npos) {
            pattern_index = star_index + 1;
            text_index = ++star_text_index;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern.size() && pattern[pattern_index] == L'*') {
        ++pattern_index;
    }
    return pattern_index == pattern.size();
}

bool archive_type_matches(std::wstring_view patterns, const fs::path& path) {
    const std::wstring filename = path.filename().wstring();
    std::size_t start = 0;
    bool saw_pattern = false;
    while (start <= patterns.size()) {
        std::size_t end = patterns.find_first_of(L";,", start);
        if (end == std::wstring_view::npos) end = patterns.size();
        std::wstring_view pattern = patterns.substr(start, end - start);
        while (!pattern.empty() && std::iswspace(pattern.front())) pattern.remove_prefix(1);
        while (!pattern.empty() && std::iswspace(pattern.back())) pattern.remove_suffix(1);
        if (!pattern.empty()) {
            saw_pattern = true;
            if (pattern == L"*" || pattern == L"*.*" ||
                wildcard_match_folded(pattern, filename)) {
                return true;
            }
        }
        if (end == patterns.size()) break;
        start = end + 1;
    }
    return !saw_pattern;
}

std::string archive_parent_path(std::string_view path) {
    const std::size_t separator = path.find_last_of('/');
    return separator == std::string_view::npos
        ? std::string{}
        : std::string(path.substr(0, separator));
}

FileSearchSourceItem search_item_from_browser_item(
    const axiom::gui::BrowserItem& item,
    const axiom::gui::BrowserLocation& parent,
    int browser_index = -1) {
    FileSearchSourceItem result;
    result.browser_index = browser_index;
    result.kind = item.kind;
    result.target_location = parent;
    result.filesystem_path = item.filesystem_path;
    result.archive_entry_path = item.archive_path;
    result.recurse_directory =
        item.kind == axiom::gui::BrowserItemKind::directory &&
        item.attributes.find(L'L') == std::wstring::npos;
    result.name = item.name;
    result.type = item.type;
    result.modified = item.modified;
    if (item.kind != axiom::gui::BrowserItemKind::directory &&
        item.kind != axiom::gui::BrowserItemKind::drive) {
        result.size = format_size(item.size);
    }
    if (!item.filesystem_path.empty()) {
        result.location = item.filesystem_path.parent_path().wstring();
    } else if (!item.archive_path.empty()) {
        result.location = axiom::gui::BrowserLocation::archive(
            parent.archive_path, archive_parent_path(item.archive_path)).display_name();
    } else {
        result.location = parent.display_name();
    }
    result.folded_name = folded_text(result.name);
    result.folded_search_text = folded_text(
        result.location.empty() ? result.name : result.location + L"\\" + result.name);
    return result;
}
}

FileSearchDialog::FileSearchDialog(HINSTANCE instance,
                 ThemePalette theme,
                 UINT dpi,
                 axiom::gui::BrowserLocation location,
                 std::shared_ptr<const axiom::gui::ArchiveCatalog> archive_catalog,
                 std::vector<FileSearchSourceItem> source)
    : instance_(instance),
      theme_(theme),
      dpi_(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi),
      scope_(L"Scope: " + location.display_name()),
      location_(std::move(location)),
      archive_catalog_(std::move(archive_catalog)),
      source_(std::move(source)) {}

FileSearchDialogResult FileSearchDialog::show(HWND owner) {
    owner_ = owner;
    if (!register_class(instance_)) {
        return result_;
    }

    const SIZE window_size = dialog_window_size_for_client(
        820, 650, kFileSearchStyle, kFileSearchExStyle, dpi_);
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
    axiom::gui::restore_named_window_placement(hwnd_, owner, L"FileSearchDialog");
    const bool owner_was_enabled = axiom::gui::disable_dialog_owner(owner, hwnd_);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    MSG message{};
    while (IsWindow(hwnd_)) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (axiom::gui::message_targets_window(hwnd_, message) &&
            message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            DestroyWindow(hwnd_);
            continue;
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

std::array<HWND, 22> FileSearchDialog::controls() const {
    return {title_, scope_label_, query_label_, search_edit_,
            match_case_, whole_name_, search_path_,
            include_files_, include_folders_, include_archives_,
            result_count_, search_button_, go_to_button_, close_button_,
            where_label_, search_area_label_, search_area_,
            archive_types_label_, archive_types_, include_subfolders_,
            find_in_archives_, skip_encrypted_};
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
    set_checkbox(id, !checkbox_checked(id));
    if (search_started_) run_search();
}

bool FileSearchDialog::is_folder_kind(axiom::gui::BrowserItemKind kind) {
    return kind == axiom::gui::BrowserItemKind::directory ||
           kind == axiom::gui::BrowserItemKind::drive;
}

bool FileSearchDialog::is_archive_kind(axiom::gui::BrowserItemKind kind) {
    return kind == axiom::gui::BrowserItemKind::archive;
}

bool FileSearchDialog::is_file_kind(axiom::gui::BrowserItemKind kind) {
    return kind == axiom::gui::BrowserItemKind::file ||
           kind == axiom::gui::BrowserItemKind::symlink ||
           kind == axiom::gui::BrowserItemKind::hardlink;
}

bool FileSearchDialog::type_allowed(const FileSearchSourceItem& item) const {
    if (is_file_kind(item.kind) && include_files_checked_) return true;
    if (is_folder_kind(item.kind) && include_folders_checked_) return true;
    if (is_archive_kind(item.kind) && include_archives_checked_) return true;
    return false;
}

bool FileSearchDialog::text_matches(const FileSearchSourceItem& item, std::wstring_view query,
                                    std::wstring_view folded_query) const {
    if (query.empty()) return true;

    if (whole_name_checked_) {
        return match_case_checked_ ? item.name == query
                                   : item.folded_name == folded_query;
    }
    if (match_case_checked_) {
        const std::wstring haystack = search_path_checked_ && !item.location.empty()
            ? item.location + L"\\" + item.name
            : item.name;
        return haystack.find(query) != std::wstring::npos;
    }
    const std::wstring_view haystack = search_path_checked_
        ? std::wstring_view(item.folded_search_text)
        : std::wstring_view(item.folded_name);
    return haystack.find(folded_query) != std::wstring_view::npos;
}

void FileSearchDialog::run_search() {
    search_started_ = true;
    search_thread_.request_stop();
    if (search_thread_.joinable()) search_thread_.join();
    const std::uint64_t generation = ++search_generation_;
    result_targets_.clear();
    results_.clear();
    EnableWindow(go_to_button_, FALSE);
    set_text(result_count_, L"Searching...");
    const std::wstring query = get_text(search_edit_);
    const std::wstring folded_query = folded_text(query);
    const std::wstring archive_patterns = get_text(archive_types_);
    int search_area = static_cast<int>(SendMessageW(search_area_, CB_GETCURSEL, 0, 0));
    if (search_area < 0) search_area = 0;
    const bool include_files = include_files_checked_;
    const bool include_folders = include_folders_checked_;
    const bool include_archives = include_archives_checked_;
    const bool include_subfolders = include_subfolders_checked_;
    const bool find_in_archives = find_in_archives_checked_;
    const bool skip_encrypted = skip_encrypted_checked_;
    const bool match_case = match_case_checked_;
    const bool whole_name = whole_name_checked_;
    const bool search_path = search_path_checked_;
    const auto source = source_;
    const auto location = location_;
    const auto active_catalog = archive_catalog_;
    HWND target = hwnd_;
    search_thread_ = std::jthread(
        [source, location, active_catalog, target, generation, query, folded_query,
         archive_patterns, search_area,
         include_files, include_folders, include_archives,
         include_subfolders, find_in_archives, skip_encrypted,
         match_case, whole_name, search_path](std::stop_token stop) {
            auto payload = std::make_unique<SearchPayload>();
            payload->generation = generation;
            payload->targets.reserve(std::min<std::size_t>(source.size(), 4096));
            payload->rows.reserve(std::min<std::size_t>(source.size(), 4096));

            const auto limit_reached = [&] {
                if (payload->targets.size() < kFileSearchResultLimit) return false;
                payload->result_limit_reached = true;
                return true;
            };
            const auto add_match = [&](const FileSearchSourceItem& item) {
                if (limit_reached()) return;
                ++payload->scanned_items;
                if ((payload->scanned_items & 1023u) == 0u) {
                    PostMessageW(target, kSearchProgressMessage,
                                 static_cast<WPARAM>(generation),
                                 static_cast<LPARAM>(payload->scanned_items));
                }
                const bool folder = is_folder_kind(item.kind);
                const bool archive = is_archive_kind(item.kind);
                const bool file = is_file_kind(item.kind);
                if ((folder && !include_folders) ||
                    (archive && !include_archives) ||
                    (file && !include_files) || (!folder && !archive && !file)) {
                    return;
                }
                bool matches = query.empty();
                if (!matches && whole_name) {
                    matches = match_case ? item.name == query
                                         : item.folded_name == folded_query;
                } else if (!matches && match_case) {
                    const std::wstring haystack = search_path && !item.location.empty()
                        ? item.location + L"\\" + item.name
                        : item.name;
                    matches = haystack.find(query) != std::wstring::npos;
                } else if (!matches) {
                    const std::wstring_view haystack = search_path
                        ? std::wstring_view(item.folded_search_text)
                        : std::wstring_view(item.folded_name);
                    matches = haystack.find(folded_query) != std::wstring_view::npos;
                }
                if (!matches) return;
                FileSearchDialogResult result;
                result.browser_index = item.browser_index;
                result.target_location = item.target_location;
                result.filesystem_path = item.filesystem_path;
                result.archive_entry_path = item.archive_entry_path;
                payload->targets.push_back(std::move(result));
                payload->rows.push_back(
                    {item.name, item.location, item.type, item.size, item.modified});
            };

            const auto scan_catalog = [&](const std::shared_ptr<const axiom::gui::ArchiveCatalog>& catalog,
                                          std::string base_directory,
                                          bool recurse) {
                std::deque<std::string> directories;
                directories.push_back(std::move(base_directory));
                while (!directories.empty() && !stop.stop_requested() && !limit_reached()) {
                    std::string directory = std::move(directories.front());
                    directories.pop_front();
                    const auto parent = axiom::gui::BrowserLocation::archive(
                        catalog->path(), directory);
                    const auto snapshot = catalog->list(parent, stop);
                    for (const auto& browser_item : snapshot.items) {
                        if (stop.stop_requested() || limit_reached()) return;
                        if (browser_item.is_parent()) continue;
                        auto item = search_item_from_browser_item(browser_item, parent);
                        add_match(item);
                        if (recurse && item.recurse_directory) {
                            directories.push_back(item.archive_entry_path);
                        }
                    }
                }
            };

            std::unordered_set<std::wstring> searched_archives;
            const auto scan_archive = [&](const fs::path& archive_path) {
                if (!find_in_archives || stop.stop_requested() || limit_reached() ||
                    !archive_type_matches(archive_patterns, archive_path)) {
                    return;
                }
                const std::wstring key = folded_text(archive_path.lexically_normal().wstring());
                if (!searched_archives.insert(key).second) return;
                try {
                    const auto* provider = axiom::archive_provider_for_path(archive_path);
                    if (provider == nullptr) return;
                    const auto capabilities = provider->capabilities(archive_path);
                    if (skip_encrypted && capabilities.encrypted) {
                        ++payload->skipped_archives;
                        return;
                    }
                    auto catalog = axiom::gui::ArchiveCatalog::load(archive_path);
                    scan_catalog(catalog, {}, include_subfolders);
                } catch (...) {
                    ++payload->read_errors;
                }
            };

            const auto process_item = [&](const FileSearchSourceItem& item) {
                add_match(item);
                if (item.kind == axiom::gui::BrowserItemKind::archive &&
                    !item.filesystem_path.empty()) {
                    scan_archive(item.filesystem_path);
                }
            };

            const auto scan_filesystem = [&](fs::path first_directory, bool recurse) {
                std::deque<fs::path> directories;
                directories.push_back(std::move(first_directory));
                while (!directories.empty() && !stop.stop_requested() && !limit_reached()) {
                    fs::path directory = std::move(directories.front());
                    directories.pop_front();
                    const auto parent = axiom::gui::BrowserLocation::filesystem(directory);
                    auto loaded = axiom::gui::load_browser_location(
                        parent, 0, nullptr, stop);
                    if (!loaded.snapshot.error.empty()) ++payload->read_errors;
                    for (const auto& browser_item : loaded.snapshot.items) {
                        if (stop.stop_requested() || limit_reached()) return;
                        if (browser_item.is_parent()) continue;
                        auto item = search_item_from_browser_item(browser_item, parent);
                        process_item(item);
                        if (recurse && item.recurse_directory) {
                            directories.push_back(item.filesystem_path);
                        }
                    }
                }
            };

            if (location.kind == axiom::gui::BrowserLocationKind::archive &&
                active_catalog && search_area == 1) {
                scan_catalog(active_catalog, {}, include_subfolders);
            } else if (location.kind == axiom::gui::BrowserLocationKind::filesystem &&
                       search_area == 1) {
                fs::path root = location.filesystem_path.root_path();
                if (root.empty()) root = location.filesystem_path;
                scan_filesystem(std::move(root), include_subfolders);
            } else {
                for (const auto& item : source) {
                    if (stop.stop_requested()) return;
                    if (limit_reached()) break;
                    process_item(item);
                }
                if (include_subfolders) {
                    if (location.kind == axiom::gui::BrowserLocationKind::archive &&
                        active_catalog) {
                        for (const auto& item : source) {
                            if (stop.stop_requested()) return;
                            if (limit_reached()) break;
                            if (item.recurse_directory) {
                                scan_catalog(active_catalog, item.archive_entry_path, true);
                            }
                        }
                    } else {
                        for (const auto& item : source) {
                            if (stop.stop_requested()) return;
                            if (limit_reached()) break;
                            if (item.recurse_directory && !item.filesystem_path.empty()) {
                                scan_filesystem(item.filesystem_path, true);
                            } else if (item.kind == axiom::gui::BrowserItemKind::drive &&
                                       !item.filesystem_path.empty()) {
                                scan_filesystem(item.filesystem_path, true);
                            }
                        }
                    }
                }
            }
            if (stop.stop_requested()) return;
            SearchPayload* raw = payload.release();
            if (!PostMessageW(target, kSearchResultsMessage, 0,
                              reinterpret_cast<LPARAM>(raw))) {
                delete raw;
            }
        });
}

void FileSearchDialog::apply_search_results(LPARAM value) {
    std::unique_ptr<SearchPayload> payload(reinterpret_cast<SearchPayload*>(value));
    if (!payload || payload->generation != search_generation_) return;
    result_targets_ = std::move(payload->targets);
    results_.set_rows(std::move(payload->rows), {}, nullptr);
    if (!result_targets_.empty()) {
        results_.select_index(0);
    }
    std::wstring status = quote_count(result_targets_.size(), L"match", L"matches") +
        L"  |  " + quote_count(payload->scanned_items, L"item scanned", L"items scanned");
    if (payload->skipped_archives != 0) {
        status += L"  |  " + quote_count(payload->skipped_archives,
                                          L"encrypted archive skipped",
                                          L"encrypted archives skipped");
    }
    if (payload->read_errors != 0) {
        status += L"  |  " + quote_count(payload->read_errors,
                                          L"read warning", L"read warnings");
    }
    if (payload->result_limit_reached) status += L"  |  result limit reached";
    set_text(result_count_, status);
    EnableWindow(go_to_button_, !result_targets_.empty());
}

void FileSearchDialog::accept_selected() {
    const int row = results_.focused_index();
    if (row < 0 || row >= static_cast<int>(result_targets_.size())) return;
    result_ = result_targets_[static_cast<std::size_t>(row)];
    result_.accepted = true;
    DestroyWindow(hwnd_);
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

    MoveWindow(title_, margin, margin, client.right - margin * 2,
               label_height, TRUE);
    MoveWindow(scope_label_, margin, margin + scale(24),
               client.right - margin * 2, label_height, TRUE);

    const int query_top = margin + scale(58);
    MoveWindow(query_label_, margin, query_top + scale(5), scale(120),
               label_height, TRUE);
    MoveWindow(search_edit_, margin + scale(126), query_top,
               client.right - margin * 2 - scale(126) - button_width - gap,
               edit_height, TRUE);
    MoveWindow(search_button_, client.right - margin - button_width, query_top,
               button_width, edit_height, TRUE);

    const int options_top = query_top + scale(42);
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

    const int find_options_top = archive_row_top + scale(40);
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
    MoveWindow(result_count_, margin, button_top + scale(5),
               client.right - margin * 3 - button_width * 2 - gap,
               label_height, TRUE);

    const int results_top = find_options_top + scale(36);
    MoveWindow(results_.hwnd(), margin, results_top,
               client.right - margin * 2,
               button_top - results_top - scale(12), TRUE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void FileSearchDialog::create_controls() {
    background_brush_ = CreateSolidBrush(theme_.window);
    edit_brush_ = CreateSolidBrush(theme_.edit);
    font_ = axiom::gui::create_dialog_font(dpi_);
    tooltip_ = axiom::gui::create_dialog_tooltip(hwnd_);
    title_ = make_control(L"STATIC", L"Find files and folders", SS_NOPREFIX);
    scope_label_ = make_control(L"STATIC", scope_.c_str(), SS_NOPREFIX);
    query_label_ = make_control(L"STATIC", L"Name/path text", SS_NOPREFIX);
    search_edit_ = make_control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
                                kSearchText);
    SendMessageW(search_edit_, EM_SETLIMITTEXT, 1024, 0);
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
    go_to_button_ = make_control(L"BUTTON", L"Go to",
                                 WS_TABSTOP | BS_OWNERDRAW,
                                 kGoToButton);
    close_button_ = make_control(L"BUTTON", L"Close",
                                 WS_TABSTOP | BS_OWNERDRAW,
                                 IDCANCEL);
    axiom::gui::add_dialog_tooltip(
        tooltip_, search_edit_,
        L"Unicode text matched against item names, and against paths when Search path is enabled.");
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
    results_.set_columns({
        {L"Name", 220}, {L"Location", 260}, {L"Type", 110},
        {L"Size", 90}, {L"Modified", 140},
    });

    for (HWND control : controls()) {
        if (control != nullptr) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
    }
    EnableWindow(go_to_button_, FALSE);
    axiom::gui::apply_dialog_dark_frame(hwnd_, theme_.dark);
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
                700, 540, kFileSearchStyle, kFileSearchExStyle, dpi_);
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
            layout();
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            const int notification = HIWORD(wparam);
            switch (id) {
                case kSearchText:
                    if (notification == EN_CHANGE && search_started_) {
                        KillTimer(hwnd_, kSearchDebounceTimer);
                        SetTimer(hwnd_, kSearchDebounceTimer, 300, nullptr);
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
                    if (notification == CBN_SELCHANGE && search_started_) run_search();
                    return 0;
                case kArchiveTypes:
                    if (search_started_ &&
                        (notification == CBN_SELCHANGE || notification == CBN_EDITCHANGE)) {
                        KillTimer(hwnd_, kSearchDebounceTimer);
                        SetTimer(hwnd_, kSearchDebounceTimer, 180, nullptr);
                    }
                    return 0;
                case kSearchButton:
                case IDOK:
                    KillTimer(hwnd_, kSearchDebounceTimer);
                    run_search();
                    return 0;
                case kGoToButton:
                    accept_selected();
                    return 0;
                case IDCANCEL:
                    DestroyWindow(hwnd_);
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_TIMER:
            if (wparam == kSearchDebounceTimer) {
                KillTimer(hwnd_, kSearchDebounceTimer);
                run_search();
                return 0;
            }
            break;
        case kTableActivateMessage:
            accept_selected();
            return 0;
        case kSearchResultsMessage:
            apply_search_results(lparam);
            return 0;
        case kSearchProgressMessage:
            if (static_cast<std::uint64_t>(wparam) == search_generation_) {
                set_text(result_count_,
                         L"Searching... " +
                             quote_count(static_cast<std::size_t>(lparam),
                                         L"item scanned", L"items scanned"));
            }
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
            DestroyWindow(hwnd_);
            return 0;
        case WM_NCDESTROY: {
            KillTimer(hwnd_, kSearchDebounceTimer);
            search_thread_.request_stop();
            if (search_thread_.joinable()) search_thread_.join();
            MSG queued{};
            while (PeekMessageW(&queued, hwnd_, kSearchResultsMessage,
                                kSearchResultsMessage, PM_REMOVE)) {
                delete reinterpret_cast<SearchPayload*>(queued.lParam);
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
