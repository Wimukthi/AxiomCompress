#define NOMINMAX
#include "sfx/runtime.hpp"

#include "archive/sfx_image.hpp"
#include "axiom/archive.hpp"
#include "core/hash.hpp"
#include "core/path_text.hpp"
#include "sfx/sfx_config.hpp"
#include "sfx/sfx_archive_reader.hpp"
#include "sfx/sfx_host.hpp"
#include "sfx/sfx_options.hpp"
#include "sfx/sfx_ui.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace axiom::sfx {
namespace fs = std::filesystem;
namespace {

std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return L"<unrepresentable text>";
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), length);
    return result;
}

std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int length =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), nullptr, 0, nullptr,
                            nullptr);
    if (length <= 0) throw std::runtime_error("text is not valid Unicode");
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), length,
                        nullptr, nullptr);
    return result;
}

class Transcript {
public:
    std::optional<fs::path> open(const fs::path& requested,
                                 const fs::path& executable,
                                 std::string& error) {
        std::error_code ec;
        fs::path normalized = fs::absolute(requested, ec);
        if (ec || normalized.empty()) {
            error = "cannot resolve the log path";
            return std::nullopt;
        }
        normalized = normalized.lexically_normal();
        if (normalized == executable.lexically_normal()) {
            error = "the log file must not be the self-extractor";
            return std::nullopt;
        }
        const bool exists = fs::exists(normalized, ec);
        if (ec) {
            error = "cannot inspect the log path: " + ec.message();
            return std::nullopt;
        }
        if (exists && fs::equivalent(normalized, executable, ec)) {
            error = "the log file must not be the self-extractor";
            return std::nullopt;
        }
        if (ec) {
            error = "cannot inspect the log path: " + ec.message();
            return std::nullopt;
        }
        stream_.open(normalized, std::ios::binary | std::ios::app);
        if (!stream_) {
            error = "cannot open the log file";
            return std::nullopt;
        }
        return normalized;
    }

    void append(std::wstring_view text) {
        if (!stream_) return;
        try {
            const std::string encoded = utf8(text);
            stream_.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
            stream_.flush();
        } catch (...) {
            // A transcript is diagnostic output. It must never turn a valid
            // extraction into a failure after the requested file was opened.
        }
    }

private:
    std::ofstream stream_;
};

void secure_clear(std::wstring& text) {
    if (!text.empty()) {
        SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
        text.clear();
    }
}

void secure_clear(std::string& text) {
    if (!text.empty()) {
        SecureZeroMemory(text.data(), text.size());
        text.clear();
    }
}

std::wstring format_size(std::uint64_t bytes) {
    constexpr const wchar_t* units[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    std::wstringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(unit == 0 ? 0 : 1);
    stream << value << L' ' << units[unit];
    return stream.str();
}

// Everything the run needs after the config and the command line have been
// merged, so the stages below never re-derive precedence.
struct Session {
    fs::path executable;
    SfxConfig config;
    SfxCommandLine command_line;
    SfxMode mode = SfxMode::interactive;
    std::wstring title = L"Axiom Self-Extractor";
    std::optional<SfxArchiveReader> archive;
    axiom::ExtractOptions options;
    std::vector<axiom::ArchiveEntry> entries;
    axiom::ArchiveCapabilities capabilities;
    axiom::ArchiveSignatureInfo signature;
    fs::path destination;
    std::uint64_t unpacked_size = 0;
    std::unique_ptr<Transcript> transcript;
};

// Interactive means both "the package did not ask for silence" and "this stub
// can actually ask a question". The mini stub is never interactive.
bool interactive(const Session& session, const SfxUi& ui) {
    return session.mode == SfxMode::interactive && ui.supports_prompts();
}

void report(const Session& session, SfxUi& ui, const std::wstring& text,
            bool error) {
    if (session.transcript) session.transcript->append(text + L"\n");
    if (session.mode == SfxMode::very_silent && !error) return;
    ui.message(session.title, text, error);
}

void write_text(Session& session, SfxUi& ui, const std::wstring& text) {
    if (session.transcript) session.transcript->append(text);
    ui.write(text);
}

// Verifies the payload against the hash prefix recorded in the descriptor. Only
// --test does this: it is a full extra read, and the archive layer already
// checks per-block CRC-32 and per-file BLAKE3 while extracting.
bool payload_hash_matches(const fs::path& executable, const SfxPayload& payload) {
    if (payload.format != SfxFormat::v2) return true;  // v1 has no hash field
    std::ifstream stream(executable, std::ios::binary);
    if (!stream) return false;
    stream.seekg(static_cast<std::streamoff>(payload.payload_offset), std::ios::beg);
    core::Blake3 hasher;
    std::vector<std::uint8_t> buffer(std::size_t{1} << 20);
    std::uint64_t remaining = payload.payload_size;
    while (remaining != 0) {
        const auto count = static_cast<std::streamsize>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        stream.read(reinterpret_cast<char*>(buffer.data()), count);
        if (stream.gcount() != count) return false;
        hasher.update(std::span<const std::uint8_t>(
            buffer.data(), static_cast<std::size_t>(count)));
        remaining -= static_cast<std::uint64_t>(count);
    }
    const auto digest = hasher.finalize();
    return std::equal(payload.payload_hash.begin(), payload.payload_hash.end(),
                      digest.begin());
}

std::optional<std::wstring> read_password_from_stdin() {
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == INVALID_HANDLE_VALUE || input == nullptr) return std::nullopt;
    std::string raw;
    std::array<char, 512> buffer{};
    DWORD read = 0;
    bool clean_end = true;
    for (;;) {
        if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                      nullptr)) {
            clean_end = GetLastError() == ERROR_BROKEN_PIPE;
            break;
        }
        if (read == 0) break;
        raw.append(buffer.data(), read);
        if (raw.size() > (1u << 16)) return std::nullopt;
    }
    if (!clean_end) return std::nullopt;
    while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r')) raw.pop_back();
    if (raw.empty()) return std::nullopt;
    return widen(raw);
}

// Entries selected by --include. Directories are implied by their contents.
std::vector<std::string> select_entries(
    const std::vector<axiom::ArchiveEntry>& entries,
    const std::vector<std::wstring>& patterns) {
    std::vector<std::string> selected;
    for (const auto& entry : entries) {
        if (entry.is_directory) continue;
        const bool matched = std::any_of(
            patterns.begin(), patterns.end(), [&](const std::wstring& pattern) {
                return matches_include_pattern(entry.path, utf8(pattern));
            });
        if (matched) selected.push_back(entry.path);
    }
    return selected;
}

int run_after_extract(Session& session, SfxUi& ui) {
    if (session.config.run_program.empty() || session.command_line.no_run) {
        return static_cast<int>(ExitCode::success);
    }
    const auto program =
        resolve_run_program(session.destination, session.config.run_program);
    if (!program) {
        report(session, ui,
               L"The configured program was not found inside the extracted "
               L"files: " +
                   widen(session.config.run_program),
               true);
        return static_cast<int>(ExitCode::run_failed);
    }
    fs::path working = session.destination;
    if (!session.config.run_working_dir.empty()) {
        const auto resolved =
            resolve_run_directory(session.destination, session.config.run_working_dir);
        if (!resolved) {
            report(session, ui,
                   L"The configured working directory was not found inside the "
                   L"extracted files.",
                   true);
            return static_cast<int>(ExitCode::run_failed);
        }
        working = *resolved;
    }
    const auto result = run_extracted_program(
        session.destination, *program, widen(session.config.run_arguments), working,
        session.config.wait_for_exit);
    if (!result.started) {
        report(session, ui, L"Could not start " + program->wstring(), true);
        return static_cast<int>(ExitCode::run_failed);
    }
    if (session.config.propagate_exit_code && result.exit_code.has_value() &&
        *result.exit_code != 0) {
        // Documented as ">8 carries the program's own code"; keep the low
        // numbers reserved for Axiom's own failures.
        const DWORD code = *result.exit_code;
        return code <= static_cast<DWORD>(ExitCode::run_failed)
                   ? static_cast<int>(ExitCode::run_failed)
                   : static_cast<int>(code);
    }
    return static_cast<int>(ExitCode::success);
}

}  // namespace

int run_self_extractor(HINSTANCE, std::span<const std::wstring> arguments,
                       SfxUi& ui) {
    Session session;

    const auto parsed = parse_sfx_command_line(arguments);
    if (!parsed.ok) {
        ui.write(parsed.error + L"\n\n" + sfx_usage_text());
        ui.flush(session.title, true);
        return static_cast<int>(ExitCode::usage);
    }
    session.command_line = parsed.value;

    auto finish = [&](int code, bool error) {
        secure_clear(session.options.password);
        if (session.transcript) {
            session.transcript->append(L"\nExit code: " + std::to_wstring(code) +
                                       L"\n");
        }
        ui.flush(session.title, error);
        return code;
    };

    try {
        std::wstring module(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, module.data(),
                                                static_cast<DWORD>(module.size()));
        if (length == 0 || length >= module.size()) {
            throw std::runtime_error("cannot locate the self-extractor");
        }
        module.resize(length);
        session.executable = fs::path(module);

        if (session.command_line.log.has_value()) {
            session.transcript = std::make_unique<Transcript>();
            std::string log_error;
            const auto log_path = session.transcript->open(
                fs::path(*session.command_line.log), session.executable, log_error);
            if (!log_path) {
                ui.write(L"Cannot open the requested SFX log: " + widen(log_error) +
                         L"\n");
                return finish(static_cast<int>(ExitCode::failure), true);
            }
            // Elevation forwards the resolved path so the child appends to the
            // same file even if the parent and child have different cwd state.
            session.command_line.log = log_path->wstring();
        }

        if (session.command_line.help) {
            write_text(session, ui, sfx_usage_text());
            return finish(static_cast<int>(ExitCode::success), false);
        }

        const auto payload = axiom::sfx_locate_payload(session.executable);
        if (!payload) {
            write_text(session, ui,
                       L"This file does not contain a valid embedded archive.\n");
            return finish(static_cast<int>(ExitCode::failure), true);
        }

        if (payload->config_size != 0) {
            const auto blob = axiom::sfx_archive_config(session.executable);
            if (!blob) {
                report(session, ui, L"The embedded configuration could not be read.",
                       true);
                return finish(static_cast<int>(ExitCode::integrity), true);
            }
            const auto decoded = decode_sfx_config(*blob);
            if (!decoded) {
                write_text(session, ui,
                           L"The embedded configuration is not valid.\n");
                return finish(static_cast<int>(ExitCode::integrity), true);
            }
            session.config = *decoded;
        }

        session.mode = session.command_line.mode.value_or(session.config.mode);
        if (!session.config.window_title.empty()) {
            session.title = widen(session.config.window_title);
        } else if (!session.config.title.empty()) {
            session.title = widen(session.config.title);
        }
        // SfxTheme::automatic leaves the UI on whatever the system reports.
        if (session.config.theme == SfxTheme::light) ui.set_theme(false);
        if (session.config.theme == SfxTheme::dark) ui.set_theme(true);

        session.archive = SfxArchiveReader::open(session.executable);
        if (!session.archive) {
            report(session, ui, L"SFX payload is not an Axiom or ZIP archive.", true);
            return finish(static_cast<int>(ExitCode::integrity), true);
        }

        session.options.overwrite =
            session.command_line.overwrite.value_or(session.config.overwrite);
        session.options.restore_mtime = session.config.restore_mtime;
        session.options.thread_count = session.command_line.threads.value_or(
            static_cast<std::size_t>(session.config.threads));

        // ---- password -----------------------------------------------------
        session.capabilities = session.archive->capabilities();
        if (session.capabilities.encrypted) {
            std::wstring password;
            if (session.command_line.password.has_value()) {
                password = *session.command_line.password;
            } else if (session.command_line.password_stdin) {
                const auto piped = read_password_from_stdin();
                if (!piped) {
                    report(session, ui, L"No password was supplied on stdin.", true);
                    return finish(static_cast<int>(ExitCode::password), true);
                }
                password = *piped;
            } else if (interactive(session, ui)) {
                if (!ui.ask_password(password)) {
                    return finish(static_cast<int>(ExitCode::cancelled), false);
                }
            } else {
                report(session, ui,
                       L"This archive is encrypted; supply --password.", true);
                return finish(static_cast<int>(ExitCode::password), true);
            }
            session.options.password = utf8(password);
            secure_clear(password);
            session.capabilities = session.archive->capabilities(
                session.options.password);
        }
        if (!session.capabilities.extract) {
            report(session, ui,
                   L"The payload cannot be read with the supplied password.", true);
            return finish(static_cast<int>(ExitCode::password), true);
        }

        // ---- authenticity -------------------------------------------------
        session.signature = session.archive->signature(session.options.password);
        if (session.signature.present && !session.signature.valid) {
            report(session, ui, L"The archive signature is invalid.", true);
            return finish(static_cast<int>(ExitCode::integrity), true);
        }

        session.entries = session.archive->list(session.options.password);
        for (const auto& entry : session.entries) {
            if (!entry.is_directory) session.unpacked_size += entry.size;
        }

        // ---- read-only modes ----------------------------------------------
        if (session.command_line.list) {
            std::wstringstream out;
            for (const auto& entry : session.entries) {
                out << (entry.is_directory ? L"      <DIR>  " : L"           ")
                    << std::setw(12) << entry.size << L"  " << widen(entry.path)
                    << L"\n";
            }
            out << session.entries.size() << L" entries, " << session.unpacked_size
                << L" bytes uncompressed\n";
            write_text(session, ui, out.str());
            return finish(static_cast<int>(ExitCode::success), false);
        }
            if (session.command_line.test) {
            if (!payload_hash_matches(session.executable, *payload)) {
                report(session, ui,
                       L"The embedded payload does not match its recorded hash.",
                       true);
                return finish(static_cast<int>(ExitCode::integrity), true);
            }
            axiom::DecompressionOptions test_options;
            test_options.password = session.options.password;
            session.archive->test(test_options);
            report(session, ui, L"Payload is intact.", false);
            return finish(static_cast<int>(ExitCode::success), false);
        }

        // ---- license ------------------------------------------------------
        if (session.config.require_accept && !session.command_line.accept) {
            const std::wstring license = widen(session.config.license_text);
            if (session.transcript && !license.empty()) {
                session.transcript->append(license + L"\n");
            }
            if (interactive(session, ui)) {
                if (!ui.ask_license(session.title, license)) {
                    return finish(static_cast<int>(ExitCode::cancelled), false);
                }
            } else {
                ui.ask_license(session.title, license);
                report(session, ui,
                       L"This package requires accepting a license; pass -y to "
                       L"accept it, or run interactively to read it.",
                       true);
                return finish(static_cast<int>(ExitCode::usage), true);
            }
        }

        // ---- destination --------------------------------------------------
        if (session.command_line.destination.has_value()) {
            session.destination = fs::path(*session.command_line.destination);
        } else if (!session.config.default_path.empty()) {
            const auto expanded = expand_sfx_path_template(
                session.config.default_path, session.executable);
            if (!expanded) {
                report(session, ui,
                       L"The configured destination could not be resolved.", true);
                return finish(static_cast<int>(ExitCode::failure), true);
            }
            session.destination = *expanded;
        } else {
            session.destination =
                session.executable.parent_path() / session.executable.stem();
        }
        if (session.config.create_subfolder) {
            session.destination /= session.executable.stem();
        }
        session.destination = fs::absolute(session.destination).lexically_normal();

        bool open_destination = session.config.open_destination;
        if (interactive(session, ui)) {
            SfxSummary summary;
            summary.archive_name = session.executable.filename().wstring();
            summary.window_title = session.title;
            summary.encrypted = session.capabilities.encrypted;
            summary.signature_present = session.signature.present;
            summary.signature_valid = session.signature.valid;
            summary.unpacked_size = session.unpacked_size;
            for (const auto& entry : session.entries) {
                if (entry.is_directory) {
                    ++summary.directory_count;
                } else {
                    ++summary.file_count;
                }
            }
            summary.comment = widen(
                session.archive->comment(session.options.password));
            summary.banner_text = widen(session.config.banner_text);
            summary.description = widen(session.config.description);

            SfxChoices choices;
            choices.destination = session.destination;
            choices.overwrite = session.options.overwrite;
            choices.thread_count = session.options.thread_count;
            choices.restore_mtime = session.options.restore_mtime;
            choices.open_destination = open_destination;
            choices.allow_path_change = session.config.allow_path_change;
            if (!ui.ask_options(summary, choices)) {
                return finish(static_cast<int>(ExitCode::cancelled), false);
            }
            session.destination = choices.destination;
            session.options.overwrite = choices.overwrite;
            session.options.restore_mtime = choices.restore_mtime;
            session.options.thread_count = choices.thread_count;
            open_destination = choices.open_destination;
        }

        // ---- elevation ----------------------------------------------------
        const bool writable = destination_is_writable(session.destination);
        const bool needs_elevation =
            session.config.elevation == SfxElevation::require ||
            (session.config.elevation == SfxElevation::automatic && !writable);
        if (needs_elevation && !process_is_elevated()) {
            if (session.command_line.password.has_value() ||
                session.command_line.password_stdin) {
                // Forwarding the password would expose it in the elevated
                // process's command line, and a runas launch cannot be piped to.
                report(session, ui,
                       L"Elevation is required, which cannot be combined with a "
                       L"password supplied on the command line.",
                       true);
                return finish(static_cast<int>(ExitCode::elevation), true);
            }
            auto quote_argument = [](std::wstring_view argument) {
                std::wstring quoted = L"\"";
                std::size_t backslashes = 0;
                for (const wchar_t character : argument) {
                    if (character == L'\\') {
                        ++backslashes;
                        continue;
                    }
                    if (character == L'\"') {
                        quoted.append(backslashes * 2 + 1, L'\\');
                        quoted.push_back(L'\"');
                        backslashes = 0;
                        continue;
                    }
                    quoted.append(backslashes, L'\\');
                    backslashes = 0;
                    quoted.push_back(character);
                }
                quoted.append(backslashes * 2, L'\\');
                quoted.push_back(L'\"');
                return quoted;
            };
            std::vector<std::wstring> forwarded_arguments;
            forwarded_arguments.push_back(session.destination.wstring());
            if (session.mode == SfxMode::silent) forwarded_arguments.push_back(L"--silent");
            if (session.mode == SfxMode::very_silent) {
                forwarded_arguments.push_back(L"--very-silent");
            }
            if (session.command_line.accept) forwarded_arguments.push_back(L"-y");
            if (session.command_line.no_run) forwarded_arguments.push_back(L"--no-run");
            if (session.command_line.overwrite.has_value()) {
                forwarded_arguments.push_back(L"--overwrite");
                forwarded_arguments.push_back(
                    *session.command_line.overwrite == ExtractOptions::Overwrite::skip
                        ? L"skip"
                        : *session.command_line.overwrite == ExtractOptions::Overwrite::fail
                              ? L"fail"
                              : L"replace");
            }
            if (session.command_line.threads.has_value()) {
                forwarded_arguments.push_back(L"--threads");
                forwarded_arguments.push_back(
                    std::to_wstring(*session.command_line.threads));
            }
            for (const auto& pattern : session.command_line.include) {
                forwarded_arguments.push_back(L"--include");
                forwarded_arguments.push_back(pattern);
            }
            if (session.command_line.log.has_value()) {
                forwarded_arguments.push_back(L"--log");
                forwarded_arguments.push_back(*session.command_line.log);
            }
            std::wstring forwarded;
            for (const auto& argument : forwarded_arguments) {
                if (!forwarded.empty()) forwarded.push_back(L' ');
                forwarded += quote_argument(argument);
            }
            secure_clear(session.options.password);
            const auto elevated_code =
                relaunch_elevated(session.executable, forwarded);
            if (!elevated_code) {
                report(session, ui, L"Elevation was refused.", true);
                return finish(static_cast<int>(ExitCode::elevation), true);
            }
            if (*elevated_code >
                static_cast<DWORD>(std::numeric_limits<int>::max())) {
                return finish(static_cast<int>(ExitCode::failure), true);
            }
            const int child_code = static_cast<int>(*elevated_code);
            return finish(child_code,
                          child_code != static_cast<int>(ExitCode::success) &&
                              child_code != static_cast<int>(ExitCode::cancelled));
        }
        if (!writable && session.config.elevation == SfxElevation::none) {
            report(session, ui,
                   L"The destination is not writable: " +
                       session.destination.wstring(),
                   true);
            return finish(static_cast<int>(ExitCode::elevation), true);
        }

        // ---- selection and free space -------------------------------------
        std::vector<std::string> selected;
        std::uint64_t required = session.unpacked_size;
        if (!session.command_line.include.empty()) {
            if (!session.capabilities.selective_extract) {
                report(session, ui,
                       L"This payload does not support selective extraction.", true);
                return finish(static_cast<int>(ExitCode::usage), true);
            }
            selected = select_entries(session.entries, session.command_line.include);
            if (selected.empty()) {
                report(session, ui, L"No entries matched --include.", true);
                return finish(static_cast<int>(ExitCode::usage), true);
            }
            required = 0;
            for (const auto& entry : session.entries) {
                if (!entry.is_directory &&
                    std::find(selected.begin(), selected.end(), entry.path) !=
                        selected.end()) {
                    required += entry.size;
                }
            }
        }

        // Failing here beats failing partway through a long extraction.
        if (const auto free_space = available_free_space(session.destination)) {
            if (*free_space < required) {
                report(session, ui,
                       L"Not enough free space: " + format_size(required) +
                           L" needed, " + format_size(*free_space) + L" available.",
                       true);
                return finish(static_cast<int>(ExitCode::disk_space), true);
            }
        }

        // ---- extract ------------------------------------------------------
        auto operation = std::make_shared<axiom::OperationControl>();
        session.options.operation = operation;
        bool cancelled = false;
        std::exception_ptr failure;
        ui.run_with_progress(operation, [&] {
            try {
                if (selected.empty()) {
                    session.archive->extract_all(session.destination,
                                                 session.options);
                } else {
                    session.archive->extract_selected(selected, session.destination,
                                                      session.options);
                }
            } catch (const axiom::OperationCancelled&) {
                cancelled = true;
            } catch (...) {
                failure = std::current_exception();
            }
        });
        secure_clear(session.options.password);
        if (failure) std::rethrow_exception(failure);
        if (cancelled) {
            report(session, ui, L"Extraction was cancelled.", false);
            return finish(static_cast<int>(ExitCode::cancelled), false);
        }

        const int ran = run_after_extract(session, ui);
        if (ran != static_cast<int>(ExitCode::success)) return finish(ran, true);

        if (open_destination && interactive(session, ui)) {
            ui.reveal(session.destination);
        }
        if (interactive(session, ui) && !session.config.auto_close) {
            const std::wstring message =
                L"Files were extracted to:\n\n" + session.destination.wstring();
            if (session.transcript) session.transcript->append(message + L"\n");
            ui.message(session.title, message, false);
        } else {
            report(session, ui, L"Extracted to " + session.destination.wstring(),
                   false);
        }
        return finish(static_cast<int>(ExitCode::success), false);
    } catch (const std::exception& error) {
        report(session, ui, L"Extraction failed:\n\n" + widen(error.what()), true);
        return finish(static_cast<int>(ExitCode::failure), true);
    }
}

}  // namespace axiom::sfx
