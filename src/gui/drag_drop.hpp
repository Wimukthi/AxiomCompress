#pragma once

#define NOMINMAX
#include <windows.h>
#include <objidl.h>

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

struct FileDragSource {
    std::function<std::vector<std::filesystem::path>()> files;
    std::vector<VirtualFileDragItem> virtual_files;
    std::function<std::vector<std::filesystem::path>()> virtual_file_paths;
    ArchiveDragPayload archive_payload;
    DWORD preferred_effect = DROPEFFECT_COPY;
    std::wstring* error_message = nullptr;
};

bool data_object_has_file_drop(IDataObject* data_object);
std::vector<std::filesystem::path> read_file_drop(IDataObject* data_object);
bool data_object_has_archive_entries(IDataObject* data_object);
bool read_archive_entries(IDataObject* data_object, ArchiveDragPayload& payload);

HRESULT do_file_drag(FileDragSource source, DWORD allowed_effects, DWORD& performed_effect);

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
