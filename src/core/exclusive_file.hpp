#pragma once

// Output files that cannot be redirected through a link.
//
// Extraction stages every file next to its final name and then renames it into
// place. Opening that staging name with a plain std::ofstream follows a symlink
// that already sits there, so whoever controls the name controls where the bytes
// land. ExclusiveFile creates the file with "create new, never follow" semantics
// instead: if anything at all exists at the path -- including a symlink,
// whatever it points to, and including a dangling one -- creation fails and the
// caller picks another name.

#include "core/crypto.hpp"
#include "core/path_text.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace axiom::core {

class ExclusiveFile {
public:
    enum class Create {
        created,  // the file is open for writing
        exists,   // the name is taken (by anything); nothing was opened
        failed,   // any other error; see the error_code
    };

    ExclusiveFile() = default;
    ~ExclusiveFile() { release(); }

    ExclusiveFile(const ExclusiveFile&) = delete;
    ExclusiveFile& operator=(const ExclusiveFile&) = delete;
    ExclusiveFile(ExclusiveFile&& other) noexcept : handle_(other.handle_) {
        other.handle_ = invalid_handle();
    }
    ExclusiveFile& operator=(ExclusiveFile&& other) noexcept {
        if (this != &other) {
            release();
            handle_ = other.handle_;
            other.handle_ = invalid_handle();
        }
        return *this;
    }

    // Creates `path` as a new regular file opened for writing. Never follows a
    // link at `path` and never opens a file that already exists.
    Create create_new(const std::filesystem::path& path, std::error_code& error) {
        release();
        error.clear();
#if defined(_WIN32)
        // CREATE_NEW fails when the name exists. FILE_FLAG_OPEN_REPARSE_POINT
        // stops a symlink or junction at the final component from being
        // resolved first, so a dangling link also reads as "exists".
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const DWORD native = GetLastError();
            if (native == ERROR_FILE_EXISTS || native == ERROR_ALREADY_EXISTS) {
                return Create::exists;
            }
            error = std::error_code(static_cast<int>(native), std::system_category());
            return Create::failed;
        }
#else
        int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        handle_ = ::open(path.c_str(), flags, 0666);
        if (handle_ < 0) {
            const int native = errno;
            handle_ = invalid_handle();
            if (native == EEXIST) return Create::exists;
            error = std::error_code(native, std::generic_category());
            return Create::failed;
        }
#endif
        return Create::created;
    }

    bool is_open() const noexcept { return handle_ != invalid_handle(); }

    // Writes every byte or returns false.
    bool write(std::span<const std::uint8_t> bytes) noexcept {
        if (!is_open()) return false;
        const std::uint8_t* cursor = bytes.data();
        std::size_t remaining = bytes.size();
        constexpr std::size_t kMaxChunk = std::size_t{1} << 30;
        while (remaining != 0) {
            const std::size_t want = std::min(remaining, kMaxChunk);
#if defined(_WIN32)
            DWORD written = 0;
            if (!WriteFile(handle_, cursor, static_cast<DWORD>(want), &written, nullptr) ||
                written == 0) {
                return false;
            }
#else
            const ssize_t written = ::write(handle_, cursor, want);
            if (written < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (written == 0) return false;
#endif
            cursor += static_cast<std::size_t>(written);
            remaining -= static_cast<std::size_t>(written);
        }
        return true;
    }

    // Forces written data to stable storage.
    bool sync() noexcept {
        if (!is_open()) return false;
#if defined(_WIN32)
        return FlushFileBuffers(handle_) != FALSE;
#else
        return ::fsync(handle_) == 0;
#endif
    }

    // Closes the file; false means the close itself reported an error (a late
    // write-back failure, for instance), so the data cannot be trusted.
    bool close() noexcept {
        if (!is_open()) return true;
#if defined(_WIN32)
        const bool ok = CloseHandle(handle_) != FALSE;
#else
        const bool ok = ::close(handle_) == 0;
#endif
        handle_ = invalid_handle();
        return ok;
    }

private:
#if defined(_WIN32)
    using Handle = HANDLE;
    static Handle invalid_handle() noexcept { return INVALID_HANDLE_VALUE; }
#else
    using Handle = int;
    static constexpr Handle invalid_handle() noexcept { return -1; }
#endif

    void release() noexcept { (void)close(); }

    Handle handle_ = invalid_handle();
};

// The part of `target`'s file name that is kept in its staging name. Long names
// are cut (on a character boundary) so that adding the random suffix cannot push
// a name that was legal past the filesystem's per-component limit.
inline std::filesystem::path::string_type staging_stem(const std::filesystem::path& target) {
    constexpr std::size_t kMaxStemUnits = 128;
    auto stem = target.filename().native();
    if (stem.size() <= kMaxStemUnits) return stem;
    std::size_t cut = kMaxStemUnits;
#if defined(_WIN32)
    // Do not split a UTF-16 surrogate pair.
    if (stem[cut - 1] >= 0xD800 && stem[cut - 1] <= 0xDBFF) --cut;
#else
    // Do not split a UTF-8 sequence: back up to the start of the character.
    while (cut > 0 && (static_cast<unsigned char>(stem[cut]) & 0xC0) == 0x80) --cut;
#endif
    stem.resize(cut);
    return stem;
}

// Creates a staging file next to `target` whose name did not exist before and
// cannot be guessed. The suffix still ends in ".axtmp" so leftovers from an
// interrupted run are recognisable. Returns the path that was created.
inline std::filesystem::path create_staging_file(const std::filesystem::path& target,
                                                 ExclusiveFile& file) {
    constexpr char kHex[] = "0123456789abcdef";
    const std::filesystem::path directory = target.parent_path();
    const auto stem = staging_stem(target);
    for (int attempt = 0; attempt < 64; ++attempt) {
        std::array<std::uint8_t, 8> random{};
        random_bytes(random);
        std::string suffix = ".";
        for (const auto byte : random) {
            suffix.push_back(kHex[byte >> 4]);
            suffix.push_back(kHex[byte & 0x0F]);
        }
        suffix += ".axtmp";
        std::filesystem::path candidate = directory / (stem + std::filesystem::path(suffix).native());

        std::error_code error;
        switch (file.create_new(candidate, error)) {
            case ExclusiveFile::Create::created:
                return candidate;
            case ExclusiveFile::Create::exists:
                continue;  // astronomically unlikely, or planted: pick another name
            case ExclusiveFile::Create::failed:
                throw std::runtime_error("cannot write file: " + path_to_utf8(candidate) +
                                         ": " + error.message());
        }
    }
    throw std::runtime_error("cannot create a unique staging file next to: " +
                             path_to_utf8(target));
}

// A staging file next to `target`, removed again on every exit path unless it was
// moved to its final name and dismissed. The handle is closed before the file is
// removed (Windows cannot delete an open file).
class StagedFile {
public:
    explicit StagedFile(const std::filesystem::path& target)
        : path_(create_staging_file(target, file_)) {}
    ~StagedFile() {
        (void)file_.close();
        if (active_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    StagedFile(const StagedFile&) = delete;
    StagedFile& operator=(const StagedFile&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }
    ExclusiveFile& file() noexcept { return file_; }
    bool write(std::span<const std::uint8_t> bytes) noexcept { return file_.write(bytes); }
    bool close() noexcept { return file_.close(); }

    // The file now lives under its final name; leave it in place.
    void dismiss() noexcept { active_ = false; }

private:
    ExclusiveFile file_;  // declared first: create_staging_file fills it in
    std::filesystem::path path_;
    bool active_ = true;
};

}  // namespace axiom::core
