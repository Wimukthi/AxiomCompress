#pragma once

// Windows-side services the self-extractor needs but that cannot live in the
// portable parts: console attachment, destination templates, free space,
// elevation, and launching the configured program.

#include "sfx/sfx_config.hpp"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace axiom::sfx {

// A GUI-subsystem process has no console of its own, so --list, --test, --help
// and silent-mode diagnostics attach to the console that launched it. When
// there is none — a double-click from Explorer — writes fall back to a dialog
// so output is never silently lost.
class HostConsole {
public:
    explicit HostConsole(HINSTANCE instance);
    ~HostConsole();

    HostConsole(const HostConsole&) = delete;
    HostConsole& operator=(const HostConsole&) = delete;

    bool attached() const { return attached_; }
    void write(std::wstring_view text);
    // Anything written while no console or redirect was attached, so a caller
    // with a UI can surface it. Clears the buffer.
    std::wstring take_buffered();

private:
    HINSTANCE instance_{};
    HANDLE output_{INVALID_HANDLE_VALUE};
    bool attached_ = false;
    bool owns_console_ = false;  // only then is FreeConsole ours to call
    bool console_ = false;       // false for a redirected file or pipe
    std::wstring buffered_;
};

// Expands %ProgramFiles%, %LOCALAPPDATA%, %SFXDIR% and friends. Shell folders
// resolve through SHGetKnownFolderPath rather than the environment, so a
// caller cannot redirect %ProgramFiles% by setting a variable before launch.
// Returns nullopt when a token is unknown or the result is not absolute.
std::optional<std::filesystem::path> expand_sfx_path_template(
    std::string_view value, const std::filesystem::path& sfx_executable);

// Free bytes available to this user on the volume holding `path`, walking up to
// the nearest existing ancestor when the destination does not exist yet.
std::optional<std::uint64_t> available_free_space(
    const std::filesystem::path& path);

// Whether a directory can actually be created and written at `path`, tested by
// doing it rather than by pattern-matching the path against Program Files.
bool destination_is_writable(const std::filesystem::path& path);

bool process_is_elevated();

// Relaunches this executable with the "runas" verb, forwarding `arguments`,
// waits for the elevated child, and returns its process exit code. A password
// from the command line is never forwarded: it would be visible in the process
// list of the elevated instance.
std::optional<DWORD> relaunch_elevated(
    const std::filesystem::path& executable, const std::wstring& arguments);

struct RunResult {
    bool started = false;
    std::optional<DWORD> exit_code;  // only when the launch waited
};

// Launches `program`, which must already have been resolved inside `root`.
RunResult run_extracted_program(const std::filesystem::path& root,
                                const std::filesystem::path& program,
                                const std::wstring& arguments,
                                const std::filesystem::path& working_directory,
                                bool wait_for_exit);

// Resolves a configured run_program against the extraction root, rejecting
// anything that escapes it or that was not produced by the extraction.
std::optional<std::filesystem::path> resolve_run_program(
    const std::filesystem::path& root, std::string_view relative);

// Resolves a configured working directory with the same containment and
// reparse-point rules as run_program, but requires a real directory.
std::optional<std::filesystem::path> resolve_run_directory(
    const std::filesystem::path& root, std::string_view relative);

// Case-insensitive glob over '/'-separated archive paths supporting * and ?.
// A pattern without a slash matches against the entry's file name as well, so
// `--include *.dll` behaves the way people expect.
bool matches_include_pattern(std::string_view archive_path,
                             std::string_view pattern);

}  // namespace axiom::sfx
