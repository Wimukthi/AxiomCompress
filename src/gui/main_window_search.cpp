// The Find Files dialog shown from the main window.

#include "gui/main_window_internal.hpp"

namespace axiom::gui {

FileSearchDialog::FileSearchDialog(HINSTANCE instance,
                 ThemePalette theme,
                 UINT dpi,
                 std::wstring scope,
                 std::vector<FileSearchSourceItem> source)
    : instance_(instance),
      theme_(theme),
      dpi_(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi),
      scope_(std::move(scope)),
      source_(std::move(source)) {}

FileSearchDialogResult FileSearchDialog::show(HWND owner) {
    owner_ = owner;
    if (!register_class(instance_)) {
        return result_;
    }

    const int width = scale(820);
    const int height = scale(560);
    const POINT position = axiom::gui::centered_window_position(owner, width, height);
    hwnd_ = CreateWindowExW(
        WS_EX_WINDOWEDGE | WS_EX_CONTROLPARENT,
        class_name(), L"Find files",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
            WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        position.x, position.y, width, height, owner, nullptr, instance_, this);
    if (hwnd_ == nullptr) return result_;

    axiom::gui::apply_axiom_window_icons(hwnd_, instance_);
    axiom::gui::restore_named_window_placement(hwnd_, owner, L"FileSearchDialog");
    const bool owner_was_enabled = axiom::gui::disable_dialog_owner(owner);
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

std::array<HWND, 14> FileSearchDialog::controls() const {
    return {title_, scope_label_, query_label_, search_edit_,
            match_case_, whole_name_, search_path_,
            include_files_, include_folders_, include_archives_,
            result_count_, search_button_, go_to_button_, close_button_};
}

bool FileSearchDialog::checkbox_checked(int id) const {
    switch (id) {
        case kMatchCase: return match_case_checked_;
        case kWholeName: return whole_name_checked_;
        case kSearchPath: return search_path_checked_;
        case kIncludeFiles: return include_files_checked_;
        case kIncludeFolders: return include_folders_checked_;
        case kIncludeArchives: return include_archives_checked_;
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
        default: return;
    }
    if (HWND control = GetDlgItem(hwnd_, id)) {
        InvalidateRect(control, nullptr, FALSE);
    }
}

void FileSearchDialog::toggle_checkbox(int id) {
    set_checkbox(id, !checkbox_checked(id));
    run_search();
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

bool FileSearchDialog::text_matches(const FileSearchSourceItem& item, std::wstring_view query) const {
    if (query.empty()) return true;
    const std::wstring haystack = search_path_checked_ && !item.location.empty()
        ? item.location + L"\\" + item.name
        : item.name;

    if (whole_name_checked_) {
        return match_case_checked_ ? item.name == query
                          : folded_text(item.name) == folded_text(query);
    }
    if (match_case_checked_) {
        return haystack.find(query) != std::wstring::npos;
    }
    return contains_folded(haystack, query);
}

void FileSearchDialog::run_search() {
    results_.clear();
    result_indices_.clear();
    const std::wstring query = get_text(search_edit_);
    for (const auto& item : source_) {
        if (!type_allowed(item) || !text_matches(item, query)) continue;
        result_indices_.push_back(item.browser_index);
        results_.append_row({item.name, item.location, item.type, item.size,
                             item.modified});
    }
    if (!result_indices_.empty()) {
        results_.select_index(0);
    }
    set_text(result_count_,
             quote_count(result_indices_.size(), L"match", L"matches"));
    EnableWindow(go_to_button_, !result_indices_.empty());
}

void FileSearchDialog::accept_selected() {
    const int row = results_.focused_index();
    if (row < 0 || row >= static_cast<int>(result_indices_.size())) return;
    result_.accepted = true;
    result_.browser_index = result_indices_[static_cast<std::size_t>(row)];
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
    MoveWindow(query_label_, margin, query_top + scale(5), scale(86),
               label_height, TRUE);
    MoveWindow(search_edit_, margin + scale(92), query_top,
               client.right - margin * 2 - scale(92) - button_width - gap,
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

    const int button_top = bottom - button_height;
    MoveWindow(close_button_, client.right - margin - button_width, button_top,
               button_width, button_height, TRUE);
    MoveWindow(go_to_button_, client.right - margin - button_width * 2 - gap,
               button_top, button_width, button_height, TRUE);
    MoveWindow(result_count_, margin, button_top + scale(5),
               client.right - margin * 3 - button_width * 2 - gap,
               label_height, TRUE);

    const int results_top = types_top + scale(36);
    MoveWindow(results_.hwnd(), margin, results_top,
               client.right - margin * 2,
               button_top - results_top - scale(12), TRUE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void FileSearchDialog::create_controls() {
    background_brush_ = CreateSolidBrush(theme_.window);
    edit_brush_ = CreateSolidBrush(theme_.edit);
    font_ = axiom::gui::create_dialog_font(dpi_);
    title_ = make_control(L"STATIC", L"Search current file list", SS_NOPREFIX);
    scope_label_ = make_control(L"STATIC", scope_.c_str(), SS_NOPREFIX);
    query_label_ = make_control(L"STATIC", L"Find", SS_NOPREFIX);
    search_edit_ = make_control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
                                kSearchText);
    match_case_ = make_control(L"BUTTON", L"Match case",
                               WS_TABSTOP | BS_OWNERDRAW | BS_CHECKBOX,
                               kMatchCase);
    whole_name_ = make_control(L"BUTTON", L"Whole name",
                               WS_TABSTOP | BS_OWNERDRAW | BS_CHECKBOX,
                               kWholeName);
    search_path_ = make_control(L"BUTTON", L"Search path",
                                WS_TABSTOP | BS_OWNERDRAW | BS_CHECKBOX,
                                kSearchPath);
    include_files_ = make_control(L"BUTTON", L"Files",
                                  WS_TABSTOP | BS_OWNERDRAW | BS_CHECKBOX,
                                  kIncludeFiles);
    include_folders_ = make_control(L"BUTTON", L"Folders",
                                    WS_TABSTOP | BS_OWNERDRAW | BS_CHECKBOX,
                                    kIncludeFolders);
    include_archives_ = make_control(L"BUTTON", L"Archives",
                                     WS_TABSTOP | BS_OWNERDRAW | BS_CHECKBOX,
                                     kIncludeArchives);
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

    set_checkbox(kIncludeFiles, true);
    set_checkbox(kIncludeFolders, true);
    set_checkbox(kIncludeArchives, true);

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
    run_search();
    SetFocus(search_edit_);
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
            info->ptMinTrackSize.x = scale(700);
            info->ptMinTrackSize.y = scale(440);
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            const int notification = HIWORD(wparam);
            switch (id) {
                case kSearchText:
                    if (notification == EN_CHANGE) run_search();
                    return 0;
                case kMatchCase:
                case kWholeName:
                case kSearchPath:
                case kIncludeFiles:
                case kIncludeFolders:
                case kIncludeArchives:
                    if (notification == BN_CLICKED) toggle_checkbox(id);
                    return 0;
                case kSearchButton:
                case IDOK:
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
        case kTableActivateMessage:
            accept_selected();
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return control_color(wparam, false);
        case WM_CTLCOLOREDIT:
            return control_color(wparam, true);
        case WM_DRAWITEM:
            if (lparam != 0) {
                const auto& draw = *reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
                switch (draw.CtlID) {
                    case kMatchCase:
                    case kWholeName:
                    case kSearchPath:
                    case kIncludeFiles:
                    case kIncludeFolders:
                    case kIncludeArchives: {
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
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(hwnd_, &client);
            FillRect(reinterpret_cast<HDC>(wparam), &client, background_brush_);
            return 1;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        case WM_NCDESTROY:
            axiom::gui::save_named_window_placement(L"FileSearchDialog", hwnd_);
            if (font_ != nullptr) axiom::gui::delete_dialog_font(font_);
            if (background_brush_ != nullptr) DeleteObject(background_brush_);
            if (edit_brush_ != nullptr) DeleteObject(edit_brush_);
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            hwnd_ = nullptr;
            return 0;
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
