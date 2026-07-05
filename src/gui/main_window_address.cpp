// MainWindow address bar: the location dropdown, recent locations,
// and favorites.

#include "gui/main_window_internal.hpp"

namespace axiom::gui {

void MainWindow::add_address_entry(std::wstring label, std::wstring value,
                       ShellIconRef icon) {
    if (value.empty()) return;
    const auto duplicate = std::find_if(
        address_entries_.begin(), address_entries_.end(),
        [&](const AddressEntry& entry) {
            return CompareStringOrdinal(entry.value.c_str(), -1,
                                        value.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
    if (duplicate != address_entries_.end()) return;
    const LRESULT item = SendMessageW(
        address_edit_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    if (item == CB_ERR || item == CB_ERRSPACE) return;
    const std::size_t entry_index = address_entries_.size();
    address_entries_.push_back({std::move(label), std::move(value), icon});
    SendMessageW(address_edit_, CB_SETITEMDATA, static_cast<WPARAM>(item),
                 static_cast<LPARAM>(entry_index));
}

void MainWindow::add_known_address(const wchar_t* label, REFKNOWNFOLDERID folder_id) {
    if (const auto path = known_folder_path(folder_id)) {
        add_address_entry(label, path->wstring(), shell_icon_for_path(*path));
    }
}

bool MainWindow::same_location_text(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring MainWindow::compact_location_label(const std::wstring& value) {
    if (_wcsicmp(value.c_str(), L"This PC") == 0) return value;
    const std::wstring archive_separator = L" :: /";
    if (const auto separator = value.find(archive_separator);
        separator != std::wstring::npos) {
        const fs::path archive = value.substr(0, separator);
        std::wstring label = archive.filename().empty()
            ? archive.wstring()
            : archive.filename().wstring();
        const std::wstring inner = value.substr(separator + archive_separator.size());
        label += L" :: /";
        label += inner;
        return label;
    }
    const fs::path path(value);
    if (path.has_filename()) return path.filename().wstring();
    return value;
}

ShellIconRef MainWindow::address_icon_for_value(const std::wstring& value) const {
    if (_wcsicmp(value.c_str(), L"This PC") == 0) {
        return shell_icon_for_path(L"folder");
    }
    const std::wstring archive_separator = L" :: /";
    if (const auto separator = value.find(archive_separator);
        separator != std::wstring::npos) {
        return shell_icon_for_path(fs::path(value.substr(0, separator)));
    }
    const fs::path path(value);
    if (!path.empty()) return shell_icon_for_path(path);
    return {};
}

bool MainWindow::favorite_contains(const std::wstring& value) const {
    return std::any_of(favorite_locations_.begin(), favorite_locations_.end(),
                       [&](const std::wstring& favorite) {
                           return same_location_text(favorite, value);
                       });
}

void MainWindow::remember_limited_unique(std::vector<std::wstring>& values,
                                    std::wstring value,
                                    int limit) {
    if (value.empty()) return;
    limit = std::clamp(limit, 0, 50);
    if (limit == 0) {
        values.clear();
        return;
    }
    std::erase_if(values, [&](const std::wstring& existing) {
        return same_location_text(existing, value);
    });
    values.insert(values.begin(), std::move(value));
    if (values.size() > static_cast<std::size_t>(limit)) {
        values.resize(static_cast<std::size_t>(limit));
    }
}

void MainWindow::add_favorite_location(std::wstring value) {
    if (value.empty()) return;
    if (favorite_contains(value)) {
        set_status(L"Location is already in Favorites.");
        return;
    }
    favorite_locations_.insert(favorite_locations_.begin(), std::move(value));
    if (favorite_locations_.size() > 50) favorite_locations_.resize(50);
    save_current_settings();
    set_status(L"Added location to Favorites.");
}

void MainWindow::remove_favorite_location(const std::wstring& value) {
    const auto old_size = favorite_locations_.size();
    std::erase_if(favorite_locations_, [&](const std::wstring& favorite) {
        return same_location_text(favorite, value);
    });
    if (favorite_locations_.size() != old_size) {
        save_current_settings();
        set_status(L"Removed location from Favorites.");
    } else {
        set_status(L"Location was not in Favorites.");
    }
}

std::wstring MainWindow::current_location_value() const {
    return history_.current().display_name();
}

std::optional<std::wstring> MainWindow::tree_location_value(const DirectoryTreeNode& node) const {
    switch (node.kind) {
        case DirectoryTreeNodeKind::computer:
            return std::wstring(L"This PC");
        case DirectoryTreeNodeKind::filesystem:
            return node.filesystem_path.wstring();
        case DirectoryTreeNodeKind::archive:
            return axiom::gui::BrowserLocation::archive(node.archive_path).display_name();
        case DirectoryTreeNodeKind::archive_directory:
            return axiom::gui::BrowserLocation::archive(
                node.archive_path, node.archive_directory).display_name();
        case DirectoryTreeNodeKind::dummy:
            break;
    }
    return std::nullopt;
}

void MainWindow::populate_address_dropdown() {
    if (address_edit_ == nullptr) return;
    const std::wstring current_text = get_text(address_edit_);
    SendMessageW(address_edit_, CB_RESETCONTENT, 0, 0);
    address_entries_.clear();

    for (const std::wstring& favorite : favorite_locations_) {
        add_address_entry(L"\u2605 " + compact_location_label(favorite),
                          favorite, address_icon_for_value(favorite));
    }

    if (application_options_.show_address_shell_locations) {
        add_address_entry(L"This PC", L"This PC", shell_icon_for_path(L"folder"));
        add_known_address(L"Home", FOLDERID_Profile);
        add_known_address(L"Desktop", FOLDERID_Desktop);
        add_known_address(L"Documents", FOLDERID_Documents);
        add_known_address(L"Downloads", FOLDERID_Downloads);
        add_known_address(L"Pictures", FOLDERID_Pictures);
        add_known_address(L"OneDrive", FOLDERID_SkyDrive);
    }

    const DWORD required = GetLogicalDriveStringsW(0, nullptr);
    if (application_options_.show_address_shell_locations && required > 0) {
        std::wstring drives(static_cast<std::size_t>(required), L'\0');
        if (GetLogicalDriveStringsW(required, drives.data()) != 0) {
            for (const wchar_t* drive = drives.c_str(); *drive != L'\0';
                 drive += wcslen(drive) + 1) {
                const fs::path path(drive);
                wchar_t volume[MAX_PATH]{};
                std::wstring label;
                if (GetVolumeInformationW(drive, volume, MAX_PATH, nullptr, nullptr,
                                          nullptr, nullptr, 0) && volume[0] != L'\0') {
                    label = std::wstring(volume) + L" (" +
                            path.root_name().wstring() + L")";
                } else {
                    label = path.root_name().wstring();
                }
                add_address_entry(std::move(label), path.wstring(),
                                  shell_icon_for_path(path, true));
            }
        }
    }

    if (application_options_.show_address_recent_locations) {
        for (const std::wstring& recent : recent_addresses_) {
            add_address_entry(L"Recent: " + compact_location_label(recent),
                              recent, address_icon_for_value(recent));
        }
        for (const std::wstring& archive : recent_archives_) {
            add_address_entry(L"Archive: " + compact_location_label(archive),
                              archive, address_icon_for_value(archive));
        }
    }

    if (application_options_.show_address_archive_children) {
        const auto& location = history_.current();
        for (const auto& item : browser_items_) {
            if (item.is_parent() || !item.is_container()) continue;
            if (location.kind == axiom::gui::BrowserLocationKind::archive) {
                add_address_entry(
                    L"    " + item.name,
                    axiom::gui::BrowserLocation::archive(
                        location.archive_path, item.archive_path).display_name(),
                    shell_icon_for_item(item));
            } else if (!item.filesystem_path.empty()) {
                add_address_entry(L"    " + item.name, item.filesystem_path.wstring(),
                                  shell_icon_for_item(item));
            }
        }
    }

    SendMessageW(address_edit_, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    COMBOBOXINFO combo_info{sizeof(combo_info)};
    if (GetComboBoxInfo(address_edit_, &combo_info) && combo_info.hwndItem != nullptr) {
        SetWindowTextW(combo_info.hwndItem, current_text.c_str());
    } else {
        set_text(address_edit_, current_text);
    }
    SendMessageW(address_edit_, CB_SETMINVISIBLE,
                 static_cast<WPARAM>(std::clamp(
                     application_options_.recent_location_count, 4, 20)), 0);
}

void MainWindow::remember_address(std::wstring value) {
    remember_limited_unique(recent_addresses_, std::move(value),
                            application_options_.recent_location_count);
}

void MainWindow::remember_archive_path(const fs::path& archive) {
    if (archive.empty()) return;
    remember_limited_unique(recent_archives_, archive.wstring(),
                            application_options_.recent_location_count);
}

void MainWindow::select_address_entry() {
    const LRESULT selection = SendMessageW(address_edit_, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR) return;
    const LRESULT entry_index = SendMessageW(
        address_edit_, CB_GETITEMDATA, static_cast<WPARAM>(selection), 0);
    if (entry_index < 0 ||
        static_cast<std::size_t>(entry_index) >= address_entries_.size()) return;
    set_text(address_edit_, address_entries_[static_cast<std::size_t>(entry_index)].value);
    on_address_go();
}

void MainWindow::update_navigation_buttons() {
    EnableWindow(navigate_back_, history_.can_back());
    EnableWindow(navigate_forward_, history_.can_forward());
    EnableWindow(navigate_up_, parent_location(history_.current()).has_value());
}

void MainWindow::on_address_go() {
    std::wstring text = get_text(address_edit_);
    if (_wcsicmp(text.c_str(), L"This PC") == 0) {
        navigate_to(axiom::gui::BrowserLocation::computer());
        return;
    }
    const std::wstring separator = L" :: /";
    if (const auto split = text.find(separator); split != std::wstring::npos) {
        const fs::path archive = text.substr(0, split);
        const std::wstring inner = text.substr(split + separator.size());
        const int needed = WideCharToMultiByte(CP_UTF8, 0, inner.data(),
                                               static_cast<int>(inner.size()),
                                               nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<std::size_t>(std::max(needed, 0)), '\0');
        if (needed > 0) {
            WideCharToMultiByte(CP_UTF8, 0, inner.data(), static_cast<int>(inner.size()),
                                utf8.data(), needed, nullptr, nullptr);
        }
        navigate_to(axiom::gui::BrowserLocation::archive(archive, std::move(utf8)));
        return;
    }
    const fs::path path(text);
    std::error_code error;
    if (fs::is_regular_file(path, error) && open_archive_path(path)) {
        return;
    } else if (fs::is_directory(path, error)) {
        navigate_to(axiom::gui::BrowserLocation::filesystem(path));
    } else {
        show_app_message(L"The location does not exist or cannot be opened.",
                         axiom::gui::MessageDialogIcon::warning);
        set_text(address_edit_, history_.current().display_name());
    }
}

}  // namespace axiom::gui
