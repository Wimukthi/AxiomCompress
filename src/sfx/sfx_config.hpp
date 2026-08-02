#pragma once

// Configuration embedded in a generated self-extracting executable.
//
// Two representations on purpose. Authors write an INI-style text file, which
// `axiomc sfx --config` parses at creation time; the executable carries a TLV
// blob, which is all the stub ever parses. The stub reads input that arrives
// with an untrusted executable, so it should carry the smallest parser that
// will do, and a text parser would be both larger and a wider attack surface.
//
// Unknown tags are rejected rather than skipped: a stub that silently ignored
// a `require_accept` it did not understand would be downgrading the author's
// stated intent.

#include "axiom/archive.hpp"
#include "sfx/sfx_options.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace axiom::sfx {

// Which extractor runtime to package. The full module carries the dialogs; the
// mini module is console-only. Declared here rather than beside the packager so
// callers can choose a tier without pulling in <windows.h>.
enum class SfxStubTier { full, mini };

enum class SfxElevation {
    none,     // never relaunch; fail if the destination is not writable
    automatic,  // relaunch only when the destination proves unwritable
    require,  // always relaunch elevated
};

enum class SfxTheme { automatic, light, dark };

struct SfxConfig {
    // Presentation. Empty means "use the built-in default".
    std::string title;
    std::string window_title;
    std::string description;
    std::string banner_text;
    SfxTheme theme = SfxTheme::automatic;

    // Destination. `default_path` may contain %TEMPLATES%; see
    // sfx_validate_path_template().
    std::string default_path;
    bool allow_path_change = true;
    bool create_subfolder = false;

    // License gate.
    std::string license_text;
    bool require_accept = false;

    // Behaviour.
    SfxMode mode = SfxMode::interactive;
    ExtractOptions::Overwrite overwrite = ExtractOptions::Overwrite::overwrite;
    bool restore_mtime = true;
    bool open_destination = true;
    bool auto_close = false;
    std::uint32_t threads = 0;

    // Selection.
    bool allow_file_selection = false;

    // Run after extract. `run_program` is relative to the extraction root and
    // must resolve inside it.
    std::string run_program;
    std::string run_arguments;
    std::string run_working_dir;
    bool wait_for_exit = true;
    bool propagate_exit_code = false;

    SfxElevation elevation = SfxElevation::none;
};

// Serialization. decode returns nullopt for a malformed, truncated, or
// unknown-tag blob; the caller treats that as a corrupt executable.
std::vector<std::uint8_t> encode_sfx_config(const SfxConfig& config);
std::optional<SfxConfig> decode_sfx_config(std::span<const std::uint8_t> blob);

struct SfxConfigTextResult {
    SfxConfig value;
    bool ok = true;
    std::string error;
    std::size_t line = 0;
};

// Parses the INI-style authoring format. Keys match the field names above.
SfxConfigTextResult parse_sfx_config_text(std::string_view text);

// Syntax-only check of a destination template: every %NAME% must be one this
// build knows how to expand, and the remainder must not contain a parent
// reference. Expansion itself is a Windows runtime concern and lives in the
// stub, so creation-time validation stays portable.
bool sfx_validate_path_template(std::string_view value, std::string& error);

// True when `name` (without the surrounding percent signs) is expandable.
bool sfx_is_known_path_token(std::string_view name);

// Rejects combinations that are individually valid but unsafe together, such
// as a fully unattended elevated run-after-extract chain.
bool sfx_validate_config(const SfxConfig& config, std::string& error);

}  // namespace axiom::sfx
