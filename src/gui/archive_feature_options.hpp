#pragma once

#include <filesystem>
#include <string>

namespace axiom::gui {

enum class ArchiveUpdateMode {
    create_new,
    add_or_replace,
    update_newer,
    fresh_existing,
    synchronize,
};

struct ArchiveFeatureOptions {
    bool store_windows_attributes = true;
    bool store_file_times = true;
    bool store_alternate_streams = true;
    bool store_links = true;
    bool store_posix_metadata = false;

    ArchiveUpdateMode update_mode = ArchiveUpdateMode::create_new;
    bool quick_open = true;
    bool lock_archive = false;
    bool repack_after_update = false;
    std::wstring comment;

    bool encrypt_data = false;
    bool encrypt_names = false;
    int kdf_preset = 1;
    // Transient only. This structure is never persisted in GUI settings.
    std::wstring password;

    std::wstring volume_size;
    int volume_unit = 2;
    int recovery_percent = 0;
    bool create_recovery_volumes = false;

    bool sign_archive = false;
    std::filesystem::path signing_key;
    bool create_sfx = false;
    std::wstring sfx_destination;
    bool sfx_run_after_extract = false;
};

struct ExtractFeatureOptions {
    bool restore_windows_attributes = true;
    bool restore_creation_time = true;
    bool restore_access_time = true;
    bool restore_alternate_streams = true;
    bool restore_links = true;
    bool restore_posix_metadata = false;

    bool auto_discover_volumes = true;
    bool attempt_recovery = true;
    bool verify_signature = true;
    bool require_trusted_signature = false;
    // Transient only. This structure is never persisted in GUI settings.
    std::wstring password;
};

struct ArchiveFeatureAvailability {
    bool metadata = false;
    bool update = false;
    bool comments = false;
    bool lock = false;
    bool quick_open = false;
    bool encryption = false;
    bool header_encryption = false;
    bool kdf_presets = false;
    bool volumes = false;
    bool recovery = false;
    bool authenticity = false;
    bool sfx = false;
    bool posix_metadata = false;
};

}  // namespace axiom::gui
