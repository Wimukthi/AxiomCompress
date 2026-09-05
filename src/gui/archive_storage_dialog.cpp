#define NOMINMAX
#include "gui/archive_dialogs.hpp"

#include "core/path_text.hpp"
#include "gui/dialog_support.hpp"
#include "gui/main_window_internal.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace axiom::gui {
namespace {

constexpr int kInformationDetails = 2970;
constexpr int kInformationCapabilities = 2971;
constexpr int kStorageComponents = 2972;
constexpr int kStorageFiles = 2973;
constexpr int kSnapshotTimeline = 2974;
constexpr int kSnapshotChanges = 2975;
constexpr int kPrimaryAction = 2976;

std::uint64_t add_storage_bytes(std::uint64_t total, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) {
        throw std::runtime_error("archive size total overflows");
    }
    return total + value;
}

std::wstring storage_size(std::uint64_t bytes) {
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

std::wstring snapshot_time(std::int64_t seconds) {
    const std::time_t stamp = static_cast<std::time_t>(seconds);
    std::tm local{};
    if (localtime_s(&local, &stamp) != 0) return L"Unknown";
    wchar_t buffer[64]{};
    if (std::wcsftime(buffer, std::size(buffer), L"%Y-%m-%d %H:%M", &local) == 0) {
        return L"Unknown";
    }
    return buffer;
}

std::wstring change_name(ArchiveSnapshotChangeKind kind) {
    switch (kind) {
        case ArchiveSnapshotChangeKind::added: return L"Added";
        case ArchiveSnapshotChangeKind::removed: return L"Removed";
        case ArchiveSnapshotChangeKind::modified: return L"Modified";
    }
    return L"Changed";
}

std::wstring snapshot_change_summary(const ArchiveSnapshotStorageInfo& snapshot) {
    return L"+" + std::to_wstring(snapshot.added_entries) + L"  ~" +
           std::to_wstring(snapshot.modified_entries) + L"  -" +
           std::to_wstring(snapshot.removed_entries);
}

std::wstring storage_summary(const ArchiveStorageAnalysis& analysis) {
    std::wstring result = L"Physical  " + storage_size(analysis.physical_bytes) +
        L"     Current logical  " + storage_size(analysis.logical_bytes) +
        (analysis.physical_layout_exact || analysis.packed_sizes_complete
             ? L"     Stored payload  " : L"     Reported payload  ") +
        storage_size(analysis.stored_payload_bytes);
    if (analysis.deduplicated) {
        result += L"     Dedup saved  " +
            storage_size(analysis.deduplication_saved_bytes);
    }
    return result;
}

std::vector<std::vector<std::wstring>> capability_rows(
    const ArchiveCapabilities& capabilities) {
    std::vector<std::vector<std::wstring>> rows;
    const auto add = [&rows](const wchar_t* label, bool available) {
        if (available) rows.push_back({label, L"Available"});
    };
    add(L"Browse directory", capabilities.list);
    add(L"Extract", capabilities.extract);
    add(L"Test integrity", capabilities.test);
    add(L"Selective extraction", capabilities.selective_extract);
    add(L"Create archive", capabilities.create);
    add(L"Update entries", capabilities.update);
    add(L"Delete entries", capabilities.delete_entries);
    add(L"Move entries", capabilities.move_entries);
    add(L"Encryption", capabilities.encryption || capabilities.encrypted);
    add(L"Comments", capabilities.comments);
    add(L"Recovery data", capabilities.recovery_records);
    add(L"Create split volumes", capabilities.can_create_volumes);
    add(L"Multi-volume archive", capabilities.is_multi_volume);
    add(L"Signatures", capabilities.authenticity);
    add(L"Snapshot commands", capabilities.snapshots);
    add(L"Snapshot repository", capabilities.snapshot_repository);
    add(L"Self-extractor", capabilities.sfx);
    add(L"Metadata", capabilities.metadata);
    add(L"Links", capabilities.links);
    if (rows.empty()) rows.push_back({L"Provider capabilities", L"None reported"});
    return rows;
}

enum class AnalysisDialogMode {
    information,
    snapshots,
};

class ArchiveAnalysisDialog {
public:
    ArchiveAnalysisDialog(const std::filesystem::path& archive_path,
                          std::wstring format_name,
                          const ArchiveStorageAnalysis& analysis,
                          ThemePalette theme,
                          AnalysisDialogMode mode,
                          ArchiveSummaryRows details = {},
                          ArchiveCapabilities capabilities = {},
                          std::wstring archive_comment = {},
                          std::function<void(HWND)> estimate_action = {})
        : archive_path_(archive_path), format_name_(std::move(format_name)),
          analysis_(analysis), theme_(theme), mode_(mode),
          detail_rows_(std::move(details)), capabilities_(capabilities),
          archive_comment_(std::move(archive_comment)),
          estimate_action_(std::move(estimate_action)) {}

    ~ArchiveAnalysisDialog() {
        if (font_ != nullptr) DeleteObject(font_);
        if (window_brush_ != nullptr) DeleteObject(window_brush_);
    }

    std::optional<std::string> show(HWND owner) {
        owner_ = owner;
        instance_ = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
        dpi_ = GetDpiForWindow(owner);
        if (!register_class()) return std::nullopt;

        const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU |
                            WS_THICKFRAME | WS_CLIPCHILDREN;
        const DWORD ex_style = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
        const int logical_width = mode_ == AnalysisDialogMode::information ? 1120 : 1080;
        const int logical_height = mode_ == AnalysisDialogMode::information ? 760 : 700;
        const SIZE size = dialog_window_size_for_client(
            logical_width, logical_height, style, ex_style, dpi_);
        const POINT position = centered_window_position(owner, size.cx, size.cy);
        window_ = CreateWindowExW(
            ex_style, class_name(), window_title(), style,
            position.x, position.y, size.cx, size.cy, owner, nullptr,
            instance_, this);
        if (window_ == nullptr) return std::nullopt;
        const int show_command = restore_named_window_placement(
            window_, owner, placement_name());
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
        return selected_snapshot_name_;
    }

private:
    static const wchar_t* class_name() { return L"AxiomArchiveAnalysisDialog"; }

    const wchar_t* window_title() const {
        return mode_ == AnalysisDialogMode::information
            ? L"Information - Axiom" : L"Snapshot timeline";
    }

    const wchar_t* placement_name() const {
        return mode_ == AnalysisDialogMode::information
            ? L"ArchiveInformation" : L"ArchiveSnapshotTimeline";
    }

    bool register_class() const {
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.lpfnWndProc = &ArchiveAnalysisDialog::window_proc;
        window_class.hInstance = instance_;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = nullptr;
        window_class.lpszClassName = class_name();
        window_class.style = CS_DBLCLKS;
        assign_axiom_window_class_icons(window_class, instance_);
        return RegisterClassExW(&window_class) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    int scale(int value) const { return scale_for_dialog_dpi(value, dpi_); }

    HWND make_static(const wchar_t* text) const {
        HWND control = CreateWindowExW(
            0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        set_dialog_control_font(control, font_);
        return control;
    }

    HWND make_button(const wchar_t* text, int id) const {
        HWND control = CreateWindowExW(
            0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        set_dialog_control_font(control, font_);
        return control;
    }

    void configure_table(DarkTableView& table, const wchar_t* accessible_name) {
        table.set_font(font_);
        table.set_dpi(dpi_);
        table.set_theme(theme_);
        table.set_options({true, true, true, false});
        table.set_sort_indicator(-1, true);
        SetWindowTextW(table.hwnd(), accessible_name);
    }

    void rebuild_font() {
        if (font_ != nullptr) DeleteObject(font_);
        font_ = create_dialog_font(dpi_);
        EnumChildWindows(window_, [](HWND child, LPARAM font) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(font_));
        for (DarkTableView* table : {
                 &details_, &capability_list_, &components_, &files_,
                 &timeline_, &changes_}) {
            if (table->hwnd() != nullptr) {
                table->set_font(font_);
                table->set_dpi(dpi_);
            }
        }
    }

    void create_controls() {
        window_brush_ = CreateSolidBrush(theme_.window);
        apply_dialog_dark_frame(window_, theme_.dark);
        apply_axiom_window_icons(window_, instance_);
        rebuild_font();

        const std::wstring header = archive_path_.filename().wstring() + L"  —  " +
            format_name_;
        header_ = make_static(header.c_str());
        path_ = make_static(archive_path_.wstring().c_str());

        if (mode_ == AnalysisDialogMode::information) {
            const std::wstring profile = analysis_.physical_layout_exact
                ? L"Archive details with exact AXAR storage accounting"
                : L"Archive details with provider-reported storage accounting";
            profile_ = make_static(profile.c_str());
            summary_ = make_static(storage_summary(analysis_).c_str());
            hint_ = make_static(
                L"Largest current files are sorted by logical size. A ~ prefix marks an estimated packed size.");

            details_.create(window_, instance_, kInformationDetails);
            details_.set_columns({{L"Property", 175}, {L"Value", 405}});
            capability_list_.create(window_, instance_, kInformationCapabilities);
            capability_list_.set_columns({{L"Capability", 225}, {L"Status", 110}});
            components_.create(window_, instance_, kStorageComponents);
            components_.set_columns({{L"Storage component", 220}, {L"Size", 130},
                                     {L"What it means", 520}});
            files_.create(window_, instance_, kStorageFiles);
            files_.set_columns({{L"Current file", 555}, {L"Logical", 125},
                                {L"Packed", 125}, {L"Chunks", 90}});

            configure_table(details_, L"Archive details");
            configure_table(capability_list_, L"Available archive capabilities");
            configure_table(components_, L"Archive storage components");
            configure_table(files_, L"Largest files in the current archive view");
            refresh_information_rows();

            if (estimate_action_) {
                primary_action_ = make_button(L"Estimate...", kPrimaryAction);
            }
        } else {
            std::wstring profile;
            if (analysis_.snapshots.empty()) {
                profile = L"This archive has no retained snapshots.";
            } else {
                profile = std::to_wstring(analysis_.snapshots.size()) +
                    (analysis_.snapshots.size() == 1
                         ? L" retained snapshot. Select it to inspect changes."
                         : L" retained snapshots. Select one to inspect changes.");
            }
            profile_ = make_static(profile.c_str());
            timeline_.create(window_, instance_, kSnapshotTimeline);
            timeline_.set_columns({{L"Snapshot", 220}, {L"Created", 155},
                                   {L"Entries", 85}, {L"Logical", 120},
                                   {L"New stored", 120}, {L"Changes", 125}});
            changes_.create(window_, instance_, kSnapshotChanges);
            changes_.set_columns({{L"Action", 95}, {L"Path", 555},
                                  {L"Previous", 120}, {L"Snapshot", 120}});
            configure_table(timeline_, L"Snapshot timeline");
            configure_table(changes_, L"Changes in the selected snapshot");
            primary_action_ = make_button(L"Extract snapshot...", kPrimaryAction);
            refresh_timeline_rows();
        }

        close_ = make_button(L"Close", IDCANCEL);
        layout();
    }

    void refresh_information_rows() {
        std::vector<std::vector<std::wstring>> details;
        details.reserve(detail_rows_.size() + (archive_comment_.empty() ? 0u : 1u));
        for (const auto& [label, value] : detail_rows_) {
            details.push_back({label, value});
        }
        if (!archive_comment_.empty()) {
            details.push_back({L"Comment", archive_comment_});
        }
        details_.set_rows(std::move(details), {}, nullptr);
        capability_list_.set_rows(capability_rows(capabilities_), {}, nullptr);

        std::vector<std::vector<std::wstring>> components;
        if (analysis_.physical_layout_exact) {
            components.push_back({L"Referenced payload", storage_size(analysis_.stored_payload_bytes),
                                  L"Compressed blocks needed by the current view and retained snapshots"});
            components.push_back({L"Unreferenced payload", storage_size(analysis_.unreferenced_payload_bytes),
                                  L"Old block data a repack can remove"});
            components.push_back({L"Metadata and services", storage_size(analysis_.metadata_and_service_bytes),
                                  L"Headers, directories, generation history, signatures, and recovery data"});
            if (analysis_.snapshot_repository) {
                components.push_back({L"History-only content", storage_size(analysis_.history_only_content_bytes),
                                      L"Unique content retained only by older snapshots"});
                components.push_back({L"History-only stored", storage_size(analysis_.history_only_stored_bytes),
                                      L"Compressed payload needed only by older snapshots"});
            }
            if (analysis_.deduplicated) {
                components.push_back({L"Deduplication saved", storage_size(analysis_.deduplication_saved_bytes),
                                      L"Repeated logical content represented once"});
            }
            components.push_back({L"Compression saved", storage_size(analysis_.compression_saved_bytes),
                                  L"Unique content bytes avoided by compression"});
        } else {
            components.push_back({L"Archive file", storage_size(analysis_.physical_bytes),
                                  L"Complete on-disk size"});
            components.push_back({L"Reported packed data", storage_size(analysis_.stored_payload_bytes),
                                  analysis_.packed_sizes_complete
                                      ? L"Sum of packed sizes reported for current files"
                                      : L"Partial total; this provider does not report every packed size"});
            if (analysis_.packed_sizes_complete) {
                components.push_back({L"Container remainder", storage_size(analysis_.metadata_and_service_bytes),
                                      L"Headers, directory records, and other format overhead"});
                components.push_back({L"Compression saved", storage_size(analysis_.compression_saved_bytes),
                                      L"Logical bytes minus reported packed bytes"});
            }
        }
        components_.set_rows(std::move(components), {}, nullptr);

        std::vector<std::vector<std::wstring>> files;
        files.reserve(analysis_.files.size());
        for (const auto& file : analysis_.files) {
            std::wstring packed = L"—";
            if (file.packed_bytes) {
                packed = (file.packed_bytes_estimated ? L"~ " : L"") +
                    storage_size(*file.packed_bytes);
            }
            files.push_back({axiom::core::path_from_utf8(file.path).wstring(),
                             storage_size(file.logical_bytes), std::move(packed),
                             file.chunk_count == 0 ? L"—" :
                                 std::to_wstring(file.chunk_count)});
        }
        files_.set_rows(std::move(files), {}, nullptr);
    }

    void refresh_timeline_rows() {
        std::vector<std::vector<std::wstring>> rows;
        rows.reserve(analysis_.snapshots.size());
        for (const auto& snapshot : analysis_.snapshots) {
            std::wstring name = axiom::core::path_from_utf8(snapshot.snapshot.name).wstring();
            if (snapshot.snapshot.current) name += L"  (current)";
            rows.push_back({std::move(name), snapshot_time(snapshot.snapshot.created),
                            std::to_wstring(snapshot.snapshot.entry_count),
                            storage_size(snapshot.snapshot.file_bytes),
                            storage_size(snapshot.new_stored_bytes),
                            snapshot_change_summary(snapshot)});
        }
        timeline_.set_rows(std::move(rows), {}, nullptr);
        if (!analysis_.snapshots.empty()) {
            const auto current = std::find_if(
                analysis_.snapshots.begin(), analysis_.snapshots.end(),
                [](const ArchiveSnapshotStorageInfo& snapshot) {
                    return snapshot.snapshot.current;
                });
            selected_snapshot_ = current == analysis_.snapshots.end()
                ? analysis_.snapshots.size() - 1
                : static_cast<std::size_t>(
                      std::distance(analysis_.snapshots.begin(), current));
            timeline_.select_index(static_cast<int>(selected_snapshot_));
            refresh_change_rows();
        } else {
            EnableWindow(primary_action_, FALSE);
            changes_.clear();
        }
    }

    void refresh_change_rows() {
        if (selected_snapshot_ >= analysis_.snapshots.size()) {
            changes_.clear();
            EnableWindow(primary_action_, FALSE);
            return;
        }
        const auto& snapshot = analysis_.snapshots[selected_snapshot_];
        std::vector<std::vector<std::wstring>> rows;
        rows.reserve(snapshot.changes.size());
        for (const auto& change : snapshot.changes) {
            rows.push_back({change_name(change.kind),
                            axiom::core::path_from_utf8(change.path).wstring(),
                            change.kind == ArchiveSnapshotChangeKind::added
                                ? L"—" : storage_size(change.old_size),
                            change.kind == ArchiveSnapshotChangeKind::removed
                                ? L"—" : storage_size(change.new_size)});
        }
        changes_.set_rows(std::move(rows), {}, nullptr);
        EnableWindow(primary_action_, TRUE);
    }

    void layout() {
        if (window_ == nullptr || header_ == nullptr) return;
        RECT client{};
        GetClientRect(window_, &client);
        const int client_width = static_cast<int>(client.right);
        const int margin = scale(16);
        const int gap = scale(9);
        const int line = scale(22);
        const int button_height = scale(32);
        const int footer_y = client.bottom - margin - button_height;
        const int width = std::max(1, client_width - margin * 2);
        const int content_bottom = footer_y - gap;

        HDWP positions = BeginDeferWindowPos(12);
        const auto place = [&positions](HWND control, int x, int y, int cx, int cy) {
            if (positions == nullptr || control == nullptr) return;
            positions = DeferWindowPos(positions, control, nullptr, x, y, cx, cy,
                                       SWP_NOACTIVATE | SWP_NOZORDER);
        };
        place(header_, margin, margin, width, line);
        place(path_, margin, margin + line, width, line);
        place(profile_, margin, margin + line * 2, width, line);

        if (mode_ == AnalysisDialogMode::information) {
            place(summary_, margin, margin + line * 3, width, line);
            place(hint_, margin, margin + line * 4, width, line);
            const int content_top = margin + line * 5 + gap;
            const int available = std::max(scale(360), content_bottom - content_top);
            const int top_height = std::clamp(available / 3, scale(130), scale(180));
            const int split_width = std::max(scale(300), (width - gap) * 3 / 5);
            place(details_.hwnd(), margin, content_top, split_width, top_height);
            place(capability_list_.hwnd(), margin + split_width + gap, content_top,
                  std::max(1, width - split_width - gap), top_height);

            const int components_top = content_top + top_height + gap;
            const int remaining = std::max(scale(240), content_bottom - components_top);
            const int components_height = std::clamp(
                remaining / 2, scale(120), scale(180));
            place(components_.hwnd(), margin, components_top, width, components_height);
            place(files_.hwnd(), margin, components_top + components_height + gap,
                  width, std::max(scale(120),
                      content_bottom - components_top - components_height - gap));
            place(primary_action_, margin, footer_y, scale(110), button_height);
        } else {
            const int content_top = margin + line * 3 + gap;
            const int available = std::max(scale(280), content_bottom - content_top);
            const int timeline_height = std::clamp(
                available * 2 / 5, scale(160), scale(250));
            place(timeline_.hwnd(), margin, content_top, width, timeline_height);
            place(changes_.hwnd(), margin, content_top + timeline_height + gap,
                  width, std::max(scale(120), available - timeline_height - gap));
            place(primary_action_, margin, footer_y, scale(142), button_height);
        }
        place(close_, client_width - margin - scale(96), footer_y,
              scale(96), button_height);
        if (positions != nullptr) EndDeferWindowPos(positions);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void paint() const {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window_, &paint);
        RECT client{};
        GetClientRect(window_, &client);
        FillRect(dc, &client, window_brush_);
        EndPaint(window_, &paint);
    }

    void close(bool use_snapshot) {
        if (use_snapshot && selected_snapshot_ < analysis_.snapshots.size()) {
            selected_snapshot_name_ = analysis_.snapshots[selected_snapshot_].snapshot.name;
        }
        save_named_window_placement(placement_name(), window_);
        HWND owner = owner_;
        const bool owner_was_enabled = owner_was_enabled_;
        owner_was_enabled_ = false;
        if (window_ != nullptr && IsWindow(window_)) destroy_modal_dialog(window_);
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
                const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE));
                const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE));
                const int width = mode_ == AnalysisDialogMode::information ? 900 : 820;
                const int height = mode_ == AnalysisDialogMode::information ? 620 : 540;
                const SIZE minimum = dialog_window_size_for_client(
                    width, height, style, ex_style, dpi_);
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
            case WM_CTLCOLORSTATIC:
                SetBkColor(reinterpret_cast<HDC>(wparam), theme_.window);
                SetTextColor(reinterpret_cast<HDC>(wparam),
                             reinterpret_cast<HWND>(lparam) == path_ ||
                                     reinterpret_cast<HWND>(lparam) == profile_ ||
                                     reinterpret_cast<HWND>(lparam) == hint_
                                 ? theme_.muted_text : theme_.text);
                return reinterpret_cast<LRESULT>(window_brush_);
            case WM_DRAWITEM:
                if (lparam != 0) {
                    draw_dialog_button(
                        *reinterpret_cast<DRAWITEMSTRUCT*>(lparam), theme_.dark);
                    return TRUE;
                }
                break;
            case kTableSelectionChangedMessage:
                if (mode_ == AnalysisDialogMode::snapshots &&
                    wparam < analysis_.snapshots.size()) {
                    selected_snapshot_ = static_cast<std::size_t>(wparam);
                    refresh_change_rows();
                }
                return 0;
            case WM_COMMAND:
                if (LOWORD(wparam) == kPrimaryAction) {
                    if (mode_ == AnalysisDialogMode::information) {
                        if (estimate_action_) estimate_action_(window_);
                    } else {
                        close(true);
                    }
                    return 0;
                }
                if (LOWORD(wparam) == IDCANCEL) {
                    close(false);
                    return 0;
                }
                break;
            case WM_CLOSE:
                close(false);
                return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
        ArchiveAnalysisDialog* self = nullptr;
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<ArchiveAnalysisDialog*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<ArchiveAnalysisDialog*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        return self != nullptr
            ? self->handle(message, wparam, lparam)
            : DefWindowProcW(window, message, wparam, lparam);
    }

    std::filesystem::path archive_path_;
    std::wstring format_name_;
    const ArchiveStorageAnalysis& analysis_;
    ThemePalette theme_;
    AnalysisDialogMode mode_;
    ArchiveSummaryRows detail_rows_;
    ArchiveCapabilities capabilities_;
    std::wstring archive_comment_;
    std::function<void(HWND)> estimate_action_;
    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    HINSTANCE instance_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    bool owner_was_enabled_ = false;
    std::size_t selected_snapshot_ = std::numeric_limits<std::size_t>::max();
    std::optional<std::string> selected_snapshot_name_;
    HBRUSH window_brush_ = nullptr;
    HFONT font_ = nullptr;
    HWND header_ = nullptr;
    HWND path_ = nullptr;
    HWND profile_ = nullptr;
    HWND summary_ = nullptr;
    HWND hint_ = nullptr;
    DarkTableView details_;
    DarkTableView capability_list_;
    DarkTableView components_;
    DarkTableView files_;
    DarkTableView timeline_;
    DarkTableView changes_;
    HWND primary_action_ = nullptr;
    HWND close_ = nullptr;
};

}  // namespace

ArchiveStorageAnalysis summarize_provider_archive_storage(
    const std::filesystem::path& archive_path,
    const std::vector<ArchiveEntry>& entries) {
    ArchiveStorageAnalysis result;
    std::error_code size_error;
    result.physical_bytes = std::filesystem::file_size(archive_path, size_error);
    if (size_error) {
        throw std::runtime_error("cannot read archive file size: " + size_error.message());
    }
    result.packed_sizes_complete = true;
    result.files.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.is_directory || entry.is_symlink || entry.is_hardlink) continue;
        result.logical_bytes = add_storage_bytes(result.logical_bytes, entry.size);
        ArchiveStorageFileInfo file;
        file.path = entry.path;
        file.logical_bytes = entry.size;
        file.packed_bytes = entry.packed_size;
        file.packed_bytes_estimated = entry.packed_size_estimated;
        file.chunk_count = entry.chunk_count;
        if (entry.packed_size) {
            result.stored_payload_bytes = add_storage_bytes(
                result.stored_payload_bytes, *entry.packed_size);
        } else {
            result.packed_sizes_complete = false;
        }
        result.files.push_back(std::move(file));
    }
    result.referenced_logical_bytes = result.logical_bytes;
    result.unique_content_bytes = result.logical_bytes;
    if (result.stored_payload_bytes > result.physical_bytes) {
        result.packed_sizes_complete = false;
    }
    if (result.packed_sizes_complete &&
        result.stored_payload_bytes <= result.physical_bytes) {
        result.metadata_and_service_bytes =
            result.physical_bytes - result.stored_payload_bytes;
        if (result.logical_bytes > result.stored_payload_bytes) {
            result.compression_saved_bytes =
                result.logical_bytes - result.stored_payload_bytes;
        }
    }
    std::sort(result.files.begin(), result.files.end(),
              [](const ArchiveStorageFileInfo& left,
                 const ArchiveStorageFileInfo& right) {
                  if (left.logical_bytes != right.logical_bytes) {
                      return left.logical_bytes > right.logical_bytes;
                  }
                  return left.path < right.path;
              });
    return result;
}

void show_archive_information_dialog(
    HWND owner,
    const std::filesystem::path& archive_path,
    const ArchiveSummaryRows& details,
    const ArchiveCapabilities& capabilities,
    const ArchiveStorageAnalysis& analysis,
    const ThemePalette& theme,
    std::wstring archive_comment,
    std::function<void(HWND)> estimate_action) {
    const auto* provider = axiom::archive_provider_for_path(archive_path);
    const std::wstring format_name = provider != nullptr
        ? std::wstring(provider->info().display_name.begin(),
                       provider->info().display_name.end())
        : L"Archive";
    ArchiveAnalysisDialog dialog(
        archive_path, format_name, analysis, theme,
        AnalysisDialogMode::information, details, capabilities,
        std::move(archive_comment), std::move(estimate_action));
    dialog.show(owner);
}

std::optional<std::string> show_archive_snapshot_timeline_dialog(
    HWND owner,
    const std::filesystem::path& archive_path,
    const std::wstring& format_name,
    const ArchiveStorageAnalysis& analysis,
    const ThemePalette& theme) {
    ArchiveAnalysisDialog dialog(
        archive_path, format_name, analysis, theme,
        AnalysisDialogMode::snapshots);
    return dialog.show(owner);
}

}  // namespace axiom::gui
