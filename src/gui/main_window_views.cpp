// The owner-drawn dark-themed controls hosted by the main window:
// DarkTableView (details list) and DarkDirectoryTreeView (folders pane).

#include "gui/main_window_internal.hpp"

namespace axiom::gui {
namespace {

std::wstring directory_tree_node_key(const DirectoryTreeNode& node) {
    switch (node.kind) {
        case DirectoryTreeNodeKind::computer:
            return L"computer";
        case DirectoryTreeNodeKind::filesystem:
            return L"fs:" + node.filesystem_path.wstring();
        case DirectoryTreeNodeKind::file:
            return L"file:" + node.filesystem_path.wstring();
        case DirectoryTreeNodeKind::archive:
            return L"archive:" + node.archive_path.wstring();
        case DirectoryTreeNodeKind::archive_directory:
            return L"archive-dir:" + node.archive_path.wstring() + L"|" +
                   std::wstring(node.archive_directory.begin(),
                                node.archive_directory.end());
        case DirectoryTreeNodeKind::dummy:
        default:
            return {};
    }
}

DirectoryTreeItem* find_tree_item_by_key(DirectoryTreeItem& item,
                                         const std::wstring& key) {
    if (!key.empty() && directory_tree_node_key(item.node) == key) return &item;
    for (auto& child : item.children) {
        if (DirectoryTreeItem* found = find_tree_item_by_key(*child, key)) {
            return found;
        }
    }
    return nullptr;
}

}  // namespace

bool DarkTableView::create(HWND parent, HINSTANCE instance, int id) {
    if (!register_class(instance)) {
        return false;
    }

    hwnd_ = CreateWindowExW(0, class_name(), L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                            0, 0, 0, 0,
                            parent,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                            instance,
                            this);
    return hwnd_ != nullptr;
}

HWND DarkTableView::hwnd() const {
    return hwnd_;
}

void DarkTableView::set_font(HFONT font) {
    font_ = font;
    invalidate();
}

void DarkTableView::set_dpi(UINT dpi) {
    dpi_ = dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
    clamp_scroll();
    invalidate();
}

void DarkTableView::set_theme(const ThemePalette& theme) {
    theme_ = theme;
    invalidate();
}

void DarkTableView::set_columns(std::vector<TableColumn> columns) {
    columns_ = std::move(columns);
    clamp_scroll();
    invalidate();
}

std::vector<int> DarkTableView::logical_column_widths() const {
    std::vector<int> widths;
    widths.reserve(columns_.size());
    for (const auto& column : columns_) {
        widths.push_back(column.logical_width);
    }
    return widths;
}

void DarkTableView::set_image_list(HIMAGELIST image_list) {
    image_list_ = image_list;
    invalidate();
}

void DarkTableView::set_sort_indicator(int column, bool ascending) {
    sort_column_ = column;
    sort_ascending_ = ascending;
    invalidate();
}

void DarkTableView::set_options(TableViewOptions options) {
    options_ = options;
    if (!options_.show_horizontal_scrollbar) {
        scroll_x_ = 0;
    }
    clamp_scroll();
    invalidate();
}

void DarkTableView::clear() {
    rows_.clear();
    icon_indices_.clear();
    selected_.clear();
    selected_row_ = -1;
    selection_anchor_ = -1;
    scroll_y_ = 0;
    scroll_x_ = 0;
    invalidate();
}

void DarkTableView::append_row(std::vector<std::wstring> row, int icon_index) {
    rows_.push_back(std::move(row));
    icon_indices_.push_back(icon_index);
    selected_.push_back(false);
    clamp_scroll();
    invalidate();
}

void DarkTableView::reserve_rows(std::size_t count) {
    rows_.reserve(count);
    icon_indices_.reserve(count);
    selected_.reserve(count);
}

void DarkTableView::append_rows(std::vector<std::vector<std::wstring>> rows,
                 std::vector<int> icon_indices,
                 HIMAGELIST image_list) {
    if (image_list_ == nullptr && image_list != nullptr) {
        image_list_ = image_list;
    }
    const std::size_t old_size = rows_.size();
    rows_.reserve(old_size + rows.size());
    icon_indices_.reserve(old_size + icon_indices.size());
    selected_.reserve(old_size + rows.size());
    for (auto& row : rows) {
        rows_.push_back(std::move(row));
    }
    for (int icon_index : icon_indices) {
        icon_indices_.push_back(icon_index);
    }
    if (icon_indices_.size() < rows_.size()) {
        icon_indices_.resize(rows_.size(), -1);
    }
    selected_.resize(rows_.size(), false);
    clamp_scroll();
    invalidate();
}

void DarkTableView::set_rows(std::vector<std::vector<std::wstring>> rows,
              std::vector<int> icon_indices,
              HIMAGELIST image_list) {
    rows_ = std::move(rows);
    icon_indices_ = std::move(icon_indices);
    if (icon_indices_.size() < rows_.size()) {
        icon_indices_.resize(rows_.size(), -1);
    } else if (icon_indices_.size() > rows_.size()) {
        icon_indices_.resize(rows_.size());
    }
    image_list_ = image_list;
    selected_.assign(rows_.size(), false);
    selected_row_ = -1;
    selection_anchor_ = -1;
    if (GetCapture() == hwnd_) ReleaseCapture();
    resizing_column_ = -1;
    dragging_scrollbar_ = false;
    dragging_horizontal_scrollbar_ = false;
    drag_candidate_ = false;
    drag_started_ = false;
    collapse_selection_on_click_ = -1;
    marquee_selecting_ = false;
    marquee_base_selection_.clear();
    typeahead_.clear();
    scroll_y_ = 0;
    scroll_x_ = 0;
    clamp_scroll();
    invalidate();
}

std::vector<int> DarkTableView::selected_indices() const {
    std::vector<int> result;
    for (int index = 0; index < static_cast<int>(selected_.size()); ++index) {
        if (selected_[index]) result.push_back(index);
    }
    return result;
}

int DarkTableView::focused_index() const {
    return selected_row_;
}

int DarkTableView::vertical_scroll_position() const {
    return scroll_y_;
}

int DarkTableView::horizontal_scroll_position() const {
    return scroll_x_;
}

std::optional<RECT> DarkTableView::cell_rect(int row, int column) const {
    if (hwnd_ == nullptr || row < 0 || row >= static_cast<int>(rows_.size()) ||
        column < 0 || column >= static_cast<int>(columns_.size())) {
        return std::nullopt;
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int content_left = table_left(client);
    const int content_right = table_right(client);
    const int rows_top = client.top + header_height();
    const int rows_limit = rows_bottom(client);
    const int row_h = row_height();
    RECT row_rect{
        content_left,
        rows_top + row * row_h - scroll_y_,
        content_right,
        rows_top + (row + 1) * row_h - scroll_y_,
    };
    RECT rows_area{content_left, rows_top, content_right, rows_limit};
    RECT visible_row{};
    if (!IntersectRect(&visible_row, &row_rect, &rows_area)) {
        return std::nullopt;
    }
    const auto widths = column_widths(client);
    int x = content_left - scroll_x_;
    for (int index = 0; index < column; ++index) {
        x += widths[static_cast<std::size_t>(index)];
    }
    RECT cell{
        x + scale(1),
        row_rect.top + scale(1),
        x + widths[static_cast<std::size_t>(column)] - scale(1),
        row_rect.bottom - scale(1),
    };
    RECT visible_cell{};
    if (!IntersectRect(&visible_cell, &cell, &rows_area)) {
        return std::nullopt;
    }
    MapWindowPoints(hwnd_, GetParent(hwnd_),
                    reinterpret_cast<POINT*>(&visible_cell), 2);
    return visible_cell;
}

void DarkTableView::set_selection_and_scroll(std::vector<int> selected_indices,
                                  int focused_index,
                              int horizontal_scroll,
                              int vertical_scroll) {
    selected_.assign(rows_.size(), false);
    int first_selected = -1;
    for (int index : selected_indices) {
        if (index < 0 || index >= static_cast<int>(rows_.size())) continue;
        selected_[static_cast<std::size_t>(index)] = true;
        if (first_selected < 0) first_selected = index;
    }
    if (focused_index < 0 || focused_index >= static_cast<int>(rows_.size()) ||
        (focused_index >= 0 &&
         focused_index < static_cast<int>(selected_.size()) &&
         !selected_[static_cast<std::size_t>(focused_index)])) {
        focused_index = first_selected;
    }
    selected_row_ = focused_index;
    selection_anchor_ = focused_index;
    scroll_x_ = horizontal_scroll;
    scroll_y_ = vertical_scroll;
    clamp_scroll();
    invalidate();
}

void DarkTableView::select_index(int row) {
    select_row(row, false, false);
}

int DarkTableView::row_at_screen_point(POINT point) const {
    if (hwnd_ == nullptr) return -1;
    ScreenToClient(hwnd_, &point);
    RECT client{};
    GetClientRect(hwnd_, &client);
    if (point.x < table_left(client) || point.x >= table_right(client) ||
        point.y < header_height() || point.y >= rows_bottom(client)) {
        return -1;
    }
    const int y = point.y - header_height() + scroll_y_;
    const int row = row_height() > 0 ? y / row_height() : -1;
    return row >= 0 && row < static_cast<int>(rows_.size()) ? row : -1;
}

void DarkTableView::select_all() {
    std::fill(selected_.begin(), selected_.end(), true);
    selected_row_ = selected_.empty() ? -1 : 0;
    selection_anchor_ = selected_row_;
    notify_parent(kTableSelectionChangedMessage);
    invalidate();
}

const wchar_t* DarkTableView::class_name() {
    return L"AxiomDarkTableView";
}

bool DarkTableView::register_class(HINSTANCE instance) {
    static ATOM atom = 0;
    if (atom != 0) {
        return true;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.hInstance = instance;
    wc.lpfnWndProc = &DarkTableView::window_proc;
    wc.lpszClassName = class_name();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    atom = RegisterClassExW(&wc);
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

int DarkTableView::scale(int value) const {
    return scale_for_dpi(dpi_, value);
}

int DarkTableView::header_height() const {
    return scale(28);
}

int DarkTableView::row_height() const {
    return scale(24);
}

int DarkTableView::scrollbar_width() const {
    return scale(8);
}

int DarkTableView::scrollbar_gap() const {
    return scale(2);
}

int DarkTableView::content_height() const {
    return row_height() * static_cast<int>(rows_.size());
}

int DarkTableView::requested_content_width() const {
    int width = 0;
    for (const auto& column : columns_) {
        width += scale(column.logical_width);
    }
    return width;
}

DarkTableView::ScrollbarVisibility DarkTableView::scrollbar_visibility(const RECT& client) const {
    ScrollbarVisibility visibility;
    const int base_width = std::max(
        0, static_cast<int>(client.right - client.left) - scale(2));
    const int base_height = std::max(
        0, static_cast<int>(client.bottom - client.top) - header_height() - scale(1));

    // Each scrollbar consumes space that can make the other one necessary.
    for (int pass = 0; pass < 3; ++pass) {
        const int width = std::max(
            0, base_width - (visibility.vertical
                                  ? scrollbar_width() + scrollbar_gap()
                                  : 0));
        if (options_.show_horizontal_scrollbar && requested_content_width() > width) {
            visibility.horizontal = true;
        }

        const int height = std::max(
            0, base_height - (visibility.horizontal
                                   ? scrollbar_width() + scrollbar_gap()
                                   : 0));
        if (content_height() > height) {
            visibility.vertical = true;
        }
    }
    return visibility;
}

int DarkTableView::rows_bottom(const RECT& client) const {
    const auto visibility = scrollbar_visibility(client);
    int bottom = client.bottom - scale(1);
    if (visibility.horizontal) {
        bottom -= scrollbar_width() + scrollbar_gap();
    }
    return std::max(static_cast<int>(client.top) + header_height(), bottom);
}

int DarkTableView::viewport_height(const RECT& client) const {
    return std::max(0, rows_bottom(client) -
                           (static_cast<int>(client.top) + header_height()));
}

int DarkTableView::viewport_height() const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    return viewport_height(client);
}

int DarkTableView::max_scroll() const {
    return std::max(0, content_height() - viewport_height());
}

void DarkTableView::clamp_scroll() {
    scroll_y_ = std::clamp(scroll_y_, 0, max_scroll());
    scroll_x_ = std::clamp(scroll_x_, 0, max_scroll_x());
}

void DarkTableView::invalidate() const {
    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

std::vector<int> DarkTableView::column_widths(int available_width) const {
    std::vector<int> widths;
    widths.reserve(columns_.size());
    int used = 0;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const int requested = scale(columns_[i].logical_width);
        if (i + 1 == columns_.size()) {
            widths.push_back(std::max(requested, available_width - used));
        } else {
            widths.push_back(requested);
            used += requested;
        }
    }
    return widths;
}

int DarkTableView::table_left(const RECT& client) const {
    return client.left + scale(1);
}

int DarkTableView::table_right(const RECT& client) const {
    int right = client.right - scale(1);
    if (scrollbar_visibility(client).vertical) {
        right -= scrollbar_width() + scrollbar_gap();
    }
    return std::max(table_left(client), right);
}

std::vector<int> DarkTableView::column_widths(const RECT& client) const {
    return column_widths(std::max(0, table_right(client) - table_left(client)));
}

int DarkTableView::max_scroll_x(const RECT& client) const {
    if (!options_.show_horizontal_scrollbar) {
        return 0;
    }
    const auto widths = column_widths(client);
    int content_width = 0;
    for (const int width : widths) {
        content_width += width;
    }
    return std::max(0, content_width - (table_right(client) - table_left(client)));
}

int DarkTableView::max_scroll_x() const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    return max_scroll_x(client);
}

int DarkTableView::column_separator_at(POINT point, const RECT& client) const {
    if (point.y < client.top + scale(1) ||
        point.y >= client.top + header_height()) {
        return -1;
    }

    const int right = table_right(client);
    const auto widths = column_widths(client);
    int x = table_left(client) - scroll_x_;
    for (int column = 0; column < static_cast<int>(widths.size()); ++column) {
        const int next_x = x + widths[static_cast<std::size_t>(column)];
        if (next_x >= table_left(client) && next_x < right &&
            std::abs(point.x - next_x) <= scale(5)) {
            return column;
        }
        x = next_x;
        if (x >= right) {
            break;
        }
    }
    return -1;
}

RECT DarkTableView::scrollbar_track_rect(const RECT& client) const {
    return RECT{client.right - scrollbar_width() - scale(1),
                client.top + header_height() + scale(1),
                client.right - scale(1),
                rows_bottom(client)};
}

RECT DarkTableView::scrollbar_thumb_rect(const RECT& client) const {
    RECT track = scrollbar_track_rect(client);
    const int track_height = std::max(0, static_cast<int>(track.bottom - track.top));
    const int content = content_height();
    const int viewport = viewport_height();
    if (content <= viewport || track_height <= 0) {
        return RECT{track.left, track.top, track.right, track.top};
    }

    const int thumb_height = std::clamp(
        MulDiv(viewport, track_height, content), scale(24), track_height);
    const int travel = std::max(0, track_height - thumb_height);
    const int top = track.top + (max_scroll() == 0 ? 0 : MulDiv(scroll_y_, travel, max_scroll()));
    return RECT{track.left, top, track.right, top + thumb_height};
}

RECT DarkTableView::horizontal_scrollbar_track_rect(const RECT& client) const {
    return RECT{table_left(client),
                rows_bottom(client) + scrollbar_gap(),
                table_right(client),
                client.bottom - scale(1)};
}

RECT DarkTableView::horizontal_scrollbar_thumb_rect(const RECT& client) const {
    const RECT track = horizontal_scrollbar_track_rect(client);
    const int track_width = std::max(0, static_cast<int>(track.right - track.left));
    const int viewport = std::max(0, table_right(client) - table_left(client));
    const int maximum = max_scroll_x(client);
    const int content = viewport + maximum;
    if (maximum <= 0 || track_width <= 0 || content <= 0) {
        return RECT{track.left, track.top, track.left, track.bottom};
    }

    const int thumb_width = std::clamp(
        MulDiv(viewport, track_width, content), scale(24), track_width);
    const int travel = std::max(0, track_width - thumb_width);
    const int left = track.left + MulDiv(scroll_x_, travel, maximum);
    return RECT{left, track.top, left + thumb_width, track.bottom};
}

bool DarkTableView::point_in_rect(POINT point, const RECT& rect) const {
    return point.x >= rect.left && point.x < rect.right &&
           point.y >= rect.top && point.y < rect.bottom;
}

void DarkTableView::set_scroll_from_thumb(POINT point, const RECT& client) {
    RECT track = scrollbar_track_rect(client);
    RECT thumb = scrollbar_thumb_rect(client);
    const int thumb_height = thumb.bottom - thumb.top;
    const int travel = std::max(1, static_cast<int>(track.bottom - track.top) - thumb_height);
    const int thumb_top = std::clamp(static_cast<int>(point.y) - drag_offset_y_,
                                     static_cast<int>(track.top),
                                     static_cast<int>(track.bottom) - thumb_height);
    scroll_y_ = MulDiv(thumb_top - track.top, max_scroll(), travel);
    clamp_scroll();
    invalidate();
}

void DarkTableView::set_horizontal_scroll_from_thumb(POINT point, const RECT& client) {
    const RECT track = horizontal_scrollbar_track_rect(client);
    const RECT thumb = horizontal_scrollbar_thumb_rect(client);
    const int thumb_width = thumb.right - thumb.left;
    const int travel = std::max(1, static_cast<int>(track.right - track.left) - thumb_width);
    const int thumb_left = std::clamp(static_cast<int>(point.x) - drag_offset_x_,
                                      static_cast<int>(track.left),
                                      static_cast<int>(track.right) - thumb_width);
    scroll_x_ = MulDiv(thumb_left - track.left, max_scroll_x(client), travel);
    clamp_scroll();
    invalidate();
}

void DarkTableView::scroll_by(int pixels) {
    scroll_y_ += pixels;
    clamp_scroll();
    invalidate();
}

void DarkTableView::scroll_horizontally_by(int pixels) {
    scroll_x_ += pixels;
    clamp_scroll();
    invalidate();
}

void DarkTableView::notify_parent(UINT message) const {
    SendMessageW(GetParent(hwnd_), message,
                 static_cast<WPARAM>(std::max(selected_row_, 0)), 0);
}

void DarkTableView::select_row(int row, bool extend, bool toggle) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        if (!extend && !toggle) std::fill(selected_.begin(), selected_.end(), false);
        selected_row_ = -1;
        selection_anchor_ = -1;
    } else if (extend && selection_anchor_ >= 0) {
        if (!toggle) std::fill(selected_.begin(), selected_.end(), false);
        const int first = std::min(selection_anchor_, row);
        const int last = std::max(selection_anchor_, row);
        for (int index = first; index <= last; ++index) selected_[index] = true;
        selected_row_ = row;
    } else if (toggle) {
        selected_[row] = !selected_[row];
        selected_row_ = row;
        selection_anchor_ = row;
    } else {
        std::fill(selected_.begin(), selected_.end(), false);
        selected_[row] = true;
        selected_row_ = row;
        selection_anchor_ = row;
    }
    ensure_selected_visible();
    notify_parent(kTableSelectionChangedMessage);
    invalidate();
}

std::wstring DarkTableView::row_match_text(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return {};
    const auto& row_values = rows_[static_cast<std::size_t>(row)];
    return row_values.empty() ? std::wstring{} : row_values.front();
}

int DarkTableView::find_typeahead_match(std::wstring_view needle, bool cycle) const {
    if (needle.empty() || rows_.empty()) return -1;
    const int count = static_cast<int>(rows_.size());
    const int start = cycle && selected_row_ >= 0 ? selected_row_ + 1 : 0;
    auto scan = [&](auto predicate) {
        for (int pass = 0; pass < 2; ++pass) {
            const int first = pass == 0 ? start : 0;
            const int last = pass == 0 ? count : std::min(start, count);
            for (int row = first; row < last; ++row) {
                if (predicate(row_match_text(row))) return row;
            }
        }
        return -1;
    };
    if (const int row = scan([&](std::wstring_view text) {
            return starts_with_folded(text, needle);
        }); row >= 0) {
        return row;
    }
    if (const int row = scan([&](std::wstring_view text) {
            return contains_folded(text, needle);
        }); row >= 0) {
        return row;
    }

    const std::wstring folded_needle = folded_text(needle);
    int best = -1;
    std::wstring best_text;
    for (int row = 0; row < count; ++row) {
        const std::wstring text = folded_text(row_match_text(row));
        if (text >= folded_needle && (best < 0 || text < best_text)) {
            best = row;
            best_text = text;
        }
    }
    return best;
}

bool DarkTableView::handle_typeahead_char(wchar_t character) {
    if (character == L'\r' || character == L'\n' || character == L'\t') return false;
    const ULONGLONG now = GetTickCount64();
    if (now - typeahead_last_tick_ > 1200) {
        typeahead_.clear();
    }
    typeahead_last_tick_ = now;

    if (character == L'\b') {
        if (!typeahead_.empty()) typeahead_.pop_back();
        if (typeahead_.empty()) return true;
        if (const int row = find_typeahead_match(typeahead_, false); row >= 0) {
            select_row(row, false, false);
        }
        return true;
    }
    if (character < L' ') return false;

    const bool repeated_single =
        typeahead_.size() == 1 &&
        std::towlower(typeahead_.front()) == std::towlower(character);
    if (repeated_single) {
        typeahead_.assign(1, character);
    } else {
        typeahead_.push_back(character);
    }

    if (const int row = find_typeahead_match(typeahead_, repeated_single); row >= 0) {
        select_row(row, false, false);
    }
    return true;
}

bool DarkTableView::point_can_select_row(POINT point, int row, const RECT& client) const {
    if (options_.full_row_select) return true;
    if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
    const auto widths = column_widths(client);
    if (widths.empty()) return false;
    const int first_left = table_left(client) - scroll_x_;
    const int first_right = first_left + widths.front();
    return point.x >= first_left && point.x < first_right;
}

void DarkTableView::begin_marquee_selection(POINT point, bool preserve_selection) {
    marquee_selecting_ = true;
    marquee_start_ = point;
    marquee_current_ = point;
    marquee_base_selection_ = selected_;
    if (!preserve_selection) {
        std::fill(marquee_base_selection_.begin(), marquee_base_selection_.end(), false);
        std::fill(selected_.begin(), selected_.end(), false);
    }
    selected_row_ = -1;
    selection_anchor_ = -1;
    SetCapture(hwnd_);
    notify_parent(kTableSelectionChangedMessage);
    invalidate();
}

void DarkTableView::update_marquee_selection(POINT point) {
    marquee_current_ = point;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const RECT rows_area{table_left(client), header_height(),
                         table_right(client), rows_bottom(client)};
    RECT marquee{
        std::min(marquee_start_.x, marquee_current_.x),
        std::min(marquee_start_.y, marquee_current_.y),
        std::max(marquee_start_.x, marquee_current_.x) + 1,
        std::max(marquee_start_.y, marquee_current_.y) + 1,
    };
    RECT clipped{};
    const bool visible = IntersectRect(&clipped, &marquee, &rows_area) != FALSE;
    std::vector<bool> next = marquee_base_selection_;
    int first_selected = -1;
    int last_selected = -1;
    if (visible) {
        for (int row = 0; row < static_cast<int>(rows_.size()); ++row) {
            const int top = header_height() + row * row_height() - scroll_y_;
            const RECT row_rect{rows_area.left, top, rows_area.right, top + row_height()};
            RECT overlap{};
            if (IntersectRect(&overlap, &row_rect, &clipped)) {
                next[static_cast<std::size_t>(row)] =
                    !marquee_base_selection_[static_cast<std::size_t>(row)];
                if (next[static_cast<std::size_t>(row)]) {
                    if (first_selected < 0) first_selected = row;
                    last_selected = row;
                }
            }
        }
    }
    if (next != selected_) {
        selected_ = std::move(next);
        selected_row_ = last_selected;
        selection_anchor_ = first_selected;
        notify_parent(kTableSelectionChangedMessage);
    }
    invalidate();
}

void DarkTableView::end_marquee_selection() {
    if (!marquee_selecting_) return;
    marquee_selecting_ = false;
    marquee_base_selection_.clear();
    if (GetCapture() == hwnd_) ReleaseCapture();
    notify_parent(kTableSelectionChangedMessage);
    invalidate();
}

void DarkTableView::ensure_selected_visible() {
    if (selected_row_ < 0) {
        return;
    }
    const int row_top = selected_row_ * row_height();
    const int row_bottom = row_top + row_height();
    if (row_top < scroll_y_) {
        scroll_y_ = row_top;
    } else if (row_bottom > scroll_y_ + viewport_height()) {
        scroll_y_ = row_bottom - viewport_height();
    }
    clamp_scroll();
}

void DarkTableView::paint_content(HDC dc, const RECT& client) {
    fill_solid_rect(dc, client, theme_.edit);
    frame_solid_rect(dc, client, theme_.border);

    HFONT font = font_ != nullptr ? font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);

    const auto visibility = scrollbar_visibility(client);
    const int content_left = table_left(client);
    const int content_right = table_right(client);
    RECT header{content_left, client.top + scale(1),
                client.right - scale(1), client.top + header_height()};
    fill_solid_rect(dc, header, theme_.panel);

    const auto widths = column_widths(client);
    const int saved_header_dc = SaveDC(dc);
    IntersectClipRect(dc, content_left, header.top, content_right, header.bottom);
    int x = content_left - scroll_x_;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const int next_x = x + widths[i];
        if (next_x > content_left && x < content_right) {
            RECT cell{x + scale(7), header.top, next_x - scale(7), header.bottom};
            SetTextColor(dc, theme_.text);
            std::wstring title = columns_[i].title;
            if (static_cast<int>(i) == sort_column_) {
                title += sort_ascending_ ? L"  ^" : L"  v";
            }
            DrawTextW(dc, title.c_str(), -1, &cell,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (options_.show_grid_lines) {
                draw_solid_line(dc, next_x, header.top, next_x, header.bottom, theme_.border);
            }
        }
        x = next_x;
        if (x >= content_right) {
            break;
        }
    }
    RestoreDC(dc, saved_header_dc);
    draw_solid_line(dc, content_left, header.bottom - scale(1),
                    content_right, header.bottom - scale(1), theme_.border);

    RECT rows_clip{content_left, header.bottom, content_right, rows_bottom(client)};
    const int saved_rows_dc = SaveDC(dc);
    IntersectClipRect(dc, rows_clip.left, rows_clip.top, rows_clip.right, rows_clip.bottom);

    const int row_h = row_height();
    const int first_row = row_h > 0 ? scroll_y_ / row_h : 0;
    int y = rows_clip.top - (row_h > 0 ? scroll_y_ % row_h : 0);
    for (int row = first_row; row < static_cast<int>(rows_.size()) && y < rows_clip.bottom; ++row) {
        RECT row_rect{rows_clip.left, y, rows_clip.right, y + row_h};
        const bool selected = row < static_cast<int>(selected_.size()) && selected_[row];
        fill_solid_rect(dc, row_rect, selected ? theme_.selection : theme_.edit);

        x = content_left - scroll_x_;
        for (std::size_t col = 0; col < columns_.size(); ++col) {
            const int next_x = x + widths[col];
            if (next_x > rows_clip.left && x < rows_clip.right) {
                RECT cell{x + scale(7), row_rect.top, next_x - scale(7), row_rect.bottom};
                if (col == 0 && image_list_ != nullptr &&
                    row < static_cast<int>(icon_indices_.size()) && icon_indices_[row] >= 0) {
                    int icon_width = 0;
                    int icon_height = 0;
                    ImageList_GetIconSize(image_list_, &icon_width, &icon_height);
                    const int icon_y = row_rect.top + std::max(0, (row_h - icon_height) / 2);
                    ImageList_Draw(image_list_, icon_indices_[row], dc, cell.left, icon_y,
                                   ILD_TRANSPARENT);
                    cell.left += icon_width + scale(5);
                }
                const std::wstring empty;
                const std::wstring& text =
                    col < rows_[row].size() ? rows_[row][col] : empty;
                SetTextColor(dc, selected ? theme_.selection_text : theme_.text);
                DrawTextW(dc, text.c_str(), -1, &cell,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                              DT_NOPREFIX);
            }
            x = next_x;
            if (x >= rows_clip.right) {
                break;
            }
        }
        if (options_.show_grid_lines) {
            draw_solid_line(dc, rows_clip.left, row_rect.bottom - scale(1),
                            rows_clip.right, row_rect.bottom - scale(1), theme_.panel);
        }
        y += row_h;
    }

    if (options_.show_grid_lines) {
        // Draw grid lines last so they remain continuous across selection fills and
        // the empty area below the final row.
        x = content_left - scroll_x_;
        for (const int width : widths) {
            const int next_x = x + width;
            if (next_x > rows_clip.left && next_x <= rows_clip.right) {
                draw_solid_line(dc, next_x, rows_clip.top, next_x, rows_clip.bottom,
                                theme_.border);
            }
            x = next_x;
            if (x >= rows_clip.right) {
                break;
            }
        }
    }
    RestoreDC(dc, saved_rows_dc);

    if (marquee_selecting_) {
        RECT marquee{
            std::min(marquee_start_.x, marquee_current_.x),
            std::min(marquee_start_.y, marquee_current_.y),
            std::max(marquee_start_.x, marquee_current_.x) + 1,
            std::max(marquee_start_.y, marquee_current_.y) + 1,
        };
        RECT rows_area{content_left, header.bottom, content_right, rows_bottom(client)};
        RECT clipped{};
        if (IntersectRect(&clipped, &marquee, &rows_area)) {
            frame_solid_rect(dc, clipped, theme_.focus);
        }
    }

    if (visibility.vertical) {
        const RECT track = scrollbar_track_rect(client);
        const RECT thumb = scrollbar_thumb_rect(client);
        fill_solid_rect(dc, track, theme_.scrollbar_track);
        fill_solid_rect(dc, thumb, dragging_scrollbar_
                                       ? theme_.scrollbar_thumb_pressed
                                       : theme_.scrollbar_thumb);
    }
    if (visibility.horizontal) {
        const RECT track = horizontal_scrollbar_track_rect(client);
        const RECT thumb = horizontal_scrollbar_thumb_rect(client);
        fill_solid_rect(dc, track, theme_.scrollbar_track);
        fill_solid_rect(dc, thumb, dragging_horizontal_scrollbar_
                                       ? theme_.scrollbar_thumb_pressed
                                       : theme_.scrollbar_thumb);
    }
    if (visibility.vertical && visibility.horizontal) {
        RECT corner{table_right(client) + scrollbar_gap(),
                    rows_bottom(client) + scrollbar_gap(),
                    client.right - scale(1), client.bottom - scale(1)};
        fill_solid_rect(dc, corner, theme_.scrollbar_track);
    }

    SelectObject(dc, old_font);
}

void DarkTableView::on_paint() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd_, &paint);
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width > 0 && height > 0) {
        RECT buffer{0, 0, width, height};
        if (paint_buffer_.ensure(dc, width, height)) {
            paint_content(paint_buffer_.dc(), buffer);
            BitBlt(dc, 0, 0, width, height, paint_buffer_.dc(), 0, 0, SRCCOPY);
        } else {
            paint_content(dc, buffer);
        }
    }
    EndPaint(hwnd_, &paint);
}

LRESULT DarkTableView::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            on_paint();
            return 0;
        case WM_SETFONT:
            font_ = reinterpret_cast<HFONT>(wparam);
            invalidate();
            return 0;
        case WM_SIZE:
            clamp_scroll();
            invalidate();
            return 0;
        case WM_MOUSEWHEEL:
            if ((GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT) != 0) {
                scroll_horizontally_by(
                    -GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * scale(96));
            } else {
                scroll_by(-GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * row_height() * 3);
            }
            return 0;
        case WM_MOUSEHWHEEL:
            scroll_horizontally_by(
                GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * scale(96));
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd_);
            RECT client{};
            GetClientRect(hwnd_, &client);
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (point.y < client.top + header_height() &&
                point.x >= table_left(client) && point.x < table_right(client)) {
                const int separator = column_separator_at(point, client);
                if (separator >= 0) {
                    resizing_column_ = separator;
                    resize_start_x_ = point.x;
                    resize_start_width_ =
                        columns_[static_cast<std::size_t>(separator)].logical_width;
                    SetCapture(hwnd_);
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return 0;
                }

                const auto widths = column_widths(client);
                int boundary = table_left(client) - scroll_x_;
                for (int column = 0; column < static_cast<int>(widths.size()); ++column) {
                    const int left = boundary;
                    boundary += widths[column];
                    if (point.x >= left && point.x < boundary) {
                        SendMessageW(GetParent(hwnd_), kTableSortMessage,
                                     static_cast<WPARAM>(column), 0);
                        break;
                    }
                }
                return 0;
            }
            const auto visibility = scrollbar_visibility(client);
            if (visibility.vertical) {
                const RECT thumb = scrollbar_thumb_rect(client);
                const RECT track = scrollbar_track_rect(client);
                if (point_in_rect(point, thumb)) {
                    dragging_scrollbar_ = true;
                    drag_offset_y_ = point.y - thumb.top;
                    SetCapture(hwnd_);
                    return 0;
                }
                if (point_in_rect(point, track)) {
                    drag_offset_y_ = (thumb.bottom - thumb.top) / 2;
                    set_scroll_from_thumb(point, client);
                    return 0;
                }
            }
            if (visibility.horizontal) {
                const RECT thumb = horizontal_scrollbar_thumb_rect(client);
                const RECT track = horizontal_scrollbar_track_rect(client);
                if (point_in_rect(point, thumb)) {
                    dragging_horizontal_scrollbar_ = true;
                    drag_offset_x_ = point.x - thumb.left;
                    SetCapture(hwnd_);
                    return 0;
                }
                if (point_in_rect(point, track)) {
                    drag_offset_x_ = (thumb.right - thumb.left) / 2;
                    set_horizontal_scroll_from_thumb(point, client);
                    return 0;
                }
            }
            if (point.x < table_left(client) || point.x >= table_right(client) ||
                point.y >= rows_bottom(client)) {
                return 0;
            }
            const int y = point.y - header_height() + scroll_y_;
            const int row = row_height() > 0 ? y / row_height() : -1;
            int valid_row = row >= 0 && row < static_cast<int>(rows_.size()) ? row : -1;
            if (valid_row >= 0 && !point_can_select_row(point, valid_row, client)) {
                valid_row = -1;
            }
            const bool extend = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool toggle = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool preserve_selection = valid_row >= 0 && !extend && !toggle &&
                selected_[static_cast<std::size_t>(valid_row)];
            if (valid_row < 0) {
                begin_marquee_selection(point, toggle);
                return 0;
            }
            if (!preserve_selection) select_row(valid_row, extend, toggle);
            drag_candidate_ = valid_row >= 0 &&
                selected_[static_cast<std::size_t>(valid_row)];
            drag_started_ = false;
            drag_start_ = point;
            collapse_selection_on_click_ = preserve_selection ? valid_row : -1;
            if (drag_candidate_) SetCapture(hwnd_);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            RECT client{};
            GetClientRect(hwnd_, &client);
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (point.y < header_height() || point.y >= rows_bottom(client) ||
                point.x < table_left(client) || point.x >= table_right(client)) {
                return 0;
            }
            const int y = point.y - header_height() + scroll_y_;
            const int row = row_height() > 0 ? y / row_height() : -1;
            if (row >= 0 && row < static_cast<int>(rows_.size()) &&
                !point_can_select_row(point, row, client)) {
                return 0;
            }
            notify_parent(kTableActivateMessage);
            return 0;
        }
        case WM_RBUTTONDOWN: {
            SetFocus(hwnd_);
            RECT client{};
            GetClientRect(hwnd_, &client);
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (point.y < header_height() || point.y >= rows_bottom(client) ||
                point.x < table_left(client) || point.x >= table_right(client)) {
                return 0;
            }
            const int y = point.y - header_height() + scroll_y_;
            const int row = row_height() > 0 ? y / row_height() : -1;
            if (row >= 0 && row < static_cast<int>(rows_.size()) &&
                point_can_select_row(point, row, client) &&
                !selected_[static_cast<std::size_t>(row)]) {
                select_row(row, false, false);
            }
            break;
        }
        case WM_CONTEXTMENU:
            // Custom child windows do not get the standard list-view
            // context-menu forwarding, so preserve the originating HWND
            // and screen point explicitly for the application shell.
            SendMessageW(GetParent(hwnd_), WM_CONTEXTMENU,
                         reinterpret_cast<WPARAM>(hwnd_), lparam);
            return 0;
        case WM_MOUSEMOVE:
            if (marquee_selecting_ && (wparam & MK_LBUTTON) != 0) {
                update_marquee_selection(
                    POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
                return 0;
            }
            if (drag_candidate_ && !drag_started_ &&
                (wparam & MK_LBUTTON) != 0) {
                const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                const int threshold_x = GetSystemMetrics(SM_CXDRAG);
                const int threshold_y = GetSystemMetrics(SM_CYDRAG);
                if (std::abs(point.x - drag_start_.x) >= threshold_x ||
                    std::abs(point.y - drag_start_.y) >= threshold_y) {
                    drag_started_ = true;
                    collapse_selection_on_click_ = -1;
                    if (GetCapture() == hwnd_) ReleaseCapture();
                    SendMessageW(GetParent(hwnd_), kTableBeginDragMessage, 0, 0);
                    drag_candidate_ = false;
                    return 0;
                }
            }
            if (resizing_column_ >= 0) {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                const int requested = scale(resize_start_width_) +
                                      point.x - resize_start_x_;
                const int width = std::max(requested, scale(48));
                columns_[static_cast<std::size_t>(resizing_column_)].logical_width =
                    std::max(48, MulDiv(width, USER_DEFAULT_SCREEN_DPI,
                                       static_cast<int>(dpi_)));
                clamp_scroll();
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                invalidate();
                return 0;
            }
            if (dragging_scrollbar_) {
                RECT client{};
                GetClientRect(hwnd_, &client);
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                set_scroll_from_thumb(point, client);
                return 0;
            }
            if (dragging_horizontal_scrollbar_) {
                RECT client{};
                GetClientRect(hwnd_, &client);
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                set_horizontal_scroll_from_thumb(point, client);
                return 0;
            }
            break;
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(hwnd_, &point);
                RECT client{};
                GetClientRect(hwnd_, &client);
                if (resizing_column_ >= 0 || column_separator_at(point, client) >= 0) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;
        case WM_LBUTTONUP:
            if (marquee_selecting_) {
                update_marquee_selection(
                    POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
                end_marquee_selection();
                return 0;
            }
            if (drag_candidate_) {
                drag_candidate_ = false;
                if (GetCapture() == hwnd_) ReleaseCapture();
                if (!drag_started_ && collapse_selection_on_click_ >= 0) {
                    select_row(collapse_selection_on_click_, false, false);
                }
                collapse_selection_on_click_ = -1;
                drag_started_ = false;
                return 0;
            }
            if (resizing_column_ >= 0) {
                resizing_column_ = -1;
                ReleaseCapture();
                invalidate();
                return 0;
            }
            if (dragging_scrollbar_) {
                dragging_scrollbar_ = false;
                ReleaseCapture();
                invalidate();
                return 0;
            }
            if (dragging_horizontal_scrollbar_) {
                dragging_horizontal_scrollbar_ = false;
                ReleaseCapture();
                invalidate();
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (!rows_.empty()) {
                if (wparam == VK_DOWN) {
                    select_row(std::min(static_cast<int>(rows_.size()) - 1, selected_row_ + 1),
                               (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                               (GetKeyState(VK_SHIFT) & 0x8000) != 0 &&
                                   (GetKeyState(VK_CONTROL) & 0x8000) != 0);
                    return 0;
                }
                if (wparam == VK_UP) {
                    select_row(std::max(0, selected_row_ < 0 ? 0 : selected_row_ - 1),
                               (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                               (GetKeyState(VK_SHIFT) & 0x8000) != 0 &&
                                   (GetKeyState(VK_CONTROL) & 0x8000) != 0);
                    return 0;
                }
                if (wparam == VK_HOME || wparam == VK_END) {
                    select_row(wparam == VK_HOME ? 0 : static_cast<int>(rows_.size()) - 1,
                               (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                               (GetKeyState(VK_SHIFT) & 0x8000) != 0 &&
                                   (GetKeyState(VK_CONTROL) & 0x8000) != 0);
                    return 0;
                }
                if (wparam == VK_SPACE &&
                    (GetKeyState(VK_CONTROL) & 0x8000) != 0 && selected_row_ >= 0) {
                    select_row(selected_row_, false, true);
                    return 0;
                }
                if (wparam == VK_RETURN) {
                    notify_parent(kTableActivateMessage);
                    return 0;
                }
                if (wparam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                    std::fill(selected_.begin(), selected_.end(), true);
                    selected_row_ = 0;
                    selection_anchor_ = 0;
                    notify_parent(kTableSelectionChangedMessage);
                    invalidate();
                    return 0;
                }
            }
            if (wparam == VK_BACK) {
                notify_parent(kTableParentMessage);
                return 0;
            }
            break;
        case WM_CHAR:
            if ((GetKeyState(VK_CONTROL) & 0x8000) == 0 &&
                (GetKeyState(VK_MENU) & 0x8000) == 0 &&
                handle_typeahead_char(static_cast<wchar_t>(wparam))) {
                return 0;
            }
            break;
        case WM_CANCELMODE:
            if (GetCapture() == hwnd_) {
                ReleaseCapture();
            }
            resizing_column_ = -1;
            dragging_scrollbar_ = false;
            dragging_horizontal_scrollbar_ = false;
            drag_candidate_ = false;
            drag_started_ = false;
            collapse_selection_on_click_ = -1;
            marquee_selecting_ = false;
            marquee_base_selection_.clear();
            invalidate();
            return 0;
        case WM_CAPTURECHANGED:
            resizing_column_ = -1;
            dragging_scrollbar_ = false;
            dragging_horizontal_scrollbar_ = false;
            drag_candidate_ = false;
            drag_started_ = false;
            collapse_selection_on_click_ = -1;
            marquee_selecting_ = false;
            marquee_base_selection_.clear();
            invalidate();
            return 0;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

LRESULT CALLBACK DarkTableView::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    DarkTableView* view = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        view = static_cast<DarkTableView*>(create->lpCreateParams);
        view->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(view));
    } else {
        view = reinterpret_cast<DarkTableView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (view != nullptr) {
        return view->handle_message(message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool DarkDirectoryTreeView::create(HWND parent, HINSTANCE instance, int id) {
    if (!register_class(instance)) return false;
    hwnd_ = CreateWindowExW(0, class_name(), L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                            0, 0, 0, 0,
                            parent,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                            instance, this);
    return hwnd_ != nullptr;
}

HWND DarkDirectoryTreeView::hwnd() const {
    return hwnd_;
}

void DarkDirectoryTreeView::set_theme(const ThemePalette& theme) {
    theme_ = theme;
    invalidate();
}

void DarkDirectoryTreeView::set_font(HFONT font) {
    font_ = font;
    invalidate();
}

void DarkDirectoryTreeView::set_dpi(UINT dpi) {
    dpi_ = dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
    invalidate();
}

void DarkDirectoryTreeView::set_populate_callback(PopulateCallback callback) {
    populate_callback_ = std::move(callback);
}

void DarkDirectoryTreeView::set_select_callback(SelectCallback callback) {
    select_callback_ = std::move(callback);
}

void DarkDirectoryTreeView::move(int x, int y, int width, int height, bool repaint) {
    if (hwnd_ != nullptr) {
        MoveWindow(hwnd_, x, y, width, height, repaint ? TRUE : FALSE);
    }
}

void DarkDirectoryTreeView::clear() {
    roots_.clear();
    visible_.clear();
    selected_ = nullptr;
    scroll_y_ = 0;
    invalidate();
}

void DarkDirectoryTreeView::begin_update() {
    ++update_depth_;
}

void DarkDirectoryTreeView::end_update() {
    if (update_depth_ == 0) return;
    --update_depth_;
    if (update_depth_ == 0 && refresh_pending_) {
        refresh_pending_ = false;
        refresh();
    }
}

DirectoryTreeItem* DarkDirectoryTreeView::insert_item(DirectoryTreeItem* parent,
                               std::wstring text,
                               DirectoryTreeNode node,
                               ShellIconRef icon,
                               bool may_have_children) {
    auto item = std::make_unique<DirectoryTreeItem>();
    item->node = std::move(node);
    item->text = std::move(text);
    item->icon = icon;
    item->parent = parent;
    item->may_have_children = may_have_children;
    DirectoryTreeItem* raw = item.get();
    if (parent != nullptr) {
        parent->children.push_back(std::move(item));
    } else {
        roots_.push_back(std::move(item));
    }
    refresh();
    return raw;
}

void DarkDirectoryTreeView::clear_children(DirectoryTreeItem& item) {
    if (is_ancestor_of(item, selected_)) selected_ = &item;
    item.children.clear();
    item.populated = false;
    refresh();
}

void DarkDirectoryTreeView::refresh() {
    if (update_depth_ > 0) {
        refresh_pending_ = true;
        return;
    }
    rebuild_visible_items();
    clamp_scroll();
    invalidate();
}

void DarkDirectoryTreeView::select_item(DirectoryTreeItem* item, bool notify) {
    if (item == nullptr) return;
    selected_ = item;
    ensure_visible(item);
    invalidate();
    if (notify && select_callback_) {
        select_callback_(*item);
    }
}

DirectoryTreeItem* DarkDirectoryTreeView::selected_item() const {
    return selected_;
}

void DarkDirectoryTreeView::set_expanded(DirectoryTreeItem* item, bool expanded) {
    if (item == nullptr || !item->may_have_children) return;
    if (expanded) ensure_populated(*item);
    item->expanded = expanded;
    refresh();
}

void DarkDirectoryTreeView::refresh_item(DirectoryTreeItem* item) {
    if (item == nullptr) return;
    clear_children(*item);
    if (item->expanded) ensure_populated(*item);
    refresh();
}

void DarkDirectoryTreeView::ensure_visible(DirectoryTreeItem* item) {
    if (item == nullptr || hwnd_ == nullptr) return;
    expand_ancestors(item);
    rebuild_visible_items();
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int viewport = viewport_height(client);
    const int row = visible_index(item);
    if (row < 0 || viewport <= 0) return;
    const int top = row * row_height();
    const int bottom = top + row_height();
    if (top < scroll_y_) {
        scroll_y_ = top;
    } else if (bottom > scroll_y_ + viewport) {
        scroll_y_ = bottom - viewport;
    }
    clamp_scroll();
}

DirectoryTreeViewState DarkDirectoryTreeView::capture_state() const {
    DirectoryTreeViewState state;
    state.vertical_scroll = scroll_y_;
    if (selected_ != nullptr) {
        state.selected_key = directory_tree_node_key(selected_->node);
    }

    const auto visit = [&](const auto& self, const DirectoryTreeItem& item) -> void {
        if (item.expanded) {
            const std::wstring key = directory_tree_node_key(item.node);
            if (!key.empty()) state.expanded_keys.push_back(key);
        }
        for (const auto& child : item.children) {
            self(self, *child);
        }
    };
    for (const auto& root : roots_) {
        visit(visit, *root);
    }
    return state;
}

void DarkDirectoryTreeView::restore_state(const DirectoryTreeViewState& state) {
    std::unordered_set<std::wstring> expanded_keys(state.expanded_keys.begin(),
                                                   state.expanded_keys.end());
    begin_update();
    const auto restore_item = [&](const auto& self, DirectoryTreeItem& item) -> void {
        const std::wstring key = directory_tree_node_key(item.node);
        item.expanded = item.may_have_children && !key.empty() &&
                        expanded_keys.find(key) != expanded_keys.end();
        if (item.expanded) {
            ensure_populated(item);
        }
        for (auto& child : item.children) {
            self(self, *child);
        }
    };
    for (auto& root : roots_) {
        restore_item(restore_item, *root);
    }
    end_update();

    DirectoryTreeItem* selected = nullptr;
    if (!state.selected_key.empty()) {
        for (auto& root : roots_) {
            selected = find_tree_item_by_key(*root, state.selected_key);
            if (selected != nullptr) break;
        }
    }
    if (selected != nullptr) {
        select_item(selected, false);
    }
    scroll_y_ = state.vertical_scroll;
    clamp_scroll();
    invalidate();
}

const wchar_t* DarkDirectoryTreeView::class_name() {
    return L"AxiomDarkDirectoryTreeView";
}

bool DarkDirectoryTreeView::register_class(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    if (GetClassInfoExW(instance, class_name(), &wc)) return true;
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = &DarkDirectoryTreeView::window_proc;
    wc.lpszClassName = class_name();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_DBLCLKS;
    return RegisterClassExW(&wc) != 0;
}

int DarkDirectoryTreeView::scale(int value) const {
    return scale_for_dpi(dpi_, value);
}

int DarkDirectoryTreeView::row_height() const {
    return scale(24);
}

int DarkDirectoryTreeView::indent_width() const {
    return scale(18);
}

int DarkDirectoryTreeView::scrollbar_width() const {
    return scale(8);
}

int DarkDirectoryTreeView::content_height() const {
    return static_cast<int>(visible_.size()) * row_height();
}

int DarkDirectoryTreeView::viewport_height(const RECT& client) const {
    return std::max(0, static_cast<int>(client.bottom - client.top) - scale(2));
}

int DarkDirectoryTreeView::max_scroll(const RECT& client) const {
    return std::max(0, content_height() - viewport_height(client));
}

bool DarkDirectoryTreeView::needs_scrollbar(const RECT& client) const {
    return content_height() > viewport_height(client);
}

RECT DarkDirectoryTreeView::content_rect(const RECT& client) const {
    RECT rect = client;
    InflateRect(&rect, -scale(1), -scale(1));
    if (needs_scrollbar(client)) rect.right -= scrollbar_width();
    return rect;
}

RECT DarkDirectoryTreeView::scrollbar_track_rect(const RECT& client) const {
    return RECT{
        client.right - scrollbar_width() - scale(1),
        client.top + scale(1),
        client.right - scale(1),
        client.bottom - scale(1),
    };
}

RECT DarkDirectoryTreeView::scrollbar_thumb_rect(const RECT& client) const {
    const RECT track = scrollbar_track_rect(client);
    const int track_height = std::max(0, static_cast<int>(track.bottom - track.top));
    const int maximum = max_scroll(client);
    if (maximum <= 0 || track_height <= 0) {
        return RECT{track.left, track.top, track.right, track.top};
    }
    const int thumb_height = std::clamp(
        MulDiv(viewport_height(client), track_height, content_height()),
        scale(24), track_height);
    const int travel = std::max(0, track_height - thumb_height);
    const int top = track.top + MulDiv(scroll_y_, travel, maximum);
    return RECT{track.left, top, track.right, top + thumb_height};
}

void DarkDirectoryTreeView::clamp_scroll() {
    if (hwnd_ == nullptr) {
        scroll_y_ = 0;
        return;
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    scroll_y_ = std::clamp(scroll_y_, 0, max_scroll(client));
}

void DarkDirectoryTreeView::scroll_by(int delta) {
    scroll_y_ += delta;
    clamp_scroll();
    invalidate();
}

void DarkDirectoryTreeView::set_scroll_from_thumb(POINT point, const RECT& client) {
    const RECT track = scrollbar_track_rect(client);
    const RECT thumb = scrollbar_thumb_rect(client);
    const int thumb_height = thumb.bottom - thumb.top;
    const int travel = std::max(1, static_cast<int>(track.bottom - track.top) - thumb_height);
    const int thumb_top = std::clamp(static_cast<int>(point.y) - drag_offset_y_,
                                     static_cast<int>(track.top),
                                     static_cast<int>(track.bottom) - thumb_height);
    scroll_y_ = MulDiv(thumb_top - track.top, max_scroll(client), travel);
    clamp_scroll();
    invalidate();
}

bool DarkDirectoryTreeView::is_ancestor_of(const DirectoryTreeItem& ancestor,
                           const DirectoryTreeItem* item) {
    for (const DirectoryTreeItem* current = item; current != nullptr;
         current = current->parent) {
        if (current == &ancestor) return true;
    }
    return false;
}

void DarkDirectoryTreeView::flatten(DirectoryTreeItem& item, int depth) {
    visible_.push_back({&item, depth});
    if (!item.expanded) return;
    for (auto& child : item.children) {
        flatten(*child, depth + 1);
    }
}

void DarkDirectoryTreeView::rebuild_visible_items() {
    visible_.clear();
    for (auto& root : roots_) {
        flatten(*root, 0);
    }
}

int DarkDirectoryTreeView::visible_index(const DirectoryTreeItem* item) const {
    for (int index = 0; index < static_cast<int>(visible_.size()); ++index) {
        if (visible_[static_cast<std::size_t>(index)].item == item) return index;
    }
    return -1;
}

void DarkDirectoryTreeView::expand_ancestors(DirectoryTreeItem* item) {
    for (DirectoryTreeItem* parent = item->parent; parent != nullptr; parent = parent->parent) {
        if (!parent->expanded) {
            ensure_populated(*parent);
            parent->expanded = true;
        }
    }
}

void DarkDirectoryTreeView::ensure_populated(DirectoryTreeItem& item) {
    if (!item.may_have_children || item.populated || !populate_callback_) return;
    populate_callback_(item);
}

void DarkDirectoryTreeView::toggle_item(DirectoryTreeItem& item) {
    if (!item.may_have_children) return;
    if (!item.expanded) ensure_populated(item);
    if (!item.children.empty() || item.populated) {
        item.expanded = !item.expanded;
    }
    refresh();
}

DirectoryTreeItem* DarkDirectoryTreeView::item_at_point(POINT point, int* depth, RECT* row_rect) {
    rebuild_visible_items();
    RECT client{};
    GetClientRect(hwnd_, &client);
    const RECT content = content_rect(client);
    if (point.x < content.left || point.x >= content.right ||
        point.y < content.top || point.y >= content.bottom) {
        return nullptr;
    }
    const int row = (point.y - content.top + scroll_y_) / row_height();
    if (row < 0 || row >= static_cast<int>(visible_.size())) return nullptr;
    if (depth != nullptr) *depth = visible_[static_cast<std::size_t>(row)].depth;
    if (row_rect != nullptr) {
        const int top = content.top + row * row_height() - scroll_y_;
        *row_rect = RECT{content.left, top, content.right, top + row_height()};
    }
    return visible_[static_cast<std::size_t>(row)].item;
}

RECT DarkDirectoryTreeView::glyph_rect_for_row(const RECT& row, int depth) const {
    const int size = scale(14);
    const int left = row.left + scale(5) + depth * indent_width();
    return RECT{left, row.top + (row_height() - size) / 2,
                left + size, row.top + (row_height() + size) / 2};
}

void DarkDirectoryTreeView::draw_chevron(HDC dc, const RECT& rect, bool expanded, COLORREF color) const {
    POINT points[3]{};
    if (expanded) {
        points[0] = {rect.left + scale(3), rect.top + scale(5)};
        points[1] = {rect.right - scale(3), rect.top + scale(5)};
        points[2] = {(rect.left + rect.right) / 2, rect.bottom - scale(4)};
    } else {
        points[0] = {rect.left + scale(5), rect.top + scale(3)};
        points[1] = {rect.left + scale(5), rect.bottom - scale(3)};
        points[2] = {rect.right - scale(4), (rect.top + rect.bottom) / 2};
    }
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    Polygon(dc, points, 3);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DarkDirectoryTreeView::paint_content(HDC dc, const RECT& client) {
    fill_solid_rect(dc, client, theme_.edit);
    frame_solid_rect(dc, client, theme_.border);
    const RECT content = content_rect(client);

    HFONT font = font_ != nullptr
        ? font_
        : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);

    rebuild_visible_items();
    const int first_row = row_height() > 0 ? std::max(0, scroll_y_ / row_height()) : 0;
    const int visible_rows = row_height() > 0
        ? (content.bottom - content.top + row_height() - 1) / row_height() + 1
        : 0;
    const int last_row = std::min(static_cast<int>(visible_.size()),
                                  first_row + visible_rows);

    for (int row = first_row; row < last_row; ++row) {
        const auto& visible = visible_[static_cast<std::size_t>(row)];
        DirectoryTreeItem& item = *visible.item;
        RECT row_rect{content.left,
                      content.top + row * row_height() - scroll_y_,
                      content.right,
                      content.top + (row + 1) * row_height() - scroll_y_};
        const bool selected = selected_ == &item;
        fill_solid_rect(dc, row_rect, selected ? theme_.selection : theme_.edit);

        const RECT glyph = glyph_rect_for_row(row_rect, visible.depth);
        if (item.may_have_children) {
            draw_chevron(dc, glyph, item.expanded,
                         selected ? theme_.selection_text : theme_.muted_text);
        }

        int text_left = glyph.right + scale(3);
        if (item.icon.image_list != nullptr && item.icon.index >= 0) {
            int icon_width = 0;
            int icon_height = 0;
            ImageList_GetIconSize(item.icon.image_list, &icon_width, &icon_height);
            const int icon_y = row_rect.top + std::max(
                0, (row_height() - icon_height) / 2);
            ImageList_Draw(item.icon.image_list, item.icon.index, dc,
                           text_left, icon_y, ILD_TRANSPARENT);
            text_left += icon_width + scale(6);
        }

        RECT text_rect{text_left, row_rect.top, row_rect.right - scale(5), row_rect.bottom};
        SetTextColor(dc, selected ? theme_.selection_text : theme_.text);
        DrawTextW(dc, item.text.c_str(), -1, &text_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

        if (selected && GetFocus() == hwnd_) {
            RECT focus = row_rect;
            InflateRect(&focus, -scale(2), -scale(2));
            frame_solid_rect(dc, focus, theme_.focus);
        }
    }

    if (needs_scrollbar(client)) {
        const RECT track = scrollbar_track_rect(client);
        const RECT thumb = scrollbar_thumb_rect(client);
        fill_solid_rect(dc, track, theme_.scrollbar_track);
        fill_solid_rect(dc, thumb, dragging_scrollbar_
                                   ? theme_.scrollbar_thumb_pressed
                                   : theme_.scrollbar_thumb);
    }

    SelectObject(dc, old_font);
}

void DarkDirectoryTreeView::on_paint() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd_, &paint);
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width > 0 && height > 0) {
        RECT buffer{0, 0, width, height};
        if (paint_buffer_.ensure(dc, width, height)) {
            paint_content(paint_buffer_.dc(), buffer);
            BitBlt(dc, 0, 0, width, height, paint_buffer_.dc(), 0, 0, SRCCOPY);
        } else {
            paint_content(dc, buffer);
        }
    }
    EndPaint(hwnd_, &paint);
}

void DarkDirectoryTreeView::invalidate() const {
    if (hwnd_ != nullptr) InvalidateRect(hwnd_, nullptr, FALSE);
}

void DarkDirectoryTreeView::select_visible_index(int index) {
    rebuild_visible_items();
    if (visible_.empty()) return;
    index = std::clamp(index, 0, static_cast<int>(visible_.size()) - 1);
    select_item(visible_[static_cast<std::size_t>(index)].item, true);
}

int DarkDirectoryTreeView::find_typeahead_match(std::wstring_view needle, bool cycle) {
    rebuild_visible_items();
    if (needle.empty() || visible_.empty()) return -1;
    const int count = static_cast<int>(visible_.size());
    const int selected_index = visible_index(selected_);
    const int start = cycle && selected_index >= 0 ? selected_index + 1 : 0;
    auto scan = [&](auto predicate) {
        for (int pass = 0; pass < 2; ++pass) {
            const int first = pass == 0 ? start : 0;
            const int last = pass == 0 ? count : std::min(start, count);
            for (int row = first; row < last; ++row) {
                const auto* item = visible_[static_cast<std::size_t>(row)].item;
                if (item != nullptr && predicate(item->text)) return row;
            }
        }
        return -1;
    };
    if (const int row = scan([&](std::wstring_view text) {
            return starts_with_folded(text, needle);
        }); row >= 0) {
        return row;
    }
    if (const int row = scan([&](std::wstring_view text) {
            return contains_folded(text, needle);
        }); row >= 0) {
        return row;
    }

    const std::wstring folded_needle = folded_text(needle);
    int best = -1;
    std::wstring best_text;
    for (int row = 0; row < count; ++row) {
        const auto* item = visible_[static_cast<std::size_t>(row)].item;
        if (item == nullptr) continue;
        const std::wstring text = folded_text(item->text);
        if (text >= folded_needle && (best < 0 || text < best_text)) {
            best = row;
            best_text = text;
        }
    }
    return best;
}

bool DarkDirectoryTreeView::handle_typeahead_char(wchar_t character) {
    if (character == L'\r' || character == L'\n' || character == L'\t') return false;
    const ULONGLONG now = GetTickCount64();
    if (now - typeahead_last_tick_ > 1200) {
        typeahead_.clear();
    }
    typeahead_last_tick_ = now;

    if (character == L'\b') {
        if (!typeahead_.empty()) typeahead_.pop_back();
        if (typeahead_.empty()) return true;
        if (const int row = find_typeahead_match(typeahead_, false); row >= 0) {
            select_visible_index(row);
        }
        return true;
    }
    if (character < L' ') return false;

    const bool repeated_single =
        typeahead_.size() == 1 &&
        std::towlower(typeahead_.front()) == std::towlower(character);
    if (repeated_single) {
        typeahead_.assign(1, character);
    } else {
        typeahead_.push_back(character);
    }

    if (const int row = find_typeahead_match(typeahead_, repeated_single); row >= 0) {
        select_visible_index(row);
    }
    return true;
}

LRESULT DarkDirectoryTreeView::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            on_paint();
            return 0;
        case WM_SETFONT:
            font_ = reinterpret_cast<HFONT>(wparam);
            invalidate();
            return 0;
        case WM_SIZE:
            clamp_scroll();
            invalidate();
            return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            invalidate();
            return 0;
        case WM_MOUSEWHEEL:
            scroll_by(-GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * row_height() * 3);
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd_);
            RECT client{};
            GetClientRect(hwnd_, &client);
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (needs_scrollbar(client)) {
                const RECT thumb = scrollbar_thumb_rect(client);
                const RECT track = scrollbar_track_rect(client);
                if (PtInRect(&thumb, point)) {
                    dragging_scrollbar_ = true;
                    drag_offset_y_ = point.y - thumb.top;
                    SetCapture(hwnd_);
                    return 0;
                }
                if (PtInRect(&track, point)) {
                    scroll_by(point.y < thumb.top ? -viewport_height(client)
                                                  : viewport_height(client));
                    return 0;
                }
            }
            int depth = 0;
            RECT row{};
            DirectoryTreeItem* item = item_at_point(point, &depth, &row);
            if (item == nullptr) return 0;
            const RECT glyph = glyph_rect_for_row(row, depth);
            if (PtInRect(&glyph, point) && item->may_have_children) {
                toggle_item(*item);
            }
            select_item(item, true);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            DirectoryTreeItem* item = item_at_point(point);
            if (item != nullptr && item->may_have_children) {
                toggle_item(*item);
                return 0;
            }
            break;
        }
        case WM_RBUTTONDOWN: {
            SetFocus(hwnd_);
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (DirectoryTreeItem* item = item_at_point(point)) {
                select_item(item, false);
            }
            return 0;
        }
        case WM_CONTEXTMENU: {
            if (lparam != static_cast<LPARAM>(-1)) {
                POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                POINT client = screen;
                ScreenToClient(hwnd_, &client);
                if (DirectoryTreeItem* item = item_at_point(client)) {
                    select_item(item, false);
                }
            }
            SendMessageW(GetParent(hwnd_), WM_CONTEXTMENU,
                         reinterpret_cast<WPARAM>(hwnd_), lparam);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (dragging_scrollbar_) {
                RECT client{};
                GetClientRect(hwnd_, &client);
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                set_scroll_from_thumb(point, client);
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (dragging_scrollbar_) {
                dragging_scrollbar_ = false;
                if (GetCapture() == hwnd_) ReleaseCapture();
                invalidate();
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            dragging_scrollbar_ = false;
            invalidate();
            return 0;
        case WM_KEYDOWN: {
            rebuild_visible_items();
            if (visible_.empty()) return 0;
            int index = visible_index(selected_);
            if (index < 0) index = 0;
            switch (wparam) {
                case VK_DOWN:
                    select_visible_index(index + 1);
                    return 0;
                case VK_UP:
                    select_visible_index(index - 1);
                    return 0;
                case VK_NEXT:
                    select_visible_index(index + std::max(1, viewport_height_for_current() / row_height()));
                    return 0;
                case VK_PRIOR:
                    select_visible_index(index - std::max(1, viewport_height_for_current() / row_height()));
                    return 0;
                case VK_HOME:
                    select_visible_index(0);
                    return 0;
                case VK_END:
                    select_visible_index(static_cast<int>(visible_.size()) - 1);
                    return 0;
                case VK_RIGHT:
                    if (selected_ != nullptr && selected_->may_have_children) {
                        if (!selected_->expanded) {
                            toggle_item(*selected_);
                        } else if (!selected_->children.empty()) {
                            select_item(selected_->children.front().get(), true);
                        }
                    }
                    return 0;
                case VK_LEFT:
                    if (selected_ != nullptr) {
                        if (selected_->expanded) {
                            selected_->expanded = false;
                            refresh();
                        } else if (selected_->parent != nullptr) {
                            select_item(selected_->parent, true);
                        }
                    }
                    return 0;
                case VK_RETURN:
                case VK_SPACE:
                    if (selected_ != nullptr && select_callback_) select_callback_(*selected_);
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_CHAR:
            if ((GetKeyState(VK_CONTROL) & 0x8000) == 0 &&
                (GetKeyState(VK_MENU) & 0x8000) == 0 &&
                handle_typeahead_char(static_cast<wchar_t>(wparam))) {
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

int DarkDirectoryTreeView::viewport_height_for_current() const {
    RECT client{};
    if (hwnd_ == nullptr) return 0;
    GetClientRect(hwnd_, &client);
    return viewport_height(client);
}

LRESULT CALLBACK DarkDirectoryTreeView::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    DarkDirectoryTreeView* view = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        view = static_cast<DarkDirectoryTreeView*>(create->lpCreateParams);
        view->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(view));
    } else {
        view = reinterpret_cast<DarkDirectoryTreeView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (view != nullptr) return view->handle_message(message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace axiom::gui
