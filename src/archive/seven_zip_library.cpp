#include "archive/seven_zip_library.hpp"

#ifdef _WIN32

#include "archive/seven_zip_abi.hpp"
#include "core/path_text.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace axiom {
namespace fs = std::filesystem;
namespace sz = seven_zip_abi;
namespace {

constexpr std::uint64_t kFileTimeEpoch =
    11'644'473'600ull * 10'000'000ull;
constexpr std::uint64_t kProgressQuantum = 1u << 20;

std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        throw std::runtime_error("7-Zip returned invalid UTF-8 text");
    }
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), result.data(), needed) != needed) {
        throw std::runtime_error("could not convert text for 7-Zip");
    }
    return result;
}

std::string wide_to_utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        throw std::runtime_error("7-Zip returned an invalid Unicode path");
    }
    std::string result(static_cast<std::size_t>(needed), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), result.data(), needed,
            nullptr, nullptr) != needed) {
        throw std::runtime_error("could not convert a 7-Zip path");
    }
    return result;
}

std::string normalize_archive_path(std::wstring_view source) {
    std::string path = wide_to_utf8(source);
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

bool safe_relative_path(std::string_view text) {
    if (text.empty()) return false;
    const fs::path path = core::path_from_utf8(text);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return false;
    }
    for (const auto& component : path) {
        if (component == "..") return false;
    }
    return true;
}

bool safe_relative_path(const fs::path& path) {
    if (path.empty() || path.is_absolute() ||
        path.has_root_name() || path.has_root_directory()) {
        return false;
    }
    for (const auto& component : path) {
        if (component == L"..") return false;
    }
    return true;
}

std::int64_t file_time_to_unix(const FILETIME& time) {
    const std::uint64_t ticks =
        static_cast<std::uint64_t>(time.dwLowDateTime) |
        (static_cast<std::uint64_t>(time.dwHighDateTime) << 32);
    if (ticks < kFileTimeEpoch) return 0;
    const std::uint64_t seconds = (ticks - kFileTimeEpoch) / 10'000'000ull;
    return seconds <= static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max())
        ? static_cast<std::int64_t>(seconds)
        : 0;
}

HRESULT hresult_from_last_error() noexcept {
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
}

std::string hresult_message(std::string_view action, HRESULT result) {
    char hex[16]{};
    std::snprintf(hex, sizeof(hex), "0x%08lX",
                  static_cast<unsigned long>(result));
    return std::string(action) + " failed (" + hex + ")";
}

class PropVariant {
public:
    PropVariant() = default;
    ~PropVariant() { PropVariantClear(&value_); }

    PropVariant(const PropVariant&) = delete;
    PropVariant& operator=(const PropVariant&) = delete;
    PropVariant(PropVariant&& other) noexcept : value_(other.value_) {
        std::memset(&other.value_, 0, sizeof(other.value_));
    }
    PropVariant& operator=(PropVariant&& other) noexcept {
        if (this != &other) {
            PropVariantClear(&value_);
            value_ = other.value_;
            std::memset(&other.value_, 0, sizeof(other.value_));
        }
        return *this;
    }

    PROPVARIANT* put() {
        PropVariantClear(&value_);
        std::memset(&value_, 0, sizeof(value_));
        return &value_;
    }
    const PROPVARIANT& get() const { return value_; }

private:
    PROPVARIANT value_{};
};

std::optional<std::wstring> property_string(const PROPVARIANT& value) {
    if (value.vt == VT_EMPTY) return std::nullopt;
    if (value.vt != VT_BSTR || value.bstrVal == nullptr) {
        throw std::runtime_error("7-Zip returned a property with an unexpected type");
    }
    return std::wstring(value.bstrVal, SysStringLen(value.bstrVal));
}

std::optional<std::uint64_t> property_u64(const PROPVARIANT& value) {
    switch (value.vt) {
        case VT_EMPTY:
            return std::nullopt;
        case VT_UI1:
            return value.bVal;
        case VT_UI2:
            return value.uiVal;
        case VT_UI4:
            return value.ulVal;
        case VT_UI8:
            return value.uhVal.QuadPart;
        case VT_I1:
            return value.cVal >= 0
                ? std::optional<std::uint64_t>(
                      static_cast<std::uint64_t>(value.cVal))
                : std::nullopt;
        case VT_I2:
            return value.iVal >= 0
                ? std::optional<std::uint64_t>(
                      static_cast<std::uint64_t>(value.iVal))
                : std::nullopt;
        case VT_I4:
            return value.lVal >= 0
                ? std::optional<std::uint64_t>(
                      static_cast<std::uint64_t>(value.lVal))
                : std::nullopt;
        case VT_I8:
            return value.hVal.QuadPart >= 0
                ? std::optional<std::uint64_t>(
                      static_cast<std::uint64_t>(value.hVal.QuadPart))
                : std::nullopt;
        default:
            throw std::runtime_error("7-Zip returned a numeric property with an unexpected type");
    }
}

bool property_bool(const PROPVARIANT& value) {
    if (value.vt == VT_EMPTY) return false;
    if (value.vt != VT_BOOL) {
        throw std::runtime_error("7-Zip returned a Boolean property with an unexpected type");
    }
    return value.boolVal != VARIANT_FALSE;
}

std::optional<FILETIME> property_file_time(const PROPVARIANT& value) {
    if (value.vt == VT_EMPTY) return std::nullopt;
    if (value.vt != VT_FILETIME) {
        throw std::runtime_error("7-Zip returned a timestamp with an unexpected type");
    }
    return value.filetime;
}

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    explicit ComPtr(T* pointer) : pointer_(pointer) {}
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept
        : pointer_(std::exchange(other.pointer_, nullptr)) {}
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            pointer_ = std::exchange(other.pointer_, nullptr);
        }
        return *this;
    }

    T* get() const { return pointer_; }
    T* operator->() const { return pointer_; }
    explicit operator bool() const { return pointer_ != nullptr; }

    T** put() {
        reset();
        return &pointer_;
    }

    void reset(T* pointer = nullptr) {
        if (pointer_ != nullptr) pointer_->Release();
        pointer_ = pointer;
    }

private:
    T* pointer_ = nullptr;
};

class RefCounted {
public:
    ULONG add_ref() noexcept {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG release() noexcept {
        const ULONG remaining =
            references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return remaining;
    }

protected:
    virtual ~RefCounted() = default;

private:
    std::atomic<ULONG> references_{1};
};

class FileInStream final : public sz::IInStream, public RefCounted {
public:
    explicit FileInStream(const fs::path& path) {
        handle_ = CreateFileW(
            path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error(
                "could not open archive for 7-Zip: " +
                core::path_to_utf8(path));
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid, void** object) noexcept override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) ||
            IsEqualIID(iid, sz::IID_IInStream)) {
            *object = static_cast<sz::IInStream*>(this);
        } else if (IsEqualIID(iid, sz::IID_ISequentialInStream)) {
            *object = static_cast<sz::ISequentialInStream*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return add_ref(); }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return release(); }

    HRESULT STDMETHODCALLTYPE Read(
        void* data, sz::UInt32 size,
        sz::UInt32* processed_size) noexcept override {
        if (processed_size != nullptr) *processed_size = 0;
        DWORD processed = 0;
        if (!ReadFile(handle_, data, size, &processed, nullptr)) {
            return hresult_from_last_error();
        }
        if (processed_size != nullptr) *processed_size = processed;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Seek(
        sz::Int64 offset, sz::UInt32 seek_origin,
        sz::UInt64* new_position) noexcept override {
        if (seek_origin > STREAM_SEEK_END) return STG_E_INVALIDFUNCTION;
        LARGE_INTEGER distance{};
        distance.QuadPart = offset;
        LARGE_INTEGER position{};
        if (!SetFilePointerEx(handle_, distance, &position, seek_origin)) {
            return hresult_from_last_error();
        }
        if (position.QuadPart < 0) return STG_E_INVALIDFUNCTION;
        if (new_position != nullptr) {
            *new_position = static_cast<sz::UInt64>(position.QuadPart);
        }
        return S_OK;
    }

protected:
    ~FileInStream() override {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::vector<fs::path> split_volume_paths(const fs::path& path) {
    const std::wstring filename = path.filename().wstring();
    const std::size_t separator = filename.find_last_of(L'.');
    if (separator == std::wstring::npos ||
        separator + 4 > filename.size()) {
        return {};
    }
    const std::wstring_view suffix(filename.data() + separator + 1,
                                   filename.size() - separator - 1);
    if (suffix.size() < 3 ||
        !std::all_of(suffix.begin(), suffix.end(), [](wchar_t character) {
            return character >= L'0' && character <= L'9';
        })) {
        return {};
    }

    const std::wstring prefix = filename.substr(0, separator + 1);
    std::vector<std::pair<std::uint64_t, fs::path>> numbered;
    std::error_code error;
    for (fs::directory_iterator iterator(path.parent_path(), error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_regular_file(error) || error) {
            error.clear();
            continue;
        }
        const std::wstring candidate = iterator->path().filename().wstring();
        if (candidate.size() <= prefix.size() ||
            candidate.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        const std::wstring_view number(
            candidate.data() + prefix.size(),
            candidate.size() - prefix.size());
        if (!std::all_of(number.begin(), number.end(), [](wchar_t character) {
                return character >= L'0' && character <= L'9';
            })) {
            continue;
        }
        std::uint64_t index = 0;
        bool overflow = false;
        for (wchar_t character : number) {
            const std::uint64_t digit =
                static_cast<std::uint64_t>(character - L'0');
            if (index > (std::numeric_limits<std::uint64_t>::max() - digit) /
                            10u) {
                overflow = true;
                break;
            }
            index = index * 10u + digit;
        }
        if (!overflow && index != 0) {
            numbered.emplace_back(index, iterator->path());
        }
    }
    if (error || numbered.size() < 2) return {};

    std::sort(numbered.begin(), numbered.end(),
              [](const auto& left, const auto& right) {
                  return left.first < right.first;
              });
    if (numbered.front().first != 1) return {};
    std::vector<fs::path> paths;
    paths.reserve(numbered.size());
    for (std::size_t index = 0; index < numbered.size(); ++index) {
        if (numbered[index].first != index + 1u) return {};
        paths.push_back(std::move(numbered[index].second));
    }
    return paths;
}

class SplitFileInStream final : public sz::IInStream, public RefCounted {
public:
    explicit SplitFileInStream(const std::vector<fs::path>& paths) {
        try {
            parts_.reserve(paths.size());
            for (const fs::path& path : paths) {
                Part part;
                part.handle = CreateFileW(
                    path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
                if (part.handle == INVALID_HANDLE_VALUE) {
                    throw std::runtime_error(
                        "could not open a split archive volume: " +
                        core::path_to_utf8(path));
                }
                LARGE_INTEGER size{};
                if (!GetFileSizeEx(part.handle, &size) ||
                    size.QuadPart < 0) {
                    CloseHandle(part.handle);
                    part.handle = INVALID_HANDLE_VALUE;
                    throw std::runtime_error(
                        "could not read a split archive volume size: " +
                        core::path_to_utf8(path));
                }
                part.start = total_size_;
                part.size = static_cast<std::uint64_t>(size.QuadPart);
                if (part.size >
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()) -
                        total_size_) {
                    CloseHandle(part.handle);
                    part.handle = INVALID_HANDLE_VALUE;
                    throw std::runtime_error("split archive is too large");
                }
                total_size_ += part.size;
                parts_.push_back(part);
            }
        } catch (...) {
            close_handles();
            throw;
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid, void** object) noexcept override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) ||
            IsEqualIID(iid, sz::IID_IInStream)) {
            *object = static_cast<sz::IInStream*>(this);
        } else if (IsEqualIID(iid, sz::IID_ISequentialInStream)) {
            *object = static_cast<sz::ISequentialInStream*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return add_ref(); }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return release(); }

    HRESULT STDMETHODCALLTYPE Read(
        void* data, sz::UInt32 size,
        sz::UInt32* processed_size) noexcept override {
        if (processed_size != nullptr) *processed_size = 0;
        auto* output = static_cast<std::uint8_t*>(data);
        sz::UInt32 total_processed = 0;
        while (total_processed < size && position_ < total_size_) {
            const auto part_iterator = std::upper_bound(
                parts_.begin(), parts_.end(), position_,
                [](std::uint64_t position, const Part& part) {
                    return position < part.start;
                });
            if (part_iterator == parts_.begin()) return E_FAIL;
            const Part& part = *std::prev(part_iterator);
            const std::uint64_t local_position = position_ - part.start;
            if (local_position >= part.size) {
                position_ = part.start + part.size;
                continue;
            }
            LARGE_INTEGER distance{};
            distance.QuadPart = static_cast<LONGLONG>(local_position);
            if (!SetFilePointerEx(
                    part.handle, distance, nullptr, FILE_BEGIN)) {
                return hresult_from_last_error();
            }
            const DWORD requested = static_cast<DWORD>(std::min<std::uint64_t>(
                size - total_processed, part.size - local_position));
            DWORD processed = 0;
            if (!ReadFile(part.handle, output + total_processed,
                          requested, &processed, nullptr)) {
                return hresult_from_last_error();
            }
            position_ += processed;
            total_processed += processed;
            if (processed == 0) break;
        }
        if (processed_size != nullptr) *processed_size = total_processed;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Seek(
        sz::Int64 offset, sz::UInt32 seek_origin,
        sz::UInt64* new_position) noexcept override {
        std::int64_t base = 0;
        switch (seek_origin) {
            case STREAM_SEEK_SET:
                break;
            case STREAM_SEEK_CUR:
                base = static_cast<std::int64_t>(position_);
                break;
            case STREAM_SEEK_END:
                base = static_cast<std::int64_t>(total_size_);
                break;
            default:
                return STG_E_INVALIDFUNCTION;
        }
        std::uint64_t target = 0;
        if (offset < 0) {
            const std::uint64_t magnitude =
                static_cast<std::uint64_t>(-(offset + 1)) + 1u;
            if (magnitude > static_cast<std::uint64_t>(base)) {
                return STG_E_INVALIDFUNCTION;
            }
            target = static_cast<std::uint64_t>(base) - magnitude;
        } else {
            const std::uint64_t positive =
                static_cast<std::uint64_t>(offset);
            if (positive >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max() - base)) {
                return STG_E_INVALIDFUNCTION;
            }
            target = static_cast<std::uint64_t>(base) + positive;
        }
        position_ = target;
        if (new_position != nullptr) *new_position = position_;
        return S_OK;
    }

protected:
    ~SplitFileInStream() override { close_handles(); }

private:
    struct Part {
        HANDLE handle = INVALID_HANDLE_VALUE;
        std::uint64_t start = 0;
        std::uint64_t size = 0;
    };

    void close_handles() noexcept {
        for (Part& part : parts_) {
            if (part.handle != INVALID_HANDLE_VALUE) {
                CloseHandle(part.handle);
                part.handle = INVALID_HANDLE_VALUE;
            }
        }
    }

    std::vector<Part> parts_;
    std::uint64_t position_ = 0;
    std::uint64_t total_size_ = 0;
};

bool set_prop_string(PROPVARIANT* value, std::wstring_view text) noexcept {
    if (value == nullptr) return false;
    std::memset(value, 0, sizeof(*value));
    value->bstrVal = SysAllocStringLen(
        text.data(), static_cast<UINT>(text.size()));
    if (value->bstrVal == nullptr && !text.empty()) return false;
    value->vt = VT_BSTR;
    return true;
}

void set_prop_u64(PROPVARIANT* value, std::uint64_t number) noexcept {
    std::memset(value, 0, sizeof(*value));
    value->vt = VT_UI8;
    value->uhVal.QuadPart = number;
}

void set_prop_u32(PROPVARIANT* value, std::uint32_t number) noexcept {
    std::memset(value, 0, sizeof(*value));
    value->vt = VT_UI4;
    value->ulVal = number;
}

void set_prop_bool(PROPVARIANT* value, bool state) noexcept {
    std::memset(value, 0, sizeof(*value));
    value->vt = VT_BOOL;
    value->boolVal = state ? VARIANT_TRUE : VARIANT_FALSE;
}

void set_prop_file_time(PROPVARIANT* value, const FILETIME& time) noexcept {
    std::memset(value, 0, sizeof(*value));
    value->vt = VT_FILETIME;
    value->filetime = time;
}

class OpenCallback final
    : public sz::IArchiveOpenCallback,
      public sz::IArchiveOpenVolumeCallback,
      public sz::ICryptoGetTextPassword,
      public RefCounted {
public:
    OpenCallback(fs::path archive_path,
                 std::string password,
                 std::shared_ptr<OperationControl> operation)
        : archive_path_(std::move(archive_path)),
          password_(utf8_to_wide(password)),
          password_defined_(!password.empty()),
          operation_(std::move(operation)) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(
                archive_path_.c_str(), GetFileExInfoStandard, &data)) {
            attributes_ = data.dwFileAttributes;
            size_ = static_cast<std::uint64_t>(data.nFileSizeLow) |
                    (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32);
            modified_ = data.ftLastWriteTime;
            have_metadata_ = true;
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid, void** object) noexcept override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) ||
            IsEqualIID(iid, sz::IID_IArchiveOpenCallback)) {
            *object = static_cast<sz::IArchiveOpenCallback*>(this);
        } else if (IsEqualIID(iid, sz::IID_IArchiveOpenVolumeCallback)) {
            *object = static_cast<sz::IArchiveOpenVolumeCallback*>(this);
        } else if (IsEqualIID(iid, sz::IID_ICryptoGetTextPassword)) {
            *object = static_cast<sz::ICryptoGetTextPassword*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return add_ref(); }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return release(); }

    HRESULT STDMETHODCALLTYPE SetTotal(
        const sz::UInt64*, const sz::UInt64*) noexcept override {
        return checkpoint();
    }

    HRESULT STDMETHODCALLTYPE SetCompleted(
        const sz::UInt64*, const sz::UInt64*) noexcept override {
        return checkpoint();
    }

    HRESULT STDMETHODCALLTYPE GetProperty(
        PROPID property, PROPVARIANT* value) noexcept override {
        if (value == nullptr) return E_POINTER;
        std::memset(value, 0, sizeof(*value));
        try {
            switch (property) {
                case sz::property::name:
                    return set_prop_string(
                               value, archive_path_.filename().wstring())
                        ? S_OK : E_OUTOFMEMORY;
                case sz::property::is_directory:
                    set_prop_bool(value, false);
                    break;
                case sz::property::size:
                    if (have_metadata_) set_prop_u64(value, size_);
                    break;
                case sz::property::attributes:
                    if (have_metadata_) set_prop_u32(value, attributes_);
                    break;
                case sz::property::modified_time:
                    if (have_metadata_) set_prop_file_time(value, modified_);
                    break;
                default:
                    break;
            }
            return S_OK;
        } catch (...) {
            remember_failure(std::current_exception());
            return E_FAIL;
        }
    }

    HRESULT STDMETHODCALLTYPE GetStream(
        const wchar_t* name, sz::IInStream** stream) noexcept override {
        if (stream == nullptr) return E_POINTER;
        *stream = nullptr;
        if (name == nullptr) return E_INVALIDARG;
        try {
            const fs::path relative(name);
            if (!safe_relative_path(relative)) return S_FALSE;
            const fs::path full =
                (archive_path_.parent_path() / relative).lexically_normal();
            std::error_code error;
            if (!fs::is_regular_file(full, error)) return S_FALSE;
            *stream = new FileInStream(full);
            return S_OK;
        } catch (...) {
            remember_failure(std::current_exception());
            return E_FAIL;
        }
    }

    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(
        BSTR* password) noexcept override {
        if (password == nullptr) return E_POINTER;
        *password = nullptr;
        password_requested_.store(true, std::memory_order_release);
        if (!password_defined_) return E_ABORT;
        *password = SysAllocStringLen(
            password_.data(), static_cast<UINT>(password_.size()));
        return *password != nullptr || password_.empty()
            ? S_OK : E_OUTOFMEMORY;
    }

    bool password_requested() const {
        return password_requested_.load(std::memory_order_acquire);
    }

    bool password_defined() const { return password_defined_; }

    void rethrow_failure() const {
        std::scoped_lock lock(failure_mutex_);
        if (failure_) std::rethrow_exception(failure_);
    }

private:
    HRESULT checkpoint() noexcept {
        try {
            if (operation_) operation_->checkpoint();
            return S_OK;
        } catch (...) {
            remember_failure(std::current_exception());
            return E_ABORT;
        }
    }

    void remember_failure(std::exception_ptr failure) const noexcept {
        try {
            std::scoped_lock lock(failure_mutex_);
            if (!failure_) failure_ = std::move(failure);
        } catch (...) {
        }
    }

    fs::path archive_path_;
    std::wstring password_;
    bool password_defined_ = false;
    std::shared_ptr<OperationControl> operation_;
    std::atomic<bool> password_requested_{false};
    bool have_metadata_ = false;
    std::uint64_t size_ = 0;
    std::uint32_t attributes_ = 0;
    FILETIME modified_{};
    mutable std::mutex failure_mutex_;
    mutable std::exception_ptr failure_;
};

struct Handler {
    std::wstring name;
    GUID class_id{};
};

fs::path executable_directory() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1) {
            return fs::path(
                       std::wstring(buffer.data(), buffer.data() + length))
                .parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::optional<fs::path> seven_zip_library_path() {
    std::vector<fs::path> candidates;
    const fs::path executable = executable_directory();
    if (!executable.empty()) {
        candidates.push_back(
            executable / L"backends" / L"7zip" / L"7z.dll");
        candidates.push_back(
            executable.parent_path() / L"backends" / L"7zip" / L"7z.dll");
    }

    std::error_code error;
    const fs::path current = fs::current_path(error);
    if (!error) {
        candidates.push_back(
            current / L"third_party" / L"7zip" / L"win-x64" / L"7z.dll");
        candidates.push_back(
            current.parent_path() / L"third_party" / L"7zip" /
            L"win-x64" / L"7z.dll");
    }

    for (const auto& candidate : candidates) {
        if (fs::is_regular_file(candidate, error) && !error) return candidate;
        error.clear();
    }
    return std::nullopt;
}

class SevenZipModule {
public:
    SevenZipModule() {
        const auto path = seven_zip_library_path();
        if (!path) {
            throw std::runtime_error("Axiom's bundled 7z.dll backend was not found");
        }
        module_ = LoadLibraryExW(
            path->c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module_ == nullptr) {
            throw std::runtime_error("Axiom could not load its bundled 7z.dll backend");
        }
        struct ModuleGuard {
            HMODULE module = nullptr;
            ~ModuleGuard() {
                if (module != nullptr) FreeLibrary(module);
            }
        } module_guard{module_};

        create_object_ = reinterpret_cast<sz::CreateObject>(
            GetProcAddress(module_, "CreateObject"));
        get_number_of_formats_ = reinterpret_cast<sz::GetNumberOfFormats>(
            GetProcAddress(module_, "GetNumberOfFormats"));
        get_handler_property_ = reinterpret_cast<sz::GetHandlerProperty2>(
            GetProcAddress(module_, "GetHandlerProperty2"));
        if (create_object_ == nullptr ||
            get_number_of_formats_ == nullptr ||
            get_handler_property_ == nullptr) {
            throw std::runtime_error(
                "the bundled 7z.dll does not expose the required archive API");
        }

        sz::UInt32 count = 0;
        const HRESULT count_result = get_number_of_formats_(&count);
        if (FAILED(count_result) || count == 0 || count > 256) {
            throw std::runtime_error(
                hresult_message("enumerating 7-Zip formats", count_result));
        }
        handlers_.reserve(count);
        for (sz::UInt32 index = 0; index < count; ++index) {
            PropVariant name_value;
            PropVariant class_value;
            if (FAILED(get_handler_property_(
                    index, sz::handler_property::name, name_value.put())) ||
                FAILED(get_handler_property_(
                    index, sz::handler_property::class_id,
                    class_value.put()))) {
                continue;
            }
            const auto name = property_string(name_value.get());
            const auto& binary = class_value.get();
            if (!name || binary.vt != VT_BSTR ||
                binary.bstrVal == nullptr ||
                SysStringByteLen(binary.bstrVal) != sizeof(GUID)) {
                continue;
            }
            Handler handler;
            handler.name = *name;
            std::memcpy(
                &handler.class_id, binary.bstrVal, sizeof(handler.class_id));
            handlers_.push_back(std::move(handler));
        }
        if (handlers_.empty()) {
            throw std::runtime_error(
                "the bundled 7z.dll reported no archive handlers");
        }
        module_guard.module = nullptr;
    }

    ~SevenZipModule() {
        // The process-wide module stays loaded until static destruction, after
        // all provider calls and COM objects have completed.
        if (module_ != nullptr) FreeLibrary(module_);
    }

    SevenZipModule(const SevenZipModule&) = delete;
    SevenZipModule& operator=(const SevenZipModule&) = delete;

    const Handler* handler(std::wstring_view name) const {
        const auto equal_ascii_case = [](std::wstring_view left,
                                         std::wstring_view right) {
            if (left.size() != right.size()) return false;
            for (std::size_t index = 0; index < left.size(); ++index) {
                wchar_t a = left[index];
                wchar_t b = right[index];
                if (a >= L'A' && a <= L'Z') a += L'a' - L'A';
                if (b >= L'A' && b <= L'Z') b += L'a' - L'A';
                if (a != b) return false;
            }
            return true;
        };
        const auto found = std::find_if(
            handlers_.begin(), handlers_.end(),
            [&](const Handler& candidate) {
                return equal_ascii_case(candidate.name, name);
            });
        return found == handlers_.end() ? nullptr : &*found;
    }

    ComPtr<sz::IInArchive> create(const Handler& handler) const {
        ComPtr<sz::IInArchive> archive;
        const HRESULT result = create_object_(
            &handler.class_id, &sz::IID_IInArchive,
            reinterpret_cast<void**>(archive.put()));
        if (FAILED(result) || !archive) {
            throw std::runtime_error(
                hresult_message("creating a 7-Zip archive handler", result));
        }
        return archive;
    }

private:
    HMODULE module_ = nullptr;
    sz::CreateObject create_object_ = nullptr;
    sz::GetNumberOfFormats get_number_of_formats_ = nullptr;
    sz::GetHandlerProperty2 get_handler_property_ = nullptr;
    std::vector<Handler> handlers_;
};

SevenZipModule& seven_zip_module() {
    static SevenZipModule module;
    return module;
}

std::vector<std::wstring_view> handler_priority(
    ArchiveFormat format, bool prefer_udf) {
    switch (format) {
        case ArchiveFormat::seven_z:
            return {L"7z"};
        case ArchiveFormat::rar:
            return {L"Rar5", L"Rar"};
        case ArchiveFormat::iso:
            return prefer_udf
                ? std::vector<std::wstring_view>{L"Udf", L"Iso"}
                : std::vector<std::wstring_view>{L"Iso", L"Udf"};
        case ArchiveFormat::cab:
            return {L"Cab"};
        default:
            return {};
    }
}

struct OpenArchive {
    ComPtr<sz::IInArchive> archive;
    ComPtr<sz::IInStream> input;
    ComPtr<OpenCallback> callback;

    OpenArchive() = default;
    OpenArchive(OpenArchive&&) noexcept = default;
    OpenArchive& operator=(OpenArchive&&) noexcept = default;
    OpenArchive(const OpenArchive&) = delete;
    OpenArchive& operator=(const OpenArchive&) = delete;

    ~OpenArchive() {
        if (archive) {
            archive->Close();
            archive.reset();
        }
    }
};

OpenArchive open_archive(
    const fs::path& archive_path,
    ArchiveFormat format,
    bool prefer_udf,
    const std::string& password,
    const std::shared_ptr<OperationControl>& operation = {}) {
    auto& module = seven_zip_module();
    HRESULT last_result = S_FALSE;
    bool requested_password = false;

    for (std::wstring_view handler_name :
         handler_priority(format, prefer_udf)) {
        const Handler* handler = module.handler(handler_name);
        if (handler == nullptr) continue;

        OpenArchive opened;
        const auto split_paths = format == ArchiveFormat::seven_z
            ? split_volume_paths(archive_path)
            : std::vector<fs::path>{};
        if (split_paths.empty()) {
            opened.input.reset(new FileInStream(archive_path));
        } else {
            opened.input.reset(new SplitFileInStream(split_paths));
        }
        opened.callback.reset(
            new OpenCallback(archive_path, password, operation));
        opened.archive = module.create(*handler);

        last_result = opened.archive->Open(
            opened.input.get(), nullptr, opened.callback.get());
        try {
            opened.callback->rethrow_failure();
        } catch (...) {
            opened.archive->Close();
            throw;
        }
        requested_password =
            requested_password || opened.callback->password_requested();
        if (last_result == S_OK) return opened;
        opened.archive->Close();
    }

    if (requested_password) {
        throw std::runtime_error(
            password.empty()
                ? "encrypted archive requires a password"
                : "wrong password for encrypted archive");
    }
    throw FormatError(
        hresult_message("opening archive with 7z.dll", last_result));
}

struct NativeItem {
    sz::UInt32 index = 0;
    ArchiveEntry entry;
    std::optional<FILETIME> modified;
};

PropVariant get_property(
    sz::IInArchive* archive, sz::UInt32 index, PROPID property) {
    PropVariant value;
    const HRESULT result =
        archive->GetProperty(index, property, value.put());
    if (FAILED(result)) {
        throw std::runtime_error(
            hresult_message("reading a 7-Zip entry property", result));
    }
    return value;
}

std::vector<NativeItem> read_items(
    sz::IInArchive* archive, bool* encrypted) {
    sz::UInt32 count = 0;
    const HRESULT count_result = archive->GetNumberOfItems(&count);
    if (FAILED(count_result)) {
        throw std::runtime_error(
            hresult_message("reading the 7-Zip item count", count_result));
    }
    if (count > 1'000'000) {
        throw FormatError("archive contains too many directory entries");
    }

    std::vector<NativeItem> items;
    items.reserve(count);
    for (sz::UInt32 index = 0; index < count; ++index) {
        NativeItem item;
        item.index = index;

        auto path_value =
            get_property(archive, index, sz::property::path);
        auto path = property_string(path_value.get());
        if (!path || path->empty()) {
            auto name_value =
                get_property(archive, index, sz::property::name);
            path = property_string(name_value.get());
        }
        if (!path || path->empty()) continue;

        item.entry.path = normalize_archive_path(*path);
        if (!safe_relative_path(std::string_view(item.entry.path))) {
            throw FormatError(
                "archive contains an unsafe path: " + item.entry.path);
        }

        auto directory_value =
            get_property(archive, index, sz::property::is_directory);
        item.entry.is_directory = property_bool(directory_value.get());

        auto size_value =
            get_property(archive, index, sz::property::size);
        if (auto size = property_u64(size_value.get())) {
            item.entry.size = item.entry.is_directory ? 0 : *size;
        }

        auto packed_value =
            get_property(archive, index, sz::property::packed_size);
        item.entry.packed_size = property_u64(packed_value.get());

        auto modified_value =
            get_property(archive, index, sz::property::modified_time);
        item.modified = property_file_time(modified_value.get());
        if (item.modified) {
            item.entry.mtime = file_time_to_unix(*item.modified);
        }

        auto crc_value =
            get_property(archive, index, sz::property::crc);
        if (auto crc = property_u64(crc_value.get());
            crc && *crc <= std::numeric_limits<std::uint32_t>::max()) {
            item.entry.crc32 = static_cast<std::uint32_t>(*crc);
            item.entry.has_crc32 = true;
        }

        auto encrypted_value =
            get_property(archive, index, sz::property::encrypted);
        if (property_bool(encrypted_value.get()) && encrypted != nullptr) {
            *encrypted = true;
        }

        auto symbolic_value =
            get_property(archive, index, sz::property::symbolic_link);
        if (auto target = property_string(symbolic_value.get());
            target && !target->empty()) {
            item.entry.is_symlink = true;
            item.entry.link_target = normalize_archive_path(*target);
        }

        auto hard_value =
            get_property(archive, index, sz::property::hard_link);
        if (auto target = property_string(hard_value.get());
            target && !target->empty()) {
            item.entry.is_hardlink = true;
            item.entry.link_target = normalize_archive_path(*target);
        }
        items.push_back(std::move(item));
    }
    return items;
}

std::string operation_result_message(sz::Int32 result) {
    switch (result) {
        case sz::operation_result::unsupported_method:
            return "archive uses an unsupported compression method";
        case sz::operation_result::data_error:
            return "archive data is corrupt";
        case sz::operation_result::crc_error:
            return "archive entry failed its CRC check";
        case sz::operation_result::unavailable:
            return "archive entry data is unavailable";
        case sz::operation_result::unexpected_end:
            return "archive ends unexpectedly";
        case sz::operation_result::data_after_end:
            return "archive entry contains trailing data";
        case sz::operation_result::is_not_archive:
            return "file is not a supported archive";
        case sz::operation_result::headers_error:
            return "archive headers are corrupt";
        case sz::operation_result::wrong_password:
            return "wrong password for encrypted archive";
        default:
            return "7-Zip reported extraction error " +
                   std::to_string(result);
    }
}

class FileOutStream final
    : public sz::ISequentialOutStream,
      public RefCounted {
public:
    using Progress = void (*)(void*, std::uint64_t) noexcept;

    FileOutStream(
        const fs::path& path, void* progress_context, Progress progress)
        : progress_context_(progress_context), progress_(progress) {
        handle_ = CreateFileW(
            path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error(
                "could not create extracted file: " +
                core::path_to_utf8(path));
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid, void** object) noexcept override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) ||
            IsEqualIID(iid, sz::IID_ISequentialOutStream)) {
            *object = static_cast<sz::ISequentialOutStream*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return add_ref(); }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return release(); }

    HRESULT STDMETHODCALLTYPE Write(
        const void* data, sz::UInt32 size,
        sz::UInt32* processed_size) noexcept override {
        if (processed_size != nullptr) *processed_size = 0;
        DWORD processed = 0;
        if (!WriteFile(handle_, data, size, &processed, nullptr)) {
            return hresult_from_last_error();
        }
        written_ += processed;
        if (processed_size != nullptr) *processed_size = processed;
        if (progress_ != nullptr) progress_(progress_context_, written_);
        return processed == size ? S_OK : E_FAIL;
    }

    HRESULT close(const std::optional<FILETIME>& modified) noexcept {
        if (handle_ == INVALID_HANDLE_VALUE) return S_OK;
        HRESULT result = S_OK;
        if (modified &&
            !SetFileTime(handle_, nullptr, nullptr, &*modified)) {
            result = hresult_from_last_error();
        }
        if (!CloseHandle(handle_) && SUCCEEDED(result)) {
            result = hresult_from_last_error();
        }
        handle_ = INVALID_HANDLE_VALUE;
        return result;
    }

protected:
    ~FileOutStream() override { close(std::nullopt); }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    void* progress_context_ = nullptr;
    Progress progress_ = nullptr;
    std::uint64_t written_ = 0;
};

class ExtractCallback final
    : public sz::IArchiveExtractCallback,
      public sz::ICryptoGetTextPassword,
      public RefCounted {
public:
    ExtractCallback(
        std::vector<NativeItem> items,
        fs::path destination,
        std::string password,
        std::shared_ptr<OperationControl> operation,
        OperationStage stage,
        std::uint64_t total_bytes,
        std::uint64_t total_items)
        : destination_(std::move(destination)),
          password_(utf8_to_wide(password)),
          password_defined_(!password.empty()),
          operation_(std::move(operation)),
          stage_(stage),
          total_bytes_(total_bytes),
          total_items_(total_items) {
        item_by_index_.reserve(items.size());
        for (auto& item : items) {
            item_by_index_.emplace(item.index, std::move(item));
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid, void** object) noexcept override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) ||
            IsEqualIID(iid, sz::IID_IArchiveExtractCallback)) {
            *object = static_cast<sz::IArchiveExtractCallback*>(this);
        } else if (IsEqualIID(iid, sz::IID_IProgress)) {
            *object = static_cast<sz::IProgress*>(this);
        } else if (IsEqualIID(iid, sz::IID_ICryptoGetTextPassword)) {
            *object = static_cast<sz::ICryptoGetTextPassword*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return add_ref(); }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return release(); }

    HRESULT STDMETHODCALLTYPE SetTotal(sz::UInt64 total) noexcept override {
        if (total_bytes_ == 0) total_bytes_ = total;
        return publish(0, true);
    }

    HRESULT STDMETHODCALLTYPE SetCompleted(
        const sz::UInt64* completed) noexcept override {
        if (completed != nullptr) {
            global_completed_.store(
                std::min<std::uint64_t>(*completed, total_bytes_),
                std::memory_order_release);
        }
        return publish(
            global_completed_.load(std::memory_order_acquire), false);
    }

    HRESULT STDMETHODCALLTYPE GetStream(
        sz::UInt32 index, sz::ISequentialOutStream** stream,
        sz::Int32 ask_mode) noexcept override {
        if (stream == nullptr) return E_POINTER;
        *stream = nullptr;
        current_output_.reset();
        try {
            checkpoint();
            const auto found = item_by_index_.find(index);
            if (found == item_by_index_.end()) return E_INVALIDARG;
            current_index_ = index;
            {
                std::scoped_lock lock(state_mutex_);
                current_path_ = found->second.entry.path;
            }
            current_file_size_.store(
                found->second.entry.size, std::memory_order_release);
            current_file_completed_.store(0, std::memory_order_release);

            if (ask_mode != sz::ask_mode::extract) return S_OK;
            const fs::path relative =
                core::path_from_utf8(found->second.entry.path);
            if (!safe_relative_path(relative)) {
                remember_failure(std::make_exception_ptr(
                    FormatError(
                        "archive path escapes extraction destination: " +
                        found->second.entry.path)));
                return E_ABORT;
            }
            const fs::path target =
                (destination_ / relative).lexically_normal();
            std::error_code error;
            if (found->second.entry.is_directory) {
                fs::create_directories(target, error);
                if (error) {
                    throw std::runtime_error(
                        "could not create extracted directory: " +
                        error.message());
                }
                return S_OK;
            }
            fs::create_directories(target.parent_path(), error);
            if (error) {
                throw std::runtime_error(
                    "could not create extracted parent directory: " +
                    error.message());
            }
            current_output_.reset(new FileOutStream(
                target, this, &ExtractCallback::file_progress));
            current_output_->AddRef();
            *stream = static_cast<sz::ISequentialOutStream*>(
                current_output_.get());
            return S_OK;
        } catch (...) {
            remember_failure(std::current_exception());
            return E_ABORT;
        }
    }

    HRESULT STDMETHODCALLTYPE PrepareOperation(
        sz::Int32) noexcept override {
        try {
            checkpoint();
            return publish(
                global_completed_.load(std::memory_order_acquire), true);
        } catch (...) {
            remember_failure(std::current_exception());
            return E_ABORT;
        }
    }

    HRESULT STDMETHODCALLTYPE SetOperationResult(
        sz::Int32 result) noexcept override {
        try {
            if (current_output_) {
                const auto found = item_by_index_.find(current_index_);
                const HRESULT close_result = current_output_->close(
                    found == item_by_index_.end()
                        ? std::optional<FILETIME>{}
                        : found->second.modified);
                current_output_.reset();
                if (FAILED(close_result)) {
                    throw std::runtime_error(
                        hresult_message(
                            "closing an extracted file", close_result));
                }
            }
            if (result != sz::operation_result::ok) {
                std::scoped_lock lock(state_mutex_);
                error_message_ = operation_result_message(result);
                return E_ABORT;
            }
            completed_items_.fetch_add(1, std::memory_order_acq_rel);
            current_file_completed_.store(
                current_file_size_.load(std::memory_order_acquire),
                std::memory_order_release);
            return publish(
                global_completed_.load(std::memory_order_acquire), true);
        } catch (...) {
            remember_failure(std::current_exception());
            return E_ABORT;
        }
    }

    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(
        BSTR* password) noexcept override {
        if (password == nullptr) return E_POINTER;
        *password = nullptr;
        password_requested_.store(true, std::memory_order_release);
        if (!password_defined_) {
            std::scoped_lock lock(state_mutex_);
            error_message_ = "encrypted archive requires a password";
            return E_ABORT;
        }
        *password = SysAllocStringLen(
            password_.data(), static_cast<UINT>(password_.size()));
        return *password != nullptr || password_.empty()
            ? S_OK : E_OUTOFMEMORY;
    }

    void finish(HRESULT extract_result) {
        rethrow_failure();
        {
            std::scoped_lock lock(state_mutex_);
            if (!error_message_.empty()) {
                throw std::runtime_error(error_message_);
            }
        }
        if (FAILED(extract_result)) {
            if (password_requested_.load(std::memory_order_acquire)) {
                throw std::runtime_error(
                    password_defined_
                        ? "wrong password for encrypted archive"
                        : "encrypted archive requires a password");
            }
            throw std::runtime_error(
                hresult_message("7-Zip extraction", extract_result));
        }
        global_completed_.store(total_bytes_, std::memory_order_release);
        completed_items_.store(total_items_, std::memory_order_release);
        const HRESULT report_result = publish(total_bytes_, true);
        if (FAILED(report_result)) rethrow_failure();
    }

private:
    static void file_progress(
        void* context, std::uint64_t completed) noexcept {
        static_cast<ExtractCallback*>(context)->on_file_progress(completed);
    }

    void on_file_progress(std::uint64_t completed) noexcept {
        current_file_completed_.store(completed, std::memory_order_release);
        const std::uint64_t previous =
            last_file_report_.load(std::memory_order_relaxed);
        if (completed >= previous + kProgressQuantum ||
            completed == current_file_size_.load(std::memory_order_acquire)) {
            last_file_report_.store(completed, std::memory_order_relaxed);
            (void)publish(
                global_completed_.load(std::memory_order_acquire), false);
        }
    }

    void checkpoint() {
        if (operation_) operation_->checkpoint();
    }

    HRESULT publish(
        std::uint64_t completed_bytes, bool force) noexcept {
        try {
            checkpoint();
            if (!operation_) return S_OK;
            const std::uint64_t previous =
                last_global_report_.load(std::memory_order_relaxed);
            if (!force && completed_bytes < previous + kProgressQuantum &&
                completed_bytes != total_bytes_) {
                return S_OK;
            }
            last_global_report_.store(
                completed_bytes, std::memory_order_relaxed);
            std::string current_path;
            {
                std::scoped_lock lock(state_mutex_);
                current_path = current_path_;
            }
            operation_->report(OperationProgress{
                stage_,
                completed_bytes,
                total_bytes_,
                completed_items_.load(std::memory_order_acquire),
                total_items_,
                std::move(current_path),
                current_file_completed_.load(std::memory_order_acquire),
                current_file_size_.load(std::memory_order_acquire)});
            return S_OK;
        } catch (...) {
            remember_failure(std::current_exception());
            return E_ABORT;
        }
    }

    void remember_failure(std::exception_ptr failure) noexcept {
        try {
            std::scoped_lock lock(failure_mutex_);
            if (!failure_) failure_ = std::move(failure);
        } catch (...) {
        }
    }

    void rethrow_failure() const {
        std::scoped_lock lock(failure_mutex_);
        if (failure_) std::rethrow_exception(failure_);
    }

    fs::path destination_;
    std::wstring password_;
    bool password_defined_ = false;
    std::shared_ptr<OperationControl> operation_;
    OperationStage stage_ = OperationStage::extracting;
    std::uint64_t total_bytes_ = 0;
    std::uint64_t total_items_ = 0;
    std::atomic<std::uint64_t> completed_items_{0};
    std::unordered_map<sz::UInt32, NativeItem> item_by_index_;
    sz::UInt32 current_index_ = 0;
    std::string current_path_;
    std::atomic<std::uint64_t> current_file_size_{0};
    ComPtr<FileOutStream> current_output_;
    std::atomic<std::uint64_t> global_completed_{0};
    std::atomic<std::uint64_t> current_file_completed_{0};
    std::atomic<std::uint64_t> last_global_report_{0};
    std::atomic<std::uint64_t> last_file_report_{0};
    std::atomic<bool> password_requested_{false};
    std::string error_message_;
    mutable std::mutex state_mutex_;
    mutable std::mutex failure_mutex_;
    std::exception_ptr failure_;
};

std::vector<sz::UInt32> select_indices(
    const std::vector<NativeItem>& items,
    const std::vector<std::string>& selected_paths) {
    if (selected_paths.empty()) return {};
    std::unordered_set<std::string> selected(
        selected_paths.begin(), selected_paths.end());
    std::unordered_set<std::string> found_paths;
    std::vector<sz::UInt32> indices;
    indices.reserve(selected.size());
    for (const auto& item : items) {
        if (selected.contains(item.entry.path)) {
            indices.push_back(item.index);
            found_paths.insert(item.entry.path);
        }
    }
    if (found_paths.size() != selected.size()) {
        throw std::runtime_error(
            "selected archive entries were not found in the 7-Zip catalog");
    }
    return indices;
}

}  // namespace

bool seven_zip_library_available() {
    try {
        (void)seven_zip_module();
        return true;
    } catch (...) {
        return false;
    }
}

SevenZipCatalog seven_zip_library_list(
    const fs::path& archive_path,
    ArchiveFormat format,
    bool prefer_udf,
    const std::string& password) {
    auto opened = open_archive(
        archive_path, format, prefer_udf, password);
    SevenZipCatalog catalog;
    catalog.directory_encrypted =
        opened.callback->password_requested();
    auto native = read_items(
        opened.archive.get(), &catalog.encrypted);
    catalog.encrypted =
        catalog.encrypted || catalog.directory_encrypted;
    catalog.entries.reserve(native.size());
    for (auto& item : native) {
        catalog.entries.push_back(std::move(item.entry));
    }
    return catalog;
}

void seven_zip_library_test(
    const fs::path& archive_path,
    ArchiveFormat format,
    bool prefer_udf,
    const std::string& password,
    const std::shared_ptr<OperationControl>& operation) {
    auto opened = open_archive(
        archive_path, format, prefer_udf, password, operation);
    bool encrypted = false;
    auto items = read_items(opened.archive.get(), &encrypted);
    std::uint64_t total_bytes = 0;
    for (const auto& item : items) {
        if (!item.entry.is_directory) total_bytes += item.entry.size;
    }

    ComPtr<ExtractCallback> callback(new ExtractCallback(
        items, {}, password, operation, OperationStage::testing,
        total_bytes, items.size()));
    const HRESULT result = opened.archive->Extract(
        nullptr, std::numeric_limits<sz::UInt32>::max(), 1,
        callback.get());
    callback->finish(result);
}

void seven_zip_library_extract(
    const fs::path& archive_path,
    ArchiveFormat format,
    bool prefer_udf,
    const std::vector<std::string>& selected_paths,
    const fs::path& destination,
    const std::string& password,
    const std::shared_ptr<OperationControl>& operation,
    std::uint64_t total_bytes,
    std::uint64_t total_items) {
    auto opened = open_archive(
        archive_path, format, prefer_udf, password, operation);
    bool encrypted = false;
    auto items = read_items(opened.archive.get(), &encrypted);
    auto indices = select_indices(items, selected_paths);

    ComPtr<ExtractCallback> callback(new ExtractCallback(
        items, destination, password, operation,
        OperationStage::extracting, total_bytes, total_items));
    const HRESULT result = opened.archive->Extract(
        indices.empty() ? nullptr : indices.data(),
        indices.empty()
            ? std::numeric_limits<sz::UInt32>::max()
            : static_cast<sz::UInt32>(indices.size()),
        0, callback.get());
    callback->finish(result);
}

}  // namespace axiom

#endif
