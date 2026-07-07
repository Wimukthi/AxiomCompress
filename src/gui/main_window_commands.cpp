// MainWindow menus and command handlers: context menus, clipboard
// commands, settings, shell integration, updates, and archive opening.

#include "gui/main_window_internal.hpp"

namespace axiom::gui {
namespace {

std::wstring_view shortcut_action_for_command(UINT command) {
    switch (command) {
        case kOpenArchive: return L"file.open_archive";
        case kCompressStream: return L"file.compress_stream";
        case kDecompressStream: return L"file.decompress_stream";
        case kExitApplication: return L"file.exit";
        case kAddFiles: return L"commands.add";
        case kExtract: return L"commands.extract";
        case kTest: return L"commands.test";
        case kUpdateArchive: return L"commands.update";
        case kFreshenArchive: return L"commands.freshen";
        case kSynchronizeArchive: return L"commands.synchronize";
        case kDeleteArchiveEntries: return L"commands.delete_archive_entries";
        case kRepackArchive: return L"commands.repack";
        case kSplitArchive: return L"commands.split";
        case kJoinArchive: return L"commands.join";
        case kView: return L"commands.view";
        case kDelete: return L"commands.delete";
        case kSelectAll: return L"commands.select_all";
        case kInfo: return L"tools.info";
        case kFind: return L"tools.find";
        case kBenchmark: return L"tools.benchmark";
        case kEditArchiveComment: return L"tools.edit_comment";
        case kLockArchive: return L"tools.lock";
        case kRepairArchive: return L"tools.repair";
        case kEditRecoveryRecord: return L"tools.recovery_record";
        case kGenerateSigningKey: return L"tools.generate_key";
        case kSignArchive: return L"tools.sign_archive";
        case kVerifyArchiveSignature: return L"tools.verify_signature";
        case kCreateSfx: return L"tools.create_sfx";
        case kNavigateBack: return L"navigation.back";
        case kNavigateForward: return L"navigation.forward";
        case kNavigateUp: return L"navigation.up";
        case kNavigateRefresh: return L"navigation.refresh";
        case kFocusAddress: return L"navigation.focus_address";
        case kAddressGo: return L"navigation.go_address";
        case kToggleTreePane: return L"options.toggle_tree";
        case kAddFavorite: return L"options.add_favorite";
        case kRemoveFavorite: return L"options.remove_favorite";
        case kSettings: return L"options.settings";
        case kCheckUpdates: return L"help.check_updates";
        case kAbout: return L"help.about";
        case kCopyPath: return L"clipboard.copy_path";
        case kCopyCrc32: return L"clipboard.copy_crc32";
        default: return {};
    }
}

UINT command_for_shortcut_action(std::wstring_view action) {
    if (action == L"file.open_archive") return kOpenArchive;
    if (action == L"file.compress_stream") return kCompressStream;
    if (action == L"file.decompress_stream") return kDecompressStream;
    if (action == L"file.exit") return kExitApplication;
    if (action == L"commands.add") return kAddFiles;
    if (action == L"commands.extract") return kExtract;
    if (action == L"commands.test") return kTest;
    if (action == L"commands.update") return kUpdateArchive;
    if (action == L"commands.freshen") return kFreshenArchive;
    if (action == L"commands.synchronize") return kSynchronizeArchive;
    if (action == L"commands.delete_archive_entries") return kDeleteArchiveEntries;
    if (action == L"commands.repack") return kRepackArchive;
    if (action == L"commands.split") return kSplitArchive;
    if (action == L"commands.join") return kJoinArchive;
    if (action == L"commands.view") return kView;
    if (action == L"commands.delete") return kDelete;
    if (action == L"commands.select_all") return kSelectAll;
    if (action == L"tools.info") return kInfo;
    if (action == L"tools.find") return kFind;
    if (action == L"tools.benchmark") return kBenchmark;
    if (action == L"tools.edit_comment") return kEditArchiveComment;
    if (action == L"tools.lock") return kLockArchive;
    if (action == L"tools.repair") return kRepairArchive;
    if (action == L"tools.recovery_record") return kEditRecoveryRecord;
    if (action == L"tools.generate_key") return kGenerateSigningKey;
    if (action == L"tools.sign_archive") return kSignArchive;
    if (action == L"tools.verify_signature") return kVerifyArchiveSignature;
    if (action == L"tools.create_sfx") return kCreateSfx;
    if (action == L"navigation.back") return kNavigateBack;
    if (action == L"navigation.forward") return kNavigateForward;
    if (action == L"navigation.up") return kNavigateUp;
    if (action == L"navigation.refresh") return kNavigateRefresh;
    if (action == L"navigation.focus_address") return kFocusAddress;
    if (action == L"navigation.go_address") return kAddressGo;
    if (action == L"options.toggle_tree") return kToggleTreePane;
    if (action == L"options.add_favorite") return kAddFavorite;
    if (action == L"options.remove_favorite") return kRemoveFavorite;
    if (action == L"options.settings") return kSettings;
    if (action == L"help.check_updates") return kCheckUpdates;
    if (action == L"help.about") return kAbout;
    if (action == L"clipboard.copy_path") return kCopyPath;
    if (action == L"clipboard.copy_crc32") return kCopyCrc32;
    return 0;
}

bool class_name_is(HWND window, const wchar_t* expected) {
    wchar_t class_name[64]{};
    GetClassNameW(window, class_name,
                  static_cast<int>(sizeof(class_name) / sizeof(class_name[0])));
    return lstrcmpiW(class_name, expected) == 0;
}

bool is_text_entry_control(HWND window) {
    if (window == nullptr) return false;
    if (class_name_is(window, L"Edit") || class_name_is(window, L"ComboBox")) {
        return true;
    }
    HWND parent = GetParent(window);
    return parent != nullptr && class_name_is(parent, L"ComboBox");
}

}  // namespace

std::wstring MainWindow::shortcut_for_command(UINT command) const {
    const std::wstring_view action = shortcut_action_for_command(command);
    if (action.empty()) return {};
    return effective_shortcut_for_command(application_options_.shortcut_overrides, action);
}

bool MainWindow::selected_has_crc32() const {
    for (int index : selected_browser_indices()) {
        if (index >= 0 && index < static_cast<int>(browser_items_.size()) &&
            browser_items_[static_cast<std::size_t>(index)].crc32) {
            return true;
        }
    }
    return false;
}

bool MainWindow::can_execute_shortcut_command(UINT command) const {
    const bool has_selection = !selected_browser_indices().empty();
    const bool has_selected_crc32 = selected_has_crc32();
    const bool has_archive = active_archive_path().has_value();
    const bool browsing_archive =
        history_.current().kind == axiom::gui::BrowserLocationKind::archive;
    const axiom::gui::ArchiveCapabilities capabilities = active_archive_capabilities();
    const bool archive_editable = capabilities.update && !capabilities.locked &&
                                  !capabilities.directory_encrypted;
    const std::wstring current_location = current_location_value();
    const bool favorite = favorite_contains(current_location);
    switch (command) {
        case kOpenArchive:
        case kCompressStream:
        case kDecompressStream:
        case kJoinArchive:
        case kGenerateSigningKey:
        case kBenchmark:
        case kSettings:
            return !busy_;
        case kExitApplication:
        case kAbout:
        case kCheckUpdates:
        case kFocusAddress:
        case kAddressGo:
            return true;
        case kAddFiles:
            return !busy_ && (!browsing_archive || archive_editable);
        case kExtract:
            return !busy_ && has_archive && capabilities.extract;
        case kTest:
            return !busy_ && has_archive && capabilities.test;
        case kUpdateArchive:
        case kFreshenArchive:
        case kSynchronizeArchive:
        case kRepackArchive:
            return !busy_ && has_archive && archive_editable;
        case kDeleteArchiveEntries:
            return !busy_ && browsing_archive && has_selection && archive_editable;
        case kEditArchiveComment:
            return !busy_ && has_archive && capabilities.comments && archive_editable;
        case kLockArchive:
            return !busy_ && has_archive && capabilities.lock && archive_editable;
        case kRepairArchive:
            return !busy_ && has_archive && capabilities.recovery_records;
        case kEditRecoveryRecord:
            return !busy_ && has_archive && capabilities.recovery_records && archive_editable;
        case kSplitArchive:
            return !busy_ && has_archive && capabilities.multi_volume;
        case kSignArchive:
            return !busy_ && has_archive && capabilities.authenticity && archive_editable;
        case kVerifyArchiveSignature:
            return !busy_ && has_archive && capabilities.authenticity;
        case kCreateSfx:
            return !busy_ && has_archive && capabilities.sfx;
        case kView:
            return has_selection;
        case kDelete:
            return !busy_ && has_selection && (!browsing_archive || archive_editable);
        case kSelectAll:
            return !browser_items_.empty();
        case kInfo:
            return has_archive || has_selection;
        case kFind:
            return !browser_items_.empty();
        case kCopyPath:
            return has_selection ||
                   history_.current().kind != axiom::gui::BrowserLocationKind::computer;
        case kCopyCrc32:
            return has_selected_crc32;
        case kNavigateBack:
            return history_.can_back();
        case kNavigateForward:
            return history_.can_forward();
        case kNavigateUp:
            return parent_location(history_.current()).has_value();
        case kNavigateRefresh:
            return !busy_;
        case kToggleTreePane:
            return true;
        case kAddFavorite:
            return !current_location.empty() && !favorite;
        case kRemoveFavorite:
            return !current_location.empty() && favorite;
        default:
            return false;
    }
}

void MainWindow::focus_address_bar() {
    if (address_edit_ == nullptr) return;
    SetFocus(address_edit_);
    COMBOBOXINFO info{sizeof(info)};
    if (GetComboBoxInfo(address_edit_, &info) && info.hwndItem != nullptr) {
        SetFocus(info.hwndItem);
        SendMessageW(info.hwndItem, EM_SETSEL, 0, -1);
    }
}

bool MainWindow::shortcut_reserved_for_focused_control(
    UINT command,
    const axiom::gui::KeyboardShortcut& shortcut,
    HWND target) const {
    if (!is_text_entry_control(target)) return false;
    if (command == kFocusAddress) return false;
    if (command == kAddressGo) {
        HWND address = GetDlgItem(hwnd_, kAddressEdit);
        return !(address != nullptr && (target == address || IsChild(address, target)));
    }
    if (shortcut.key == VK_RETURN || shortcut.key == VK_DELETE ||
        shortcut.key == VK_BACK || shortcut.key == VK_TAB) {
        return true;
    }
    if (shortcut.ctrl && !shortcut.alt && !shortcut.shift) {
        switch (shortcut.key) {
            case 'A':
            case 'C':
            case 'V':
            case 'X':
            case 'Z':
                return true;
            default:
                break;
        }
    }
    return false;
}

bool MainWindow::translate_keyboard_shortcut(const MSG& message) {
    const auto pressed = keyboard_shortcut_from_message(message);
    if (!pressed) return false;
    const HWND root = GetAncestor(message.hwnd, GA_ROOT);
    if (root != hwnd_) return false;

    for (const auto& shortcut_command : kShortcutCommandCatalog) {
        const std::wstring shortcut_text = effective_shortcut_for_command(
            application_options_.shortcut_overrides, shortcut_command.id);
        const auto shortcut = parse_keyboard_shortcut(shortcut_text);
        if (!shortcut || shortcut->key == 0 || *shortcut != *pressed) continue;

        const UINT command = command_for_shortcut_action(shortcut_command.id);
        if (command == 0) return false;
        if (command == kAddressGo) {
            HWND address = GetDlgItem(hwnd_, kAddressEdit);
            if (address == nullptr ||
                (message.hwnd != address && !IsChild(address, message.hwnd))) {
                continue;
            }
        }
        if (shortcut_reserved_for_focused_control(command, *shortcut, message.hwnd)) {
            continue;
        }
        if (!can_execute_shortcut_command(command)) return true;
        SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
        return true;
    }
    return false;
}

std::vector<axiom::gui::CustomMenuItem> MainWindow::menu_items(UINT menu_id) const {
    const bool has_selection = !selected_browser_indices().empty();
    const bool has_archive = active_archive_path().has_value();
    const bool browsing_archive =
        history_.current().kind == axiom::gui::BrowserLocationKind::archive;
    const axiom::gui::ArchiveCapabilities capabilities = active_archive_capabilities();
    const bool archive_editable = capabilities.update && !capabilities.locked &&
                                  !capabilities.directory_encrypted;
    const std::wstring current_location = current_location_value();
    const bool favorite = favorite_contains(current_location);
    const auto shortcut = [this](UINT command) { return shortcut_for_command(command); };
    switch (menu_id) {
        case kMenuFile:
            return {
                {kOpenArchive, L"&Open archive...", shortcut(kOpenArchive), !busy_},
                {0, L"", L"", false, true},
                {kCompressStream, L"&Compress single file...", shortcut(kCompressStream), !busy_},
                {kDecompressStream, L"&Decompress stream...", shortcut(kDecompressStream), !busy_},
                {0, L"", L"", false, true},
                {kExitApplication, L"E&xit", shortcut(kExitApplication)},
            };
        case kMenuCommands:
            return {
                {kAddFiles, L"&Add to archive...", shortcut(kAddFiles),
                 !busy_ && (!browsing_archive || archive_editable)},
                {kExtract, L"&Extract...", shortcut(kExtract),
                 !busy_ && has_archive && capabilities.extract},
                {kTest, L"&Test archive", shortcut(kTest),
                 !busy_ && has_archive && capabilities.test},
                {0, L"", L"", false, true},
                {kUpdateArchive, L"&Update archive...", shortcut(kUpdateArchive),
                  !busy_ && has_archive && archive_editable},
                {kFreshenArchive, L"&Freshen archive...", shortcut(kFreshenArchive),
                 !busy_ && has_archive && archive_editable},
                {kSynchronizeArchive, L"S&ynchronize archive...", shortcut(kSynchronizeArchive),
                  !busy_ && has_archive && archive_editable},
                {kDeleteArchiveEntries, L"Delete from archive...", shortcut(kDeleteArchiveEntries),
                  !busy_ && browsing_archive && has_selection && archive_editable},
                {kRepackArchive, L"&Repack archive...", shortcut(kRepackArchive),
                  !busy_ && has_archive && archive_editable},
                {kSplitArchive, L"S&plit archive...", shortcut(kSplitArchive),
                 !busy_ && has_archive && capabilities.multi_volume},
                {kJoinArchive, L"&Join archive volumes...", shortcut(kJoinArchive), !busy_},
                {0, L"", L"", false, true},
                {kView, L"&View", shortcut(kView), has_selection},
                {kDelete, browsing_archive ? L"&Delete from archive" : L"&Delete",
                 shortcut(kDelete), !busy_ && has_selection &&
                      (!browsing_archive || archive_editable)},
                {kSelectAll, L"Select &all", shortcut(kSelectAll), !browser_items_.empty()},
            };
        case kMenuTools:
            return {
                {kInfo, L"Archive &information", shortcut(kInfo), has_archive || has_selection},
                {kFind, L"&Find files...", shortcut(kFind), !browser_items_.empty()},
                {0, L"", L"", false, true},
                {kBenchmark, L"&Benchmark...", shortcut(kBenchmark), !busy_},
                {0, L"", L"", false, true},
                {kEditArchiveComment, L"Edit archive &comment...", shortcut(kEditArchiveComment),
                  !busy_ && has_archive && capabilities.comments && archive_editable},
                {kLockArchive, L"&Lock archive...", shortcut(kLockArchive),
                  !busy_ && has_archive && capabilities.lock && archive_editable},
                {kRepairArchive, L"&Repair archive...", shortcut(kRepairArchive),
                 !busy_ && has_archive && capabilities.recovery_records},
                {kEditRecoveryRecord, L"Recovery &record...", shortcut(kEditRecoveryRecord),
                 !busy_ && has_archive && capabilities.recovery_records && archive_editable},
                {0, L"", L"", false, true},
                {kGenerateSigningKey, L"&Generate signing key...", shortcut(kGenerateSigningKey),
                 !busy_},
                {kSignArchive, L"&Sign archive...", shortcut(kSignArchive),
                 !busy_ && has_archive && capabilities.authenticity && archive_editable},
                {kVerifyArchiveSignature, L"Verify &signature...", shortcut(kVerifyArchiveSignature),
                 !busy_ && has_archive && capabilities.authenticity},
                {kCreateSfx, L"Create &self-extracting archive...", shortcut(kCreateSfx),
                 !busy_ && has_archive && capabilities.sfx},
            };
        case kMenuOptions:
            return {
                {kToggleTreePane, L"Show &tree pane", shortcut(kToggleTreePane), true, false,
                 tree_pane_visible_},
                {0, L"", L"", false, true},
                {static_cast<UINT>(favorite ? kRemoveFavorite : kAddFavorite),
                 favorite ? L"Remove current location from &Favorites"
                          : L"Pin current location to &Favorites",
                 shortcut(favorite ? kRemoveFavorite : kAddFavorite), !current_location.empty()},
                {0, L"", L"", false, true},
                {kSettings, L"&Settings...", shortcut(kSettings), !busy_},
            };
        case kMenuHelp:
            return {
                {kCheckUpdates, L"Check for &Updates...", shortcut(kCheckUpdates)},
                {0, L"", L"", false, true},
                {kAbout, L"&About Axiom", shortcut(kAbout)},
            };
        default:
            return {};
    }
}

void MainWindow::show_browser_context_menu(POINT point) {
    const bool has_selection = !selected_browser_indices().empty();
    const bool has_selected_crc32 = selected_has_crc32();
    const bool has_archive = active_archive_path().has_value();
    const bool browsing_archive =
        history_.current().kind == axiom::gui::BrowserLocationKind::archive;
    const axiom::gui::ArchiveCapabilities capabilities = active_archive_capabilities();
    const bool archive_editable = capabilities.update && !capabilities.locked &&
                                  !capabilities.directory_encrypted;
    const std::wstring current_location = current_location_value();
    const bool favorite = favorite_contains(current_location);
    if (point.x == -1 && point.y == -1) {
        RECT list_rect{};
        GetWindowRect(list_, &list_rect);
        point = {list_rect.left + scale(24), list_rect.top + scale(24)};
    }
    const auto shortcut = [this](UINT command) { return shortcut_for_command(command); };
    std::vector<axiom::gui::CustomMenuItem> items{
        {kView, L"&View", shortcut(kView), has_selection},
        {0, L"", L"", false, true},
        {kAddFiles, L"&Add to archive...", shortcut(kAddFiles),
         !busy_ && has_selection && (!browsing_archive || archive_editable)},
        {kExtract, L"&Extract...", shortcut(kExtract),
         !busy_ && has_archive && capabilities.extract},
        {kTest, L"&Test archive", shortcut(kTest),
         !busy_ && has_archive && capabilities.test},
        {0, L"", L"", false, true},
        {kDelete, browsing_archive ? L"&Delete from archive" : L"&Delete", shortcut(kDelete),
         !busy_ && has_selection && (!browsing_archive || archive_editable)},
        {kInfo, L"&Information", shortcut(kInfo), has_selection || has_archive},
        {kFind, L"&Find files...", shortcut(kFind), !browser_items_.empty()},
        {kSelectAll, L"Select &all", shortcut(kSelectAll), !browser_items_.empty()},
        {0, L"", L"", false, true},
        {kCopyPath, L"Copy &path", shortcut(kCopyPath), has_selection ||
             history_.current().kind != axiom::gui::BrowserLocationKind::computer},
        {kCopyCrc32, L"Copy CRC-&32", shortcut(kCopyCrc32), has_selected_crc32},
        {0, L"", L"", false, true},
        {static_cast<UINT>(favorite ? kRemoveFavorite : kAddFavorite),
         favorite ? L"Remove current location from &Favorites"
                  : L"Pin current location to &Favorites",
         shortcut(favorite ? kRemoveFavorite : kAddFavorite), !current_location.empty()},
    };
    const UINT command = menu_bar_.show_context_menu(std::move(items), point);
    if (command != 0) SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
}

void MainWindow::show_tree_context_menu(POINT point) {
    DirectoryTreeItem* item = tree_view_.selected_item();
    if (item == nullptr) return;
    const DirectoryTreeNode& node = item->node;
    const bool is_filesystem =
        node.kind == DirectoryTreeNodeKind::filesystem ||
        node.kind == DirectoryTreeNodeKind::file ||
        node.kind == DirectoryTreeNodeKind::archive;
    std::optional<fs::path> archive_path;
    if (node.kind == DirectoryTreeNodeKind::archive) {
        archive_path = node.filesystem_path;
    } else if (node.kind == DirectoryTreeNodeKind::archive_directory) {
        archive_path = node.archive_path;
    } else if (node.kind == DirectoryTreeNodeKind::file &&
               axiom::archive_provider_for_path(node.filesystem_path) != nullptr) {
        archive_path = node.filesystem_path;
    }
    const bool is_archive = archive_path.has_value();

    if (point.x == -1 && point.y == -1) {
        RECT tree_rect{};
        GetWindowRect(tree_view_.hwnd(), &tree_rect);
        point = {tree_rect.left + scale(32), tree_rect.top + scale(32)};
    }

    axiom::gui::ArchiveCapabilities capabilities{};
    if (archive_path) {
        if (const auto* provider = axiom::archive_provider_for_path(*archive_path)) {
            try {
                capabilities = provider->capabilities(*archive_path);
            } catch (...) {
                capabilities = {};
            }
        }
    }
    std::vector<axiom::gui::CustomMenuItem> items{
        {kTreeOpen, L"&Open", shortcut_for_command(kView),
         node.kind != DirectoryTreeNodeKind::dummy},
        {kTreeRefresh, L"&Refresh tree", shortcut_for_command(kNavigateRefresh), !busy_},
        {0, L"", L"", false, true},
        {kTreeExpand, L"E&xpand", L"", item->may_have_children && !item->expanded},
        {kTreeCollapse, L"Co&llapse", L"", item->may_have_children && item->expanded},
        {0, L"", L"", false, true},
        {kTreeOpenInExplorer, L"Open in &Explorer", L"", is_filesystem},
        {kTreeAddToArchive, L"&Add to archive...", shortcut_for_command(kAddFiles),
         !busy_ && is_filesystem},
        {0, L"", L"", false, true},
        {kTreeExtractArchive, L"E&xtract archive...", shortcut_for_command(kExtract),
         !busy_ && is_archive && capabilities.extract},
        {kTreeTestArchive, L"&Test archive", shortcut_for_command(kTest),
         !busy_ && is_archive && capabilities.test},
        {kTreeArchiveInfo, L"Archive &information", shortcut_for_command(kInfo), is_archive},
    };
    if (const auto location = tree_location_value(node)) {
        const bool favorite = favorite_contains(*location);
        items.push_back({0, L"", L"", false, true});
        items.push_back({
            static_cast<UINT>(favorite ? kTreeRemoveFavorite : kTreeAddFavorite),
            favorite ? L"Remove from &Favorites" : L"Pin to &Favorites",
            shortcut_for_command(favorite ? kRemoveFavorite : kAddFavorite),
            true
        });
    }

    const UINT command = menu_bar_.show_context_menu(std::move(items), point);
    if (command == 0) return;
    switch (command) {
        case kTreeOpen:
            on_tree_selection_changed(*item);
            break;
        case kTreeRefresh:
            rebuild_directory_tree();
            break;
        case kTreeExpand:
            tree_view_.set_expanded(item, true);
            break;
        case kTreeCollapse:
            tree_view_.set_expanded(item, false);
            break;
        case kTreeOpenInExplorer:
            if (node.kind == DirectoryTreeNodeKind::archive ||
                node.kind == DirectoryTreeNodeKind::file) {
                const std::wstring params =
                    L"/select,\"" + node.filesystem_path.wstring() + L"\"";
                ShellExecuteW(hwnd_, L"open", L"explorer.exe", params.c_str(),
                              nullptr, SW_SHOWNORMAL);
            } else if (node.kind == DirectoryTreeNodeKind::filesystem) {
                ShellExecuteW(hwnd_, L"open", node.filesystem_path.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        case kTreeAddToArchive:
            if (is_filesystem) {
                create_archive_from_paths({node.filesystem_path});
            }
            break;
        case kTreeExtractArchive:
            if (archive_path) {
                navigate_to(axiom::gui::BrowserLocation::archive(*archive_path));
                on_extract();
            }
            break;
        case kTreeTestArchive:
            if (archive_path) {
                navigate_to(axiom::gui::BrowserLocation::archive(*archive_path));
                on_test();
            }
            break;
        case kTreeArchiveInfo:
            if (archive_path) {
                navigate_to(axiom::gui::BrowserLocation::archive(*archive_path));
                on_info();
            }
            break;
        case kTreeAddFavorite:
            if (const auto location = tree_location_value(node)) {
                add_favorite_location(*location);
            }
            break;
        case kTreeRemoveFavorite:
            if (const auto location = tree_location_value(node)) {
                remove_favorite_location(*location);
            }
            break;
    }
}

bool MainWindow::copy_text_to_clipboard(const std::wstring& text) const {
    if (text.empty()) return false;
    if (!OpenClipboard(hwnd_)) return false;
    struct ClipboardGuard {
        ~ClipboardGuard() { CloseClipboard(); }
    } guard;

    if (!EmptyClipboard()) return false;
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) return false;
    void* target = GlobalLock(memory);
    if (target == nullptr) {
        GlobalFree(memory);
        return false;
    }
    std::memcpy(target, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        return false;
    }
    return true;
}

std::wstring MainWindow::newline_join(const std::vector<std::wstring>& lines) {
    std::wstring text;
    for (const auto& line : lines) {
        if (line.empty()) continue;
        if (!text.empty()) text += L"\r\n";
        text += line;
    }
    return text;
}

void MainWindow::on_copy_paths() {
    std::vector<std::wstring> lines;
    for (int index : selected_browser_indices()) {
        const auto& item = browser_items_[static_cast<std::size_t>(index)];
        if (item.is_parent()) continue;
        if (auto text = display_path_for_item(item); !text.empty()) {
            lines.push_back(std::move(text));
        }
    }
    if (lines.empty()) lines.push_back(current_location_value());
    if (!copy_text_to_clipboard(newline_join(lines))) {
        show_app_message(L"Could not copy the path text to the clipboard.",
                         axiom::gui::MessageDialogIcon::error,
                         L"Copy path");
        return;
    }
    set_status(quote_count(lines.size(), L"path copied.", L"paths copied."));
}

void MainWindow::on_copy_crc32() {
    std::vector<std::wstring> lines;
    const auto selection = selected_browser_indices();
    for (int index : selection) {
        const auto& item = browser_items_[static_cast<std::size_t>(index)];
        if (!item.crc32) continue;
        std::wstring line = format_crc32(item.crc32);
        if (selection.size() > 1) {
            line = item.name + L"\t" + line;
        }
        lines.push_back(std::move(line));
    }
    if (lines.empty()) {
        show_app_message(L"No selected item has a CRC-32 value.",
                         axiom::gui::MessageDialogIcon::information,
                         L"Copy CRC-32");
        return;
    }
    if (!copy_text_to_clipboard(newline_join(lines))) {
        show_app_message(L"Could not copy the CRC-32 text to the clipboard.",
                         axiom::gui::MessageDialogIcon::error,
                         L"Copy CRC-32");
        return;
    }
    set_status(quote_count(lines.size(), L"CRC-32 copied.", L"CRC-32 values copied."));
}

void MainWindow::on_info() {
    const auto indices = selected_browser_indices();
    std::uint64_t bytes = 0;
    for (int index : indices) bytes += browser_items_[index].size;
    if (const auto archive = active_archive_path()) {
        axiom::gui::ArchiveSummaryRows details;
        details.push_back({L"Location", history_.current().display_name()});
        details.push_back({
            indices.empty() ? L"Items" : L"Selection",
            indices.empty()
                ? quote_count(browser_items_.size(), L"item", L"items")
                : quote_count(indices.size(), L"selected item", L"selected items")
        });
        if (bytes != 0) {
            details.push_back({L"Selected size", format_size(bytes)});
        }
        details.push_back({L"Archive path", archive->wstring()});
        const auto* provider = active_archive_provider();
        const auto capabilities = active_archive_capabilities();
        std::wstring archive_comment;
        std::string metadata_password;
        if (provider != nullptr) {
            details.push_back({L"Format", widen(provider->info().display_name)});
        }
        std::error_code archive_size_error;
        const auto archive_file_size = fs::file_size(*archive, archive_size_error);
        if (!archive_size_error) {
            details.push_back({L"Packed total", format_size(archive_file_size)});
        }
        if (provider != nullptr && provider->info().native) {
            try {
                const auto encryption = axiom::archive_encryption_mode(*archive);
                if (encryption == axiom::ArchiveEncryptionMode::data_and_directory) {
                    auto password = password_for_archive_edit(*archive);
                    if (!password) return;
                    metadata_password = std::move(*password);
                }
                details.push_back({
                    L"Encryption",
                    encryption != axiom::ArchiveEncryptionMode::none
                        ? L"File data encrypted"
                        : L"None"
                });
                details.push_back({
                    L"State",
                    axiom::archive_is_locked(*archive, metadata_password)
                        ? L"Locked (read-only)"
                        : L"Editable"
                });
                archive_comment = widen(axiom::archive_comment(
                    *archive, metadata_password));
            } catch (...) {
                secure_clear(metadata_password);
                details.push_back({L"State", L"Could not read archive metadata"});
            }
        } else {
            const bool can_write = capabilities.create || capabilities.update ||
                                   capabilities.delete_entries ||
                                   capabilities.move_entries;
            details.push_back({L"Provider mode", can_write ? L"Read/write" : L"Read-only"});
            details.push_back({L"Full extraction", capabilities.extract ? L"Supported" : L"Not supported"});
            details.push_back({L"Selective extraction",
                               capabilities.selective_extract ? L"Supported" : L"Not supported"});
            details.push_back({L"Integrity test", capabilities.test ? L"Supported" : L"Not supported"});
        }
        std::vector<axiom::ArchiveEntry> loaded_entries;
        const std::vector<axiom::ArchiveEntry>* entries = nullptr;
        if (archive_catalog_ && same_filesystem_path(archive_catalog_->path(), *archive)) {
            entries = &archive_catalog_->entries();
        } else if (provider != nullptr && capabilities.list) {
            try {
                loaded_entries = provider->list(*archive, metadata_password);
                entries = &loaded_entries;
            } catch (...) {
                entries = nullptr;
            }
        }
        if (entries != nullptr) {
            const auto totals = summarize_archive_entries(*entries);
            details.push_back({L"Unpacked total", format_size(totals.unpacked)});
            if (!archive_size_error) {
                details.push_back({
                    L"Overall ratio",
                    format_ratio(archive_file_size, totals.unpacked)
                });
            }
            if (totals.has_packed) {
                details.push_back({
                    totals.packed_estimated ? L"Packed data (estimated)" : L"Packed data",
                    format_packed_size(totals.packed, totals.packed_estimated)
                });
            }
        }
        secure_clear(metadata_password);
        axiom::gui::show_archive_information_dialog(
            hwnd_, *archive, details, capabilities, std::move(archive_comment));
        return;
    }
    std::wstring message = L"Location: " + history_.current().display_name() + L"\n\n";
    message += indices.empty()
        ? quote_count(browser_items_.size(), L"item", L"items")
        : quote_count(indices.size(), L"selected item", L"selected items");
    if (bytes != 0) message += L"\nSize: " + format_size(bytes);
    show_app_message(message, axiom::gui::MessageDialogIcon::information, L"Information");
}

void MainWindow::on_about() {
    axiom::gui::show_about_dialog(hwnd_, instance_, dpi_, theme_.dark, kCheckUpdates);
    const bool automatic_updates = axiom::gui::automatic_update_checks_enabled();
    if (application_options_.automatic_update_checks != automatic_updates) {
        application_options_.automatic_update_checks = automatic_updates;
        save_current_settings();
    }
}

void MainWindow::on_benchmark() {
    axiom::gui::show_benchmark_dialog(hwnd_, instance_, dpi_, theme_.dark);
}

void MainWindow::maybe_start_automatic_update_check() {
    if (axiom::gui::automatic_update_check_due()) {
        begin_update_check(axiom::gui::UpdateCheckKind::automatic);
    }
}

void MainWindow::begin_update_check(axiom::gui::UpdateCheckKind kind) {
    if (update_check_in_progress_) {
        if (kind == axiom::gui::UpdateCheckKind::manual) {
            show_app_message(L"An update check is already running.",
                             axiom::gui::MessageDialogIcon::information);
        }
        return;
    }
    update_check_in_progress_ = true;
    if (kind == axiom::gui::UpdateCheckKind::manual) {
        SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    }
    axiom::gui::start_update_check(hwnd_, instance_, kind);
}

void MainWindow::begin_update_download(const axiom::gui::UpdateInfo& update) {
    if (update_download_in_progress_) {
        show_app_message(L"An update download is already running.",
                         axiom::gui::MessageDialogIcon::information);
        return;
    }
    update_download_in_progress_ = true;
    show_app_message(L"Axiom will download the installer in the background.",
                     axiom::gui::MessageDialogIcon::information,
                     L"Axiom Update");
    axiom::gui::start_update_download(hwnd_, update);
}

void MainWindow::on_update_check_complete(LPARAM lparam) {
    std::unique_ptr<axiom::gui::UpdateCheckResult> result(
        reinterpret_cast<axiom::gui::UpdateCheckResult*>(lparam));
    update_check_in_progress_ = false;
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if (!result) return;

    const bool manual = result->kind == axiom::gui::UpdateCheckKind::manual;
    if (!result->success) {
        if (manual) {
            show_app_message(L"Could not check for updates:\n\n" + result->message,
                             axiom::gui::MessageDialogIcon::error,
                             L"Axiom Update");
        }
        return;
    }
    if (!result->update_available) {
        if (manual) {
            show_app_message(
                L"Axiom is up to date.\n\nInstalled version: " +
                    axiom::gui::current_executable_version(instance_),
                axiom::gui::MessageDialogIcon::information,
                L"Axiom Update");
        }
        return;
    }

    std::wstring message = L"Axiom " + result->update.version +
                           L" is available.\n\nDownload and install it now?";
    if (show_app_message(message, axiom::gui::MessageDialogIcon::question,
                         L"Axiom Update", axiom::gui::MessageDialogButtons::yes_no,
                         IDYES) == IDYES) {
        begin_update_download(result->update);
    }
}

void MainWindow::on_update_download_complete(LPARAM lparam) {
    std::unique_ptr<axiom::gui::UpdateDownloadResult> result(
        reinterpret_cast<axiom::gui::UpdateDownloadResult*>(lparam));
    update_download_in_progress_ = false;
    if (!result) return;
    if (!result->success) {
        show_app_message(L"Could not download the Axiom update:\n\n" + result->message,
                         axiom::gui::MessageDialogIcon::error,
                         L"Axiom Update");
        return;
    }
    if (busy_) {
        show_app_message(
            L"The update was downloaded to:\n\n" + result->installer_path +
                L"\n\nFinish or cancel the active archive operation before running it.",
            axiom::gui::MessageDialogIcon::information,
            L"Axiom Update");
        return;
    }
    const std::wstring message = L"Axiom " + result->update.version +
        L" has been downloaded.\n\nRun the installer now? Axiom will close if it starts successfully.";
    if (show_app_message(message, axiom::gui::MessageDialogIcon::question,
                         L"Axiom Update", axiom::gui::MessageDialogButtons::yes_no,
                         IDYES) != IDYES) {
        return;
    }
    SetLastError(ERROR_SUCCESS);
    const auto launch = reinterpret_cast<INT_PTR>(ShellExecuteW(
        hwnd_, L"runas", result->installer_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (launch <= 32) {
        show_app_message(L"Could not launch the installer:\n\n" +
                             axiom::gui::last_error_text(),
                         axiom::gui::MessageDialogIcon::error,
                         L"Axiom Update");
        return;
    }
    SendMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void MainWindow::on_find_files() {
    std::vector<FileSearchSourceItem> source;
    source.reserve(browser_items_.size());
    for (int index = 0; index < static_cast<int>(browser_items_.size()); ++index) {
        const auto& item = browser_items_[static_cast<std::size_t>(index)];
        if (item.is_parent()) continue;
        FileSearchSourceItem source_item;
        source_item.browser_index = index;
        source_item.kind = item.kind;
        source_item.name = item.name;
        source_item.type = item.type;
        source_item.modified = item.modified;
        if (item.kind != axiom::gui::BrowserItemKind::directory &&
            item.kind != axiom::gui::BrowserItemKind::drive) {
            source_item.size = format_size(item.size);
        }

        if (!item.filesystem_path.empty()) {
            source_item.location = item.filesystem_path.parent_path().wstring();
        } else if (!item.archive_path.empty()) {
            const auto separator = item.archive_path.find_last_of('/');
            source_item.location = separator == std::string::npos
                ? L"/"
                : L"/" + widen(std::string_view(item.archive_path).substr(0, separator));
        } else {
            source_item.location = history_.current().display_name();
        }
        source.push_back(std::move(source_item));
    }

    if (source.empty()) {
        show_app_message(L"There are no searchable items in the current file list.",
                         axiom::gui::MessageDialogIcon::information,
                         L"Find files");
        return;
    }

    FileSearchDialog dialog(instance_, theme_, dpi_,
                            L"Scope: " + history_.current().display_name(),
                            std::move(source));
    const auto result = dialog.show(hwnd_);
    if (!result.accepted || result.browser_index < 0 ||
        result.browser_index >= static_cast<int>(browser_items_.size())) {
        return;
    }
    table_.select_index(result.browser_index);
    SetFocus(list_);
}

void MainWindow::apply_application_options(
    const axiom::gui::ApplicationDialogOptions& updated_options) {
    const auto previous = application_options_;
    if (updated_options == previous) {
        return;
    }
    application_options_ = updated_options;

    selected_level_ = application_options_.default_level;
    selected_thread_count_ = application_options_.default_thread_count;
    selected_dictionary_size_ = application_options_.default_dictionary_size;
    selected_word_size_ = application_options_.default_word_size;
    selected_solid_block_size_ = application_options_.default_solid_block_size;

    if (!application_options_.cache_passwords ||
        application_options_.password_prompt_mode != previous.password_prompt_mode ||
        application_options_.cache_passwords != previous.cache_passwords) {
        clear_archive_password();
    }

    const bool theme_changed =
        application_options_.theme_mode != previous.theme_mode ||
        application_options_.accent_color_mode != previous.accent_color_mode ||
        application_options_.custom_accent_color != previous.custom_accent_color;
    const bool icon_style_changed =
        application_options_.toolbar_icon_style != previous.toolbar_icon_style;
    const bool toolbar_commands_changed =
        normalize_toolbar_commands(application_options_.toolbar_commands) !=
        normalize_toolbar_commands(previous.toolbar_commands);
    const bool table_options_changed =
        application_options_.show_grid_lines != previous.show_grid_lines ||
        application_options_.show_horizontal_scrollbar !=
            previous.show_horizontal_scrollbar ||
        application_options_.full_row_select != previous.full_row_select;
    const bool address_options_changed =
        application_options_.show_address_shell_locations !=
            previous.show_address_shell_locations ||
        application_options_.show_address_recent_locations !=
            previous.show_address_recent_locations ||
        application_options_.show_address_archive_children !=
            previous.show_address_archive_children ||
        application_options_.recent_location_count != previous.recent_location_count;
    const bool browser_content_changed =
        application_options_.show_hidden != previous.show_hidden ||
        application_options_.show_parent_entry != previous.show_parent_entry;
    const bool temp_options_changed =
        application_options_.temp_folder_mode != previous.temp_folder_mode ||
        application_options_.temp_folder != previous.temp_folder ||
        application_options_.temp_cleanup_days != previous.temp_cleanup_days ||
        application_options_.wipe_encrypted_temp_files !=
            previous.wipe_encrypted_temp_files;
    const bool shell_options_changed =
        application_options_.associate_axar != previous.associate_axar ||
        application_options_.associate_zip != previous.associate_zip ||
        application_options_.associate_7z != previous.associate_7z ||
        application_options_.associate_rar != previous.associate_rar ||
        application_options_.associate_tar != previous.associate_tar ||
        application_options_.associate_iso != previous.associate_iso ||
        application_options_.associate_cab != previous.associate_cab ||
        application_options_.context_open != previous.context_open ||
        application_options_.context_add != previous.context_add ||
        application_options_.context_extract != previous.context_extract ||
        application_options_.context_test != previous.context_test;

    if (theme_changed || icon_style_changed) apply_theme();
    if (toolbar_commands_changed) {
        layout();
        update_toolbar_button_states();
    }
    if (table_options_changed) apply_table_options();
    if (address_options_changed) {
        const int limit = std::clamp(application_options_.recent_location_count, 0, 50);
        if (limit == 0) {
            recent_addresses_.clear();
            recent_archives_.clear();
        } else if (recent_addresses_.size() > static_cast<std::size_t>(limit)) {
            recent_addresses_.resize(static_cast<std::size_t>(limit));
            if (recent_archives_.size() > static_cast<std::size_t>(limit)) {
                recent_archives_.resize(static_cast<std::size_t>(limit));
            }
        } else if (recent_archives_.size() > static_cast<std::size_t>(limit)) {
            recent_archives_.resize(static_cast<std::size_t>(limit));
        }
        populate_address_dropdown();
    }
    if (temp_options_changed) cleanup_old_temp_directories();
    if (shell_options_changed) apply_shell_integration();

    save_current_settings();
    if (browser_content_changed) on_navigate_refresh();
}

void MainWindow::on_settings() {
    auto dialog_options = application_options_;
    if (!axiom::gui::show_application_settings_dialog(
            hwnd_, dialog_options,
            [this](const axiom::gui::ApplicationDialogOptions& applied_options) {
                apply_application_options(applied_options);
            })) {
        return;
    }
    apply_application_options(dialog_options);
}

void MainWindow::apply_shell_integration() const {
    const fs::path executable = current_executable_path();
    if (executable.empty()) return;
    const std::wstring classes = L"Software\\Classes\\";
    const std::wstring file_context = classes + L"*\\shell\\Axiom";
    const std::wstring directory_context = classes + L"Directory\\shell\\Axiom";
    const std::wstring file_subcommands_id = L"AxiomCompress.ContextMenu.File";
    const std::wstring directory_subcommands_id = L"AxiomCompress.ContextMenu.Directory";
    const std::wstring file_subcommands = classes + file_subcommands_id;
    const std::wstring directory_subcommands = classes + directory_subcommands_id;
    const std::wstring axar_context = classes + L"SystemFileAssociations\\.axar\\shell\\";

    const auto icon_value = [&](int icon_id = IDI_AXIOM) {
        if (icon_id == IDI_AXIOM) return quote_argument(executable);
        return quote_argument(executable) + L",-" + std::to_wstring(icon_id);
    };
    const auto archive_applies_to = [] {
        return std::wstring(
            L"System.FileExtension:=\".axar\" OR "
            L"System.FileExtension:=\".zip\" OR "
            L"System.FileExtension:=\".jar\" OR "
            L"System.FileExtension:=\".war\" OR "
            L"System.FileExtension:=\".apk\" OR "
            L"System.FileExtension:=\".7z\" OR "
            L"System.FileExtension:=\".rar\" OR "
            L"System.FileExtension:=\".r00\" OR "
            L"System.FileExtension:=\".r01\" OR "
            L"System.FileExtension:=\".r02\" OR "
            L"System.FileExtension:=\".r03\" OR "
            L"System.FileExtension:=\".r04\" OR "
            L"System.FileExtension:=\".r05\" OR "
            L"System.FileExtension:=\".r06\" OR "
            L"System.FileExtension:=\".r07\" OR "
            L"System.FileExtension:=\".r08\" OR "
            L"System.FileExtension:=\".r09\" OR "
            L"System.FileExtension:=\".tar\" OR "
            L"System.FileExtension:=\".gz\" OR "
            L"System.FileExtension:=\".xz\" OR "
            L"System.FileExtension:=\".bz2\" OR "
            L"System.FileExtension:=\".zst\" OR "
            L"System.FileExtension:=\".tgz\" OR "
            L"System.FileExtension:=\".txz\" OR "
            L"System.FileExtension:=\".tbz2\" OR "
            L"System.FileExtension:=\".tzst\" OR "
            L"System.FileExtension:=\".iso\" OR "
            L"System.FileExtension:=\".cab\"");
    };
    const auto apply_shell_subcommand =
        [&](const std::wstring& parent, const std::wstring& order,
            const std::wstring& label, const std::wstring& command,
            int icon_id, const std::wstring& applies_to = {}) {
            const std::wstring key = parent + L"\\shell\\" + order;
            set_registry_string(HKEY_CURRENT_USER, key, nullptr, label);
            set_registry_string(HKEY_CURRENT_USER, key, L"MUIVerb", label);
            set_registry_string(HKEY_CURRENT_USER, key, L"Icon", icon_value(icon_id));
            if (!applies_to.empty()) {
                set_registry_string(HKEY_CURRENT_USER, key, L"AppliesTo", applies_to);
            }
            set_registry_string(HKEY_CURRENT_USER, key + L"\\command", nullptr, command);
        };
    const auto apply_shell_submenu =
        [&](const std::wstring& parent, const std::wstring& subcommands_id,
            const std::wstring& subcommands_key, bool enabled,
            const std::wstring& applies_to = {}) {
            delete_registry_tree(HKEY_CURRENT_USER, parent);
            delete_registry_tree(HKEY_CURRENT_USER, subcommands_key);
            if (!enabled) return false;
            set_registry_string(HKEY_CURRENT_USER, parent, nullptr, L"Axiom");
            set_registry_string(HKEY_CURRENT_USER, parent, L"MUIVerb", L"Axiom");
            set_registry_string(HKEY_CURRENT_USER, parent, L"Icon", icon_value());
            set_registry_string(HKEY_CURRENT_USER, parent, L"ExtendedSubCommandsKey",
                                subcommands_id);
            if (!applies_to.empty()) {
                set_registry_string(HKEY_CURRENT_USER, parent, L"AppliesTo", applies_to);
            }
            return true;
        };

    const auto apply_association =
        [&](bool enabled, const std::wstring& extension,
            const std::wstring& prog_id, const std::wstring& file_type,
            int icon_index = 0) {
            const std::wstring extension_key = classes + extension;
            const std::wstring prog_key = classes + prog_id;
            if (enabled) {
                set_registry_string(HKEY_CURRENT_USER, extension_key, nullptr, prog_id);
                set_registry_string(HKEY_CURRENT_USER, prog_key, nullptr, file_type);
                set_registry_string(HKEY_CURRENT_USER, prog_key + L"\\DefaultIcon", nullptr,
                                    quote_argument(executable) + L"," +
                                        std::to_wstring(icon_index));
                set_registry_string(HKEY_CURRENT_USER,
                                    prog_key + L"\\shell\\open\\command", nullptr,
                                    quoted_executable_command(executable, L"\"%1\""));
            } else {
                if (registry_string(HKEY_CURRENT_USER, extension_key, nullptr) == prog_id) {
                    delete_registry_tree(HKEY_CURRENT_USER, extension_key);
                }
                delete_registry_tree(HKEY_CURRENT_USER, prog_key);
            }
        };
    const auto apply_many =
        [&](bool enabled, std::initializer_list<const wchar_t*> extensions,
            const std::wstring& prog_prefix, const std::wstring& file_type,
            int icon_index = 0) {
            for (const wchar_t* extension : extensions) {
                std::wstring suffix = extension;
                if (!suffix.empty() && suffix.front() == L'.') suffix.erase(suffix.begin());
                apply_association(enabled, extension,
                                  prog_prefix + L"." + suffix,
                                  file_type, icon_index);
            }
        };

    apply_association(application_options_.associate_axar, L".axar",
                      L"AxiomCompress.Archive", L"Axiom archive", -IDI_AXIOM);
    apply_many(application_options_.associate_zip,
               {L".zip", L".jar", L".war", L".apk"},
               L"AxiomCompress.Zip", L"ZIP archive", -IDI_ARCHIVE_ZIP);
    apply_association(application_options_.associate_7z, L".7z",
                      L"AxiomCompress.7z", L"7z archive", -IDI_ARCHIVE_7Z);
    apply_many(application_options_.associate_rar,
               {L".rar", L".r00", L".r01", L".r02", L".r03", L".r04",
                L".r05", L".r06", L".r07", L".r08", L".r09"},
               L"AxiomCompress.Rar", L"RAR archive", -IDI_ARCHIVE_RAR);
    apply_many(application_options_.associate_tar,
               {L".tar", L".tgz", L".txz", L".tbz2", L".tzst"},
               L"AxiomCompress.Tar", L"TAR archive", -IDI_ARCHIVE_TAR);
    apply_association(application_options_.associate_iso, L".iso",
                      L"AxiomCompress.Iso", L"ISO image", -IDI_ARCHIVE_ISO);
    apply_association(application_options_.associate_cab, L".cab",
                      L"AxiomCompress.Cab", L"CAB archive", -IDI_ARCHIVE_CAB);

    if (!application_options_.associate_tar) {
        for (const wchar_t* extension :
             {L".tar.gz", L".tar.xz", L".tar.bz2", L".tar.zst"}) {
            const std::wstring extension_key = classes + extension;
            const std::wstring prog_id =
                L"AxiomCompress.Tar." + std::wstring(extension + 1);
            if (registry_string(HKEY_CURRENT_USER, extension_key, nullptr) == prog_id) {
                delete_registry_tree(HKEY_CURRENT_USER, extension_key);
            }
            delete_registry_tree(HKEY_CURRENT_USER, classes + prog_id);
        }
    } else {
        for (const wchar_t* extension :
             {L".tar.gz", L".tar.xz", L".tar.bz2", L".tar.zst"}) {
            const std::wstring prog_id =
                L"AxiomCompress.Tar." + std::wstring(extension + 1);
            apply_association(true, extension, prog_id, L"TAR archive",
                              -IDI_ARCHIVE_TAR);
        }
    }

    // Remove pre-submenu flat verbs. Keeping this unconditional lets upgraded
    // installs migrate cleanly the next time Axiom starts or settings are
    // applied.
    delete_registry_tree(HKEY_CURRENT_USER, axar_context + L"AxiomOpen");
    delete_registry_tree(HKEY_CURRENT_USER, axar_context + L"AxiomExtract");
    delete_registry_tree(HKEY_CURRENT_USER, axar_context + L"AxiomTest");
    delete_registry_tree(HKEY_CURRENT_USER, classes + L"*\\shell\\AxiomAdd");
    delete_registry_tree(HKEY_CURRENT_USER, classes + L"Directory\\shell\\AxiomAdd");

    const bool archive_commands =
        application_options_.context_open ||
        application_options_.context_extract ||
        application_options_.context_test;
    const bool file_menu = application_options_.context_add || archive_commands;
    const std::wstring archive_filter = archive_applies_to();
    if (apply_shell_submenu(file_context, file_subcommands_id, file_subcommands, file_menu,
                            application_options_.context_add ? L"" : archive_filter)) {
        if (application_options_.context_open) {
            apply_shell_subcommand(file_subcommands, L"010Open",
                                   L"Open with Axiom",
                                   quoted_executable_command(executable, L"\"%1\""),
                                   IDI_AXIOM, archive_filter);
        }
        if (application_options_.context_extract) {
            apply_shell_subcommand(file_subcommands, L"020Extract",
                                   L"Extract with Axiom...",
                                   quoted_executable_command(executable, L"--extract \"%1\""),
                                   IDI_ARCHIVE_ZIP, archive_filter);
        }
        if (application_options_.context_test) {
            apply_shell_subcommand(file_subcommands, L"030Test",
                                   L"Test with Axiom",
                                   quoted_executable_command(executable, L"--test \"%1\""),
                                   IDI_ARCHIVE_ZIP, archive_filter);
        }
        if (application_options_.context_add) {
            apply_shell_subcommand(file_subcommands, L"040Add",
                                   L"Add to Axiom archive...",
                                   quoted_executable_command(executable, L"--add \"%1\""),
                                   IDI_AXIOM);
        }
    }

    if (apply_shell_submenu(directory_context, directory_subcommands_id,
                            directory_subcommands, application_options_.context_add)) {
        apply_shell_subcommand(directory_subcommands, L"010Add",
                               L"Add to Axiom archive...",
                               quoted_executable_command(executable, L"--add \"%1\""),
                               IDI_AXIOM);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

bool MainWindow::maybe_execute_sfx_archive(const fs::path& path) {
    if (!axiom::is_axiom_sfx_archive(path)) return false;
    const int choice = show_app_message(
        L"This is an Axiom self-extracting archive.\n\n"
        L"Choose Yes to run the self-extractor, or No to open the embedded archive "
        L"in Axiom.",
        axiom::gui::MessageDialogIcon::question,
        L"Open self-extracting archive",
        axiom::gui::MessageDialogButtons::yes_no,
        IDNO);
    if (choice != IDYES) return false;

    const fs::path working_directory = path.parent_path();
    HINSTANCE result = ShellExecuteW(
        hwnd_, L"open", path.c_str(), nullptr,
        working_directory.empty() ? nullptr : working_directory.c_str(),
        SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        show_app_message(L"Could not run the self-extracting archive.",
                         axiom::gui::MessageDialogIcon::error,
                         L"Open self-extracting archive");
    }
    return true;
}

bool MainWindow::open_archive_path(const fs::path& path) {
    if (const auto joined = joined_archive_path_for_volume(path)) {
        try {
            const auto info = axiom::archive_volume_set_info(path);
            std::wstring prompt =
                L"This is one volume of a split Axiom archive. Reconstruct and open the "
                L"complete archive?\n\n" + joined->wstring() + L"\n\n" +
                std::to_wstring(info.data_volumes) + L" data volume(s), " +
                std::to_wstring(info.recovery_volumes) + L" recovery volume(s)";
            std::error_code exists_error;
            if (fs::exists(*joined, exists_error)) {
                prompt += L"\n\nThe existing complete archive will be replaced.";
            }
            if (show_app_message(prompt, axiom::gui::MessageDialogIcon::question,
                                 L"Open split archive",
                                 axiom::gui::MessageDialogButtons::yes_no,
                                 IDYES) != IDYES) {
                return true;
            }
            operation_archive_output_ = *joined;
            operation_open_after_ = *joined;
            const fs::path volume = path;
            const fs::path output = *joined;
            start_operation(
                L"Joining archive volumes...", L"Archive volumes joined successfully.",
                [volume, output](const std::shared_ptr<axiom::OperationControl>& operation) {
                    axiom::join_archive_volumes(volume, output, operation);
                });
            return true;
        } catch (const axiom::FormatError&) {
            // A normal file can contain `.part` or `.rev` in its name. Only a
            // validated Axiom volume is intercepted here.
        } catch (const std::exception& error) {
            show_app_message(widen(error.what()), axiom::gui::MessageDialogIcon::error,
                             L"Open split archive");
            return true;
        }
    }
    if (!axiom::gui::is_supported_archive(path)) return false;
    if (maybe_execute_sfx_archive(path)) return true;
    remember_archive_path(path);
    navigate_to(axiom::gui::BrowserLocation::archive(path));
    return true;
}

void MainWindow::on_open_archive() {
    auto path = pick_open_archive(hwnd_);
    if (path && !open_archive_path(*path)) {
        show_app_message(L"The selected file is not a supported archive or Axiom volume.",
                         axiom::gui::MessageDialogIcon::warning, L"Open archive");
    }
}

void MainWindow::on_drop_files(HDROP drop) {
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<fs::path> paths;
    paths.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        const UINT length = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
        DragQueryFileW(drop, i, path.data(), length + 1);
        path.resize(length);
        paths.emplace_back(std::move(path));
    }
    DragFinish(drop);
    if (paths.size() == 1 && open_archive_path(paths.front())) {
        return;
    }
    create_archive_from_paths(std::move(paths));
}

void MainWindow::save_current_settings() {
    persisted_settings_.application = application_options_;
    persisted_settings_.sort_column = sort_column_;
    persisted_settings_.sort_ascending = sort_ascending_;
    persisted_settings_.tree_width =
        tree_width_ > 0 ? MulDiv(tree_width_, USER_DEFAULT_SCREEN_DPI,
                                 static_cast<int>(dpi_ == 0
                                                      ? USER_DEFAULT_SCREEN_DPI
                                                      : dpi_))
                        : 0;
    persisted_settings_.tree_pane_visible = tree_pane_visible_;
    persisted_settings_.column_widths = table_.logical_column_widths();
    persisted_settings_.recent_locations = recent_addresses_;
    persisted_settings_.recent_archives = recent_archives_;
    persisted_settings_.favorite_locations = favorite_locations_;
    persisted_settings_.last_location = history_.current().display_name();
    persisted_settings_.placement.length = sizeof(WINDOWPLACEMENT);
    persisted_settings_.has_placement = application_options_.restore_window_placement &&
        GetWindowPlacement(hwnd_, &persisted_settings_.placement) != FALSE;
    axiom::gui::save_gui_settings(persisted_settings_);
}

fs::path MainWindow::log_file_path() const {
    fs::path folder;
    if (!application_options_.log_folder.empty()) {
        folder = application_options_.log_folder;
    } else if (const auto local = known_folder_path(FOLDERID_LocalAppData)) {
        folder = *local / L"AxiomCompress" / L"Logs";
    } else {
        folder = fs::temp_directory_path() / L"AxiomCompress" / L"Logs";
    }
    std::error_code ignored;
    fs::create_directories(folder, ignored);
    return folder / L"Axiom.log";
}

void MainWindow::append_log(const std::wstring& message) const {
    if (!application_options_.verbose_logging) return;
    try {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t stamp[64]{};
        swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u ",
                   now.wYear, now.wMonth, now.wDay, now.wHour,
                   now.wMinute, now.wSecond);
        std::ofstream log(log_file_path(), std::ios::binary | std::ios::app);
        if (!log) return;
        const std::string line = utf8(std::wstring(stamp) + message + L"\r\n");
        log.write(line.data(), static_cast<std::streamsize>(line.size()));
    } catch (...) {
    }
}

}  // namespace axiom::gui
