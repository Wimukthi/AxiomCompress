#pragma once

#define NOMINMAX
#include <windows.h>
#include <objidl.h>

#include "axiom/axiom.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace axiom::gui {

struct ArchiveDragPayload {
    std::filesystem::path archive_path;
    std::vector<std::string> entry_paths;
};

struct VirtualFileDragItem {
    std::wstring relative_path;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    bool is_directory = false;
};

// How far the drop target has read the dragged files. The target may go on
// reading after DoDragDrop returns, so this outlives the drag: `released` says
// the target let go of the data, and `open_streams` how many files it still has
// open.
struct DragTransferStatus {
    std::shared_ptr<OperationControl> operation;
    std::uint64_t total_bytes = 0;
    std::uint64_t total_files = 0;
    std::atomic_uint64_t completed_bytes{0};
    std::atomic_uint64_t completed_files{0};
    std::atomic_int open_streams{0};
    std::atomic_bool released{false};
    // A target that copies on its own thread says when it starts and ends
    // (IDataObjectAsyncCapability), so the source knows when its files are free.
    std::atomic_bool async_started{false};
    std::atomic_bool async_ended{false};
    std::atomic<HRESULT> async_result{S_OK};
};

struct FileDragSource {
    std::function<std::vector<std::filesystem::path>()> files;
    std::vector<VirtualFileDragItem> virtual_files;
    std::function<std::vector<std::filesystem::path>()> virtual_file_paths;
    std::shared_ptr<OperationControl> transfer_operation;
    // Optional: shared with the caller to follow the transfer to its end.
    std::shared_ptr<DragTransferStatus> transfer_status;
    // Optional: called once when the mouse is released over another program
    // that accepts the drop, before the drop reaches it, to create the files
    // `files` names. False cancels the drop.
    std::function<bool()> prepare_drop;
    ArchiveDragPayload archive_payload;
    DWORD preferred_effect = DROPEFFECT_COPY;
    std::wstring* error_message = nullptr;
};

bool data_object_has_file_drop(IDataObject* data_object);
std::vector<std::filesystem::path> read_file_drop(IDataObject* data_object);
bool data_object_has_archive_entries(IDataObject* data_object);
bool read_archive_entries(IDataObject* data_object, ArchiveDragPayload& payload);

HRESULT do_file_drag(FileDragSource source, DWORD allowed_effects, DWORD& performed_effect);

// Publishes a shell-compatible file list and renders it immediately so staged
// archive entries remain pasteable after the IDataObject is released.
HRESULT set_file_clipboard(FileDragSource source);

class OleDropTarget final {
public:
    using Query = std::function<DWORD(IDataObject*, POINT, DWORD, DWORD)>;
    using DropHandler = std::function<DWORD(IDataObject*, POINT, DWORD, DWORD)>;

    OleDropTarget() = default;
    ~OleDropTarget();

    OleDropTarget(const OleDropTarget&) = delete;
    OleDropTarget& operator=(const OleDropTarget&) = delete;

    bool register_window(HWND window, Query query, DropHandler drop);
    void revoke();

private:
    class Implementation;
    HWND window_ = nullptr;
    Implementation* implementation_ = nullptr;
};

}  // namespace axiom::gui
