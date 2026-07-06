// MainWindow browser wiring: the folders tree pane, list-view
// population and sorting, navigation, and selection/status updates.

#include "gui/main_window_internal.hpp"

namespace axiom::gui {

ShellIconRef MainWindow::tree_icon_for_filesystem(const fs::path& path, bool drive) const {
    return shell_icon_for_path(path, drive);
}

ShellIconRef MainWindow::tree_icon_for_archive(const fs::path& path) const {
    axiom::gui::BrowserItem item;
    item.kind = axiom::gui::BrowserItemKind::archive;
    item.filesystem_path = path;
    item.name = path.filename().wstring();
    return shell_icon_for_item(item);
}

DirectoryTreeItem* MainWindow::insert_tree_item(DirectoryTreeItem* parent,
                                    std::wstring text,
                                    DirectoryTreeNode node,
                                    ShellIconRef icon,
                                    bool may_have_children) {
    return tree_view_.insert_item(parent, std::move(text), std::move(node),
                                  icon, may_have_children);
}

bool MainWindow::tree_should_show_filesystem_item(const WIN32_FIND_DATAW& data) const {
    if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
        return false;
    }
    if (!application_options_.show_hidden &&
        ((data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0 ||
         (data.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0)) {
        return false;
    }
    return true;
}

bool MainWindow::filesystem_has_tree_children(const fs::path& path) const {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW((path / L"*").c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (!tree_should_show_filesystem_item(data)) continue;
        found = true;
        break;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return found;
}

void MainWindow::populate_tree_filesystem_children(DirectoryTreeItem& item, const fs::path& path) {
    std::vector<TreeChildCandidate> candidates;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW((path / L"*").c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!tree_should_show_filesystem_item(data)) continue;
        const bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const fs::path child_path = path / data.cFileName;
        const bool archive = !directory && path_has_supported_archive_extension(child_path);
        candidates.push_back({data.cFileName, child_path, directory, archive, false});
    } while (FindNextFileW(find, &data));
    FindClose(find);

    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        if (left.directory != right.directory) return left.directory > right.directory;
        return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
    });

    for (const auto& child : candidates) {
        DirectoryTreeNode node;
        node.kind = child.archive ? DirectoryTreeNodeKind::archive
                                  : child.directory ? DirectoryTreeNodeKind::filesystem
                                                    : DirectoryTreeNodeKind::file;
        node.filesystem_path = child.path;
        node.archive_path = child.archive ? child.path : fs::path{};
        const bool has_children = child.archive || child.directory;
        insert_tree_item(&item, child.name, std::move(node),
                         child.archive ? tree_icon_for_archive(child.path)
                                       : tree_icon_for_filesystem(child.path),
                         has_children);
    }
}

void MainWindow::add_known_tree_folder(DirectoryTreeItem& parent, const wchar_t* label,
                           REFKNOWNFOLDERID folder_id) {
    if (const auto path = known_folder_path(folder_id)) {
        DirectoryTreeNode node;
        node.kind = DirectoryTreeNodeKind::filesystem;
        node.filesystem_path = *path;
        insert_tree_item(&parent, label, std::move(node),
                         tree_icon_for_filesystem(*path),
                         filesystem_has_tree_children(*path));
    }
}

void MainWindow::populate_tree_computer_children(DirectoryTreeItem& item) {
    add_known_tree_folder(item, L"Home", FOLDERID_Profile);
    add_known_tree_folder(item, L"Desktop", FOLDERID_Desktop);
    add_known_tree_folder(item, L"Documents", FOLDERID_Documents);
    add_known_tree_folder(item, L"Downloads", FOLDERID_Downloads);
    add_known_tree_folder(item, L"Pictures", FOLDERID_Pictures);
    add_known_tree_folder(item, L"OneDrive", FOLDERID_SkyDrive);

    const DWORD required = GetLogicalDriveStringsW(0, nullptr);
    if (required == 0) return;
    std::wstring drives(static_cast<std::size_t>(required), L'\0');
    if (GetLogicalDriveStringsW(required, drives.data()) == 0) return;
    for (const wchar_t* drive = drives.c_str(); *drive != L'\0';
         drive += wcslen(drive) + 1) {
        const fs::path path(drive);
        wchar_t volume[MAX_PATH]{};
        std::wstring label;
        if (GetVolumeInformationW(drive, volume, MAX_PATH, nullptr, nullptr,
                                  nullptr, nullptr, 0) && volume[0] != L'\0') {
            label = std::wstring(volume) + L" (" + path.root_name().wstring() + L")";
        } else {
            label = path.root_name().wstring();
        }
        DirectoryTreeNode node;
        node.kind = DirectoryTreeNodeKind::filesystem;
        node.filesystem_path = path;
        insert_tree_item(&item, std::move(label), std::move(node),
                         tree_icon_for_filesystem(path, true), true);
    }
}

std::shared_ptr<const axiom::gui::ArchiveCatalog> MainWindow::tree_archive_catalog(
    const fs::path& archive) {
    if (archive_catalog_ && same_filesystem_path(archive_catalog_->path(), archive)) {
        return archive_catalog_;
    }
    try {
        std::string password;
        if (!archive_password_path_.empty() &&
            same_filesystem_path(archive_password_path_, archive)) {
            password = archive_password_;
        }
        return axiom::gui::ArchiveCatalog::load(archive, password);
    } catch (...) {
        return {};
    }
}

void MainWindow::populate_tree_archive_children(DirectoryTreeItem& item, const fs::path& archive,
                                    const std::string& directory) {
    auto catalog = tree_archive_catalog(archive);
    if (!catalog) return;
    const auto snapshot = catalog->list(axiom::gui::BrowserLocation::archive(archive, directory),
                                        std::stop_token{});
    std::vector<axiom::gui::BrowserItem> directories;
    for (const auto& child : snapshot.items) {
        if (child.kind == axiom::gui::BrowserItemKind::directory) {
            directories.push_back(child);
        }
    }
    std::sort(directories.begin(), directories.end(), [](const auto& left, const auto& right) {
        return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
    });
    for (const auto& child : directories) {
        DirectoryTreeNode node;
        node.kind = DirectoryTreeNodeKind::archive_directory;
        node.archive_path = archive;
        node.archive_directory = child.archive_path;
        // Keep archive trees lazy. Large ISO/7z/RAR images can contain thousands of
        // entries; probing every sibling directory here rescans the full catalog for
        // each child and can make the GUI look like it crashed while opening.
        insert_tree_item(&item, child.name, std::move(node),
                         tree_icon_for_filesystem(L"folder"),
                         true);
    }
}

void MainWindow::populate_tree_item(DirectoryTreeItem& item) {
    auto& node = item.node;
    if (node.kind == DirectoryTreeNodeKind::dummy || item.populated) {
        return;
    }
    tree_view_.begin_update();
    tree_view_.clear_children(item);
    switch (node.kind) {
        case DirectoryTreeNodeKind::computer:
            populate_tree_computer_children(item);
            break;
        case DirectoryTreeNodeKind::filesystem:
            populate_tree_filesystem_children(item, node.filesystem_path);
            break;
        case DirectoryTreeNodeKind::file:
            break;
        case DirectoryTreeNodeKind::archive:
            populate_tree_archive_children(item, node.archive_path, {});
            break;
        case DirectoryTreeNodeKind::archive_directory:
            populate_tree_archive_children(item, node.archive_path, node.archive_directory);
            break;
        case DirectoryTreeNodeKind::dummy:
            break;
    }
    item.populated = true;
    node.populated = true;
    item.may_have_children = !item.children.empty();
    tree_view_.end_update();
}

DirectoryTreeItem* MainWindow::find_tree_child_by_filesystem_path(DirectoryTreeItem& parent,
                                                      const fs::path& path) const {
    for (const auto& child : parent.children) {
        const auto& node = child->node;
        if ((node.kind == DirectoryTreeNodeKind::filesystem ||
             node.kind == DirectoryTreeNodeKind::file ||
             node.kind == DirectoryTreeNodeKind::archive) &&
            same_filesystem_path(node.filesystem_path, path)) {
            return child.get();
        }
    }
    return nullptr;
}

DirectoryTreeItem* MainWindow::find_tree_child_by_archive_directory(DirectoryTreeItem& parent,
                                                        const fs::path& archive,
                                                        const std::string& directory) const {
    for (const auto& child : parent.children) {
        const auto& node = child->node;
        if (node.kind == DirectoryTreeNodeKind::archive_directory &&
            same_filesystem_path(node.archive_path, archive) &&
            node.archive_directory == directory) {
            return child.get();
        }
    }
    return nullptr;
}

DirectoryTreeItem* MainWindow::ensure_filesystem_tree_path(const fs::path& path) {
    if (tree_computer_item_ == nullptr || path.empty()) return nullptr;
    populate_tree_item(*tree_computer_item_);
    tree_computer_item_->expanded = true;
    fs::path current_path = path.root_path();
    if (current_path.empty()) current_path = path.root_name();
    if (current_path.empty()) return tree_computer_item_;

    DirectoryTreeItem* current =
        find_tree_child_by_filesystem_path(*tree_computer_item_, current_path);
    if (current == nullptr) {
        DirectoryTreeNode node;
        node.kind = DirectoryTreeNodeKind::filesystem;
        node.filesystem_path = current_path;
        current = insert_tree_item(tree_computer_item_, current_path.wstring(), std::move(node),
                                   tree_icon_for_filesystem(current_path, true), true);
    }

    fs::path relative = path.lexically_relative(current_path);
    if (relative.empty() || relative.native() == L".") return current;
    for (const auto& part : relative) {
        if (part.empty() || part.native() == L".") continue;
        populate_tree_item(*current);
        current->expanded = true;
        current_path /= part;
        DirectoryTreeItem* child = find_tree_child_by_filesystem_path(*current, current_path);
        const bool final_component = same_filesystem_path(current_path, path);
        if (child == nullptr) {
            DirectoryTreeNode node;
            const DWORD attributes = GetFileAttributesW(current_path.c_str());
            const bool directory = attributes != INVALID_FILE_ATTRIBUTES &&
                                   (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool archive = !directory &&
                (path_has_supported_archive_extension(current_path) ||
                 (final_component &&
                  axiom::archive_provider_for_path(current_path) != nullptr));
            node.kind = archive ? DirectoryTreeNodeKind::archive
                                : directory ? DirectoryTreeNodeKind::filesystem
                                            : DirectoryTreeNodeKind::file;
            node.filesystem_path = current_path;
            node.archive_path = node.kind == DirectoryTreeNodeKind::archive
                ? current_path
                : fs::path{};
            child = insert_tree_item(current, part.wstring(), std::move(node),
                                     node.kind == DirectoryTreeNodeKind::archive
                                         ? tree_icon_for_archive(current_path)
                                         : tree_icon_for_filesystem(current_path),
                                     node.kind == DirectoryTreeNodeKind::archive ||
                                         (node.kind == DirectoryTreeNodeKind::filesystem &&
                                          filesystem_has_tree_children(current_path)));
        } else if (final_component &&
                   child->node.kind == DirectoryTreeNodeKind::file &&
                   axiom::archive_provider_for_path(current_path) != nullptr) {
            child->node.kind = DirectoryTreeNodeKind::archive;
            child->node.archive_path = current_path;
            child->icon = tree_icon_for_archive(current_path);
            child->may_have_children = true;
            child->populated = false;
            child->node.populated = false;
            tree_view_.clear_children(*child);
        }
        current = child;
    }
    return current;
}

DirectoryTreeItem* MainWindow::ensure_archive_tree_path(const fs::path& archive,
                                            const std::string& directory) {
    DirectoryTreeItem* archive_item = ensure_filesystem_tree_path(archive);
    if (archive_item == nullptr) return nullptr;
    populate_tree_item(*archive_item);
    if (directory.empty()) return archive_item;

    DirectoryTreeItem* current = archive_item;
    std::string current_directory;
    std::size_t begin = 0;
    while (begin < directory.size()) {
        const std::size_t end = directory.find('/', begin);
        const std::string part = directory.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        current_directory = current_directory.empty()
            ? part
            : current_directory + "/" + part;
        populate_tree_item(*current);
        current->expanded = true;
        DirectoryTreeItem* child =
            find_tree_child_by_archive_directory(*current, archive, current_directory);
        if (child == nullptr) {
            DirectoryTreeNode node;
            node.kind = DirectoryTreeNodeKind::archive_directory;
            node.archive_path = archive;
            node.archive_directory = current_directory;
            child = insert_tree_item(current, widen(part), std::move(node),
                                     tree_icon_for_filesystem(L"folder"),
                                     true);
        }
        current = child;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return current;
}

void MainWindow::sync_tree_to_location(const axiom::gui::BrowserLocation& location) {
    DirectoryTreeItem* target = nullptr;
    if (location.kind == axiom::gui::BrowserLocationKind::computer) {
        target = tree_computer_item_;
    } else if (location.kind == axiom::gui::BrowserLocationKind::filesystem) {
        target = ensure_filesystem_tree_path(location.filesystem_path);
    } else {
        target = ensure_archive_tree_path(location.archive_path, location.archive_directory);
    }
    if (target != nullptr) {
        syncing_tree_ = true;
        tree_view_.select_item(target, false);
        syncing_tree_ = false;
    }
}

void MainWindow::rebuild_directory_tree() {
    tree_view_.clear();
    DirectoryTreeNode root;
    root.kind = DirectoryTreeNodeKind::computer;
    tree_computer_item_ = insert_tree_item(nullptr, L"This PC", std::move(root),
                                           tree_icon_for_filesystem(L"folder"), true);
    if (tree_computer_item_ != nullptr) {
        populate_tree_item(*tree_computer_item_);
        tree_computer_item_->expanded = true;
        tree_view_.refresh();
    }
}

void MainWindow::on_tree_selection_changed(const DirectoryTreeItem& item) {
    if (syncing_tree_ || busy_) return;
    const auto& node = item.node;
    if (node.kind == DirectoryTreeNodeKind::dummy) return;
    switch (node.kind) {
        case DirectoryTreeNodeKind::computer:
            navigate_to(axiom::gui::BrowserLocation::computer());
            break;
        case DirectoryTreeNodeKind::filesystem:
            navigate_to(axiom::gui::BrowserLocation::filesystem(node.filesystem_path));
            break;
        case DirectoryTreeNodeKind::file:
            if (!open_archive_path(node.filesystem_path)) {
                const fs::path parent = node.filesystem_path.parent_path();
                if (!parent.empty()) {
                    navigate_to(axiom::gui::BrowserLocation::filesystem(parent));
                }
            }
            break;
        case DirectoryTreeNodeKind::archive:
            open_archive_path(node.filesystem_path);
            break;
        case DirectoryTreeNodeKind::archive_directory:
            navigate_to(axiom::gui::BrowserLocation::archive(node.archive_path,
                                                             node.archive_directory));
            break;
        case DirectoryTreeNodeKind::dummy:
            break;
    }
}

void MainWindow::add_status_size(BrowserStatusTotals& totals, std::uint64_t size) {
    totals.has_size = true;
    totals.size = saturated_add(totals.size, size);
}

void MainWindow::add_status_item(BrowserStatusTotals& totals,
                            const axiom::gui::BrowserItem& item) {
    if (item.is_parent()) return;
    ++totals.items;
    switch (item.kind) {
        case axiom::gui::BrowserItemKind::drive:
            ++totals.drives;
            add_status_size(totals, item.size);
            break;
        case axiom::gui::BrowserItemKind::directory:
            ++totals.folders;
            break;
        case axiom::gui::BrowserItemKind::archive:
            ++totals.archives;
            add_status_size(totals, item.size);
            break;
        case axiom::gui::BrowserItemKind::symlink:
        case axiom::gui::BrowserItemKind::hardlink:
            ++totals.links;
            break;
        case axiom::gui::BrowserItemKind::file:
            ++totals.files;
            add_status_size(totals, item.size);
            break;
        case axiom::gui::BrowserItemKind::parent:
            break;
    }
    if (item.packed_size) {
        totals.has_packed = true;
        totals.packed = saturated_add(totals.packed, *item.packed_size);
        totals.packed_estimated = totals.packed_estimated || item.packed_size_estimated;
    }
}

MainWindow::BrowserStatusTotals MainWindow::summarize_browser_status(const std::vector<int>* indices) const {
    BrowserStatusTotals totals;
    if (indices == nullptr) {
        for (const auto& item : browser_items_) {
            add_status_item(totals, item);
        }
        return totals;
    }
    for (const int index : *indices) {
        if (index >= 0 && index < static_cast<int>(browser_items_.size())) {
            add_status_item(totals, browser_items_[static_cast<std::size_t>(index)]);
        }
    }
    return totals;
}

void MainWindow::append_status_part(std::vector<std::wstring>& parts, std::wstring text) {
    if (!text.empty()) parts.push_back(std::move(text));
}

std::wstring MainWindow::join_status_parts(const std::vector<std::wstring>& parts,
                                      std::wstring_view separator) {
    std::wstring result;
    for (const auto& part : parts) {
        if (!result.empty()) result += separator;
        result += part;
    }
    return result;
}

std::wstring MainWindow::status_breakdown(const BrowserStatusTotals& totals) {
    std::vector<std::wstring> parts;
    append_status_part(parts, quote_count(totals.drives, L"drive", L"drives"));
    append_status_part(parts, quote_count(totals.folders, L"folder", L"folders"));
    append_status_part(parts, quote_count(totals.archives, L"archive", L"archives"));
    append_status_part(parts, quote_count(totals.files, L"file", L"files"));
    append_status_part(parts, quote_count(totals.links, L"link", L"links"));
    std::erase_if(parts, [](const std::wstring& text) {
        return text.rfind(L"0 ", 0) == 0;
    });
    return join_status_parts(parts);
}

std::wstring MainWindow::status_location_suffix() const {
    const auto& location = history_.current();
    if (location.kind == axiom::gui::BrowserLocationKind::filesystem) {
        return location.filesystem_path.wstring();
    }
    if (location.kind == axiom::gui::BrowserLocationKind::archive) {
        std::wstring suffix;
        if (archive_catalog_ && same_filesystem_path(archive_catalog_->path(),
                                                     location.archive_path)) {
            suffix = widen(archive_catalog_->format_info().display_name);
        } else if (const auto* provider = axiom::archive_provider_for_path(location.archive_path)) {
            suffix = widen(provider->info().display_name);
        } else {
            suffix = L"Archive";
        }
        suffix += L": ";
        suffix += location.archive_path.filename().wstring();
        suffix += L" :: /";
        if (!location.archive_directory.empty()) suffix += widen(location.archive_directory);
        return suffix;
    }
    return L"This PC";
}

void MainWindow::update_browser_status(const std::vector<int>* selected_indices) {
    const bool has_selection = selected_indices != nullptr && !selected_indices->empty();
    const BrowserStatusTotals totals = summarize_browser_status(selected_indices);
    std::wstring text = has_selection
        ? quote_count(totals.items, L"item selected", L"items selected")
        : quote_count(totals.items, L"item", L"items");

    const std::wstring breakdown = status_breakdown(totals);
    if (!breakdown.empty()) {
        text += L": ";
        text += breakdown;
    }
    if (totals.has_size) {
        text += history_.current().kind == axiom::gui::BrowserLocationKind::archive
            ? L", " + format_size(totals.size) + L" unpacked"
            : L", " + format_size(totals.size);
    }
    if (totals.has_packed) {
        text += L", ";
        text += totals.packed_estimated ? L"\u2248 " : L"";
        text += format_size(totals.packed);
        text += L" packed";
    }
    const std::wstring suffix = status_location_suffix();
    if (!suffix.empty()) {
        text += L" | ";
        text += suffix;
    }
    set_status(text);
}

void MainWindow::update_browser_status_for_current_selection() {
    const std::vector<int> selected = selected_browser_indices();
    if (selected.empty()) {
        update_browser_status();
    } else {
        update_browser_status(&selected);
    }
}

std::wstring MainWindow::folded_extension_key(fs::path path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t value) {
                       return static_cast<wchar_t>(std::towlower(value));
                   });
    return extension.empty() ? L"<none>" : extension;
}

std::wstring MainWindow::folded_path_key(fs::path path) {
    std::wstring text = path.lexically_normal().wstring();
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t value) {
                       return static_cast<wchar_t>(std::towlower(value));
                   });
    return text;
}

std::wstring MainWindow::shell_icon_cache_key(const axiom::gui::BrowserItem& item,
                                         bool prefer_generic_unique_icon) {
    if (item_needs_real_shell_icon(item) && !prefer_generic_unique_icon) {
        return L"path:" + folded_path_key(item.filesystem_path);
    }
    switch (item.kind) {
        case axiom::gui::BrowserItemKind::parent:
        case axiom::gui::BrowserItemKind::directory:
            return L"folder";
        case axiom::gui::BrowserItemKind::drive:
            return L"drive:" + item.filesystem_path.root_name().wstring();
        case axiom::gui::BrowserItemKind::archive:
            return L"archive:" + folded_extension_key(
                item.filesystem_path.empty() ? fs::path(item.name) : item.filesystem_path);
        case axiom::gui::BrowserItemKind::symlink:
            return L"symlink:" + folded_extension_key(fs::path(item.name));
        case axiom::gui::BrowserItemKind::hardlink:
            return L"hardlink:" + folded_extension_key(fs::path(item.name));
        case axiom::gui::BrowserItemKind::file:
            return L"file:" + folded_extension_key(
                item.filesystem_path.empty() ? fs::path(item.name) : item.filesystem_path);
    }
    return L"file:<none>";
}

ShellIconRef MainWindow::cached_shell_icon_for_item(const axiom::gui::BrowserItem& item,
                                        bool prefer_generic_unique_icon) {
    const std::wstring key = shell_icon_cache_key(item, prefer_generic_unique_icon);
    if (const auto found = shell_icon_cache_.find(key); found != shell_icon_cache_.end()) {
        return found->second;
    }
    axiom::gui::BrowserItem icon_item = item;
    if (prefer_generic_unique_icon && item_needs_real_shell_icon(icon_item)) {
        icon_item.filesystem_path.clear();
    }
    ShellIconRef icon = shell_icon_for_item(icon_item);
    shell_icon_cache_.emplace(key, icon);
    return icon;
}

std::vector<int> MainWindow::selected_browser_indices() const {
    std::vector<int> result;
    for (int index : table_.selected_indices()) {
        if (index >= 0 && index < static_cast<int>(browser_items_.size())) result.push_back(index);
    }
    return result;
}

MainWindow::BrowserViewState MainWindow::capture_browser_view_state() const {
    BrowserViewState state;
    state.horizontal_scroll = table_.horizontal_scroll_position();
    state.vertical_scroll = table_.vertical_scroll_position();
    state.tree = tree_view_.capture_state();
    for (int index : selected_browser_indices()) {
        state.selected_ids.push_back(browser_items_[static_cast<std::size_t>(index)].id);
    }
    const int focused = table_.focused_index();
    if (focused >= 0 && focused < static_cast<int>(browser_items_.size())) {
        state.focused_id = browser_items_[static_cast<std::size_t>(focused)].id;
    }
    return state;
}

void MainWindow::restore_browser_view_state(const BrowserViewState& state) {
    std::unordered_set<std::uint64_t> selected_ids(state.selected_ids.begin(),
                                                   state.selected_ids.end());
    std::vector<int> selected_indices;
    selected_indices.reserve(state.selected_ids.size());
    int focused_index = -1;
    for (int index = 0; index < static_cast<int>(browser_items_.size()); ++index) {
        const auto id = browser_items_[static_cast<std::size_t>(index)].id;
        if (selected_ids.find(id) != selected_ids.end()) {
            selected_indices.push_back(index);
        }
        if (state.focused_id && id == *state.focused_id) {
            focused_index = index;
        }
    }
    table_.set_selection_and_scroll(std::move(selected_indices), focused_index,
                                    state.horizontal_scroll, state.vertical_scroll);
}

std::optional<MainWindow::BrowserViewState> MainWindow::current_or_pending_browser_view_state() const {
    if (table_population_active_ && pending_table_restore_state_) {
        BrowserViewState state = *pending_table_restore_state_;
        state.tree = tree_view_.capture_state();
        return state;
    }
    if (displayed_browser_location_ && *displayed_browser_location_ == history_.current()) {
        return capture_browser_view_state();
    }
    return std::nullopt;
}

void MainWindow::remember_current_history_view_state() {
    if (browser_history_states_.size() < history_.size()) {
        browser_history_states_.resize(history_.size());
    }
    if (history_.index() >= browser_history_states_.size()) return;
    if (auto state = current_or_pending_browser_view_state()) {
        browser_history_states_[history_.index()] = std::move(*state);
    }
}

std::optional<MainWindow::BrowserViewState> MainWindow::saved_history_view_state() const {
    if (history_.index() >= browser_history_states_.size()) return std::nullopt;
    return browser_history_states_[history_.index()];
}

std::vector<fs::path> MainWindow::selected_filesystem_paths() const {
    std::vector<fs::path> result;
    for (int index : selected_browser_indices()) {
        const auto& item = browser_items_[index];
        if (!item.filesystem_path.empty() && !item.is_parent()) {
            result.push_back(item.filesystem_path);
        }
    }
    return result;
}

std::vector<std::string> MainWindow::selected_archive_paths() const {
    std::vector<std::string> result;
    for (int index : selected_browser_indices()) {
        const auto& item = browser_items_[index];
        if (!item.is_parent() && !item.archive_path.empty()) {
            result.push_back(item.archive_path);
        }
    }
    return result;
}

std::wstring MainWindow::display_path_for_item(const axiom::gui::BrowserItem& item) const {
    if (!item.filesystem_path.empty()) return item.filesystem_path.wstring();
    if (!item.archive_path.empty() &&
        history_.current().kind == axiom::gui::BrowserLocationKind::archive) {
        return axiom::gui::BrowserLocation::archive(
            history_.current().archive_path, item.archive_path).display_name();
    }
    return {};
}

void MainWindow::navigate_to(axiom::gui::BrowserLocation location, bool record_history) {
    if (!prepare_archive_password(location)) return;
    std::optional<BrowserViewState> restore_state;
    const bool same_displayed_location =
        displayed_browser_location_ && *displayed_browser_location_ == location;
    if (record_history) {
        remember_current_history_view_state();
    }
    if (same_displayed_location) {
        restore_state = capture_browser_view_state();
    }

    cancel_browser_table_population();
    pending_browser_view_state_.reset();
    directory_watcher_.stop();
    KillTimer(hwnd_, kDirectoryRefreshTimer);
    if (record_history) {
        history_.navigate(location);
        browser_history_states_.resize(history_.size());
    } else if (!restore_state) {
        restore_state = saved_history_view_state();
    }
    pending_browser_view_state_ = std::move(restore_state);
    update_navigation_buttons();
    set_text(address_edit_, location.display_name());
    remember_address(location.display_name());
    table_.clear();
    browser_items_.clear();
    set_status(L"Loading " + location.display_name() + L"...");

    const std::uint64_t generation = ++browser_generation_;
    auto catalog = archive_catalog_;
    std::string archive_password;
    if (location.kind == axiom::gui::BrowserLocationKind::archive &&
        !archive_password_path_.empty() &&
        same_filesystem_path(archive_password_path_, location.archive_path)) {
        archive_password = archive_password_;
    } else if (location.kind == axiom::gui::BrowserLocationKind::archive &&
               !pending_archive_password_.empty()) {
        archive_password = pending_archive_password_;
        secure_clear(pending_archive_password_);
    }
    HWND target = hwnd_;
    browser_thread_ = std::jthread(
        [target, generation, location = std::move(location), catalog = std::move(catalog),
         archive_password = std::move(archive_password)](
            std::stop_token stop) mutable {
            auto result = std::make_unique<axiom::gui::BrowserLoadResult>(
                axiom::gui::load_browser_location(location, generation, std::move(catalog), stop,
                                                  archive_password));
            secure_clear(archive_password);
            if (stop.stop_requested()) return;
            auto* payload = result.release();
            if (!PostMessageW(target, kBrowserLoadedMessage, 0,
                              reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        });
}

void MainWindow::sort_browser_items_for_table() {
    const auto rank = [](const axiom::gui::BrowserItem& item) {
        if (item.kind == axiom::gui::BrowserItemKind::parent) return 0;
        if (item.kind == axiom::gui::BrowserItemKind::drive ||
            item.kind == axiom::gui::BrowserItemKind::directory) return 1;
        return 2;
    };
    std::stable_sort(browser_items_.begin(), browser_items_.end(),
                     [&](const auto& left, const auto& right) {
        if (rank(left) != rank(right)) return rank(left) < rank(right);
        int comparison = 0;
        switch (sort_column_) {
            case 1:
                comparison = left.size < right.size ? -1 : (left.size > right.size ? 1 : 0);
                break;
            case 2: {
                const auto left_size = left.packed_size.value_or(0);
                const auto right_size = right.packed_size.value_or(0);
                comparison = left_size < right_size ? -1 : (left_size > right_size ? 1 : 0);
                break;
            }
            case 3: comparison = _wcsicmp(left.type.c_str(), right.type.c_str()); break;
            case 4: comparison = _wcsicmp(left.modified.c_str(), right.modified.c_str()); break;
            case 5:
                comparison = left.crc32.value_or(0) < right.crc32.value_or(0) ? -1
                    : (left.crc32.value_or(0) > right.crc32.value_or(0) ? 1 : 0);
                break;
            case 6: comparison = _wcsicmp(left.attributes.c_str(), right.attributes.c_str()); break;
            default: comparison = _wcsicmp(left.name.c_str(), right.name.c_str()); break;
        }
        if (comparison == 0) comparison = _wcsicmp(left.name.c_str(), right.name.c_str());
        return sort_ascending_ ? comparison < 0 : comparison > 0;
    });
}

std::vector<std::wstring> MainWindow::browser_row_for_item(
    const axiom::gui::BrowserItem& item) const {
    const bool show_size = item.kind == axiom::gui::BrowserItemKind::file ||
                           item.kind == axiom::gui::BrowserItemKind::archive ||
                           item.kind == axiom::gui::BrowserItemKind::drive;
    return {
        item.name,
        show_size ? format_size(item.size) : L"",
        format_packed_size(item.packed_size, item.packed_size_estimated),
        item.type,
        item.modified,
        format_crc32(item.crc32),
        item.attributes,
    };
}

bool MainWindow::append_browser_table_batch() {
    if (!table_population_active_ ||
        table_population_generation_ != browser_generation_) {
        return true;
    }
    constexpr std::size_t kBrowserPopulateBatchSize = 512;
    constexpr std::size_t kGenericIconThreshold = 512;
    const std::size_t first = table_population_next_;
    const std::size_t last = std::min(browser_items_.size(),
                                      first + kBrowserPopulateBatchSize);
    std::vector<std::vector<std::wstring>> rows;
    std::vector<int> icon_indices;
    rows.reserve(last - first);
    icon_indices.reserve(last - first);
    HIMAGELIST image_list = nullptr;
    const bool prefer_generic_unique_icons =
        browser_items_.size() >= kGenericIconThreshold;
    for (std::size_t index = first; index < last; ++index) {
        const auto& item = browser_items_[index];
        const ShellIconRef icon =
            cached_shell_icon_for_item(item, prefer_generic_unique_icons);
        if (image_list == nullptr && icon.image_list != nullptr) image_list = icon.image_list;
        rows.push_back(browser_row_for_item(item));
        icon_indices.push_back(icon.index);
    }
    table_.append_rows(std::move(rows), std::move(icon_indices), image_list);
    table_population_next_ = last;
    return table_population_next_ >= browser_items_.size();
}

void MainWindow::cancel_browser_table_population() {
    KillTimer(hwnd_, kBrowserPopulateTimer);
    table_population_active_ = false;
    table_population_next_ = 0;
    table_population_generation_ = 0;
    pending_table_restore_state_.reset();
}

void MainWindow::finish_browser_table_population() {
    KillTimer(hwnd_, kBrowserPopulateTimer);
    table_population_active_ = false;
    if (pending_table_restore_state_) {
        restore_browser_view_state(*pending_table_restore_state_);
        pending_table_restore_state_.reset();
    }
    update_browser_status_for_current_selection();
}

void MainWindow::begin_browser_table_population(std::optional<BrowserViewState> restore_state) {
    cancel_browser_table_population();
    sort_browser_items_for_table();
    table_.set_sort_indicator(sort_column_, sort_ascending_);
    table_.clear();
    table_.reserve_rows(browser_items_.size());
    pending_table_restore_state_ = std::move(restore_state);
    table_population_next_ = 0;
    table_population_generation_ = browser_generation_;
    table_population_active_ = true;
    if (browser_items_.empty() || append_browser_table_batch()) {
        finish_browser_table_population();
        return;
    }
    SetTimer(hwnd_, kBrowserPopulateTimer, 1, nullptr);
}

void MainWindow::on_browser_populate_timer() {
    if (!table_population_active_) {
        KillTimer(hwnd_, kBrowserPopulateTimer);
        return;
    }
    if (append_browser_table_batch()) {
        finish_browser_table_population();
    }
}

void MainWindow::on_table_sort(int column) {
    BrowserViewState state = table_population_active_ && pending_table_restore_state_
        ? *pending_table_restore_state_
        : capture_browser_view_state();
    if (column == sort_column_) {
        sort_ascending_ = !sort_ascending_;
    } else {
        sort_column_ = column;
        sort_ascending_ = true;
    }
    begin_browser_table_population(std::move(state));
}

void MainWindow::on_browser_loaded(LPARAM lparam) {
    std::unique_ptr<axiom::gui::BrowserLoadResult> result(
        reinterpret_cast<axiom::gui::BrowserLoadResult*>(lparam));
    if (!result || result->snapshot.generation != browser_generation_) return;
    const auto loaded_location = result->snapshot.location;

    if (result->archive_catalog) archive_catalog_ = std::move(result->archive_catalog);
    browser_items_ = std::move(result->snapshot.items);
    if (!application_options_.show_parent_entry) {
        std::erase_if(browser_items_, [](const auto& item) { return item.is_parent(); });
    }
    if (!application_options_.show_hidden) {
        std::erase_if(browser_items_, [](const auto& item) {
            return item.attributes.find(L'H') != std::wstring::npos ||
                   item.attributes.find(L'S') != std::wstring::npos;
        });
    }
    std::optional<BrowserViewState> restore_state;
    if (pending_browser_view_state_) {
        restore_state = std::move(pending_browser_view_state_);
        pending_browser_view_state_.reset();
    }
    std::optional<BrowserViewState> tree_restore_state = restore_state;
    begin_browser_table_population(std::move(restore_state));
    displayed_browser_location_ = loaded_location;
    if (!result->snapshot.error.empty()) {
        set_status(L"Cannot read location: " + result->snapshot.error);
    } else {
        if (result->snapshot.location.kind == axiom::gui::BrowserLocationKind::filesystem) {
            directory_watcher_.start(result->snapshot.location.filesystem_path, hwnd_,
                                     kDirectoryChangedMessage);
        }
    }
    update_navigation_buttons();
    sync_tree_to_location(loaded_location);
    if (tree_restore_state) {
        tree_view_.restore_state(tree_restore_state->tree);
    }
}

void MainWindow::on_navigate_back() {
    remember_current_history_view_state();
    if (auto location = history_.back()) navigate_to(*location, false);
}

void MainWindow::on_navigate_forward() {
    remember_current_history_view_state();
    if (auto location = history_.forward()) navigate_to(*location, false);
}

void MainWindow::on_navigate_up() {
    if (auto location = parent_location(history_.current())) navigate_to(*location);
}

void MainWindow::on_navigate_refresh() {
    navigate_to(history_.current(), false);
}

bool MainWindow::launch_viewed_archive_file(const fs::path& file, const fs::path& staging_root,
                                bool sensitive_temp) {
    if (application_options_.warn_executable_open && has_executable_extension(file)) {
        if (show_app_message(
                L"This archive entry is executable or script-like:\n\n" +
                    file.filename().wstring() +
                    L"\n\nOnly open it if you trust the archive.",
                axiom::gui::MessageDialogIcon::warning,
                L"Open archive file",
                axiom::gui::MessageDialogButtons::yes_no,
                IDNO) != IDYES) {
            return false;
        }
    }

    std::wstring parameters;
    const std::wstring directory = file.parent_path().wstring();
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.hwnd = hwnd_;
    execute.nShow = SW_SHOWNORMAL;
    const std::wstring& external_tool = !application_options_.external_viewer.empty()
        ? application_options_.external_viewer
        : application_options_.external_editor;
    if (!external_tool.empty()) {
        parameters = quote_argument(file);
        execute.lpFile = external_tool.c_str();
        execute.lpParameters = parameters.c_str();
        execute.lpDirectory = directory.c_str();
    } else {
        execute.lpVerb = L"open";
        execute.lpFile = file.c_str();
        execute.lpDirectory = directory.c_str();
    }
    if (!ShellExecuteExW(&execute)) {
        throw std::runtime_error("Windows could not open this file type");
    }

    if (!application_options_.keep_viewed_files_until_exit && !staging_root.empty()) {
        const bool wipe = sensitive_temp && application_options_.wipe_encrypted_temp_files;
        const fs::path cleanup_root = staging_root;
        HANDLE process = execute.hProcess;
        std::thread([cleanup_root, wipe, process] {
            if (process != nullptr) {
                WaitForSingleObject(process, INFINITE);
                CloseHandle(process);
            } else {
                Sleep(10000);
            }
            remove_temp_directory(cleanup_root, wipe);
        }).detach();
    } else if (execute.hProcess != nullptr) {
        CloseHandle(execute.hProcess);
    }
    return true;
}

void MainWindow::activate_browser_item(int index) {
    if (index < 0 || index >= static_cast<int>(browser_items_.size())) return;
    const auto& item = browser_items_[index];
    if (item.is_parent()) {
        on_navigate_up();
    } else if (item.kind == axiom::gui::BrowserItemKind::drive ||
               item.kind == axiom::gui::BrowserItemKind::directory) {
        if (history_.current().kind == axiom::gui::BrowserLocationKind::archive) {
            navigate_to(axiom::gui::BrowserLocation::archive(
                history_.current().archive_path, item.archive_path));
        } else {
            navigate_to(axiom::gui::BrowserLocation::filesystem(item.filesystem_path));
        }
    } else if (item.kind == axiom::gui::BrowserItemKind::archive) {
        open_archive_path(item.filesystem_path);
    } else if (!item.filesystem_path.empty()) {
        if (joined_archive_path_for_volume(item.filesystem_path) &&
            open_archive_path(item.filesystem_path)) {
            return;
        }
        if (open_archive_path(item.filesystem_path)) {
            return;
        }
        ShellExecuteW(hwnd_, L"open", item.filesystem_path.c_str(), nullptr,
                      item.filesystem_path.parent_path().c_str(), SW_SHOWNORMAL);
    } else if (history_.current().kind == axiom::gui::BrowserLocationKind::archive &&
               !item.archive_path.empty()) {
        const fs::path archive = history_.current().archive_path;
        if (application_options_.file_open_mode == 1 &&
            show_app_message(L"Extract this archive entry to a temporary folder and open it?\n\n" +
                                 item.name,
                             axiom::gui::MessageDialogIcon::question,
                             L"Open archive file",
                             axiom::gui::MessageDialogButtons::yes_no,
                             IDYES) != IDYES) {
            return;
        }
        auto password = password_for_archive_edit(archive);
        if (!password) return;
        try {
            const bool sensitive_temp = !password->empty();
            const auto staged = extract_archive_entries_to_staging(
                archive, {item.archive_path}, *password, false, sensitive_temp);
            secure_clear(*password);
            if (staged.paths.empty()) {
                throw std::runtime_error("the archive file was not extracted");
            }
            if (launch_viewed_archive_file(staged.paths.front(), staged.directory,
                                           sensitive_temp)) {
                set_status(L"Opened " + item.name + L" from the archive.");
            }
        } catch (const axiom::OperationCancelled&) {
            secure_clear(*password);
            set_status(L"Opening the archive file was cancelled.");
        } catch (const std::exception& error) {
            secure_clear(*password);
            show_app_message(widen(error.what()), axiom::gui::MessageDialogIcon::error,
                             L"Open archive file");
        }
    }
}

void MainWindow::on_table_activate() {
    activate_browser_item(table_.focused_index());
}

void MainWindow::on_table_selection_changed() {
    const auto indices = selected_browser_indices();
    if (indices.empty()) {
        update_browser_status();
    } else {
        update_browser_status(&indices);
    }
    update_toolbar_button_states();
}

void MainWindow::on_select_all() {
    table_.select_all();
}

}  // namespace axiom::gui
