// Main application window: creation, layout, message dispatch, and the
// run_axiom_gui entry point. Helpers and the other MainWindow method
// groups live in the main_window_*.cpp translation units.

#include "gui/main_window_internal.hpp"

namespace axiom::gui {

MainWindow::~MainWindow() {
    drop_target_.revoke();
    clear_archive_password();
    secure_clear(pending_archive_password_);
    restore_operation_priority();
    for (const auto& temp : temp_directories_) {
        remove_temp_directory(temp.path,
                              temp.sensitive &&
                                  application_options_.wipe_encrypted_temp_files);
    }
    reset_theme_brushes();
    reset_font();
}

bool MainWindow::create(HINSTANCE instance,
            int show_command,
            std::wstring initial_path,
            AxiomGuiStartupCommand startup_command) {
    instance_ = instance;
    startup_command_ = std::move(startup_command);
    persisted_settings_ = axiom::gui::load_gui_settings();
    application_options_ = persisted_settings_.application;
    tree_pane_visible_ = persisted_settings_.tree_pane_visible;
    configure_dialog_appearance(application_options_);
    recent_addresses_ = persisted_settings_.recent_locations;
    recent_archives_ = persisted_settings_.recent_archives;
    favorite_locations_ = persisted_settings_.favorite_locations;
    selected_level_ = application_options_.default_level;
    selected_thread_count_ = application_options_.default_thread_count;
    selected_dictionary_size_ = application_options_.default_dictionary_size;
    selected_word_size_ = application_options_.default_word_size;
    selected_solid_block_size_ = application_options_.default_solid_block_size;
    theme_ = make_theme(application_options_.theme_mode,
                        application_options_.accent_color_mode,
                        application_options_.custom_accent_color);
    cleanup_old_temp_directories();
    apply_shell_integration();
    sort_column_ = persisted_settings_.sort_column;
    sort_ascending_ = persisted_settings_.sort_ascending;
    std::wstring configured_startup = persisted_settings_.last_location;
    if (application_options_.startup_location_mode == 1) {
        configured_startup.clear();
    } else if (application_options_.startup_location_mode == 2) {
        if (const auto desktop = known_folder_path(FOLDERID_Desktop)) {
            configured_startup = desktop->wstring();
        }
    } else if (application_options_.startup_location_mode == 3 &&
               !application_options_.startup_custom_path.empty()) {
        configured_startup = application_options_.startup_custom_path;
    }
    initial_path_ = initial_path.empty() ? std::move(configured_startup)
                                         : std::move(initial_path);
    const UINT system_dpi = GetDpiForSystem();
    const int initial_width = MulDiv(1080, static_cast<int>(system_dpi), USER_DEFAULT_SCREEN_DPI);
    const int initial_height = MulDiv(720, static_cast<int>(system_dpi), USER_DEFAULT_SCREEN_DPI);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = &MainWindow::window_proc;
    wc.lpszClassName = L"AxiomGuiWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    axiom::gui::assign_axiom_window_class_icons(wc, instance);
    wc.hbrBackground = nullptr;

    if (RegisterClassExW(&wc) == 0) {
        return false;
    }

    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"Axiom",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, initial_width, initial_height,
                            nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        return false;
    }
    axiom::gui::apply_axiom_window_icons(hwnd_, instance);

    bool restored_window_placement = false;
    if (application_options_.restore_window_placement &&
        persisted_settings_.has_placement &&
        axiom::gui::window_placement_is_visible(persisted_settings_.placement)) {
        if (persisted_settings_.placement.showCmd == SW_SHOWMINIMIZED) {
            persisted_settings_.placement.showCmd = SW_SHOWNORMAL;
        }
        SetWindowPlacement(hwnd_, &persisted_settings_.placement);
        restored_window_placement = true;
    } else if (application_options_.restore_window_placement &&
               persisted_settings_.has_placement) {
        RECT rect{};
        GetWindowRect(hwnd_, &rect);
        const POINT position = axiom::gui::centered_window_position(
            nullptr, rect.right - rect.left, rect.bottom - rect.top);
        SetWindowPos(hwnd_, nullptr, position.x, position.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    ShowWindow(hwnd_, restored_window_placement
                          ? persisted_settings_.placement.showCmd
                          : show_command);
    UpdateWindow(hwnd_);
    on_initial_navigate();
    return true;
}

bool MainWindow::translate_menu_message(const MSG& message) {
    return menu_bar_.translate_message(message);
}

HWND MainWindow::make_control(const wchar_t* class_name,
                  const wchar_t* text,
                  DWORD style,
                  int id,
                  DWORD ex_style) {
    return CreateWindowExW(ex_style, class_name, text, style | WS_CHILD | WS_VISIBLE,
                           0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           instance_, nullptr);
}

void MainWindow::apply_edit_margins() const {
    const LPARAM margins = MAKELPARAM(scale(2), scale(2));
    if (address_edit_ != nullptr) {
        COMBOBOXINFO info{sizeof(info)};
        if (GetComboBoxInfo(address_edit_, &info) && info.hwndItem != nullptr) {
            SendMessageW(info.hwndItem, EM_SETMARGINS,
                         EC_LEFTMARGIN | EC_RIGHTMARGIN, margins);
        }
        SendMessageW(address_edit_, CB_SETITEMHEIGHT, 0, scale(24));
        SendMessageW(address_edit_, CB_SETITEMHEIGHT,
                     static_cast<WPARAM>(-1), scale(24));
    }
}

void MainWindow::add_tooltip(HWND control, const wchar_t* text) const {
    if (tooltip_ == nullptr || control == nullptr) return;
    TOOLINFOW tool{};
    tool.cbSize = sizeof(tool);
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = hwnd_;
    tool.uId = reinterpret_cast<UINT_PTR>(control);
    tool.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
}

int MainWindow::show_app_message(
    std::wstring_view message,
    axiom::gui::MessageDialogIcon icon,
    std::wstring_view title,
    axiom::gui::MessageDialogButtons buttons,
    int default_result) const {
    return axiom::gui::show_message_dialog(
        hwnd_, instance_, dpi_, theme_.dark, title, message, icon, buttons, default_result);
}

void MainWindow::on_create() {
    update_dpi(GetDpiForWindow(hwnd_));
    if (persisted_settings_.tree_width > 0) {
        tree_width_ = scale(persisted_settings_.tree_width);
    }

    menu_bar_.create(
        hwnd_, instance_,
        {{kMenuFile, L"&File"}, {kMenuCommands, L"&Commands"},
         {kMenuTools, L"&Tools"}, {kMenuOptions, L"&Options"},
         {kMenuHelp, L"&Help"}},
        [this](UINT menu_id) { return menu_items(menu_id); },
        [this](UINT command) {
            SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
        });

    add_files_ = make_control(L"BUTTON", L"Add", WS_TABSTOP | BS_OWNERDRAW, kAddFiles);
    open_archive_ = make_control(L"BUTTON", L"Open archive", WS_TABSTOP | BS_OWNERDRAW, kOpenArchive);
    extract_ = make_control(L"BUTTON", L"Extract", WS_TABSTOP | BS_OWNERDRAW, kExtract);
    test_ = make_control(L"BUTTON", L"Test", WS_TABSTOP | BS_OWNERDRAW, kTest);

    navigate_back_ = make_control(L"BUTTON", L"<", WS_TABSTOP | BS_OWNERDRAW, kNavigateBack);
    navigate_forward_ = make_control(L"BUTTON", L">", WS_TABSTOP | BS_OWNERDRAW, kNavigateForward);
    navigate_up_ = make_control(L"BUTTON", L"Up", WS_TABSTOP | BS_OWNERDRAW, kNavigateUp);
    navigate_refresh_ = make_control(L"BUTTON", L"Refresh", WS_TABSTOP | BS_OWNERDRAW, kNavigateRefresh);
    address_edit_ = make_control(
        L"COMBOBOX", L"",
        WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL |
            CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        kAddressEdit);
    set_text(address_edit_, L"This PC");
    address_go_ = make_control(L"BUTTON", L"Go", WS_TABSTOP | BS_OWNERDRAW, kAddressGo);
    view_ = make_control(L"BUTTON", L"View", WS_TABSTOP | BS_OWNERDRAW, kView);
    delete_ = make_control(L"BUTTON", L"Delete", WS_TABSTOP | BS_OWNERDRAW, kDelete);
    info_ = make_control(L"BUTTON", L"Info", WS_TABSTOP | BS_OWNERDRAW, kInfo);
    settings_ = make_control(L"BUTTON", L"Settings", WS_TABSTOP | BS_OWNERDRAW, kSettings);

    tree_view_.create(hwnd_, instance_, kTree);
    tree_view_.set_populate_callback([this](DirectoryTreeItem& item) {
        populate_tree_item(item);
    });
    tree_view_.set_select_callback([this](const DirectoryTreeItem& item) {
        on_tree_selection_changed(item);
    });
    SetWindowTextW(tree_view_.hwnd(), L"Folders and archive directories");

    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                               WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                               hwnd_, nullptr, instance_, nullptr);
    add_tooltip(navigate_back_, L"Back");
    add_tooltip(navigate_forward_, L"Forward");
    add_tooltip(navigate_up_, L"Up one level");
    add_tooltip(navigate_refresh_, L"Refresh");

    table_.create(hwnd_, instance_, kList);
    list_ = table_.hwnd();
    std::vector<TableColumn> columns{
        {L"Name", 300},
        {L"Size", 105},
        {L"Packed", 105},
        {L"Type", 140},
        {L"Modified", 150},
        {L"CRC-32", 90},
        {L"Attributes", 90},
    };
    if (persisted_settings_.column_widths.size() == columns.size()) {
        for (std::size_t index = 0; index < columns.size(); ++index) {
            columns[index].logical_width =
                std::clamp(persisted_settings_.column_widths[index], 48, 2000);
        }
    }
    table_.set_columns(std::move(columns));
    apply_table_options();
    rebuild_directory_tree();

    status_ = make_control(L"STATIC", L"Ready", SS_LEFT, kStatus);
    // Custom controls do not expose painted content to accessibility tools, so give
    // each HWND a stable semantic name for UI Automation and screen readers.
    SetWindowTextW(list_, L"Files and archive contents");

    apply_edit_margins();
    apply_fonts();
    DragAcceptFiles(hwnd_, TRUE);
    drop_target_.register_window(
        list_,
        [this](IDataObject* object, POINT point, DWORD keys, DWORD allowed) {
            return query_file_drop(object, point, keys, allowed);
        },
        [this](IDataObject* object, POINT point, DWORD keys, DWORD allowed) {
            return perform_file_drop(object, point, keys, allowed);
        });
    apply_theme();
    set_busy(false);
    history_.reset(axiom::gui::BrowserLocation::computer());
    browser_history_states_.assign(history_.size(), std::nullopt);
    maybe_start_automatic_update_check();
}

void MainWindow::on_initial_navigate() {
    if (startup_command_.kind == AxiomGuiStartupCommand::Kind::add_to_archive) {
        std::vector<fs::path> paths;
        paths.reserve(startup_command_.paths.size());
        for (const auto& path : startup_command_.paths) {
            if (!path.empty()) paths.emplace_back(path);
        }
        navigate_to(history_.current(), false);
        create_archive_from_paths(std::move(paths));
        return;
    }
    if (initial_path_.empty()) {
        navigate_to(history_.current(), false);
    } else {
        set_text(address_edit_, initial_path_);
        on_address_go();
    }
    if (startup_command_.kind == AxiomGuiStartupCommand::Kind::extract_archive) {
        on_extract();
    } else if (startup_command_.kind == AxiomGuiStartupCommand::Kind::test_archive) {
        on_test();
    }
}

int MainWindow::browser_pane_top() const {
    return menu_bar_.preferred_height() + scale(8) + scale(30) + scale(6) +
           scale(30) + scale(6);
}

void MainWindow::layout_browser_panes(const RECT& client, int y, bool invalidate_splitter_only) {
    const int margin = scale(8);
    const int bottom_height = scale(28);
    const int splitter_width = scale(5);
    const int minimum_tree_width = scale(180);
    const int minimum_list_width = scale(220);
    const int width = client.right - client.left;
    const int right = width - margin;
    const int content_height = std::max(
        scale(80), static_cast<int>(client.bottom) - y - bottom_height - margin);
    const int content_width = std::max(0, width - 2 * margin);
    const RECT previous_splitter = tree_splitter_rect_;
    if (!tree_pane_visible_) {
        tree_splitter_rect_ = {};
        if (tree_view_.hwnd() != nullptr) {
            ShowWindow(tree_view_.hwnd(), SW_HIDE);
        }
        MoveWindow(list_, margin, y, content_width, content_height, FALSE);
        if (invalidate_splitter_only) {
            RECT dirty = previous_splitter;
            InflateRect(&dirty, scale(2), 0);
            InvalidateRect(hwnd_, &dirty, FALSE);
        }
        return;
    }

    if (tree_view_.hwnd() != nullptr) {
        ShowWindow(tree_view_.hwnd(), SW_SHOW);
    }
    if (tree_width_ <= 0) tree_width_ = scale(250);
    const int maximum_tree_width = std::max(
        minimum_tree_width,
        content_width - splitter_width - minimum_list_width);
    tree_width_ = std::clamp(tree_width_, minimum_tree_width, maximum_tree_width);
    if (content_width < minimum_tree_width + splitter_width + minimum_list_width) {
        tree_width_ = std::max(0, content_width / 3);
    }

    const int tree_x = margin;
    const int tree_w = std::clamp(tree_width_, 0, content_width);
    tree_splitter_rect_ = {
        tree_x + tree_w,
        y,
        tree_x + tree_w + (tree_w > 0 ? splitter_width : 0),
        y + content_height,
    };
    const int list_x = tree_splitter_rect_.right;
    const int list_w = std::max(0, right - list_x);
    tree_view_.move(tree_x, y, tree_w, content_height, false);
    MoveWindow(list_, list_x, y, list_w, content_height, FALSE);

    if (invalidate_splitter_only) {
        RECT dirty{};
        UnionRect(&dirty, &previous_splitter, &tree_splitter_rect_);
        InflateRect(&dirty, scale(2), 0);
        InvalidateRect(hwnd_, &dirty, FALSE);
    }
}

void MainWindow::layout_browser_panes_for_current_size() {
    RECT client{};
    GetClientRect(hwnd_, &client);
    layout_browser_panes(client, browser_pane_top(), true);
}

void MainWindow::repaint_browser_panes_now() const {
    if (tree_pane_visible_ && tree_view_.hwnd() != nullptr) {
        RedrawWindow(tree_view_.hwnd(), nullptr, nullptr,
                     RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
    }
    if (list_ != nullptr) {
        RedrawWindow(list_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
    }
    if (hwnd_ != nullptr) {
        UpdateWindow(hwnd_);
    }
}

void MainWindow::layout() {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int margin = scale(8);
    const int button_height = scale(30);
    const int edit_height = scale(30);
    const int gap = scale(6);
    const int width = client.right - client.left;
    const int right = width - margin;

    for (HWND child : transient_labels_) {
        DestroyWindow(child);
    }
    transient_labels_.clear();

    const int menu_height = menu_bar_.preferred_height();
    menu_bar_.move(0, 0, width, menu_height);

    int y = menu_height + margin;
    int x = margin;
    auto place = [&](HWND child, int logical_width) {
        const int width_px = scale(logical_width);
        ShowWindow(child, SW_SHOW);
        MoveWindow(child, x, y, width_px, button_height, TRUE);
        x += width_px + gap;
    };

    place(add_files_, 72);
    place(extract_, 88);
    place(test_, 64);
    place(view_, 64);
    place(delete_, 70);
    place(info_, 60);
    place(open_archive_, 104);
    place(settings_, 82);
    y += button_height + gap;

    x = margin;
    place(navigate_back_, 36);
    place(navigate_forward_, 36);
    place(navigate_up_, 36);
    place(navigate_refresh_, 36);
    const int go_width = scale(48);
    const int address_x = x;
    const int address_width = std::max(scale(100), right - go_width - gap - address_x);
    MoveWindow(address_edit_, address_x, y, address_width, scale(360), TRUE);
    SendMessageW(address_edit_, CB_SETDROPPEDWIDTH,
                 static_cast<WPARAM>(address_width), 0);
    ShowWindow(address_edit_, SW_SHOW);
    MoveWindow(address_go_, right - go_width, y, go_width, button_height, TRUE);
    ShowWindow(address_go_, SW_SHOW);
    y += edit_height + gap;

    const int bottom_height = scale(28);
    layout_browser_panes(client, y, false);
    const int bottom_y = client.bottom - bottom_height;
    MoveWindow(status_, margin, bottom_y + scale(2), width - margin * 2, scale(20), TRUE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::set_status(const std::wstring& text) {
    set_text(status_, text);
}

void MainWindow::set_busy(bool busy) {
    busy_ = busy;
    EnableWindow(add_files_, !busy);
    EnableWindow(open_archive_, !busy);
    EnableWindow(extract_, !busy);
    EnableWindow(test_, !busy);
    EnableWindow(tree_view_.hwnd(), !busy && tree_pane_visible_);
    EnableWindow(delete_, !busy);
    EnableWindow(settings_, !busy);
    operation_paused_ = false;
}

void MainWindow::toggle_tree_pane() {
    tree_pane_visible_ = !tree_pane_visible_;
    if (!tree_pane_visible_ && dragging_tree_splitter_) {
        dragging_tree_splitter_ = false;
        if (GetCapture() == hwnd_) ReleaseCapture();
    }
    if (!tree_pane_visible_ && GetFocus() == tree_view_.hwnd() && list_ != nullptr) {
        SetFocus(list_);
    }
    EnableWindow(tree_view_.hwnd(), !busy_ && tree_pane_visible_);
    layout();
    repaint_browser_panes_now();
    save_current_settings();
}

int MainWindow::selected_level() const {
    return selected_level_;
}

axiom::CompressionOptions MainWindow::compression_options() const {
    axiom::CompressionOptions options;
    axiom::apply_compression_level(options, selected_level());
    options.thread_count = selected_thread_count_;
    options.io_buffer_size = configured_io_buffer_size(application_options_);
    if (selected_dictionary_size_ != 0) {
        options.window_size = selected_dictionary_size_;
    }
    if (selected_word_size_ != 0) {
        options.nice_length = selected_word_size_;
    }
    if (selected_solid_block_size_ != 0) {
        options.block_size = selected_solid_block_size_;
        options.auto_block_size_for_threads = false;
    }
    if (application_options_.memory_limit_mode == 1) {
        if (const auto limit = parse_size_setting(application_options_.memory_limit)) {
            const std::size_t capped = static_cast<std::size_t>(std::min<std::uint64_t>(
                *limit, std::numeric_limits<std::size_t>::max()));
            const std::size_t practical_cap = std::max<std::size_t>(capped, 64u << 10);
            options.window_size = std::min(options.window_size, practical_cap);
            options.block_size = std::min(options.block_size, practical_cap);
            options.auto_block_size_for_threads = false;
        }
    }
    return options;
}

void MainWindow::apply_table_options() {
    table_.set_options({
        application_options_.show_grid_lines,
        application_options_.show_horizontal_scrollbar,
        application_options_.full_row_select,
    });
}

LRESULT MainWindow::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            on_create();
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_DPICHANGED: {
            update_dpi(HIWORD(wparam));
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr,
                         suggested->left,
                         suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT rect{};
            GetClientRect(hwnd_, &rect);
            FillRect(reinterpret_cast<HDC>(wparam), &rect,
                     window_brush_ != nullptr ? window_brush_ : GetSysColorBrush(COLOR_WINDOW));
            return 1;
        }
        case WM_PAINT:
            paint_shell();
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            apply_theme();
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORLISTBOX:
            return paint_control_background(reinterpret_cast<HWND>(lparam),
                                             reinterpret_cast<HDC>(wparam),
                                             message);
        case WM_DRAWITEM:
            if (lparam != 0) {
                const auto& draw = *reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
                if (draw.CtlType == ODT_COMBOBOX && draw.CtlID == kAddressEdit) {
                    draw_address_entry(draw);
                    return TRUE;
                }
                draw_owner_button(draw);
                return TRUE;
            }
            break;
        case WM_CONTEXTMENU:
            if (tree_pane_visible_ &&
                reinterpret_cast<HWND>(wparam) == tree_view_.hwnd()) {
                show_tree_context_menu({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
                return 0;
            }
            if (reinterpret_cast<HWND>(wparam) == list_ ||
                reinterpret_cast<HWND>(wparam) == hwnd_) {
                show_browser_context_menu({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
                return 0;
            }
            break;
        case WM_LBUTTONDOWN: {
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (tree_pane_visible_ && PtInRect(&tree_splitter_rect_, point)) {
                dragging_tree_splitter_ = true;
                tree_splitter_start_x_ = point.x;
                tree_width_start_ = tree_width_;
                SetCapture(hwnd_);
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (dragging_tree_splitter_) {
                RECT client{};
                GetClientRect(hwnd_, &client);
                const int margin = scale(8);
                const int splitter_width = scale(5);
                const int minimum_tree_width = scale(180);
                const int minimum_list_width = scale(220);
                const int content_width = std::max(
                    0, static_cast<int>(client.right - client.left) - 2 * margin);
                const int maximum_tree_width = std::max(
                    minimum_tree_width,
                    content_width - splitter_width - minimum_list_width);
                const int requested_width = tree_width_start_ +
                                            static_cast<int>(point.x) -
                                            tree_splitter_start_x_;
                const int next_tree_width =
                    std::clamp(requested_width, minimum_tree_width, maximum_tree_width);
                if (next_tree_width != tree_width_) {
                    tree_width_ = next_tree_width;
                    layout_browser_panes_for_current_size();
                    repaint_browser_panes_now();
                }
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return 0;
            }
            if (tree_pane_visible_ && PtInRect(&tree_splitter_rect_, point)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return 0;
            }
            break;
        }
        case WM_LBUTTONUP:
            if (dragging_tree_splitter_) {
                dragging_tree_splitter_ = false;
                if (GetCapture() == hwnd_) ReleaseCapture();
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            dragging_tree_splitter_ = false;
            break;
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(hwnd_, &point);
                if ((tree_pane_visible_ && PtInRect(&tree_splitter_rect_, point)) ||
                    dragging_tree_splitter_) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;
        case WM_DROPFILES:
            on_drop_files(reinterpret_cast<HDROP>(wparam));
            return 0;
        case WM_TIMER:
            if (wparam == kDirectoryRefreshTimer) {
                KillTimer(hwnd_, kDirectoryRefreshTimer);
                if (!busy_) on_navigate_refresh();
                return 0;
            }
            if (wparam == kBrowserPopulateTimer) {
                on_browser_populate_timer();
                return 0;
            }
            break;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case kOpenArchive: on_open_archive(); return 0;
                case kAddFiles: on_add_to_archive(); return 0;
                case kExtract: on_extract(); return 0;
                case kTest: on_test(); return 0;
                case kNavigateBack: on_navigate_back(); return 0;
                case kNavigateForward: on_navigate_forward(); return 0;
                case kNavigateUp: on_navigate_up(); return 0;
                case kNavigateRefresh: on_navigate_refresh(); return 0;
                case kAddressGo: on_address_go(); return 0;
                case kView: on_view(); return 0;
                case kDelete: on_delete_selected(); return 0;
                case kInfo: on_info(); return 0;
                case kFind: on_find_files(); return 0;
                case kCopyPath: on_copy_paths(); return 0;
                case kCopyCrc32: on_copy_crc32(); return 0;
                case kAddFavorite: add_favorite_location(current_location_value()); return 0;
                case kRemoveFavorite: remove_favorite_location(current_location_value()); return 0;
                case kToggleTreePane: toggle_tree_pane(); return 0;
                case kFocusAddress: focus_address_bar(); return 0;
                case kUpdateArchive:
                    on_update_archive(axiom::gui::ArchiveUpdateMode::update_newer);
                    return 0;
                case kFreshenArchive:
                    on_update_archive(axiom::gui::ArchiveUpdateMode::fresh_existing);
                    return 0;
                case kSynchronizeArchive:
                    on_update_archive(axiom::gui::ArchiveUpdateMode::synchronize);
                    return 0;
                case kDeleteArchiveEntries: on_delete_from_archive(); return 0;
                case kRepackArchive: on_repack_archive(); return 0;
                case kEditArchiveComment: on_edit_archive_comment(); return 0;
                case kLockArchive: on_lock_archive(); return 0;
                case kRepairArchive: on_repair_archive(); return 0;
                case kCreateRecoveryVolumes: on_create_recovery_volumes(); return 0;
                case kVerifyArchiveSignature: on_verify_archive_signature(); return 0;
                case kCreateSfx: on_create_sfx(); return 0;
                case kBenchmark: on_benchmark(); return 0;
                case kSettings: on_settings(); return 0;
                case kSelectAll: on_select_all(); return 0;
                case kAbout: on_about(); return 0;
                case kCheckUpdates:
                    begin_update_check(axiom::gui::UpdateCheckKind::manual);
                    return 0;
                case kExitApplication: SendMessageW(hwnd_, WM_CLOSE, 0, 0); return 0;
                case kAddressEdit:
                    if (HIWORD(wparam) == CBN_DROPDOWN) {
                        populate_address_dropdown();
                        return 0;
                    }
                    if (HIWORD(wparam) == CBN_SELENDOK) {
                        select_address_entry();
                        return 0;
                    }
                    break;
            }
            break;
        case kOperationDoneMessage:
            on_operation_done(lparam);
            return 0;
        case kOperationProgressMessage:
            on_operation_progress(lparam);
            return 0;
        case axiom::gui::kUpdateCheckCompleteMessage:
            on_update_check_complete(lparam);
            return 0;
        case axiom::gui::kUpdateDownloadCompleteMessage:
            on_update_download_complete(lparam);
            return 0;
        case kBrowserLoadedMessage:
            on_browser_loaded(lparam);
            return 0;
        case kTableActivateMessage:
            on_table_activate();
            return 0;
        case kTableSelectionChangedMessage:
            on_table_selection_changed();
            return 0;
        case kTableParentMessage:
            if (history_.can_back()) {
                on_navigate_back();
            } else {
                on_navigate_up();
            }
            return 0;
        case kTableSortMessage:
            on_table_sort(static_cast<int>(wparam));
            return 0;
        case kTableBeginDragMessage:
            on_table_begin_drag();
            return 0;
        case kDirectoryChangedMessage:
            // Coalesce bursts from file copies and archive creation into one reload.
            // While an archive operation is active, its growing output would otherwise
            // rebuild the entire browser repeatedly and visibly flash the main window.
            if (busy_) return 0;
            KillTimer(hwnd_, kDirectoryRefreshTimer);
            SetTimer(hwnd_, kDirectoryRefreshTimer, 300, nullptr);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize.x = scale(760);
            limits->ptMinTrackSize.y = scale(480);
            return 0;
        }
        case WM_CLOSE:
            save_current_settings();
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kDirectoryRefreshTimer);
            KillTimer(hwnd_, kBrowserPopulateTimer);
            drop_target_.revoke();
            directory_watcher_.stop();
            browser_thread_.request_stop();
            operation_runner_.cancel();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

LRESULT CALLBACK MainWindow::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    MainWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        window = static_cast<MainWindow*>(create->lpCreateParams);
        window->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window != nullptr) {
        return window->handle_message(message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace axiom::gui

using namespace axiom::gui;

int run_axiom_gui(HINSTANCE instance,
                  int show_command,
                  std::wstring initial_path,
                  AxiomGuiStartupCommand startup_command) {
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        SetProcessDPIAware();
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    const HRESULT com = OleInitialize(nullptr);
    if (FAILED(com)) {
        axiom::gui::show_message_dialog(
            nullptr, instance, GetDpiForSystem(), system_prefers_dark_mode(),
            L"Axiom", L"Failed to initialize COM.",
            axiom::gui::MessageDialogIcon::error);
        return 1;
    }

    if (const auto sfx_result = run_embedded_sfx(instance, initial_path)) {
        OleUninitialize();
        return *sfx_result;
    }

    if (startup_command.kind == AxiomGuiStartupCommand::Kind::add_to_archive) {
        const int result = run_quick_add_to_archive(instance, startup_command.paths);
        OleUninitialize();
        return result;
    }

    MainWindow window;
    if (!window.create(instance, show_command, std::move(initial_path),
                       std::move(startup_command))) {
        OleUninitialize();
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (window.translate_menu_message(message)) continue;
        if (window.translate_keyboard_shortcut(message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    OleUninitialize();
    return static_cast<int>(message.wParam);
}
