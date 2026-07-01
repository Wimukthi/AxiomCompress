#define NOMINMAX
#include "gui/archive_feature_dialogs.hpp"

#include "gui/dialog_support.hpp"
#include "gui/message_dialog.hpp"

#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace axiom::gui {
namespace fs = std::filesystem;
namespace {

constexpr wchar_t kFeatureDialogClass[] = L"AxiomArchiveFeatureOptionsDialog";

constexpr int kPageMetadata = 0;
constexpr int kPageUpdate = 1;
constexpr int kPageSecurity = 2;
constexpr int kPageRecovery = 3;
constexpr int kPageAuthenticity = 4;

constexpr int kNavBase = 4100;
constexpr int kAccept = IDOK;
constexpr int kCancel = IDCANCEL;

constexpr int kUpdateMode = 4220;
constexpr int kLockArchive = 4222;
constexpr int kComment = 4223;
constexpr int kRepackAfterUpdate = 4224;
constexpr int kEncryptData = 4230;
constexpr int kEncryptNames = 4231;
constexpr int kPassword = 4232;
constexpr int kConfirmPassword = 4233;
constexpr int kShowPassword = 4234;
constexpr int kVolumeSize = 4240;
constexpr int kVolumeUnit = 4241;
constexpr int kRecoveryPercent = 4242;
constexpr int kRecoveryVolumes = 4243;
constexpr int kAttemptRecovery = 4245;
constexpr int kSignArchive = 4250;
constexpr int kSigningKey = 4251;
constexpr int kCreateSfx = 4252;
constexpr int kSfxDestination = 4253;
constexpr int kBrowseSigningKey = 4254;
constexpr int kVerifySignature = 4255;
constexpr int kBrowseSfxDestination = 4256;

constexpr std::array<const wchar_t*, 5> kUpdateModeNames{
    L"Create a new archive", L"Add or replace entries",
    L"Update entries that are newer", L"Freshen existing entries",
    L"Synchronize with source"};
constexpr std::array<const wchar_t*, 4> kVolumeUnitNames{
    L"KiB", L"MiB", L"GiB", L"TiB"};

struct PlacedControl {
    HWND window{};
    int page{};
    int x{};
    int y{};
    int width{};
    int height{};
    bool wrapped{};
};

template <typename T>
class ComPtr {
public:
    ~ComPtr() { reset(); }
    T** put() {
        reset();
        return &value_;
    }
    T* get() const { return value_; }
    T* operator->() const { return value_; }

private:
    void reset() {
        if (value_ != nullptr) value_->Release();
        value_ = nullptr;
    }
    T* value_ = nullptr;
};

std::wstring control_text(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(window, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

std::optional<fs::path> shell_item_path(IShellItem* item) {
    if (item == nullptr) return std::nullopt;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    fs::path result(path);
    CoTaskMemFree(path);
    return result;
}

void set_initial_folder(IFileDialog* dialog, const fs::path& path) {
    if (dialog == nullptr || path.empty()) return;
    std::error_code error;
    fs::path folder = fs::is_directory(path, error) ? path : path.parent_path();
    if (folder.empty() || !fs::is_directory(folder, error)) return;
    ComPtr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr,
                                              IID_PPV_ARGS(item.put())))) {
        dialog->SetFolder(item.get());
    }
}

std::optional<fs::path> browse_signing_key(HWND owner, const fs::path& initial = {}) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) return std::nullopt;
    const COMDLG_FILTERSPEC filters[] = {
        {L"Axiom signing keys", L"*.key"}, {L"All files", L"*.*"}};
    dialog->SetFileTypes(static_cast<UINT>(sizeof(filters) / sizeof(filters[0])), filters);
    dialog->SetTitle(L"Choose signing key");
    FILEOPENDIALOGOPTIONS flags{};
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    set_initial_folder(dialog.get(), initial);
    if (dialog->Show(owner) != S_OK) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return std::nullopt;
    return shell_item_path(item.get());
}

std::optional<fs::path> browse_save_sfx(HWND owner, const fs::path& initial = {}) {
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) return std::nullopt;
    const COMDLG_FILTERSPEC filters[] = {
        {L"Axiom self-extractors", L"*.exe"}, {L"All files", L"*.*"}};
    dialog->SetFileTypes(static_cast<UINT>(sizeof(filters) / sizeof(filters[0])), filters);
    dialog->SetDefaultExtension(L"exe");
    dialog->SetTitle(L"Choose SFX output path");
    set_initial_folder(dialog.get(), initial);
    if (!initial.filename().empty()) {
        dialog->SetFileName(initial.filename().c_str());
    }
    if (dialog->Show(owner) != S_OK) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return std::nullopt;
    return shell_item_path(item.get());
}

class ArchiveFeatureDialog {
public:
    ArchiveFeatureDialog(ArchiveFeatureDialogContext context,
                         ArchiveFeatureOptions archive_options,
                         ExtractFeatureOptions extract_options,
                         ArchiveFeatureAvailability availability,
                         std::wstring suggested_sfx_output = {})
        : context_(context),
          archive_options_(std::move(archive_options)),
          extract_options_(std::move(extract_options)),
          availability_(availability),
          suggested_sfx_output_(std::move(suggested_sfx_output)) {}

    ~ArchiveFeatureDialog() {
        delete_dialog_font(font_);
        if (background_brush_ != nullptr) DeleteObject(background_brush_);
        if (edit_brush_ != nullptr) DeleteObject(edit_brush_);
    }

    bool show(HWND owner) {
        owner_ = owner;
        instance_ = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
        dpi_ = GetDpiForWindow(owner);
        dark_ = dialog_system_prefers_dark_mode();
        if (!register_class()) return false;

        const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        const DWORD ex_style = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
        RECT rect{0, 0, scale(760), scale(550)};
        AdjustWindowRectExForDpi(&rect, style, FALSE, ex_style, dpi_);
        RECT owner_rect{};
        GetWindowRect(owner, &owner_rect);
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        const int x = owner_rect.left + (owner_rect.right - owner_rect.left - width) / 2;
        const int y = owner_rect.top + (owner_rect.bottom - owner_rect.top - height) / 2;
        const wchar_t* title = context_ == ArchiveFeatureDialogContext::create_or_update
            ? L"Advanced archive options"
            : context_ == ArchiveFeatureDialogContext::comment
                ? L"Archive comment"
                : context_ == ArchiveFeatureDialogContext::password_prompt
                    ? L"Archive password"
                    : L"Advanced extraction options";
        window_ = CreateWindowExW(ex_style, kFeatureDialogClass, title, style,
                                  x, y, width, height, owner, nullptr, instance_, this);
        if (window_ == nullptr) return false;
        apply_axiom_window_icons(window_, instance_);
        EnableWindow(owner, FALSE);
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);

        MSG message{};
        while (IsWindow(window_)) {
            const BOOL status = GetMessageW(&message, nullptr, 0, 0);
            if (status <= 0) {
                if (status == 0) PostQuitMessage(static_cast<int>(message.wParam));
                break;
            }
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (IsWindow(owner)) {
            EnableWindow(owner, TRUE);
            SetActiveWindow(owner);
            SetFocus(owner);
        }
        return accepted_;
    }

    const ArchiveFeatureOptions& archive_options() const { return archive_options_; }
    const ExtractFeatureOptions& extract_options() const { return extract_options_; }

private:
    bool register_class() const {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &ArchiveFeatureDialog::window_proc;
        window_class.hInstance = instance_;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.lpszClassName = kFeatureDialogClass;
        assign_axiom_window_class_icons(window_class, instance_);
        return RegisterClassExW(&window_class) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    int scale(int value) const { return scale_for_dialog_dpi(value, dpi_); }

    HWND make_control(const wchar_t* type, const wchar_t* text, DWORD style, int id,
                      bool enabled = true) {
        HWND control = CreateWindowExW(
            0, type, text, WS_CHILD | style, 0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        set_dialog_control_font(control, font_);
        apply_dialog_control_theme(control, dark_);
        EnableWindow(control, enabled ? TRUE : FALSE);
        return control;
    }

    HWND place(int page, const wchar_t* type, const wchar_t* text, DWORD style, int id,
               int x, int y, int width, int height, bool enabled = true,
               bool wrapped = false) {
        HWND control = make_control(type, text, style, id, enabled);
        placed_.push_back({control, page, x, y, width, height, wrapped});
        return control;
    }

    HWND page_label(int page, const wchar_t* text, int x, int y, int width,
                    bool enabled = true, bool wrapped = false) {
        return place(page, L"STATIC", text,
                     SS_LEFT | SS_NOPREFIX | (wrapped ? SS_EDITCONTROL : 0), 0,
                     x, y, width, wrapped ? 48 : 24, enabled, wrapped);
    }

    HWND page_edit(int page, const wchar_t* text, int id, int x, int y, int width,
                   int height, bool enabled, DWORD extra_style = ES_AUTOHSCROLL) {
        HWND edit = place(page, L"EDIT", text, WS_TABSTOP | WS_BORDER | extra_style, id,
                          x, y, width, height, enabled);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(scale(6), scale(6)));
        return edit;
    }

    HWND page_combo(int page, int id, int x, int y, int width, int drop_height,
                    const wchar_t* const* items, std::size_t item_count,
                    int selection, bool enabled) {
        HWND combo = place(page, L"COMBOBOX", L"",
                           WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
                               CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                           id, x, y, width, drop_height, enabled);
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, scale(24));
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), scale(24));
        for (std::size_t i = 0; i < item_count; ++i) {
            SendMessageW(combo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(items[i]));
        }
        SendMessageW(combo, CB_SETCURSEL,
                     static_cast<WPARAM>(std::clamp(selection, 0,
                         static_cast<int>(item_count) - 1)), 0);
        return combo;
    }

    void page_checkbox(int page, int id, const wchar_t* text, bool& value,
                       int y, bool enabled) {
        HWND checkbox = place(page, L"BUTTON", text,
                              WS_TABSTOP | BS_AUTOCHECKBOX | BS_OWNERDRAW,
                              id, 8, y, 470, 28, enabled);
        checkbox_values_[id] = &value;
        checkbox_windows_[id] = checkbox;
    }

    void page_heading(int page, const wchar_t* title, const wchar_t* description) {
        page_label(page, title, 8, 0, 500);
        page_label(page, description, 8, 28, 510, true, true);
    }

    void create_metadata_page() {
        if (context_ == ArchiveFeatureDialogContext::create_or_update) {
            page_heading(kPageMetadata, L"Metadata and links",
                         L"Supported filesystem metadata is captured automatically.");
            page_label(kPageMetadata,
                       L"Axiom stores Windows attributes and full-precision timestamps, NTFS "
                       L"alternate data streams, symbolic and hard links, and POSIX mode and "
                       L"ownership where the host exposes them.",
                       8, 76, 500, true, true);
            page_label(kPageMetadata,
                       L"Unsupported special files are skipped; metadata that the destination "
                       L"filesystem cannot represent is restored on a best-effort basis.",
                       8, 150, 500, true, true);
        } else {
            page_heading(kPageMetadata, L"Restore metadata and links",
                         L"Stored filesystem metadata is restored automatically.");
            page_label(kPageMetadata,
                       L"Attributes, creation/access times, alternate data streams, links, and "
                       L"POSIX metadata are restored when supported by the destination.",
                       8, 76, 500, true, true);
            page_label(kPageMetadata,
                       L"Modification-time restoration remains available in the main extraction "
                       L"dialog because it is the one metadata policy exposed by the engine.",
                       8, 150, 500, true, true);
        }
    }

    void create_update_page() {
        if (context_ == ArchiveFeatureDialogContext::comment) {
            page_heading(kPageUpdate, L"Archive comment",
                         L"Add, replace, or remove the comment stored in this archive.");
            page_label(kPageUpdate, L"Comment", 8, 75, 160, true);
            comment_ = page_edit(kPageUpdate, archive_options_.comment.c_str(), kComment,
                                 8, 104, 500, 240, true,
                                 ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN);
            return;
        }

        page_heading(kPageUpdate, L"Archive update behavior",
                     L"Configure in-place editing, synchronization, and archive service data.");
        page_label(kPageUpdate, L"Update mode", 8, 75, 120, availability_.update);
        update_mode_ = page_combo(
            kPageUpdate, kUpdateMode, 145, 70, 290, 190,
            kUpdateModeNames.data(), kUpdateModeNames.size(),
            static_cast<int>(archive_options_.update_mode), availability_.update);
        page_label(kPageUpdate,
                   L"Axiom always writes a self-locating central directory for immediate listing.",
                   8, 116, 500, true, true);
        page_checkbox(kPageUpdate, kLockArchive, L"Lock archive against further changes",
                      archive_options_.lock_archive, 150, availability_.lock);
        page_checkbox(kPageUpdate, kRepackAfterUpdate,
                      L"Repack affected solid runs after updating",
                      archive_options_.repack_after_update, 184, availability_.update);
        page_label(kPageUpdate, L"Archive comment", 8, 231, 160, availability_.comments);
        comment_ = page_edit(kPageUpdate, archive_options_.comment.c_str(), kComment,
                             8, 260, 500, 100, availability_.comments,
                             ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN);
    }

    void create_security_page() {
        page_heading(kPageSecurity, L"Encryption",
                     L"Passwords are never saved in GUI settings and will be cleared after use.");
        if (context_ == ArchiveFeatureDialogContext::create_or_update) {
            page_checkbox(kPageSecurity, kEncryptData, L"Encrypt file data",
                          archive_options_.encrypt_data, 72, availability_.encryption);
            page_checkbox(kPageSecurity, kEncryptNames, L"Encrypt file names and headers",
                          archive_options_.encrypt_names, 106,
                          availability_.header_encryption);
            page_label(kPageSecurity, L"Password", 8, 157, 130, availability_.encryption);
            password_ = page_edit(kPageSecurity, archive_options_.password.c_str(), kPassword,
                                  145, 150, 300, 30, availability_.encryption,
                                  ES_PASSWORD | ES_AUTOHSCROLL);
            page_label(kPageSecurity, L"Confirm password", 8, 199, 130,
                       availability_.encryption);
            confirm_password_ = page_edit(
                kPageSecurity, archive_options_.password.c_str(), kConfirmPassword,
                145, 192, 300, 30, availability_.encryption,
                ES_PASSWORD | ES_AUTOHSCROLL);
            page_checkbox(kPageSecurity, kShowPassword, L"Show password",
                          show_password_, 231, availability_.encryption);
            page_label(kPageSecurity,
                       L"Argon2id uses the current format defaults; parameters are stored in the "
                       L"archive for deterministic decoding.",
                       8, 283, 500, true, true);
        } else {
            page_label(kPageSecurity, L"Password", 8, 79, 130, availability_.encryption);
            password_ = page_edit(kPageSecurity, extract_options_.password.c_str(), kPassword,
                                  145, 72, 300, 30, availability_.encryption,
                                  ES_PASSWORD | ES_AUTOHSCROLL);
            page_checkbox(kPageSecurity, kShowPassword, L"Show password",
                          show_password_, 114, availability_.encryption);
        }
    }

    void create_recovery_page() {
        page_heading(kPageRecovery, L"Recovery and volumes",
                     L"Configure split archives, recovery records, and missing-volume behavior.");
        if (context_ == ArchiveFeatureDialogContext::create_or_update) {
            page_label(kPageRecovery, L"Split volume size", 8, 77, 135, availability_.volumes);
            volume_size_ = page_edit(kPageRecovery, archive_options_.volume_size.c_str(),
                                     kVolumeSize, 155, 70, 160, 30, availability_.volumes);
            volume_unit_ = page_combo(
                kPageRecovery, kVolumeUnit, 325, 70, 110, 150,
                kVolumeUnitNames.data(), kVolumeUnitNames.size(),
                archive_options_.volume_unit, availability_.volumes);
            page_label(kPageRecovery, L"Recovery record", 8, 125, 135, availability_.recovery);
            recovery_percent_ = page_edit(
                kPageRecovery, std::to_wstring(archive_options_.recovery_percent).c_str(),
                kRecoveryPercent, 155, 118, 90, 30, availability_.recovery, ES_NUMBER);
            page_label(kPageRecovery, L"% of archive data", 255, 125, 150,
                       availability_.recovery);
            page_checkbox(kPageRecovery, kRecoveryVolumes,
                          L"Create recovery volumes for missing-volume reconstruction",
                          archive_options_.create_recovery_volumes, 168,
                          availability_.recovery && availability_.volumes);
        } else {
            page_checkbox(kPageRecovery, kAttemptRecovery,
                          L"Attempt recovery before reporting damaged data",
                          extract_options_.attempt_recovery, 72, availability_.recovery);
            page_label(kPageRecovery,
                       L"Split sets are joined before browsing or extraction. Opening any surviving "
                       L"data or recovery volume discovers adjacent members and reconstructs the "
                       L"complete archive when enough shards remain.",
                       8, 126, 500, availability_.volumes, true);
        }
    }

    void create_authenticity_page() {
        if (context_ == ArchiveFeatureDialogContext::create_or_update) {
            page_heading(kPageAuthenticity, L"Authenticity and self-extraction",
                         L"Sign completed archives or package them with the native SFX stub.");
            page_checkbox(kPageAuthenticity, kSignArchive, L"Sign archive with Monocypher EdDSA",
                          archive_options_.sign_archive, 72, availability_.authenticity);
            page_label(kPageAuthenticity, L"Signing key", 8, 121, 125,
                       availability_.authenticity);
            signing_key_ = page_edit(kPageAuthenticity,
                                     archive_options_.signing_key.wstring().c_str(),
                                      kSigningKey, 145, 114, 260, 30,
                                      availability_.authenticity);
            browse_signing_key_ = place(kPageAuthenticity, L"BUTTON", L"Browse...",
                                         WS_TABSTOP | BS_OWNERDRAW, kBrowseSigningKey,
                                         415, 114, 88, 30, availability_.authenticity);
            page_checkbox(kPageAuthenticity, kCreateSfx,
                           L"Create a self-extracting Windows executable",
                           archive_options_.create_sfx, 166, availability_.sfx);
            page_label(kPageAuthenticity, L"SFX output path", 8, 215, 125,
                       availability_.sfx);
            const std::wstring displayed_sfx_output =
                archive_options_.sfx_destination.empty()
                    ? suggested_sfx_output_
                    : archive_options_.sfx_destination;
            sfx_destination_uses_default_ = archive_options_.sfx_destination.empty();
            sfx_destination_ = page_edit(kPageAuthenticity,
                                          displayed_sfx_output.c_str(),
                                          kSfxDestination, 145, 208, 260, 30,
                                          availability_.sfx);
            browse_sfx_destination_ = place(kPageAuthenticity, L"BUTTON", L"Browse...",
                                            WS_TABSTOP | BS_OWNERDRAW,
                                            kBrowseSfxDestination,
                                            415, 208, 88, 30, availability_.sfx);
            page_label(kPageAuthenticity,
                       L"The .exe embeds the completed .axar and is created beside it by default.",
                       8, 244, 500, availability_.sfx, true);
            page_label(kPageAuthenticity,
                       L"Runtime destination, overwrite policy, thread count, timestamp restore, "
                       L"and destination-folder opening are selected by the self-extractor.",
                       8, 280, 500, availability_.sfx, true);
        } else {
            page_heading(kPageAuthenticity, L"Signature verification",
                         L"Control how authenticity signatures are handled during extraction.");
            page_checkbox(kPageAuthenticity, kVerifySignature,
                          L"Verify signed archives before extraction",
                          extract_options_.verify_signature, 72, availability_.authenticity);
            page_label(kPageAuthenticity,
                       L"Invalid signatures block extraction. Unsigned archives remain allowed; "
                       L"trusted-key pinning is available through the CLI verify command.",
                       8, 126, 500, availability_.authenticity, true);
        }
    }

    void create_controls() {
        const DialogColors colors = dialog_colors(dark_);
        background_brush_ = CreateSolidBrush(colors.background);
        edit_brush_ = CreateSolidBrush(colors.control_background);
        font_ = create_dialog_font(dpi_);
        apply_dialog_dark_frame(window_, dark_);

        heading_ = make_control(L"STATIC",
            context_ == ArchiveFeatureDialogContext::create_or_update
                ? L"Archive feature options"
                : context_ == ArchiveFeatureDialogContext::comment
                    ? L"Edit archive comment"
                    : context_ == ArchiveFeatureDialogContext::password_prompt
                        ? L"Password required"
                        : L"Extraction feature options",
            SS_LEFT | SS_NOPREFIX, 0);
        banner_ = make_control(
            L"STATIC",
            context_ == ArchiveFeatureDialogContext::comment
                ? L"Archive comments are optional; an empty comment removes the current one."
                : context_ == ArchiveFeatureDialogContext::password_prompt
                    ? L"The password is retained only in memory while this archive is open."
                    : context_ == ArchiveFeatureDialogContext::create_or_update
                        ? L"Configure archive metadata, security, recovery, volumes, and packaging."
                        : L"Configure restoration, security verification, and recovery behavior.",
            SS_LEFT | SS_NOPREFIX, 0);
        ShowWindow(heading_, SW_SHOW);
        ShowWindow(banner_, SW_SHOW);

        if (context_ == ArchiveFeatureDialogContext::comment) {
            logical_pages_ = {kPageUpdate};
        } else if (context_ == ArchiveFeatureDialogContext::password_prompt) {
            logical_pages_ = {kPageSecurity};
        } else if (context_ == ArchiveFeatureDialogContext::create_or_update) {
            logical_pages_ = {kPageMetadata, kPageUpdate, kPageSecurity,
                              kPageRecovery, kPageAuthenticity};
        } else {
            logical_pages_ = {kPageMetadata, kPageSecurity,
                              kPageRecovery, kPageAuthenticity};
        }
        const std::array<const wchar_t*, 5> create_names{
            L"Metadata", L"Update", L"Security", L"Recovery & volumes", L"Authenticity & SFX"};
        const std::array<const wchar_t*, 5> extract_names{
            L"Restore", L"", L"Security", L"Recovery & volumes", L"Authenticity"};
        for (int page : logical_pages_) {
            const wchar_t* text = context_ == ArchiveFeatureDialogContext::comment
                ? L"Comment"
                : context_ == ArchiveFeatureDialogContext::password_prompt
                    ? L"Security"
                    : context_ == ArchiveFeatureDialogContext::create_or_update
                        ? create_names[static_cast<std::size_t>(page)]
                        : extract_names[static_cast<std::size_t>(page)];
            nav_buttons_[page] = make_control(L"BUTTON", text,
                                              WS_TABSTOP | BS_OWNERDRAW,
                                              kNavBase + page);
            ShowWindow(nav_buttons_[page], SW_SHOW);
        }

        create_metadata_page();
        if (context_ == ArchiveFeatureDialogContext::create_or_update ||
            context_ == ArchiveFeatureDialogContext::comment) {
            create_update_page();
        }
        create_security_page();
        create_recovery_page();
        create_authenticity_page();

        accept_ = make_control(L"BUTTON", L"OK", WS_TABSTOP | BS_OWNERDRAW, kAccept);
        cancel_ = make_control(L"BUTTON", L"Cancel", WS_TABSTOP | BS_OWNERDRAW, kCancel);
        ShowWindow(accept_, SW_SHOW);
        ShowWindow(cancel_, SW_SHOW);
        SendMessageW(window_, DM_SETDEFID, IDOK, 0);
        select_page(logical_pages_.front());
        layout();
    }

    void select_page(int page) {
        current_page_ = page;
        for (const PlacedControl& control : placed_) {
            ShowWindow(control.window, control.page == current_page_ ? SW_SHOW : SW_HIDE);
        }
        for (const auto& [logical_page, button] : nav_buttons_) {
            InvalidateRect(button, nullptr, TRUE);
        }
    }

    void layout() {
        if (window_ == nullptr) return;
        RECT client{};
        GetClientRect(window_, &client);
        const int margin = scale(18);
        const int nav_width = scale(166);
        const int top = scale(76);
        const int content_left = margin + nav_width + scale(20);
        MoveWindow(heading_, margin, scale(16), client.right - margin * 2, scale(26), TRUE);
        MoveWindow(banner_, margin, scale(43), client.right - margin * 2, scale(24), TRUE);

        int nav_y = top;
        for (int page : logical_pages_) {
            MoveWindow(nav_buttons_[page], margin, nav_y, nav_width, scale(36), TRUE);
            nav_y += scale(42);
        }
        for (const PlacedControl& control : placed_) {
            const int available_width = std::max(
                scale(120), static_cast<int>(client.right) - margin -
                                content_left - scale(control.x));
            const int width = std::min(scale(control.width), available_width);
            int height = scale(control.height);
            if (control.wrapped) {
                HDC dc = GetDC(control.window);
                HGDIOBJ old_font = SelectObject(dc, font_);
                RECT measured{0, 0, width, 0};
                const std::wstring text = control_text(control.window);
                DrawTextW(dc, text.c_str(), -1, &measured,
                          DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
                SelectObject(dc, old_font);
                ReleaseDC(control.window, dc);
                height = std::max(height, static_cast<int>(measured.bottom) + scale(3));
            }
            MoveWindow(control.window,
                       content_left + scale(control.x), top + scale(control.y),
                       width, height, TRUE);
        }
        const int button_width = scale(88);
        const int button_height = scale(30);
        const int button_y = client.bottom - margin - button_height;
        MoveWindow(cancel_, client.right - margin - button_width, button_y,
                   button_width, button_height, TRUE);
        MoveWindow(accept_, client.right - margin - button_width * 2 - scale(10),
                   button_y, button_width, button_height, TRUE);
        InvalidateRect(window_, nullptr, TRUE);
    }

    void rebuild_font() {
        delete_dialog_font(font_);
        font_ = create_dialog_font(dpi_);
        set_dialog_control_font(heading_, font_);
        set_dialog_control_font(banner_, font_);
        set_dialog_control_font(accept_, font_);
        set_dialog_control_font(cancel_, font_);
        for (const auto& [page, button] : nav_buttons_) set_dialog_control_font(button, font_);
        for (const PlacedControl& control : placed_) set_dialog_control_font(control.window, font_);
    }

    void draw_nav_button(const DRAWITEMSTRUCT& draw, int page) const {
        if (page != current_page_) {
            draw_dialog_button(draw, dark_);
            return;
        }
        const DialogColors colors = dialog_colors(dark_);
        HBRUSH background = CreateSolidBrush(colors.selection_background);
        FillRect(draw.hDC, &draw.rcItem, background);
        DeleteObject(background);
        HBRUSH border = CreateSolidBrush(colors.focus_border);
        FrameRect(draw.hDC, &draw.rcItem, border);
        DeleteObject(border);
        wchar_t text[128]{};
        GetWindowTextW(draw.hwndItem, text,
                       static_cast<int>(sizeof(text) / sizeof(text[0])));
        HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw.hwndItem, WM_GETFONT, 0, 0));
        HGDIOBJ old_font = SelectObject(draw.hDC, font);
        SetBkMode(draw.hDC, TRANSPARENT);
        SetTextColor(draw.hDC, colors.selection_text);
        RECT text_rect = draw.rcItem;
        text_rect.left += scale(12);
        DrawTextW(draw.hDC, text, -1, &text_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(draw.hDC, old_font);
    }

    void draw_item(const DRAWITEMSTRUCT& draw) const {
        if (draw.CtlType == ODT_COMBOBOX) {
            draw_dialog_combo_item(draw, dark_);
            return;
        }
        const int id = GetDlgCtrlID(draw.hwndItem);
        if (id >= kNavBase && id < kNavBase + 5) {
            draw_nav_button(draw, id - kNavBase);
            return;
        }
        if (auto found = checkbox_values_.find(id); found != checkbox_values_.end()) {
            draw_dialog_checkbox(draw, dark_, *found->second);
            return;
        }
        draw_dialog_button(draw, dark_);
    }

    void toggle_checkbox(int id) {
        auto found = checkbox_values_.find(id);
        if (found == checkbox_values_.end()) return;
        HWND checkbox = checkbox_windows_[id];
        if (!IsWindowEnabled(checkbox)) return;
        *found->second = !*found->second;
        if (id == kEncryptNames && archive_options_.encrypt_names) {
            archive_options_.encrypt_data = true;
            if (const auto data = checkbox_windows_.find(kEncryptData);
                data != checkbox_windows_.end()) {
                InvalidateRect(data->second, nullptr, TRUE);
            }
        } else if (id == kEncryptData && !archive_options_.encrypt_data) {
            archive_options_.encrypt_names = false;
            if (const auto names = checkbox_windows_.find(kEncryptNames);
                names != checkbox_windows_.end()) {
                InvalidateRect(names->second, nullptr, TRUE);
            }
        }
        if (id == kShowPassword) {
            const WPARAM password_character = show_password_ ? 0 : static_cast<WPARAM>(L'\x25cf');
            SendMessageW(password_, EM_SETPASSWORDCHAR, password_character, 0);
            if (confirm_password_ != nullptr) {
                SendMessageW(confirm_password_, EM_SETPASSWORDCHAR, password_character, 0);
            }
            InvalidateRect(password_, nullptr, TRUE);
            if (confirm_password_ != nullptr) InvalidateRect(confirm_password_, nullptr, TRUE);
        }
        InvalidateRect(checkbox, nullptr, TRUE);
    }

    void browse_signing_key_path() {
        if (signing_key_ == nullptr || !IsWindowEnabled(signing_key_)) return;
        if (const auto selected = browse_signing_key(window_, control_text(signing_key_))) {
            SetWindowTextW(signing_key_, selected->c_str());
        }
    }

    void browse_sfx_output_path() {
        if (sfx_destination_ == nullptr || !IsWindowEnabled(sfx_destination_)) return;
        if (const auto selected = browse_save_sfx(window_, control_text(sfx_destination_))) {
            SetWindowTextW(sfx_destination_, selected->c_str());
            sfx_destination_uses_default_ = false;
        }
    }

    void accept() {
        if (update_mode_ != nullptr && IsWindowEnabled(update_mode_)) {
            const LRESULT selection = SendMessageW(update_mode_, CB_GETCURSEL, 0, 0);
            if (selection != CB_ERR) {
                archive_options_.update_mode = static_cast<ArchiveUpdateMode>(selection);
            }
        }
        if (volume_unit_ != nullptr && IsWindowEnabled(volume_unit_)) {
            const LRESULT selection = SendMessageW(volume_unit_, CB_GETCURSEL, 0, 0);
            if (selection != CB_ERR) archive_options_.volume_unit = static_cast<int>(selection);
        }

        if (password_ != nullptr && IsWindowEnabled(password_)) {
            const std::wstring password = control_text(password_);
            if (context_ == ArchiveFeatureDialogContext::create_or_update) {
                if (archive_options_.encrypt_data) {
                    const bool has_comment = comment_ != nullptr &&
                                             !control_text(comment_).empty();
                    if (archive_options_.encrypt_names &&
                        (has_comment || archive_options_.lock_archive)) {
                        show_message_dialog(
                            window_, instance_, dpi_, dark_, L"Axiom",
                            L"Comments and locking cannot currently be changed after file-name "
                            L"encryption is applied. Clear those options or encrypt file data only.",
                            MessageDialogIcon::warning);
                        return;
                    }
                    if (password.empty()) {
                        show_message_dialog(window_, instance_, dpi_, dark_, L"Axiom",
                                            L"Enter a password to encrypt the archive.",
                                            MessageDialogIcon::warning);
                        return;
                    }
                    if (confirm_password_ == nullptr ||
                        password != control_text(confirm_password_)) {
                        show_message_dialog(window_, instance_, dpi_, dark_, L"Axiom",
                                            L"The passwords do not match.",
                                            MessageDialogIcon::warning);
                        return;
                    }
                    archive_options_.password = password;
                } else {
                    archive_options_.password.clear();
                }
            } else {
                if (password.empty()) {
                    show_message_dialog(window_, instance_, dpi_, dark_, L"Axiom",
                                        L"Enter the archive password.",
                                        MessageDialogIcon::warning);
                    return;
                }
                extract_options_.password = password;
            }
        }
        if (comment_ != nullptr && IsWindowEnabled(comment_)) {
            archive_options_.comment = control_text(comment_);
        }
        if (volume_size_ != nullptr && IsWindowEnabled(volume_size_)) {
            archive_options_.volume_size = control_text(volume_size_);
        }
        if (recovery_percent_ != nullptr && IsWindowEnabled(recovery_percent_)) {
            try {
                archive_options_.recovery_percent = std::clamp(
                    std::stoi(control_text(recovery_percent_)), 0, 100);
            } catch (...) {
                archive_options_.recovery_percent = 0;
            }
        }
        if (signing_key_ != nullptr && IsWindowEnabled(signing_key_)) {
            archive_options_.signing_key = control_text(signing_key_);
        }
        if (sfx_destination_ != nullptr && IsWindowEnabled(sfx_destination_)) {
            std::wstring output = control_text(sfx_destination_);
            if (!output.empty()) {
                std::filesystem::path output_path(output);
                if (lstrcmpiW(output_path.extension().c_str(), L".exe") != 0) {
                    output_path.replace_extension(L".exe");
                }
                output = output_path.wstring();
            }
            archive_options_.sfx_destination =
                sfx_destination_uses_default_ && output == suggested_sfx_output_
                    ? std::wstring{}
                    : std::move(output);
        }
        if (password_ != nullptr) {
            SetWindowTextW(password_, L"");
            if (confirm_password_ != nullptr) SetWindowTextW(confirm_password_, L"");
        }
        accepted_ = true;
        DestroyWindow(window_);
    }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_CREATE:
                create_controls();
                return 0;
            case WM_SIZE:
                layout();
                return 0;
            case WM_DPICHANGED: {
                dpi_ = HIWORD(wparam);
                const auto* suggested = reinterpret_cast<const RECT*>(lparam);
                SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                rebuild_font();
                layout();
                return 0;
            }
            case WM_ERASEBKGND: {
                RECT client{};
                GetClientRect(window_, &client);
                FillRect(reinterpret_cast<HDC>(wparam), &client, background_brush_);
                return 1;
            }
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                HDC dc = BeginPaint(window_, &paint);
                const DialogColors colors = dialog_colors(dark_);
                const int x = scale(194);
                HPEN pen = CreatePen(PS_SOLID, 1, colors.border);
                HGDIOBJ old_pen = SelectObject(dc, pen);
                MoveToEx(dc, x, scale(76), nullptr);
                LineTo(dc, x, scale(480));
                SelectObject(dc, old_pen);
                DeleteObject(pen);
                EndPaint(window_, &paint);
                return 0;
            }
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN: {
                const DialogColors colors = dialog_colors(dark_);
                HDC dc = reinterpret_cast<HDC>(wparam);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(lparam))
                                     ? colors.text
                                     : colors.disabled_text);
                return reinterpret_cast<LRESULT>(background_brush_);
            }
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX: {
                const DialogColors colors = dialog_colors(dark_);
                HDC dc = reinterpret_cast<HDC>(wparam);
                SetBkColor(dc, colors.control_background);
                SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(lparam))
                                     ? colors.text
                                     : colors.disabled_text);
                return reinterpret_cast<LRESULT>(edit_brush_);
            }
            case WM_DRAWITEM:
                if (lparam != 0) {
                    draw_item(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
                    return TRUE;
                }
                break;
            case WM_COMMAND: {
                const int id = LOWORD(wparam);
                if (id >= kNavBase && id < kNavBase + 5) {
                    select_page(id - kNavBase);
                    return 0;
                }
                if (checkbox_values_.contains(id)) {
                    toggle_checkbox(id);
                    return 0;
                }
                if (id == kBrowseSigningKey) {
                    browse_signing_key_path();
                    return 0;
                }
                if (id == kBrowseSfxDestination) {
                    browse_sfx_output_path();
                    return 0;
                }
                if (id == kAccept) {
                    accept();
                    return 0;
                }
                if (id == kCancel) {
                    DestroyWindow(window_);
                    return 0;
                }
                break;
            }
            case WM_CLOSE:
                if (password_ != nullptr) {
                    SetWindowTextW(password_, L"");
                    if (confirm_password_ != nullptr) SetWindowTextW(confirm_password_, L"");
                }
                DestroyWindow(window_);
                return 0;
            case WM_NCDESTROY:
                window_ = nullptr;
                return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
        ArchiveFeatureDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            dialog = static_cast<ArchiveFeatureDialog*>(create->lpCreateParams);
            dialog->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
        } else {
            dialog = reinterpret_cast<ArchiveFeatureDialog*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        return dialog != nullptr
            ? dialog->handle_message(message, wparam, lparam)
            : DefWindowProcW(window, message, wparam, lparam);
    }

    ArchiveFeatureDialogContext context_;
    ArchiveFeatureOptions archive_options_;
    ExtractFeatureOptions extract_options_;
    ArchiveFeatureAvailability availability_;
    std::wstring suggested_sfx_output_;
    HWND owner_{};
    HWND window_{};
    HINSTANCE instance_{};
    UINT dpi_{USER_DEFAULT_SCREEN_DPI};
    bool dark_{};
    bool accepted_{};
    bool show_password_{};
    bool sfx_destination_uses_default_{};
    HFONT font_{};
    HBRUSH background_brush_{};
    HBRUSH edit_brush_{};
    HWND heading_{};
    HWND banner_{};
    HWND accept_{};
    HWND cancel_{};
    HWND update_mode_{};
    HWND comment_{};
    HWND password_{};
    HWND confirm_password_{};
    HWND volume_size_{};
    HWND volume_unit_{};
    HWND recovery_percent_{};
    HWND signing_key_{};
    HWND browse_signing_key_{};
    HWND sfx_destination_{};
    HWND browse_sfx_destination_{};
    int current_page_{kPageMetadata};
    std::vector<int> logical_pages_;
    std::unordered_map<int, HWND> nav_buttons_;
    std::vector<PlacedControl> placed_;
    std::unordered_map<int, bool*> checkbox_values_;
    std::unordered_map<int, HWND> checkbox_windows_;
};

std::wstring capability_state(bool supported) {
    return supported ? L"Supported" : L"Not reported by the current reader";
}

}  // namespace

bool show_archive_feature_options_dialog(
    HWND owner,
    ArchiveFeatureDialogContext context,
    ArchiveFeatureOptions& archive_options,
    ExtractFeatureOptions& extract_options,
    const ArchiveFeatureAvailability& availability,
    std::wstring suggested_sfx_output) {
    ArchiveFeatureDialog dialog(context, archive_options, extract_options, availability,
                                std::move(suggested_sfx_output));
    if (!dialog.show(owner)) return false;
    archive_options = dialog.archive_options();
    extract_options = dialog.extract_options();
    return true;
}

bool show_archive_password_dialog(HWND owner, std::wstring& password) {
    ArchiveFeatureOptions archive_options;
    ExtractFeatureOptions extract_options;
    extract_options.password = password;
    ArchiveFeatureAvailability availability;
    availability.encryption = true;
    ArchiveFeatureDialog dialog(ArchiveFeatureDialogContext::password_prompt,
                                archive_options, extract_options, availability);
    if (!dialog.show(owner)) return false;
    password = dialog.extract_options().password;
    return true;
}

bool show_archive_comment_dialog(HWND owner, std::wstring& comment) {
    ArchiveFeatureOptions archive_options;
    archive_options.comment = comment;
    ExtractFeatureOptions extract_options;
    ArchiveFeatureAvailability availability;
    availability.comments = true;
    ArchiveFeatureDialog dialog(ArchiveFeatureDialogContext::comment,
                                archive_options, extract_options, availability);
    if (!dialog.show(owner)) return false;
    comment = dialog.archive_options().comment;
    return true;
}

void show_archive_feature_summary_dialog(
    HWND owner,
    const std::filesystem::path& archive_path,
    const ArchiveCapabilities& capabilities) {
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    std::wstring message = L"Archive: " + archive_path.filename().wstring() + L"\n\n";
    message += L"Selective extraction: " + capability_state(capabilities.selective_extract);
    message += L"\nIn-place update and synchronization: " + capability_state(capabilities.update);
    message += L"\nArchive comments: " + capability_state(capabilities.comments);
    message += L"\nEncryption: " + capability_state(capabilities.encryption);
    message += L"\nWindows metadata and links: " +
               capability_state(capabilities.metadata && capabilities.links);
    message += capabilities.encrypted ? L"\nState: Encrypted" : L"\nState: Not encrypted";
    if (capabilities.locked) message += L"\nState: Locked (read-only)";
    message += L"\nRecovery records: " + capability_state(capabilities.recovery_records);
    message += L"\nMulti-volume archives: " + capability_state(capabilities.multi_volume);
    message += L"\nAuthenticity signatures: " + capability_state(capabilities.authenticity);
    show_message_dialog(owner, instance, GetDpiForWindow(owner),
                        dialog_system_prefers_dark_mode(),
                        L"Archive Features", message,
                        MessageDialogIcon::information);
}

}  // namespace axiom::gui
