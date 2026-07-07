// MainWindow file and archive operations: compress/extract/test,
// archive editing commands, drag-and-drop staging, and operation lifecycle.

#include "gui/main_window_internal.hpp"

#include <algorithm>
#include <unordered_set>

namespace axiom::gui {

namespace {

constexpr wchar_t kCommandInputDialogClass[] = L"AxiomCommandInputDialog";
constexpr int kCommandInputBase = 5200;

struct CommandInputField {
    std::wstring label;
    std::wstring value;
    DWORD style = ES_AUTOHSCROLL;
};

struct CommandInputDialogState {
    HWND window{};
    HWND owner{};
    HWND heading{};
    std::vector<HWND> labels;
    std::vector<HWND> edits;
    HWND ok{};
    HWND cancel{};
    HINSTANCE instance{};
    HFONT font{};
    HBRUSH background_brush{};
    HBRUSH control_brush{};
    UINT dpi{USER_DEFAULT_SCREEN_DPI};
    bool dark{};
    bool accepted{};
    std::wstring title;
    std::wstring heading_text;
    std::wstring placement_name;
    std::vector<CommandInputField>* fields{};
};

std::wstring control_text_local(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(window, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

std::vector<HWND> command_input_controls(CommandInputDialogState* state) {
    std::vector<HWND> controls;
    if (state == nullptr) return controls;
    controls.reserve(3 + state->labels.size() + state->edits.size());
    controls.push_back(state->heading);
    controls.insert(controls.end(), state->labels.begin(), state->labels.end());
    controls.insert(controls.end(), state->edits.begin(), state->edits.end());
    controls.push_back(state->ok);
    controls.push_back(state->cancel);
    return controls;
}

void layout_command_input_dialog(CommandInputDialogState* state) {
    if (state == nullptr || state->window == nullptr) return;
    RECT client{};
    GetClientRect(state->window, &client);
    const int margin = scale_for_dialog_dpi(20, state->dpi);
    const int label_width = scale_for_dialog_dpi(170, state->dpi);
    const int row_height = scale_for_dialog_dpi(30, state->dpi);
    const int row_gap = scale_for_dialog_dpi(12, state->dpi);
    const int button_width = scale_for_dialog_dpi(88, state->dpi);
    const int button_height = scale_for_dialog_dpi(30, state->dpi);
    const int button_gap = scale_for_dialog_dpi(10, state->dpi);
    const int button_top = client.bottom - margin - button_height;
    const int heading_height = scale_for_dialog_dpi(82, state->dpi);
    MoveWindow(state->heading, margin, margin, client.right - margin * 2,
               heading_height, TRUE);
    int y = margin + heading_height + scale_for_dialog_dpi(10, state->dpi);
    const int edit_left = margin + label_width + scale_for_dialog_dpi(12, state->dpi);
    const int edit_width = std::max(scale_for_dialog_dpi(120, state->dpi),
                                    static_cast<int>(client.right) - edit_left - margin);
    for (std::size_t index = 0; index < state->labels.size(); ++index) {
        MoveWindow(state->labels[index], margin, y + scale_for_dialog_dpi(5, state->dpi),
                   label_width, row_height, TRUE);
        MoveWindow(state->edits[index], edit_left, y, edit_width, row_height, TRUE);
        y += row_height + row_gap;
    }
    MoveWindow(state->cancel, client.right - margin - button_width, button_top,
               button_width, button_height, TRUE);
    MoveWindow(state->ok, client.right - margin - button_width * 2 - button_gap,
               button_top, button_width, button_height, TRUE);
    InvalidateRect(state->window, nullptr, TRUE);
}

void rebuild_command_input_fonts(CommandInputDialogState* state) {
    delete_dialog_font(state->font);
    state->font = create_dialog_font(state->dpi);
    for (HWND control : command_input_controls(state)) {
        set_dialog_control_font(control, state->font);
    }
}

void apply_command_input_theme(CommandInputDialogState* state) {
    if (state == nullptr) return;
    apply_dialog_dark_frame(state->window, state->dark);
    for (HWND control : command_input_controls(state)) {
        apply_dialog_control_theme(control, state->dark);
    }
}

LRESULT CALLBACK command_input_dialog_proc(HWND hwnd, UINT message,
                                           WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<CommandInputDialogState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = create == nullptr
                    ? nullptr
                    : static_cast<CommandInputDialogState*>(create->lpCreateParams);
        if (state == nullptr) return FALSE;
        state->window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);

    switch (message) {
        case WM_CREATE: {
            const DialogColors colors = dialog_colors(state->dark);
            state->background_brush = CreateSolidBrush(colors.background);
            state->control_brush = CreateSolidBrush(colors.control_background);
            state->font = create_dialog_font(state->dpi);
            state->heading = CreateWindowExW(
                0, L"STATIC", state->heading_text.c_str(),
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_NOPREFIX,
                0, 0, 0, 0, hwnd, nullptr, state->instance, nullptr);
            const std::size_t field_count = state->fields == nullptr ? 0 : state->fields->size();
            state->labels.resize(field_count);
            state->edits.resize(field_count);
            for (std::size_t index = 0; index < field_count; ++index) {
                const auto& field = (*state->fields)[index];
                state->labels[index] = CreateWindowExW(
                    0, L"STATIC", field.label.c_str(),
                    WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_NOPREFIX,
                    0, 0, 0, 0, hwnd, nullptr, state->instance, nullptr);
                state->edits[index] = CreateWindowExW(
                    0, L"EDIT", field.value.c_str(),
                    WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP |
                        WS_BORDER | field.style,
                    0, 0, 0, 0, hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCommandInputBase + index)),
                    state->instance, nullptr);
                SendMessageW(state->edits[index], EM_SETMARGINS,
                             EC_LEFTMARGIN | EC_RIGHTMARGIN,
                             MAKELPARAM(scale_for_dialog_dpi(8, state->dpi),
                                        scale_for_dialog_dpi(8, state->dpi)));
            }
            state->ok = CreateWindowExW(
                0, L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDOK),
                state->instance, nullptr);
            state->cancel = CreateWindowExW(
                0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDCANCEL),
                state->instance, nullptr);
            for (HWND control : command_input_controls(state)) {
                set_dialog_control_font(control, state->font);
            }
            apply_command_input_theme(state);
            layout_command_input_dialog(state);
            SendMessageW(hwnd, DM_SETDEFID, IDOK, 0);
            if (!state->edits.empty()) {
                SetFocus(state->edits.front());
                SendMessageW(state->edits.front(), EM_SETSEL, 0, -1);
            }
            return 0;
        }
        case WM_SIZE:
            layout_command_input_dialog(state);
            return 0;
        case WM_DPICHANGED: {
            state->dpi = HIWORD(wparam);
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            rebuild_command_input_fonts(state);
            layout_command_input_dialog(state);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = scale_for_dialog_dpi(520, state->dpi);
            info->ptMinTrackSize.y = scale_for_dialog_dpi(260, state->dpi);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == IDOK) {
                if (state->fields != nullptr) {
                    for (std::size_t index = 0; index < state->fields->size(); ++index) {
                        (*state->fields)[index].value = control_text_local(state->edits[index]);
                    }
                }
                state->accepted = true;
                save_named_window_placement(state->placement_name, hwnd);
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wparam) == IDCANCEL) {
                save_named_window_placement(state->placement_name, hwnd);
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            const DialogColors colors = dialog_colors(state->dark);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, colors.text);
            return reinterpret_cast<LRESULT>(state->background_brush);
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            const DialogColors colors = dialog_colors(state->dark);
            SetBkColor(dc, colors.control_background);
            SetTextColor(dc, colors.text);
            return reinterpret_cast<LRESULT>(state->control_brush);
        }
        case WM_DRAWITEM:
            if (lparam != 0) {
                draw_dialog_button(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam), state->dark);
                return TRUE;
            }
            break;
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(hwnd, &client);
            FillRect(reinterpret_cast<HDC>(wparam), &client, state->background_brush);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            FillRect(dc, &paint.rcPaint, state->background_brush);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_CLOSE:
            save_named_window_placement(state->placement_name, hwnd);
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            delete_dialog_font(state->font);
            if (state->background_brush != nullptr) DeleteObject(state->background_brush);
            if (state->control_brush != nullptr) DeleteObject(state->control_brush);
            state->window = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool show_command_input_dialog(HWND owner,
                               HINSTANCE instance,
                               std::wstring title,
                               std::wstring heading,
                               std::vector<CommandInputField>& fields,
                               std::wstring placement_name) {
    if (instance == nullptr) instance = GetModuleHandleW(nullptr);
    const UINT dpi = owner != nullptr ? GetDpiForWindow(owner) : GetDpiForSystem();
    CommandInputDialogState state{};
    state.owner = owner;
    state.instance = instance;
    state.dpi = dpi;
    state.dark = dialog_should_use_dark();
    state.title = std::move(title);
    state.heading_text = std::move(heading);
    state.placement_name = std::move(placement_name);
    state.fields = &fields;

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &command_input_dialog_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kCommandInputDialogClass;
    assign_axiom_window_class_icons(window_class, instance);
    if (RegisterClassExW(&window_class) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        show_message_dialog(owner, instance, dpi, state.dark, state.title,
                            last_error_text(), MessageDialogIcon::error);
        return false;
    }

    const int width = scale_for_dialog_dpi(560, dpi);
    const int height = scale_for_dialog_dpi(
        210 + static_cast<int>(fields.size()) * 42, dpi);
    const POINT position = centered_window_position(owner, width, height);
    HWND dialog = CreateWindowExW(
        WS_EX_WINDOWEDGE | WS_EX_CONTROLPARENT, kCommandInputDialogClass,
        state.title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | WS_CLIPSIBLINGS |
            WS_THICKFRAME,
        position.x, position.y, width, height, owner, nullptr, instance, &state);
    if (dialog == nullptr) {
        show_message_dialog(owner, instance, dpi, state.dark, state.title,
                            last_error_text(), MessageDialogIcon::error);
        return false;
    }
    apply_axiom_window_icons(dialog, instance);
    restore_named_window_placement(dialog, owner, state.placement_name);
    const bool owner_was_enabled = disable_dialog_owner(owner);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    MSG message{};
    while (IsWindow(dialog)) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (message_targets_window(dialog, message) &&
            message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            save_named_window_placement(state.placement_name, dialog);
            DestroyWindow(dialog);
            continue;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    restore_dialog_owner(owner, owner_was_enabled);
    return state.accepted;
}

void set_dialog_initial_path(IFileDialog* dialog, const fs::path& path) {
    if (dialog == nullptr || path.empty()) return;
    const fs::path folder = path.has_filename() ? path.parent_path() : path;
    if (!folder.empty()) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr,
                                                 IID_PPV_ARGS(item.put())))) {
            dialog->SetFolder(item.get());
        }
    }
}

std::optional<fs::path> pick_single_file(HWND owner,
                                         const wchar_t* title,
                                         const COMDLG_FILTERSPEC* filters = nullptr,
                                         UINT filter_count = 0) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) {
        return std::nullopt;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    if (title != nullptr) dialog->SetTitle(title);
    if (filters != nullptr && filter_count != 0) {
        dialog->SetFileTypes(filter_count, filters);
    }
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return std::nullopt;
    return shell_item_path(item.get());
}

std::optional<fs::path> pick_save_file(HWND owner,
                                       const wchar_t* title,
                                       const fs::path& suggested,
                                       const COMDLG_FILTERSPEC* filters,
                                       UINT filter_count,
                                       const wchar_t* default_extension) {
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) {
        return std::nullopt;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
    if (title != nullptr) dialog->SetTitle(title);
    if (filters != nullptr && filter_count != 0) {
        dialog->SetFileTypes(filter_count, filters);
    }
    if (default_extension != nullptr) dialog->SetDefaultExtension(default_extension);
    if (!suggested.empty()) {
        dialog->SetFileName(suggested.filename().c_str());
        set_dialog_initial_path(dialog.get(), suggested);
    }
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return std::nullopt;
    return shell_item_path(item.get());
}

std::optional<unsigned> parse_unsigned_field(std::wstring text, unsigned maximum) {
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), [](wchar_t ch) {
        return !std::iswspace(ch);
    }));
    while (!text.empty() && std::iswspace(text.back())) text.pop_back();
    if (text.empty()) return std::nullopt;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long value = std::wcstoul(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str()) return std::nullopt;
    while (*end != L'\0' && std::iswspace(*end)) ++end;
    if (*end != L'\0' || value > maximum) return std::nullopt;
    return static_cast<unsigned>(value);
}

template <std::size_t Size>
std::array<std::uint8_t, Size> read_key_file(const fs::path& path) {
    std::array<std::uint8_t, Size> key{};
    std::ifstream input(path, std::ios::binary);
    if (!input || !input.read(reinterpret_cast<char*>(key.data()),
                              static_cast<std::streamsize>(key.size())) ||
        input.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("invalid signing key file");
    }
    return key;
}

void write_key_file(const fs::path& path, const std::uint8_t* data, std::size_t size) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output || !output.write(reinterpret_cast<const char*>(data),
                                 static_cast<std::streamsize>(size))) {
        throw std::runtime_error("could not write key file");
    }
}

std::string normalized_drag_archive_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    while (!path.empty() && path.back() == '/') path.pop_back();
    return path;
}

bool archive_path_is_under_directory(std::string_view path, std::string_view directory) {
    if (path == directory) return true;
    return path.size() > directory.size() &&
           path.compare(0, directory.size(), directory) == 0 &&
           path[directory.size()] == '/';
}

std::wstring archive_drag_relative_name(std::string_view archive_path,
                                        std::string_view current_directory) {
    std::string relative;
    if (!current_directory.empty() &&
        archive_path.size() > current_directory.size() &&
        archive_path.compare(0, current_directory.size(), current_directory) == 0 &&
        archive_path[current_directory.size()] == '/') {
        relative = std::string(archive_path.substr(current_directory.size() + 1));
    } else {
        relative = std::string(archive_path);
    }
    return widen(relative);
}

void ensure_virtual_parent_directories(
    std::string_view archive_path,
    std::string_view relative_path,
    std::vector<VirtualFileDragItem>& items,
    std::vector<std::string>& materialized_archive_paths,
    std::unordered_set<std::string>& seen_relative_paths) {
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = relative_path.find('/', start);
        if (slash == std::string_view::npos) break;
        const std::string relative_parent(relative_path.substr(0, slash));
        if (!relative_parent.empty() && seen_relative_paths.insert(relative_parent).second) {
            VirtualFileDragItem parent;
            parent.relative_path = widen(relative_parent);
            parent.is_directory = true;
            items.push_back(std::move(parent));
            const std::size_t parent_length = static_cast<std::size_t>(
                relative_parent.size());
            const std::size_t archive_parent_length =
                archive_path.size() >= relative_path.size()
                    ? archive_path.size() - relative_path.size() + parent_length
                    : parent_length;
            materialized_archive_paths.push_back(
                std::string(archive_path.substr(0, archive_parent_length)));
        }
        start = slash + 1;
    }
}

void add_virtual_archive_item(
    std::string archive_path,
    std::string_view current_directory,
    bool is_directory,
    std::uint64_t size,
    std::int64_t mtime,
    std::vector<VirtualFileDragItem>& items,
    std::vector<std::string>& materialized_archive_paths,
    std::unordered_set<std::string>& seen_relative_paths) {
    archive_path = normalized_drag_archive_path(std::move(archive_path));
    if (archive_path.empty()) return;
    const std::wstring relative_wide =
        archive_drag_relative_name(archive_path, current_directory);
    std::string relative = utf8(relative_wide);
    relative = normalized_drag_archive_path(std::move(relative));
    if (relative.empty()) return;
    ensure_virtual_parent_directories(archive_path, relative, items,
                                      materialized_archive_paths,
                                      seen_relative_paths);
    if (!seen_relative_paths.insert(relative).second) return;
    VirtualFileDragItem item;
    item.relative_path = std::move(relative_wide);
    item.size = is_directory ? 0 : size;
    item.mtime = mtime;
    item.is_directory = is_directory;
    items.push_back(std::move(item));
    materialized_archive_paths.push_back(std::move(archive_path));
}

}  // namespace

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
    const std::string current_directory =
        normalized_drag_archive_path(history_.current().archive_directory);

    std::vector<axiom::gui::VirtualFileDragItem> virtual_items;
    std::vector<std::string> materialized_archive_paths;
    std::unordered_set<std::string> selected_exact;
    std::unordered_set<std::string> selected_directories;
    std::unordered_set<std::string> described_relative_paths;
    selected_exact.reserve(entries.size());
    selected_directories.reserve(entries.size());
    for (const int index : selected_browser_indices()) {
        const auto& item = browser_items_[static_cast<std::size_t>(index)];
        if (item.is_parent() || item.archive_path.empty()) continue;
        const std::string path = normalized_drag_archive_path(item.archive_path);
        selected_exact.insert(path);
        if (item.kind == axiom::gui::BrowserItemKind::directory) {
            selected_directories.insert(path);
        }
        add_virtual_archive_item(path, current_directory,
                                 item.kind == axiom::gui::BrowserItemKind::directory,
                                 item.size, 0, virtual_items, materialized_archive_paths,
                                 described_relative_paths);
    }
    if (archive_catalog_) {
        for (const auto& entry : archive_catalog_->entries()) {
            const std::string path = normalized_drag_archive_path(entry.path);
            if (path.empty()) continue;
            bool selected = selected_exact.find(path) != selected_exact.end();
            if (!selected) {
                for (const auto& directory : selected_directories) {
                    if (archive_path_is_under_directory(path, directory)) {
                        selected = true;
                        break;
                    }
                }
            }
            if (!selected) continue;
            add_virtual_archive_item(path, current_directory, entry.is_directory,
                                     entry.size, entry.mtime, virtual_items,
                                     materialized_archive_paths,
                                     described_relative_paths);
        }
    }
    if (virtual_items.empty()) return;

    auto password = password_for_archive_edit(archive);
    if (!password) return;

    std::wstring drag_error;
    axiom::gui::FileDragSource source;
    source.archive_payload = {archive, entries};
    source.virtual_files = virtual_items;
    source.preferred_effect = DROPEFFECT_COPY;
    source.error_message = &drag_error;
    source.virtual_file_paths =
        [this, archive, entries,
         materialized_archive_paths = std::move(materialized_archive_paths),
         password = std::move(*password)]() mutable {
        try {
            auto staged = extract_archive_entries_to_staging(
                archive, entries, password, true, !password.empty());
            secure_clear(password);
            std::vector<fs::path> paths;
            paths.reserve(materialized_archive_paths.size());
            for (const auto& path : materialized_archive_paths) {
                paths.push_back(staged.directory / fs::path(widen(path)));
            }
            return paths;
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

void MainWindow::on_edit_recovery_record() {
    const auto archive = active_archive_path();
    if (!archive || busy_) return;
    if (!active_archive_is_editable()) return;
    const auto capabilities = active_archive_capabilities();
    if (!capabilities.recovery_records) {
        show_app_message(L"This archive format does not support recovery records.",
                         axiom::gui::MessageDialogIcon::information,
                         L"Recovery record");
        return;
    }

    axiom::ArchiveRecoveryInfo info;
    try {
        info = axiom::archive_recovery_info(*archive);
    } catch (const std::exception& error) {
        show_app_message(widen(error.what()), axiom::gui::MessageDialogIcon::error,
                         L"Recovery record");
        return;
    }

    std::wstring heading = info.present
        ? L"Current recovery record: " + std::to_wstring(info.percent) + L"% (" +
              std::to_wstring(info.data_shards) + L" data + " +
              std::to_wstring(info.parity_shards) + L" parity shards, protects " +
              format_size(info.protected_size) + L").\n\nEnter 1..100 to rebuild, or 0 to remove."
        : L"This archive has no recovery record.\n\nEnter 1..100 to create one; 0 leaves it disabled.";
    std::vector<CommandInputField> fields{
        {L"Recovery percent", info.present ? std::to_wstring(info.percent) : L"0"}
    };
    if (!show_command_input_dialog(hwnd_, instance_, L"Recovery record",
                                   std::move(heading), fields,
                                   L"Dialog.RecoveryRecord")) {
        return;
    }
    const auto percent = parse_unsigned_field(fields[0].value, 100);
    if (!percent) {
        show_app_message(L"Enter a recovery percentage from 0 to 100.",
                         axiom::gui::MessageDialogIcon::warning,
                         L"Recovery record");
        return;
    }
    if (*percent == 0 && !info.present) {
        set_status(L"Archive has no recovery record.");
        return;
    }
    if (*percent == 0 &&
        show_app_message(L"Remove the recovery record from this archive?",
                         axiom::gui::MessageDialogIcon::warning,
                         L"Recovery record",
                         axiom::gui::MessageDialogButtons::yes_no,
                         IDNO) != IDYES) {
        return;
    }

    operation_archive_output_ = *archive;
    start_operation(
        *percent == 0 ? L"Removing recovery record..." : L"Updating recovery record...",
        *percent == 0 ? L"Recovery record removed." : L"Recovery record updated.",
        [archive = *archive, percent = *percent](
            std::shared_ptr<axiom::OperationControl> operation) {
            axiom::set_archive_recovery(archive, percent, operation);
        });
}

void MainWindow::on_split_archive() {
    fs::path archive;
    if (const auto active = active_archive_path()) {
        archive = *active;
    } else if (const auto picked = pick_open_archive(hwnd_)) {
        archive = *picked;
    } else {
        return;
    }
    if (busy_) return;
    const auto* provider = axiom::archive_provider_for_path(archive);
    if (provider == nullptr) {
        show_app_message(L"Choose a supported archive first.",
                         axiom::gui::MessageDialogIcon::information,
                         L"Split archive");
        return;
    }
    const auto capabilities = provider->capabilities(archive);
    if (!capabilities.multi_volume) {
        show_app_message(L"This archive format does not support Axiom split volumes.",
                         axiom::gui::MessageDialogIcon::information,
                         L"Split archive");
        return;
    }

    static constexpr std::array<const wchar_t*, 4> kUnitSuffixes{L"K", L"M", L"G", L"T"};
    std::wstring default_volume = application_options_.default_volume_size.empty()
        ? L"700M"
        : application_options_.default_volume_size +
              kUnitSuffixes[std::clamp(application_options_.default_volume_unit, 0, 3)];
    std::vector<CommandInputField> fields{
        {L"Volume size", std::move(default_volume)},
        {L"Recovery volumes", application_options_.default_recovery_volumes ? L"1" : L"0"},
    };
    if (!show_command_input_dialog(
            hwnd_, instance_, L"Split archive",
            L"Create numbered archive volumes beside the source archive.\n\n"
            L"Use K, M, G, or T suffixes for volume size. Recovery volumes are optional.",
            fields, L"Dialog.SplitArchive")) {
        return;
    }
    const auto volume_size = parse_size_setting(fields[0].value);
    const auto recovery_count = parse_unsigned_field(fields[1].value, 100000);
    if (!volume_size || *volume_size == 0 || !recovery_count) {
        show_app_message(L"Enter a positive volume size and a recovery volume count from 0 to 100000.",
                         axiom::gui::MessageDialogIcon::warning,
                         L"Split archive");
        return;
    }
    operation_archive_output_ = archive;
    start_operation(
        L"Splitting archive...", L"Archive volumes were created beside the source archive.",
        [archive, volume_size = *volume_size, recovery_count = *recovery_count](
            std::shared_ptr<axiom::OperationControl> operation) {
            axiom::create_archive_volumes(archive, volume_size, recovery_count, operation);
        });
}

void MainWindow::on_join_archive() {
    if (busy_) return;
    const COMDLG_FILTERSPEC filters[] = {
        {L"Axiom split volumes", L"*.part*.axar;*.rev*"},
        {L"Axiom archives", L"*.axar"},
        {L"All files", L"*.*"},
    };
    const auto volume = pick_single_file(hwnd_, L"Choose an Axiom archive volume",
                                         filters, static_cast<UINT>(std::size(filters)));
    if (!volume) return;
    fs::path suggested = joined_archive_path_for_volume(*volume).value_or(
        volume->parent_path() / (volume->stem().wstring() + L".axar"));
    const COMDLG_FILTERSPEC save_filters[] = {
        {L"Axiom archive", L"*.axar"},
        {L"All files", L"*.*"},
    };
    const auto output = pick_save_file(hwnd_, L"Save joined archive", suggested,
                                       save_filters, static_cast<UINT>(std::size(save_filters)),
                                       L"axar");
    if (!output) return;
    if (same_filesystem_path(*volume, *output)) {
        show_app_message(L"Choose an output path different from the selected volume.",
                         axiom::gui::MessageDialogIcon::warning,
                         L"Join archive volumes");
        return;
    }
    operation_archive_output_ = *output;
    operation_open_after_ = *output;
    start_operation(
        L"Joining archive volumes...", L"Archive volumes joined successfully.",
        [volume = *volume, output = *output](
            std::shared_ptr<axiom::OperationControl> operation) {
            axiom::join_archive_volumes(volume, output, operation);
        });
}

void MainWindow::on_generate_signing_key() {
    if (busy_) return;
    const COMDLG_FILTERSPEC secret_filters[] = {
        {L"Axiom secret signing key", L"*.key"},
        {L"All files", L"*.*"},
    };
    const fs::path suggested_secret = application_options_.default_signing_key.empty()
        ? fs::path(L"axiom-signing.key")
        : fs::path(application_options_.default_signing_key);
    const auto secret = pick_save_file(hwnd_, L"Save secret signing key",
                                       suggested_secret,
                                       secret_filters,
                                       static_cast<UINT>(std::size(secret_filters)),
                                       L"key");
    if (!secret) return;
    const COMDLG_FILTERSPEC public_filters[] = {
        {L"Axiom public signing key", L"*.pub"},
        {L"All files", L"*.*"},
    };
    const fs::path suggested_public =
        secret->parent_path() / (secret->stem().wstring() + L".pub");
    const auto public_key = pick_save_file(hwnd_, L"Save public signing key",
                                           suggested_public, public_filters,
                                           static_cast<UINT>(std::size(public_filters)),
                                           L"pub");
    if (!public_key) return;
    if (same_filesystem_path(*secret, *public_key)) {
        show_app_message(L"Choose different paths for the secret and public key files.",
                         axiom::gui::MessageDialogIcon::warning,
                         L"Generate signing key");
        return;
    }

    start_operation(
        L"Generating signing key...", L"Signing key files were generated.",
        [secret = *secret, public_key = *public_key](
            std::shared_ptr<axiom::OperationControl> operation) {
            if (operation) operation->checkpoint();
            auto key = axiom::generate_archive_signing_key();
            try {
                write_key_file(secret, key.secret_key.data(), key.secret_key.size());
                write_key_file(public_key, key.public_key.data(), key.public_key.size());
            } catch (...) {
                SecureZeroMemory(key.secret_key.data(), key.secret_key.size());
                throw;
            }
            SecureZeroMemory(key.secret_key.data(), key.secret_key.size());
        });
}

void MainWindow::on_sign_archive() {
    const auto archive = active_archive_path();
    if (!archive || busy_) return;
    if (!active_archive_is_editable()) return;
    const auto capabilities = active_archive_capabilities();
    if (!capabilities.authenticity) {
        show_app_message(L"This archive format does not support Axiom signatures.",
                         axiom::gui::MessageDialogIcon::information,
                         L"Sign archive");
        return;
    }
    const COMDLG_FILTERSPEC filters[] = {
        {L"Axiom secret signing key", L"*.key"},
        {L"All files", L"*.*"},
    };
    const auto secret = pick_single_file(hwnd_, L"Choose the secret signing key",
                                         filters, static_cast<UINT>(std::size(filters)));
    if (!secret) return;
    auto password = password_for_archive_edit(*archive);
    if (!password) return;
    auto options = compression_options();
    options.password = std::move(*password);
    operation_archive_output_ = *archive;
    start_operation(
        L"Signing archive...", L"Archive signed successfully.",
        [archive = *archive, secret = *secret, options](
            std::shared_ptr<axiom::OperationControl> operation) mutable {
            auto run_options = options;
            run_options.operation = std::move(operation);
            axiom::ArchiveSigningKey key;
            key.secret_key = read_key_file<64>(secret);
            std::copy_n(key.secret_key.begin() + 32, key.public_key.size(),
                        key.public_key.begin());
            try {
                axiom::sign_archive(archive, key, run_options);
            } catch (...) {
                SecureZeroMemory(key.secret_key.data(), key.secret_key.size());
                throw;
            }
            SecureZeroMemory(key.secret_key.data(), key.secret_key.size());
        });
}

void MainWindow::on_compress_stream() {
    if (busy_) return;
    const COMDLG_FILTERSPEC input_filters[] = {{L"All files", L"*.*"}};
    const auto input = pick_single_file(hwnd_, L"Choose file to compress",
                                        input_filters,
                                        static_cast<UINT>(std::size(input_filters)));
    if (!input) return;
    const fs::path suggested =
        input->parent_path() / (input->filename().wstring() + L".axc");
    const COMDLG_FILTERSPEC output_filters[] = {
        {L"Axiom compressed stream", L"*.axc"},
        {L"All files", L"*.*"},
    };
    const auto output = pick_save_file(hwnd_, L"Save compressed stream", suggested,
                                       output_filters,
                                       static_cast<UINT>(std::size(output_filters)),
                                       L"axc");
    if (!output) return;
    if (same_filesystem_path(*input, *output)) {
        show_app_message(L"Choose an output path different from the input file.",
                         axiom::gui::MessageDialogIcon::warning,
                         L"Compress stream");
        return;
    }
    start_operation(
        L"Compressing stream...", L"Single-stream file compressed.",
        [input = *input, output = *output, options = compression_options()](
            std::shared_ptr<axiom::OperationControl> operation) mutable {
            auto run_options = options;
            run_options.operation = std::move(operation);
            axiom::compress_file(input, output, run_options);
        });
}

void MainWindow::on_decompress_stream() {
    if (busy_) return;
    const COMDLG_FILTERSPEC input_filters[] = {
        {L"Axiom compressed stream", L"*.axc"},
        {L"All files", L"*.*"},
    };
    const auto input = pick_single_file(hwnd_, L"Choose stream to decompress",
                                        input_filters,
                                        static_cast<UINT>(std::size(input_filters)));
    if (!input) return;
    fs::path suggested = *input;
    if (suggested.extension() == L".axc") {
        suggested.replace_extension();
        if (suggested.filename().empty()) suggested = input->parent_path() / L"decompressed";
    } else {
        suggested += L".out";
    }
    const COMDLG_FILTERSPEC output_filters[] = {{L"All files", L"*.*"}};
    const auto output = pick_save_file(hwnd_, L"Save decompressed file", suggested,
                                       output_filters,
                                       static_cast<UINT>(std::size(output_filters)),
                                       nullptr);
    if (!output) return;
    if (same_filesystem_path(*input, *output)) {
        show_app_message(L"Choose an output path different from the input stream.",
                         axiom::gui::MessageDialogIcon::warning,
                         L"Decompress stream");
        return;
    }
    axiom::DecompressionOptions options;
    options.thread_count = selected_thread_count_;
    options.io_buffer_size = configured_io_buffer_size(application_options_);
    start_operation(
        L"Decompressing stream...", L"Single-stream file decompressed.",
        [input = *input, output = *output, options](
            std::shared_ptr<axiom::OperationControl> operation) mutable {
            auto run_options = options;
            run_options.operation = std::move(operation);
            axiom::decompress_file(input, output, run_options);
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
