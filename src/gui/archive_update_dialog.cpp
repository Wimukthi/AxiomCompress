#define NOMINMAX
#include "gui/archive_dialogs.hpp"

#include "core/path_text.hpp"
#include "gui/dialog_support.hpp"
#include "gui/main_window_internal.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>

namespace axiom::gui {
namespace {

constexpr int kPlanSearch = 2960;
constexpr int kPlanFilter = 2961;
constexpr int kPlanTable = 2962;
constexpr int kPlanWarning = 2963;

const wchar_t* action_name(ArchiveUpdatePlanAction action) {
    switch (action) {
        case ArchiveUpdatePlanAction::add: return L"Add";
        case ArchiveUpdatePlanAction::replace: return L"Replace";
        case ArchiveUpdatePlanAction::remove: return L"Remove";
        case ArchiveUpdatePlanAction::unchanged: return L"Unchanged";
        case ArchiveUpdatePlanAction::ignored: return L"Ignored";
        case ArchiveUpdatePlanAction::conflict: return L"Conflict";
    }
    return L"";
}

std::wstring plan_mode_name(ArchiveUpdateMode mode) {
    switch (mode) {
        case ArchiveUpdateMode::add_or_replace: return L"Add / replace";
        case ArchiveUpdateMode::update_newer: return L"Update";
        case ArchiveUpdateMode::fresh_existing: return L"Freshen";
        case ArchiveUpdateMode::synchronize: return L"Synchronize";
        default: return L"Update";
    }
}

std::wstring plan_description(ArchiveUpdateMode mode) {
    switch (mode) {
        case ArchiveUpdateMode::add_or_replace:
            return L"Review every entry that will be added or replaced.";
        case ArchiveUpdateMode::fresh_existing:
            return L"Only newer files already in the archive will be replaced; missing source items are ignored.";
        case ArchiveUpdateMode::synchronize:
            return L"The archive will mirror this complete source. Entries missing from the source will be removed.";
        default:
            return L"New source items will be added and newer files will replace archived copies.";
    }
}

std::wstring format_plan_bytes(std::uint64_t bytes) {
    static constexpr std::array<const wchar_t*, 6> units{
        L"B", L"KiB", L"MiB", L"GiB", L"TiB", L"PiB"};
    long double value = static_cast<long double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0L && unit + 1 < units.size()) {
        value /= 1024.0L;
        ++unit;
    }
    std::wostringstream output;
    output << std::fixed << std::setprecision(unit == 0 ? 0 : 1)
           << static_cast<double>(value) << L' ' << units[unit];
    return output.str();
}

std::wstring archive_path_text(std::string_view path) {
    return axiom::core::path_from_utf8(path).wstring();
}

std::wstring size_change_text(const ArchiveUpdatePlanItem& item) {
    if (item.source_directory || item.source_symlink ||
        item.archive_directory || item.archive_symlink) {
        return L"-";
    }
    switch (item.action) {
        case ArchiveUpdatePlanAction::add:
            return L"+ " + format_plan_bytes(item.source_size);
        case ArchiveUpdatePlanAction::replace:
            return format_plan_bytes(item.archive_size) + L"  ->  " +
                   format_plan_bytes(item.source_size);
        case ArchiveUpdatePlanAction::remove:
            return L"- " + format_plan_bytes(item.archive_size);
        default:
            return format_plan_bytes(
                item.source_path.empty() ? item.archive_size : item.source_size);
    }
}

std::wstring plan_summary(const ArchiveUpdatePlan& plan) {
    std::vector<std::wstring> parts;
    const auto add_count = [&parts](std::size_t count, const wchar_t* name) {
        if (count != 0) parts.push_back(std::to_wstring(count) + L" " + name);
    };
    add_count(plan.added, L"add");
    add_count(plan.replaced, L"replace");
    add_count(plan.removed, L"remove");
    add_count(plan.conflicts, L"conflict");
    add_count(plan.ignored, L"ignored");
    add_count(plan.unchanged, L"unchanged");
    if (parts.empty()) parts.push_back(L"No content differences");

    std::wstring result;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index != 0) result += L"  |  ";
        result += parts[index];
    }
    if (plan.added_bytes != 0) {
        result += L"  |  new " + format_plan_bytes(plan.added_bytes);
    }
    if (plan.replaced != 0) {
        result += L"  |  replace " +
                  format_plan_bytes(plan.replacement_archive_bytes) + L" with " +
                  format_plan_bytes(plan.replacement_source_bytes);
    }
    if (plan.removed_bytes != 0) {
        result += L"  |  remove " + format_plan_bytes(plan.removed_bytes);
    }
    return result;
}

class ArchiveUpdatePlanDialog {
public:
    ArchiveUpdatePlanDialog(const ArchiveUpdatePlan& plan, ThemePalette theme)
        : plan_(plan), theme_(theme) {}

    ~ArchiveUpdatePlanDialog() {
        if (font_ != nullptr) DeleteObject(font_);
        if (window_brush_ != nullptr) DeleteObject(window_brush_);
        if (edit_brush_ != nullptr) DeleteObject(edit_brush_);
    }

    bool show(HWND owner) {
        owner_ = owner;
        instance_ = reinterpret_cast<HINSTANCE>(
            GetWindowLongPtrW(owner, GWLP_HINSTANCE));
        dpi_ = GetDpiForWindow(owner);
        if (!register_class()) return false;

        const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU |
                            WS_THICKFRAME | WS_CLIPCHILDREN;
        const DWORD ex_style = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
        const SIZE size = dialog_window_size_for_client(
            1040, 650, style, ex_style, dpi_);
        const POINT position = centered_window_position(owner, size.cx, size.cy);
        const std::wstring title = plan_mode_name(plan_.mode) + L" preview";
        window_ = CreateWindowExW(
            ex_style, class_name(), title.c_str(), style,
            position.x, position.y, size.cx, size.cy, owner, nullptr,
            instance_, this);
        if (window_ == nullptr) return false;
        const int show_command = restore_named_window_placement(
            window_, owner, L"ArchiveUpdatePreview");
        owner_was_enabled_ = disable_dialog_owner(owner, window_);
        ShowWindow(window_, show_command);
        UpdateWindow(window_);

        MSG message{};
        while (IsWindow(window_) && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return accepted_;
    }

private:
    static const wchar_t* class_name() {
        return L"AxiomArchiveUpdatePlanDialog";
    }

    bool register_class() const {
        WNDCLASSEXW existing{};
        existing.cbSize = sizeof(existing);
        if (GetClassInfoExW(instance_, class_name(), &existing)) return true;
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.hInstance = instance_;
        window_class.lpfnWndProc = &ArchiveUpdatePlanDialog::window_proc;
        window_class.lpszClassName = class_name();
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        assign_axiom_window_class_icons(window_class, instance_);
        return RegisterClassExW(&window_class) != 0;
    }

    int scale(int value) const {
        return MulDiv(value, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
    }

    HWND make_button(const wchar_t* text, int id) const {
        HWND control = CreateWindowExW(
            0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance_, nullptr);
        set_dialog_control_font(control, font_);
        apply_dialog_control_theme(control, theme_.dark);
        return control;
    }

    HWND make_static(const wchar_t* text, int id = 0) const {
        HWND control = CreateWindowExW(
            0, L"STATIC", text,
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            0, 0, 0, 0, window_,
            id == 0 ? nullptr
                    : reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance_, nullptr);
        set_dialog_control_font(control, font_);
        return control;
    }

    void rebuild_font() {
        if (font_ != nullptr) DeleteObject(font_);
        font_ = create_dialog_font(dpi_);
        EnumChildWindows(window_, [](HWND child, LPARAM font) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(font_));
        table_.set_font(font_);
        table_.set_dpi(dpi_);
    }

    void create_controls() {
        window_brush_ = CreateSolidBrush(theme_.window);
        edit_brush_ = CreateSolidBrush(theme_.edit);
        apply_dialog_dark_frame(window_, theme_.dark);
        apply_axiom_window_icons(window_, instance_);
        rebuild_font();

        const std::wstring description = plan_description(plan_.mode) +
            L" No archive changes have been made.";
        description_ = make_static(description.c_str());
        const std::wstring archive = L"Archive:  " + plan_.archive_path.wstring();
        archive_ = make_static(archive.c_str());

        search_ = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPlanSearch)),
            instance_, nullptr);
        set_dialog_control_font(search_, font_);
        apply_dialog_control_theme(search_, theme_.dark);
        SendMessageW(search_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(scale(7), scale(7)));
        SendMessageW(search_, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(L"Filter paths, sources, or reasons"));

        filter_ = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
            0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPlanFilter)),
            instance_, nullptr);
        set_dialog_control_font(filter_, font_);
        apply_dialog_control_theme(filter_, theme_.dark);
        for (const wchar_t* item : {
                 L"All items", L"Changes only", L"Added", L"Replaced",
                 L"Removed", L"Unchanged", L"Ignored", L"Conflicts"}) {
            SendMessageW(filter_, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(item));
        }
        const int initial_filter = plan_.has_changes() || plan_.conflicts != 0 ? 1 : 0;
        SendMessageW(filter_, CB_SETCURSEL, initial_filter, 0);

        table_.create(window_, instance_, kPlanTable);
        table_.set_font(font_);
        table_.set_dpi(dpi_);
        table_.set_theme(theme_);
        table_.set_options({true, true, true, false});
        table_.set_columns({
            {L"Action", 92},
            {L"Archive path", 310},
            {L"Source", 310},
            {L"Logical size", 145},
            {L"Reason", 300},
        });
        table_.set_sort_indicator(-1, true);
        SetWindowTextW(table_.hwnd(), L"Planned archive changes");

        summary_ = make_static(plan_summary(plan_).c_str());
        std::wstring warning;
        if (!plan_.notice.empty()) warning = plan_.notice;
        if (plan_.conflicts != 0) {
            if (!warning.empty()) warning += L"  ";
            warning += L"Resolve the listed conflicts before this operation can run.";
        } else if (plan_.removed != 0) {
            if (!warning.empty()) warning += L"  ";
            warning += L"Removed entries cannot be recovered from the updated archive.";
        }
        warning_ = make_static(warning.c_str(), kPlanWarning);

        std::wstring effects;
        if (!plan_.additional_effects.empty()) {
            effects = L"Also: ";
            for (std::size_t index = 0; index < plan_.additional_effects.size(); ++index) {
                if (index != 0) effects += L"  |  ";
                effects += plan_.additional_effects[index];
            }
        }
        effects_ = make_static(effects.c_str());

        const std::wstring apply_text = plan_mode_name(plan_.mode);
        accept_ = make_button(apply_text.c_str(), IDOK);
        cancel_ = make_button(plan_.can_apply() ? L"Cancel" : L"Close", IDCANCEL);
        EnableWindow(accept_, plan_.can_apply());
        refresh_rows();
        layout();
        SetFocus(search_);
    }

    bool action_matches_filter(ArchiveUpdatePlanAction action, int filter) const {
        switch (filter) {
            case 1:
                return action == ArchiveUpdatePlanAction::add ||
                       action == ArchiveUpdatePlanAction::replace ||
                       action == ArchiveUpdatePlanAction::remove ||
                       action == ArchiveUpdatePlanAction::conflict;
            case 2: return action == ArchiveUpdatePlanAction::add;
            case 3: return action == ArchiveUpdatePlanAction::replace;
            case 4: return action == ArchiveUpdatePlanAction::remove;
            case 5: return action == ArchiveUpdatePlanAction::unchanged;
            case 6: return action == ArchiveUpdatePlanAction::ignored;
            case 7: return action == ArchiveUpdatePlanAction::conflict;
            default: return true;
        }
    }

    void refresh_rows() {
        wchar_t buffer[32768]{};
        GetWindowTextW(search_, buffer, static_cast<int>(std::size(buffer)));
        const std::wstring needle = folded_text(buffer);
        const int filter = static_cast<int>(SendMessageW(filter_, CB_GETCURSEL, 0, 0));
        std::vector<std::vector<std::wstring>> rows;
        rows.reserve(plan_.items.size());
        for (const auto& item : plan_.items) {
            if (!action_matches_filter(item.action, filter)) continue;
            const std::wstring archive_path = archive_path_text(item.archive_path);
            const std::wstring source = item.source_path.wstring();
            std::wstring searchable = action_name(item.action);
            searchable += L' ';
            searchable += archive_path;
            searchable += L' ';
            searchable += source;
            searchable += L' ';
            searchable += item.reason;
            if (!needle.empty() && !contains_folded(searchable, needle)) continue;
            rows.push_back({
                action_name(item.action), archive_path, source,
                size_change_text(item), item.reason,
            });
        }
        table_.set_rows(std::move(rows), {}, nullptr);
    }

    void layout() {
        if (window_ == nullptr) return;
        RECT client{};
        GetClientRect(window_, &client);
        const int margin = scale(16);
        const int gap = scale(9);
        const int line = scale(22);
        const int edit_height = scale(30);
        const int combo_width = scale(150);
        const int button_width = scale(104);
        const int button_height = scale(32);
        const int footer_y = client.bottom - margin - button_height;
        const int effects_height = plan_.additional_effects.empty() ? 0 : line;
        const bool show_warning = GetWindowTextLengthW(warning_) != 0;
        const int warning_height = show_warning ? line : 0;
        const int summary_y = footer_y - gap - line;
        const int effects_y = summary_y - effects_height;
        const int warning_y = effects_y - warning_height;
        const int table_bottom = warning_y - gap;
        const int search_y = margin + line * 2 + gap;
        const int table_y = search_y + edit_height + gap;
        const int content_width = std::max(
            1, static_cast<int>(client.right) - margin * 2);
        const int search_width = std::max(scale(180), content_width - combo_width - gap);
        search_frame_ = {margin, search_y, margin + search_width, search_y + edit_height};

        int right = client.right - margin;
        const int cancel_x = right - button_width;
        right = cancel_x - gap;
        const int accept_x = right - button_width;
        HDWP positions = BeginDeferWindowPos(10);
        const auto place = [&positions](HWND control, int x, int y,
                                        int width, int height, bool show = true) {
            if (positions == nullptr) return;
            positions = DeferWindowPos(
                positions, control, nullptr, x, y, width, height,
                SWP_NOACTIVATE | SWP_NOZORDER |
                    (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        };
        place(description_, margin, margin, content_width, line);
        place(archive_, margin, margin + line, content_width, line);
        place(search_, margin + scale(1), search_y + scale(1),
              search_width - scale(2), edit_height - scale(2));
        place(filter_, margin + search_width + gap, search_y,
              combo_width, scale(240));
        place(table_.hwnd(), margin, table_y, content_width,
              std::max(scale(160), table_bottom - table_y));
        place(warning_, margin, warning_y, content_width, warning_height, show_warning);
        place(effects_, margin, effects_y, content_width, effects_height,
              effects_height != 0);
        place(summary_, margin, summary_y, content_width, line);
        place(accept_, accept_x, footer_y, button_width, button_height);
        place(cancel_, cancel_x, footer_y, button_width, button_height);
        if (positions != nullptr) EndDeferWindowPos(positions);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void paint() const {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window_, &paint);
        RECT client{};
        GetClientRect(window_, &client);
        FillRect(dc, &client, window_brush_);
        HBRUSH border = CreateSolidBrush(
            GetFocus() == search_ ? theme_.focus : theme_.border);
        FrameRect(dc, &search_frame_, border);
        DeleteObject(border);
        EndPaint(window_, &paint);
    }

    void close(bool accepted) {
        accepted_ = accepted;
        save_named_window_placement(L"ArchiveUpdatePreview", window_);
        HWND owner = owner_;
        const bool owner_was_enabled = owner_was_enabled_;
        owner_was_enabled_ = false;
        if (window_ != nullptr && IsWindow(window_)) {
            destroy_modal_dialog(window_);
        }
        restore_dialog_owner(owner, owner_was_enabled);
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_CREATE:
                create_controls();
                return 0;
            case WM_SIZE:
                layout();
                return 0;
            case WM_GETMINMAXINFO: {
                auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
                const DWORD style = static_cast<DWORD>(
                    GetWindowLongPtrW(window_, GWL_STYLE));
                const DWORD ex_style = static_cast<DWORD>(
                    GetWindowLongPtrW(window_, GWL_EXSTYLE));
                const SIZE minimum = dialog_window_size_for_client(
                    820, 500, style, ex_style, dpi_);
                limits->ptMinTrackSize = {minimum.cx, minimum.cy};
                return 0;
            }
            case WM_DPICHANGED: {
                dpi_ = HIWORD(wparam);
                const auto* suggested = reinterpret_cast<const RECT*>(lparam);
                SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                rebuild_font();
                apply_axiom_window_icons(window_, instance_);
                layout();
                return 0;
            }
            case WM_PAINT:
                paint();
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_CTLCOLOREDIT:
                SetBkColor(reinterpret_cast<HDC>(wparam), theme_.edit);
                SetTextColor(reinterpret_cast<HDC>(wparam), theme_.text);
                return reinterpret_cast<LRESULT>(edit_brush_);
            case WM_CTLCOLORSTATIC: {
                const HWND control = reinterpret_cast<HWND>(lparam);
                SetBkColor(reinterpret_cast<HDC>(wparam), theme_.window);
                COLORREF color = theme_.text;
                if (control == warning_) {
                    color = plan_.conflicts != 0
                        ? (theme_.dark ? RGB(255, 130, 130) : RGB(170, 35, 35))
                        : (theme_.dark ? RGB(255, 196, 105) : RGB(145, 80, 0));
                } else if (control == archive_ || control == effects_) {
                    color = theme_.muted_text;
                }
                SetTextColor(reinterpret_cast<HDC>(wparam), color);
                return reinterpret_cast<LRESULT>(window_brush_);
            }
            case WM_DRAWITEM: {
                const auto& draw = *reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
                if (draw.CtlID == kPlanFilter) {
                    draw_dialog_combo_item(draw, theme_.dark);
                } else {
                    draw_dialog_button(draw, theme_.dark);
                }
                return TRUE;
            }
            case WM_COMMAND: {
                const int id = LOWORD(wparam);
                const int notification = HIWORD(wparam);
                if (id == kPlanSearch && notification == EN_CHANGE) {
                    refresh_rows();
                    return 0;
                }
                if (id == kPlanSearch &&
                    (notification == EN_SETFOCUS || notification == EN_KILLFOCUS)) {
                    InvalidateRect(window_, &search_frame_, FALSE);
                    return 0;
                }
                if (id == kPlanFilter && notification == CBN_SELCHANGE) {
                    refresh_rows();
                    return 0;
                }
                if (id == IDOK && plan_.can_apply()) {
                    close(true);
                    return 0;
                }
                if (id == IDCANCEL) {
                    close(false);
                    return 0;
                }
                break;
            }
            case kTableActivateMessage:
            case kTableSelectionChangedMessage:
            case kTableSortMessage:
            case kTableParentMessage:
                return 0;
            case WM_CLOSE:
                close(false);
                return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
        ArchiveUpdatePlanDialog* self = nullptr;
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<ArchiveUpdatePlanDialog*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<ArchiveUpdatePlanDialog*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        return self != nullptr
            ? self->handle(message, wparam, lparam)
            : DefWindowProcW(window, message, wparam, lparam);
    }

    const ArchiveUpdatePlan& plan_;
    ThemePalette theme_;
    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    HINSTANCE instance_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    bool owner_was_enabled_ = false;
    bool accepted_ = false;
    HBRUSH window_brush_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    HFONT font_ = nullptr;
    RECT search_frame_{};
    HWND description_ = nullptr;
    HWND archive_ = nullptr;
    HWND search_ = nullptr;
    HWND filter_ = nullptr;
    DarkTableView table_;
    HWND warning_ = nullptr;
    HWND effects_ = nullptr;
    HWND summary_ = nullptr;
    HWND accept_ = nullptr;
    HWND cancel_ = nullptr;
};

}  // namespace

bool show_archive_update_plan_dialog(HWND owner,
                                     const ArchiveUpdatePlan& plan,
                                     const ThemePalette& theme) {
    ArchiveUpdatePlanDialog dialog(plan, theme);
    return dialog.show(owner);
}

}  // namespace axiom::gui
