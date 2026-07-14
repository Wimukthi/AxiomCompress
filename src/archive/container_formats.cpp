// Archive format detection and the provider registry: file-magic and extension
// sniffing for the supported formats, the AXAR ArchiveProvider (a thin adapter
// over the public AXAR API), and provider lookup for a given path. Extracted
// from container.cpp; the AXAR engine stays there.

#include "axiom/archive.hpp"

#include "archive/container_internal.hpp"
#include "archive/system_provider.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace axiom {
namespace {

namespace fs = std::filesystem;



bool file_starts_with_magic(const fs::path& path,
                            std::span<const std::uint8_t> expected) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    ByteVector bytes(expected.size());
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    return static_cast<std::size_t>(stream.gcount()) == bytes.size() &&
           std::equal(expected.begin(), expected.end(), bytes.begin());
}

bool file_has_magic_at(const fs::path& path,
                       std::uint64_t offset,
                       std::span<const std::uint8_t> expected) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    if (size < 0 ||
        static_cast<std::uint64_t>(size) < offset + expected.size()) {
        return false;
    }
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    ByteVector bytes(expected.size());
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    return static_cast<std::size_t>(stream.gcount()) == bytes.size() &&
           std::equal(expected.begin(), expected.end(), bytes.begin());
}

bool has_ascii_suffix(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.substr(text.size() - suffix.size()) == suffix;
}

bool has_ascii_suffix(std::wstring_view text, std::wstring_view suffix) {
    return text.size() >= suffix.size() &&
           text.substr(text.size() - suffix.size()) == suffix;
}

std::wstring lower_ascii_path_name(const fs::path& path) {
    return lower_ascii(path.filename().wstring());
}

bool looks_like_native_archive_file(const fs::path& path) {
    return file_starts_with_magic(path, kArchiveMagic) ||
           sfx_embedded_archive_range(path).has_value();
}

bool looks_like_zip_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    stream.seekg(0, std::ios::end);
    const auto end = stream.tellg();
    if (end < std::streamoff(4)) return false;
    const auto size = static_cast<std::uint64_t>(end);

    std::array<std::uint8_t, 4> signature{};
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(signature.data()),
                static_cast<std::streamsize>(signature.size()));
    if (stream && signature[0] == 'P' && signature[1] == 'K' &&
        ((signature[2] == 0x03 && signature[3] == 0x04) ||
         (signature[2] == 0x05 && signature[3] == 0x06) ||
         (signature[2] == 0x07 && signature[3] == 0x08))) {
        return true;
    }
    if (!(signature[0] == 'M' && signature[1] == 'Z')) {
        return false;
    }

    // ZIP self-extractors and other wrapped ZIP containers place the central
    // directory at the end. The classic EOCD record must live within the last
    // 65,557 bytes (22 byte record + 65,535 byte comment).
    constexpr std::uint64_t kMaxEocdSearch = 65557;
    const std::uint64_t search_size = std::min<std::uint64_t>(size, kMaxEocdSearch);
    ByteVector tail(static_cast<std::size_t>(search_size));
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(size - search_size), std::ios::beg);
    stream.read(reinterpret_cast<char*>(tail.data()),
                static_cast<std::streamsize>(tail.size()));
    if (!stream) return false;
    constexpr std::array<std::uint8_t, 4> kEocd{{'P', 'K', 0x05, 0x06}};
    return std::search(tail.begin(), tail.end(), kEocd.begin(), kEocd.end()) != tail.end();
}

bool looks_like_7z_file(const fs::path& path) {
    constexpr std::array<std::uint8_t, 6> kSevenZMagic{{0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c}};
    return file_starts_with_magic(path, kSevenZMagic);
}

bool looks_like_rar_file(const fs::path& path) {
    constexpr std::array<std::uint8_t, 7> kRar4Magic{{'R', 'a', 'r', '!', 0x1a, 0x07, 0x00}};
    constexpr std::array<std::uint8_t, 8> kRar5Magic{{'R', 'a', 'r', '!', 0x1a, 0x07, 0x01, 0x00}};
    return file_starts_with_magic(path, kRar4Magic) ||
           file_starts_with_magic(path, kRar5Magic);
}

bool looks_like_tar_file(const fs::path& path) {
    constexpr std::array<std::uint8_t, 5> kUstarMagic{{'u', 's', 't', 'a', 'r'}};
    return file_has_magic_at(path, 257, kUstarMagic);
}

bool looks_like_iso_file(const fs::path& path) {
    constexpr std::array<std::uint8_t, 5> kIsoMagic{{'C', 'D', '0', '0', '1'}};
    return file_has_magic_at(path, 0x8001, kIsoMagic);
}

bool looks_like_cab_file(const fs::path& path) {
    constexpr std::array<std::uint8_t, 4> kCabMagic{{'M', 'S', 'C', 'F'}};
    return file_starts_with_magic(path, kCabMagic);
}

bool has_7z_extension(const fs::path& path) {
    return lower_ascii(path.extension().wstring()) == L".7z";
}

bool has_rar_extension(const fs::path& path) {
    const std::wstring name = lower_ascii_path_name(path);
    const std::wstring extension = lower_ascii(path.extension().wstring());
    if (extension == L".rar") return true;
    if (extension.size() == 4 && extension[0] == L'.' && extension[1] == L'r' &&
        extension[2] >= L'0' && extension[2] <= L'9' &&
        extension[3] >= L'0' && extension[3] <= L'9') {
        return true;
    }
    return name.find(L".part") != std::wstring::npos && has_ascii_suffix(name, L".rar");
}

bool has_tar_extension(const fs::path& path) {
    const std::wstring name = lower_ascii_path_name(path);
    constexpr std::array<std::wstring_view, 9> kTarSuffixes{
        L".tar", L".tar.gz", L".tgz", L".tar.xz", L".txz",
        L".tar.bz2", L".tbz2", L".tar.zst", L".tzst"};
    return std::any_of(kTarSuffixes.begin(), kTarSuffixes.end(),
                       [&](std::wstring_view suffix) {
                           return has_ascii_suffix(name, suffix);
                       });
}

bool has_iso_extension(const fs::path& path) {
    return lower_ascii(path.extension().wstring()) == L".iso";
}

bool has_cab_extension(const fs::path& path) {
    return lower_ascii(path.extension().wstring()) == L".cab";
}

class AxarArchiveProvider final : public ArchiveProvider {
public:
    const ArchiveFormatInfo& info() const override {
        return kArchiveFormats[kAxarFormatIndex];
    }

    bool matches_path(const std::filesystem::path& path) const override {
        return lower_ascii(path.extension().wstring()) == L".axar" ||
               is_axiom_archive_volume(path);
    }

    ArchiveCapabilities capabilities(const std::filesystem::path& archive_path,
                                     const std::string& password) const override {
        ArchiveCapabilities result;
        const bool embedded_sfx = is_axiom_sfx_archive(archive_path);
        const bool volume_set = is_axiom_archive_volume(archive_path);
        const bool writable = !embedded_sfx && !volume_set;
        result.list = true;
        result.extract = true;
        result.test = true;
        result.create = writable;
        result.packed_sizes = true;
        result.selective_extract = true;
        result.update = writable;
        result.delete_entries = writable;
        result.move_entries = writable;
        result.encryption = writable;
        result.recovery_records = writable;
        result.can_create_volumes = writable;
        result.is_multi_volume = volume_set;
        result.comments = writable;
        result.lock = writable;
        result.metadata = true;
        result.links = true;
        result.authenticity = true;
        result.sfx = writable;
        // The state flags below require opening the archive, which throws for a
        // path that does not exist yet (a new archive about to be created) or a
        // damaged file. A capability query must not throw — callers use it to
        // enable UI commands before any operation runs — so probe failures
        // leave the state flags at their defaults and the actual operation
        // reports the precise error.
        try {
            const auto encryption_mode = archive_encryption_mode(archive_path);
            result.encrypted = encryption_mode != ArchiveEncryptionMode::none;
            result.directory_encrypted =
                encryption_mode == ArchiveEncryptionMode::data_and_directory;
            if (!result.directory_encrypted || !password.empty()) {
                result.locked = archive_is_locked(archive_path, password);
            }
        } catch (...) {
        }
        return result;
    }

    std::vector<ArchiveEntry> list(const std::filesystem::path& archive_path,
                                   const std::string& password) const override {
        return list_archive(archive_path, password);
    }

    void test(const std::filesystem::path& archive_path,
              const DecompressionOptions& options) const override {
        test_archive(archive_path, options);
    }

    void extract_all(const std::filesystem::path& archive_path,
                     const std::filesystem::path& dest_dir,
                     const ExtractOptions& options) const override {
        extract_archive(archive_path, dest_dir, options);
    }

    void extract_selected(const std::filesystem::path& archive_path,
                          const std::vector<std::string>& entries,
                          const std::filesystem::path& dest_dir,
                          const ExtractOptions& options) const override {
        extract_entries(archive_path, entries, dest_dir, options);
    }

    void create(const std::vector<std::filesystem::path>& inputs,
                const std::filesystem::path& archive_path,
                const CompressionOptions& options) const override {
        create_archive(inputs, archive_path, options);
    }

    void add(const std::vector<std::filesystem::path>& inputs,
             const std::filesystem::path& archive_path,
             const CompressionOptions& options) const override {
        add_to_archive(inputs, archive_path, options);
    }

    void add_mapped(const std::vector<ArchiveInput>& inputs,
                    const std::filesystem::path& archive_path,
                    const CompressionOptions& options) const override {
        add_to_archive(inputs, archive_path, options);
    }

    void update(const std::vector<std::filesystem::path>& inputs,
                const std::filesystem::path& archive_path,
                const CompressionOptions& options,
                bool fresh_only) const override {
        update_archive(inputs, archive_path, options, fresh_only);
    }

    void sync(const std::vector<std::filesystem::path>& inputs,
              const std::filesystem::path& archive_path,
              const CompressionOptions& options) const override {
        sync_archive(inputs, archive_path, options);
    }

    void delete_entries(const std::filesystem::path& archive_path,
                        const std::vector<std::string>& paths,
                        const CompressionOptions& options) const override {
        delete_from_archive(archive_path, paths, options);
    }

    void move_entries(const std::filesystem::path& archive_path,
                      const std::vector<ArchiveMove>& moves,
                      const CompressionOptions& options) const override {
        move_archive_entries(archive_path, moves, options);
    }
};

const AxarArchiveProvider kAxarProvider;
const std::array<const ArchiveProvider*, 2> kArchiveProviders{&kAxarProvider,
                                                              &zip_archive_provider()};

}  // namespace

std::span<const ArchiveFormatInfo> supported_archive_formats() {
    return kArchiveFormats;
}

const ArchiveProvider* archive_provider_for_path(const std::filesystem::path& path) {
    if (const auto* provider = archive_provider_for_contents(path)) {
        return provider;
    }
#ifdef _WIN32
    if (const auto* provider = system_archive_provider_for_extension(path)) {
        return provider;
    }
#endif
    for (const auto* provider : kArchiveProviders) {
        if (provider->matches_path(path)) {
            return provider;
        }
    }
    return nullptr;
}

const ArchiveProvider* archive_provider_for_contents(const std::filesystem::path& path) {
    if (looks_like_native_archive_file(path) || is_axiom_archive_volume(path)) {
        return &kAxarProvider;
    }
    if (looks_like_zip_file(path)) {
        return &zip_archive_provider();
    }
#ifdef _WIN32
    if (const auto* provider = system_archive_provider_for_contents(path)) {
        return provider;
    }
#endif
    return nullptr;
}

bool is_supported_archive(const std::filesystem::path& path) {
    return archive_provider_for_path(path) != nullptr;
}

bool is_native_archive(const std::filesystem::path& path) {
    const auto* provider = archive_provider_for_path(path);
    return provider != nullptr && provider->info().native;
}

bool is_axiom_sfx_archive(const std::filesystem::path& path) {
    return sfx_embedded_payload_range(path).has_value();
}

}  // namespace axiom
