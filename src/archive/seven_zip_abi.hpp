#pragma once

// Minimal public 7-Zip engine ABI used by Axiom's dynamic 7z.dll adapter.
// The interface layout and identifiers match 7-Zip 26.00's SDK headers
// (CPP/7zip/{IDecl,IStream,IProgress,IPassword,Archive/IArchive}.h). Keeping
// this narrow declaration local avoids importing 7-Zip's application framework
// while still talking to the unmodified, separately distributed LGPL DLL.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <oleauto.h>
#include <propidl.h>

#include <cstdint>

namespace axiom::seven_zip_abi {

using UInt32 = std::uint32_t;
using Int32 = std::int32_t;
using UInt64 = std::uint64_t;
using Int64 = std::int64_t;

inline constexpr GUID make_interface_guid(unsigned group, unsigned id) {
    return GUID{0x23170F69, 0x40C1, 0x278A,
                {0, 0, 0, static_cast<unsigned char>(group),
                 0, static_cast<unsigned char>(id), 0, 0}};
}

inline constexpr GUID IID_ISequentialInStream =
    make_interface_guid(3, 0x01);
inline constexpr GUID IID_ISequentialOutStream =
    make_interface_guid(3, 0x02);
inline constexpr GUID IID_IInStream =
    make_interface_guid(3, 0x03);
inline constexpr GUID IID_IProgress =
    make_interface_guid(0, 0x05);
inline constexpr GUID IID_IArchiveOpenCallback =
    make_interface_guid(6, 0x10);
inline constexpr GUID IID_IArchiveExtractCallback =
    make_interface_guid(6, 0x20);
inline constexpr GUID IID_IArchiveOpenVolumeCallback =
    make_interface_guid(6, 0x30);
inline constexpr GUID IID_IInArchive =
    make_interface_guid(6, 0x60);
inline constexpr GUID IID_ICryptoGetTextPassword =
    make_interface_guid(5, 0x10);

struct __declspec(novtable) ISequentialInStream : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Read(
        void* data, UInt32 size, UInt32* processed_size) noexcept = 0;
};

struct __declspec(novtable) ISequentialOutStream : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Write(
        const void* data, UInt32 size, UInt32* processed_size) noexcept = 0;
};

struct __declspec(novtable) IInStream : ISequentialInStream {
    virtual HRESULT STDMETHODCALLTYPE Seek(
        Int64 offset, UInt32 seek_origin, UInt64* new_position) noexcept = 0;
};

struct __declspec(novtable) IProgress : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetTotal(UInt64 total) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCompleted(
        const UInt64* completed) noexcept = 0;
};

struct __declspec(novtable) IArchiveOpenCallback : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetTotal(
        const UInt64* files, const UInt64* bytes) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCompleted(
        const UInt64* files, const UInt64* bytes) noexcept = 0;
};

struct __declspec(novtable) IArchiveOpenVolumeCallback : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetProperty(
        PROPID property, PROPVARIANT* value) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStream(
        const wchar_t* name, IInStream** stream) noexcept = 0;
};

struct __declspec(novtable) ICryptoGetTextPassword : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(
        BSTR* password) noexcept = 0;
};

struct __declspec(novtable) IArchiveExtractCallback : IProgress {
    virtual HRESULT STDMETHODCALLTYPE GetStream(
        UInt32 index, ISequentialOutStream** stream, Int32 ask_mode) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE PrepareOperation(Int32 ask_mode) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 result) noexcept = 0;
};

struct __declspec(novtable) IInArchive : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Open(
        IInStream* stream, const UInt64* max_check_start_position,
        IArchiveOpenCallback* callback) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfItems(
        UInt32* item_count) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProperty(
        UInt32 index, PROPID property, PROPVARIANT* value) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE Extract(
        const UInt32* indices, UInt32 item_count, Int32 test_mode,
        IArchiveExtractCallback* callback) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetArchiveProperty(
        PROPID property, PROPVARIANT* value) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfProperties(
        UInt32* property_count) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyInfo(
        UInt32 index, BSTR* name, PROPID* property, VARTYPE* type) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfArchiveProperties(
        UInt32* property_count) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetArchivePropertyInfo(
        UInt32 index, BSTR* name, PROPID* property, VARTYPE* type) noexcept = 0;
};

namespace handler_property {
inline constexpr PROPID name = 0;
inline constexpr PROPID class_id = 1;
}  // namespace handler_property

namespace property {
inline constexpr PROPID path = 3;
inline constexpr PROPID name = 4;
inline constexpr PROPID is_directory = 6;
inline constexpr PROPID size = 7;
inline constexpr PROPID packed_size = 8;
inline constexpr PROPID attributes = 9;
inline constexpr PROPID modified_time = 12;
inline constexpr PROPID encrypted = 15;
inline constexpr PROPID crc = 19;
inline constexpr PROPID symbolic_link = 54;
inline constexpr PROPID error_flags = 71;
inline constexpr PROPID hard_link = 90;
}  // namespace property

namespace ask_mode {
inline constexpr Int32 extract = 0;
inline constexpr Int32 test = 1;
inline constexpr Int32 skip = 2;
inline constexpr Int32 read_external = 3;
}  // namespace ask_mode

namespace operation_result {
inline constexpr Int32 ok = 0;
inline constexpr Int32 unsupported_method = 1;
inline constexpr Int32 data_error = 2;
inline constexpr Int32 crc_error = 3;
inline constexpr Int32 unavailable = 4;
inline constexpr Int32 unexpected_end = 5;
inline constexpr Int32 data_after_end = 6;
inline constexpr Int32 is_not_archive = 7;
inline constexpr Int32 headers_error = 8;
inline constexpr Int32 wrong_password = 9;
}  // namespace operation_result

using CreateObject = HRESULT(WINAPI*)(
    const GUID* class_id, const GUID* interface_id, void** object);
using GetNumberOfFormats = HRESULT(WINAPI*)(UInt32* format_count);
using GetHandlerProperty2 = HRESULT(WINAPI*)(
    UInt32 format_index, PROPID property, PROPVARIANT* value);

}  // namespace axiom::seven_zip_abi
#endif
