#include "sfx/sfx_options.hpp"

#include <charconv>
#include <string_view>

namespace axiom::sfx {
namespace {

bool is_option(std::wstring_view text) {
    return text.size() >= 2 && (text[0] == L'-' || text[0] == L'/');
}

// Accepts "-o value", "--output value", "--output=value", and "/o value" so a
// command line written in either Windows or POSIX style behaves the same.
std::wstring_view option_name(std::wstring_view text) {
    if (text.starts_with(L"--")) return text.substr(2);
    if (text.starts_with(L"-") || text.starts_with(L"/")) return text.substr(1);
    return text;
}

std::optional<std::size_t> parse_count(std::wstring_view text) {
    if (text.empty()) return std::nullopt;
    std::size_t value = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') return std::nullopt;
        const std::size_t digit = static_cast<std::size_t>(character - L'0');
        if (value > (static_cast<std::size_t>(-1) - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
    }
    return value;
}

std::optional<ExtractOptions::Overwrite> parse_overwrite(std::wstring_view text) {
    if (text == L"replace") return ExtractOptions::Overwrite::overwrite;
    if (text == L"skip") return ExtractOptions::Overwrite::skip;
    if (text == L"fail") return ExtractOptions::Overwrite::fail;
    return std::nullopt;
}

struct Cursor {
    std::span<const std::wstring> arguments;
    std::size_t index = 0;
    std::wstring pending_value;
    bool has_pending = false;

    // Consumes the value belonging to the option at `index`, whether it arrived
    // as "--name=value" or as a following argument.
    bool take_value(std::wstring& out) {
        if (has_pending) {
            out = pending_value;
            has_pending = false;
            return true;
        }
        if (index + 1 >= arguments.size()) return false;
        out = arguments[++index];
        return true;
    }
};

SfxCommandLineResult fail(std::wstring message) {
    SfxCommandLineResult result;
    result.ok = false;
    result.error = std::move(message);
    return result;
}

}  // namespace

SfxCommandLineResult parse_sfx_command_line(
    std::span<const std::wstring> arguments) {
    SfxCommandLineResult result;
    SfxCommandLine& value = result.value;
    Cursor cursor{arguments};

    for (; cursor.index < arguments.size(); ++cursor.index) {
        const std::wstring& argument = arguments[cursor.index];
        if (!is_option(argument)) {
            if (value.destination.has_value()) {
                return fail(L"more than one destination was given: " + argument);
            }
            if (argument.empty()) return fail(L"destination must not be empty");
            value.destination = argument;
            continue;
        }

        std::wstring_view name = option_name(argument);
        cursor.has_pending = false;
        if (const auto equals = name.find(L'='); equals != std::wstring_view::npos) {
            cursor.pending_value = std::wstring(name.substr(equals + 1));
            cursor.has_pending = true;
            name = name.substr(0, equals);
        }

        std::wstring option_value;
        auto require_value = [&](const wchar_t* label) -> bool {
            if (!cursor.take_value(option_value)) {
                result = fail(std::wstring(L"missing value for ") + label);
                return false;
            }
            return true;
        };

        if (name == L"o" || name == L"output") {
            if (!require_value(L"--output")) return result;
            if (option_value.empty()) return fail(L"--output expects a directory");
            if (value.destination.has_value()) {
                return fail(L"the destination was given twice");
            }
            value.destination = option_value;
        } else if (name == L"s" || name == L"silent") {
            value.mode = SfxMode::silent;
        } else if (name == L"very-silent" || name == L"verysilent") {
            value.mode = SfxMode::very_silent;
        } else if (name == L"y" || name == L"accept") {
            value.accept = true;
        } else if (name == L"p" || name == L"password") {
            if (!require_value(L"--password")) return result;
            if (option_value.empty()) return fail(L"--password expects text");
            value.password = option_value;
        } else if (name == L"password-stdin") {
            value.password_stdin = true;
        } else if (name == L"overwrite") {
            if (!require_value(L"--overwrite")) return result;
            const auto parsed = parse_overwrite(option_value);
            if (!parsed) {
                return fail(L"--overwrite expects replace, skip, or fail");
            }
            value.overwrite = *parsed;
        } else if (name == L"threads") {
            if (!require_value(L"--threads")) return result;
            const auto parsed = parse_count(option_value);
            if (!parsed) return fail(L"--threads expects a whole number");
            if (*parsed > kSfxMaxThreads) {
                return fail(L"--threads must be between 0 and 4096");
            }
            value.threads = *parsed;
        } else if (name == L"include") {
            if (!require_value(L"--include")) return result;
            if (option_value.empty()) return fail(L"--include expects a pattern");
            value.include.push_back(option_value);
        } else if (name == L"no-run") {
            value.no_run = true;
        } else if (name == L"list" || name == L"l") {
            value.list = true;
        } else if (name == L"test" || name == L"t") {
            value.test = true;
        } else if (name == L"log") {
            if (!require_value(L"--log")) return result;
            if (option_value.empty()) return fail(L"--log expects a file path");
            value.log = option_value;
        } else if (name == L"?" || name == L"h" || name == L"help") {
            value.help = true;
        } else {
            return fail(L"unknown option: " + argument);
        }

        if (cursor.has_pending) {
            return fail(argument.substr(0, argument.find(L'=')) +
                        L" does not take a value");
        }
    }

    if (value.password.has_value() && value.password_stdin) {
        return fail(L"--password and --password-stdin cannot be combined");
    }
    if (value.list && value.test) {
        return fail(L"--list and --test cannot be combined");
    }
    // Silent mode against a payload that requires license acceptance is
    // rejected too, but only the stub knows whether acceptance is required, so
    // that check lives there rather than here.
    return result;
}

std::wstring sfx_usage_text() {
    return
        L"Usage: <self-extractor>.exe [options] [destination]\n"
        L"\n"
        L"  -o, --output <dir>       extract into <dir>\n"
        L"  -s, --silent             progress and errors only\n"
        L"      --very-silent        no window at all\n"
        L"  -y, --accept             accept the license, assume yes\n"
        L"  -p, --password <text>    password for an encrypted payload\n"
        L"      --password-stdin     read the password from standard input\n"
        L"      --overwrite <mode>   replace, skip, or fail\n"
        L"      --threads <n>        worker threads, 0 for automatic\n"
        L"      --include <pattern>  extract only matching entries, repeatable\n"
        L"      --no-run             do not run the configured program\n"
        L"      --list               print the contents and exit\n"
        L"      --test               verify integrity and exit\n"
        L"      --log <file>         append a transcript to <file>\n"
        L"  -?, --help               show this text\n"
        L"\n"
        L"Exit codes: 0 success, 1 failed, 2 usage, 3 cancelled, 4 password,\n"
        L"5 integrity, 6 disk space, 7 elevation, 8 run-after-extract failed.\n";
}

}  // namespace axiom::sfx
