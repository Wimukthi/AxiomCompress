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
    // Output .exe path chosen on the General row, not an extraction folder.
    std::wstring sfx_destination;

    // Configuration embedded in the generated extractor. These mirror the keys
    // documented for `axiomc sfx --config`; anything not exposed here keeps its
    // documented default. Indices match the combo boxes on the SFX options
    // page and are validated before use.
    int sfx_stub_tier = 0;    // 0 = full window, 1 = console only
    int sfx_overwrite = 0;    // 0 = replace, 1 = skip, 2 = stop
    int sfx_mode = 0;         // 0 = interactive, 1 = silent, 2 = no window
    int sfx_elevation = 0;    // 0 = never, 1 = when needed, 2 = always
    int sfx_theme = 0;        // 0 = system, 1 = light, 2 = dark
    std::wstring sfx_title;
    std::wstring sfx_description;
    std::wstring sfx_default_path;
    std::wstring sfx_run_program;
    std::wstring sfx_run_arguments;
    std::wstring sfx_license_text;
    bool sfx_allow_path_change = true;
    bool sfx_require_accept = false;
    bool sfx_open_destination = true;
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
