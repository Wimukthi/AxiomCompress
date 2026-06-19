#include "gui/drag_drop.hpp"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace axiom::gui {
namespace {

constexpr std::uint32_t kArchivePayloadMagic = 0x31445841;  // AXD1

CLIPFORMAT archive_entries_format() {
    static const CLIPFORMAT format =
        static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Axiom.ArchiveEntries.v1"));
    return format;
}

CLIPFORMAT preferred_drop_effect_format() {
    static const CLIPFORMAT format =
        static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
    return format;
}

bool format_matches(const FORMATETC& requested, CLIPFORMAT format) {
    return requested.cfFormat == format &&
           (requested.tymed & TYMED_HGLOBAL) != 0 &&
           requested.dwAspect == DVASPECT_CONTENT && requested.lindex == -1;
}

HGLOBAL make_global(const void* bytes, std::size_t size) {
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
    if (global == nullptr) return nullptr;
    void* destination = GlobalLock(global);
    if (destination == nullptr) {
        GlobalFree(global);
        return nullptr;
    }
    if (size != 0) std::memcpy(destination, bytes, size);
    GlobalUnlock(global);
    return global;
}

HGLOBAL make_drop_files(const std::vector<std::filesystem::path>& paths) {
    std::size_t characters = 1;
    for (const auto& path : paths) {
        const auto text = path.wstring();
        if (text.size() > std::numeric_limits<std::size_t>::max() - characters - 1) {
            return nullptr;
        }
        characters += text.size() + 1;
    }
    if (characters > (std::numeric_limits<std::size_t>::max() - sizeof(DROPFILES)) /
                         sizeof(wchar_t)) {
        return nullptr;
    }
    const std::size_t bytes = sizeof(DROPFILES) + characters * sizeof(wchar_t);
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (global == nullptr) return nullptr;
    auto* drop = static_cast<DROPFILES*>(GlobalLock(global));
    if (drop == nullptr) {
        GlobalFree(global);
        return nullptr;
    }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    auto* cursor = reinterpret_cast<wchar_t*>(reinterpret_cast<std::byte*>(drop) + sizeof(DROPFILES));
    for (const auto& path : paths) {
        const auto text = path.wstring();
        std::memcpy(cursor, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        cursor += text.size() + 1;
    }
    *cursor = L'\0';
    GlobalUnlock(global);
    return global;
}

void append_wstring(std::vector<std::byte>& bytes, const std::wstring& text) {
    const std::size_t old_size = bytes.size();
    bytes.resize(old_size + (text.size() + 1) * sizeof(wchar_t));
    std::memcpy(bytes.data() + old_size, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
}

HGLOBAL make_archive_payload(const ArchiveDragPayload& payload) {
    const std::uint32_t count = static_cast<std::uint32_t>(payload.entry_paths.size());
    std::vector<std::byte> bytes(sizeof(kArchivePayloadMagic) + sizeof(count));
    std::memcpy(bytes.data(), &kArchivePayloadMagic, sizeof(kArchivePayloadMagic));
    std::memcpy(bytes.data() + sizeof(kArchivePayloadMagic), &count, sizeof(count));
    append_wstring(bytes, payload.archive_path.wstring());
    for (const auto& entry : payload.entry_paths) {
        const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                entry.data(), static_cast<int>(entry.size()),
                                                nullptr, 0);
        if (needed <= 0) return nullptr;
        std::wstring wide(static_cast<std::size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            entry.data(), static_cast<int>(entry.size()),
                            wide.data(), needed);
        append_wstring(bytes, wide);
    }
    return make_global(bytes.data(), bytes.size());
}

bool copy_medium(IDataObject* object, CLIPFORMAT format, STGMEDIUM& medium) {
    FORMATETC request{format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return SUCCEEDED(object->GetData(&request, &medium));
}

std::wstring error_text(const std::exception& error) {
    const std::string text = error.what();
    if (text.empty()) return L"Could not prepare the dragged archive entries.";
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            text.data(), static_cast<int>(text.size()),
                                            nullptr, 0);
    if (needed <= 0) return L"Could not prepare the dragged archive entries.";
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        text.data(), static_cast<int>(text.size()), result.data(), needed);
    return result;
}

class FormatEnumerator final : public IEnumFORMATETC {
public:
    explicit FormatEnumerator(std::vector<FORMATETC> formats) : formats_(std::move(formats)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IEnumFORMATETC) {
            *object = static_cast<IEnumFORMATETC*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = --references_;
        if (references == 0) delete this;
        return references;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG count, FORMATETC* formats, ULONG* fetched) override {
        if (formats == nullptr || (count != 1 && fetched == nullptr)) return E_POINTER;
        ULONG copied = 0;
        while (copied < count && index_ < formats_.size()) {
            formats[copied++] = formats_[index_++];
        }
        if (fetched != nullptr) *fetched = copied;
        return copied == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        const std::size_t remaining = formats_.size() - index_;
        const std::size_t skipped = std::min<std::size_t>(count, remaining);
        index_ += skipped;
        return skipped == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Reset() override {
        index_ = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** copy) override {
        if (copy == nullptr) return E_POINTER;
        auto* result = new (std::nothrow) FormatEnumerator(formats_);
        if (result == nullptr) return E_OUTOFMEMORY;
        result->index_ = index_;
        *copy = result;
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
    std::vector<FORMATETC> formats_;
    std::size_t index_ = 0;
};

class FileDataObject final : public IDataObject {
public:
    explicit FileDataObject(FileDragSource source) : source_(std::move(source)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDataObject) {
            *object = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = --references_;
        if (references == 0) delete this;
        return references;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (format == nullptr || medium == nullptr) return E_POINTER;
        *medium = {};
        HGLOBAL global = nullptr;
        if (format_matches(*format, CF_HDROP)) {
            try {
                if (!files_ready_) {
                    files_ = source_.files ? source_.files() : std::vector<std::filesystem::path>{};
                    files_ready_ = true;
                }
                if (files_.empty()) return DV_E_FORMATETC;
                global = make_drop_files(files_);
            } catch (const std::exception& error) {
                if (source_.error_message != nullptr) *source_.error_message = error_text(error);
                return E_FAIL;
            } catch (...) {
                if (source_.error_message != nullptr) {
                    *source_.error_message = L"Could not prepare the dragged archive entries.";
                }
                return E_FAIL;
            }
        } else if (format_matches(*format, archive_entries_format()) &&
                   !source_.archive_payload.entry_paths.empty()) {
            global = make_archive_payload(source_.archive_payload);
        } else if (format_matches(*format, preferred_drop_effect_format())) {
            global = make_global(&source_.preferred_effect, sizeof(source_.preferred_effect));
        } else {
            return DV_E_FORMATETC;
        }
        if (global == nullptr) return E_OUTOFMEMORY;
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = global;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
        return DATA_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (format == nullptr) return E_POINTER;
        if (format_matches(*format, CF_HDROP) ||
            format_matches(*format, preferred_drop_effect_format()) ||
            (format_matches(*format, archive_entries_format()) &&
             !source_.archive_payload.entry_paths.empty())) {
            return S_OK;
        }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override {
        if (output == nullptr) return E_POINTER;
        output->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** enumerator) override {
        if (enumerator == nullptr) return E_POINTER;
        *enumerator = nullptr;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        std::vector<FORMATETC> formats{
            {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
            {preferred_drop_effect_format(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        };
        if (!source_.archive_payload.entry_paths.empty()) {
            formats.push_back({archive_entries_format(), nullptr,
                               DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
        }
        *enumerator = new (std::nothrow) FormatEnumerator(std::move(formats));
        return *enumerator == nullptr ? E_OUTOFMEMORY : S_OK;
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    std::atomic<ULONG> references_{1};
    FileDragSource source_;
    bool files_ready_ = false;
    std::vector<std::filesystem::path> files_;
};

class DropSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDropSource) {
            *object = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = --references_;
        if (references == 0) delete this;
        return references;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD key_state) override {
        if (escape) return DRAGDROP_S_CANCEL;
        if ((key_state & MK_LBUTTON) == 0) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    std::atomic<ULONG> references_{1};
};

DWORD clamp_effect(DWORD effect, DWORD allowed) {
    effect &= allowed & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    if ((effect & DROPEFFECT_MOVE) != 0) return DROPEFFECT_MOVE;
    if ((effect & DROPEFFECT_COPY) != 0) return DROPEFFECT_COPY;
    if ((effect & DROPEFFECT_LINK) != 0) return DROPEFFECT_LINK;
    return DROPEFFECT_NONE;
}

}  // namespace

bool data_object_has_file_drop(IDataObject* data_object) {
    if (data_object == nullptr) return false;
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return data_object->QueryGetData(&format) == S_OK;
}

std::vector<std::filesystem::path> read_file_drop(IDataObject* data_object) {
    std::vector<std::filesystem::path> paths;
    if (data_object == nullptr) return paths;
    STGMEDIUM medium{};
    if (!copy_medium(data_object, CF_HDROP, medium)) return paths;
    const HDROP drop = static_cast<HDROP>(medium.hGlobal);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    paths.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
        if (DragQueryFileW(drop, index, path.data(), length + 1) != 0) {
            path.resize(length);
            paths.emplace_back(std::move(path));
        }
    }
    ReleaseStgMedium(&medium);
    return paths;
}

bool data_object_has_archive_entries(IDataObject* data_object) {
    if (data_object == nullptr) return false;
    FORMATETC format{archive_entries_format(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return data_object->QueryGetData(&format) == S_OK;
}

bool read_archive_entries(IDataObject* data_object, ArchiveDragPayload& payload) {
    payload = {};
    if (data_object == nullptr) return false;
    STGMEDIUM medium{};
    if (!copy_medium(data_object, archive_entries_format(), medium)) return false;
    const std::size_t bytes = GlobalSize(medium.hGlobal);
    const auto* data = static_cast<const std::byte*>(GlobalLock(medium.hGlobal));
    bool valid = false;
    if (data != nullptr && bytes >= sizeof(std::uint32_t) * 2) {
        std::uint32_t magic = 0;
        std::uint32_t count = 0;
        std::memcpy(&magic, data, sizeof(magic));
        std::memcpy(&count, data + sizeof(magic), sizeof(count));
        const auto* cursor = reinterpret_cast<const wchar_t*>(data + sizeof(magic) + sizeof(count));
        const auto* end = reinterpret_cast<const wchar_t*>(data + bytes);
        auto next_string = [&]() -> const wchar_t* {
            if (cursor >= end) return nullptr;
            const wchar_t* start = cursor;
            while (cursor < end && *cursor != L'\0') ++cursor;
            if (cursor >= end) return nullptr;
            ++cursor;
            return start;
        };
        if (magic == kArchivePayloadMagic && count <= 100000) {
            if (const wchar_t* archive = next_string()) {
                payload.archive_path = archive;
                payload.entry_paths.reserve(count);
                valid = true;
                for (std::uint32_t index = 0; index < count; ++index) {
                    const wchar_t* entry = next_string();
                    if (entry == nullptr) {
                        valid = false;
                        break;
                    }
                    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                                            entry, -1, nullptr, 0,
                                                            nullptr, nullptr);
                    if (needed <= 1) {
                        valid = false;
                        break;
                    }
                    std::string encoded(static_cast<std::size_t>(needed), '\0');
                    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                        entry, -1, encoded.data(), needed,
                                        nullptr, nullptr);
                    encoded.resize(static_cast<std::size_t>(needed - 1));
                    payload.entry_paths.push_back(std::move(encoded));
                }
            }
        }
    }
    if (data != nullptr) GlobalUnlock(medium.hGlobal);
    ReleaseStgMedium(&medium);
    if (!valid) payload = {};
    return valid;
}

HRESULT do_file_drag(FileDragSource source, DWORD allowed_effects, DWORD& performed_effect) {
    performed_effect = DROPEFFECT_NONE;
    auto* data = new (std::nothrow) FileDataObject(std::move(source));
    auto* source_object = new (std::nothrow) DropSource();
    if (data == nullptr || source_object == nullptr) {
        if (data != nullptr) data->Release();
        if (source_object != nullptr) source_object->Release();
        return E_OUTOFMEMORY;
    }
    const HRESULT result = DoDragDrop(data, source_object, allowed_effects, &performed_effect);
    data->Release();
    source_object->Release();
    return result;
}

class OleDropTarget::Implementation final : public IDropTarget {
public:
    Implementation(Query query, DropHandler drop)
        : query_(std::move(query)), drop_(std::move(drop)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = --references_;
        if (references == 0) delete this;
        return references;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* object, DWORD key_state,
                                        POINTL point, DWORD* effect) override {
        if (effect == nullptr) return E_POINTER;
        release_current();
        current_ = object;
        if (current_ != nullptr) current_->AddRef();
        const POINT screen{point.x, point.y};
        *effect = clamp_effect(query_ ? query_(object, screen, key_state, *effect)
                                      : DROPEFFECT_NONE, *effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD key_state, POINTL point, DWORD* effect) override {
        if (effect == nullptr) return E_POINTER;
        const POINT screen{point.x, point.y};
        *effect = clamp_effect(query_ ? query_(current_, screen, key_state, *effect)
                                      : DROPEFFECT_NONE, *effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        release_current();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* object, DWORD key_state,
                                   POINTL point, DWORD* effect) override {
        if (effect == nullptr) return E_POINTER;
        const DWORD allowed = *effect;
        const POINT screen{point.x, point.y};
        *effect = clamp_effect(drop_ ? drop_(object, screen, key_state, allowed)
                                    : DROPEFFECT_NONE, allowed);
        release_current();
        return S_OK;
    }

private:
    ~Implementation() { release_current(); }
    void release_current() {
        if (current_ != nullptr) {
            current_->Release();
            current_ = nullptr;
        }
    }

    std::atomic<ULONG> references_{1};
    Query query_;
    DropHandler drop_;
    IDataObject* current_ = nullptr;
};

OleDropTarget::~OleDropTarget() { revoke(); }

bool OleDropTarget::register_window(HWND window, Query query, DropHandler drop) {
    revoke();
    auto* implementation = new (std::nothrow) Implementation(std::move(query), std::move(drop));
    if (implementation == nullptr) return false;
    const HRESULT result = RegisterDragDrop(window, implementation);
    if (FAILED(result)) {
        implementation->Release();
        return false;
    }
    window_ = window;
    implementation_ = implementation;
    return true;
}

void OleDropTarget::revoke() {
    if (window_ != nullptr) {
        RevokeDragDrop(window_);
        window_ = nullptr;
    }
    if (implementation_ != nullptr) {
        implementation_->Release();
        implementation_ = nullptr;
    }
}

}  // namespace axiom::gui
