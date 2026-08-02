#pragma once

// Command line accepted by a generated self-extracting executable.
//
// Parsing is deliberately free of Windows APIs and of any I/O so it can be
// tested directly. The stub applies the result; axiomc links the same parser so
// `axiomc sfx` can reject a default command line its own extractor would not
// accept.

#include "axiom/archive.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace axiom::sfx {

// Documented in docs/SFX_ARCHITECTURE.md and CLI_GUIDE.md. Scripts branch on
// these, so the numbers are part of the interface: do not renumber them.
enum class ExitCode : int {
    success = 0,
    failure = 1,       // extraction failed
    usage = 2,         // bad command line
    cancelled = 3,     // dismissed by the user
    password = 4,      // wrong or missing password
    integrity = 5,     // descriptor, payload hash, or signature check failed
    disk_space = 6,    // not enough free space at the destination
    elevation = 7,     // elevation required and unavailable or refused
    run_failed = 8,    // extracted, but run_program could not be started
};

enum class SfxMode {
    interactive,  // full UI
    silent,       // progress and errors only
    very_silent,  // no window at all
};

// Keep command-line, text-config, and binary-config paths on the same bounded
// worker-count contract. Zero means automatic; nonzero values are capped so a
// hostile SFX cannot request an absurd number of workers.
inline constexpr std::size_t kSfxMaxThreads = 4096;

// Every field that can also come from the embedded config is optional here, so
// the stub can tell "not specified" from "specified as the default value" and
// apply the documented precedence.
struct SfxCommandLine {
    std::optional<std::wstring> destination;
    std::optional<SfxMode> mode;
    bool accept = false;
    std::optional<std::wstring> password;
    bool password_stdin = false;
    std::optional<ExtractOptions::Overwrite> overwrite;
    std::optional<std::size_t> threads;
    std::vector<std::wstring> include;
    bool no_run = false;
    bool list = false;
    bool test = false;
    bool help = false;
    std::optional<std::wstring> log;
};

struct SfxCommandLineResult {
    SfxCommandLine value;
    bool ok = true;
    std::wstring error;  // populated only when ok is false
};

// `arguments` excludes argv[0]. A lone positional is the destination, which is
// how every pre-0.9 self-extractor accepted one.
SfxCommandLineResult parse_sfx_command_line(
    std::span<const std::wstring> arguments);

std::wstring sfx_usage_text();

}  // namespace axiom::sfx
