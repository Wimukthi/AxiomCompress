#include "sfx/sfx_config.hpp"

#include "archive/sfx_image.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <unordered_set>

namespace axiom::sfx {
namespace {

// TLV tags. Values are part of the on-disk format: append, never reuse.
enum class Tag : std::uint16_t {
    title = 1,
    window_title = 2,
    description = 3,
    banner_text = 4,
    theme = 5,
    default_path = 6,
    allow_path_change = 7,
    create_subfolder = 8,
    license_text = 9,
    require_accept = 10,
    mode = 11,
    overwrite = 12,
    restore_mtime = 13,
    open_destination = 14,
    auto_close = 15,
    threads = 16,
    allow_file_selection = 17,
    run_program = 18,
    run_arguments = 19,
    run_working_dir = 20,
    wait_for_exit = 21,
    propagate_exit_code = 22,
    elevation = 23,
};

constexpr std::uint16_t kConfigVersion = 1;
constexpr std::array<std::uint8_t, 4> kConfigMagic = {'A', 'X', 'C', 'F'};
// A generated SFX is not a document format; these ceilings keep a corrupt blob
// from turning into a huge allocation.
constexpr std::size_t kMaxTextLength = 1u << 20;
constexpr std::size_t kMaxPathLength = 32767;

constexpr std::array<std::string_view, 10> kPathTokens = {
    "ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA", "APPDATA",
    "USERPROFILE", "DESKTOP", "DOCUMENTS", "TEMP", "SFXDIR", "SFXNAME",
};

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
        out.push_back(static_cast<std::uint8_t>((value >> (index * 8)) & 0xFF));
    }
}

void put_text(std::vector<std::uint8_t>& out, Tag tag, std::string_view text) {
    if (text.empty()) return;
    put_u16(out, static_cast<std::uint16_t>(tag));
    put_u32(out, static_cast<std::uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

void put_scalar(std::vector<std::uint8_t>& out, Tag tag, std::uint32_t value) {
    put_u16(out, static_cast<std::uint16_t>(tag));
    put_u32(out, 4);
    put_u32(out, value);
}

struct Reader {
    std::span<const std::uint8_t> data;
    std::size_t offset = 0;

    bool remaining(std::size_t count) const {
        return data.size() - offset >= count;
    }
    std::uint16_t u16() {
        const std::uint16_t value =
            static_cast<std::uint16_t>(data[offset]) |
            static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(data[offset + 1]) << 8);
        offset += 2;
        return value;
    }
    std::uint32_t u32() {
        std::uint32_t value = 0;
        for (unsigned index = 0; index < 4; ++index) {
            value |= static_cast<std::uint32_t>(data[offset + index]) << (index * 8);
        }
        offset += 4;
        return value;
    }
};

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

std::optional<bool> parse_bool(std::string_view text) {
    if (text == "1" || text == "true" || text == "yes" || text == "on") return true;
    if (text == "0" || text == "false" || text == "no" || text == "off") return false;
    return std::nullopt;
}

// Interprets "\n" in authored text so a description or license can span lines
// without needing a multi-line INI syntax.
std::string unescape(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\\' && index + 1 < text.size()) {
            const char next = text[index + 1];
            if (next == 'n') { result.push_back('\n'); ++index; continue; }
            if (next == 't') { result.push_back('\t'); ++index; continue; }
            if (next == '\\') { result.push_back('\\'); ++index; continue; }
        }
        result.push_back(text[index]);
    }
    return result;
}

bool valid_relative_path_text(std::string_view value, std::string_view field,
                              std::string& error) {
    if (value.empty()) return true;
    if (value.front() == '/' || value.front() == '\\' ||
        (value.size() >= 2 && value[1] == ':')) {
        error = std::string(field) + " must be relative to the extraction root";
        return false;
    }
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find_first_of("/\\", begin);
        const auto component = value.substr(
            begin, end == std::string_view::npos ? std::string_view::npos
                                                  : end - begin);
        if (component == "..") {
            error = std::string(field) + " must not contain '..'";
            return false;
        }
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    if (value.find(':') != std::string_view::npos) {
        error = std::string(field) + " must not contain ':'";
        return false;
    }
    return true;
}

bool valid_utf8(std::string_view value) {
    const auto continuation = [](unsigned char byte) {
        return byte >= 0x80 && byte <= 0xBF;
    };
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7F) continue;
        if (first >= 0xC2 && first <= 0xDF) {
            if (index + 1 >= value.size() ||
                !continuation(static_cast<unsigned char>(value[index + 1]))) {
                return false;
            }
            ++index;
            continue;
        }
        if (first == 0xE0) {
            if (index + 2 >= value.size() ||
                static_cast<unsigned char>(value[index + 1]) < 0xA0 ||
                static_cast<unsigned char>(value[index + 1]) > 0xBF ||
                !continuation(static_cast<unsigned char>(value[index + 2]))) {
                return false;
            }
            index += 2;
            continue;
        }
        if ((first >= 0xE1 && first <= 0xEC) ||
            (first >= 0xEE && first <= 0xEF)) {
            if (index + 2 >= value.size() ||
                !continuation(static_cast<unsigned char>(value[index + 1])) ||
                !continuation(static_cast<unsigned char>(value[index + 2]))) {
                return false;
            }
            index += 2;
            continue;
        }
        if (first == 0xED) {
            if (index + 2 >= value.size() ||
                static_cast<unsigned char>(value[index + 1]) < 0x80 ||
                static_cast<unsigned char>(value[index + 1]) > 0x9F ||
                !continuation(static_cast<unsigned char>(value[index + 2]))) {
                return false;
            }
            index += 2;
            continue;
        }
        if (first == 0xF0) {
            if (index + 3 >= value.size() ||
                static_cast<unsigned char>(value[index + 1]) < 0x90 ||
                static_cast<unsigned char>(value[index + 1]) > 0xBF ||
                !continuation(static_cast<unsigned char>(value[index + 2])) ||
                !continuation(static_cast<unsigned char>(value[index + 3]))) {
                return false;
            }
            index += 3;
            continue;
        }
        if (first >= 0xF1 && first <= 0xF3) {
            if (index + 3 >= value.size() ||
                !continuation(static_cast<unsigned char>(value[index + 1])) ||
                !continuation(static_cast<unsigned char>(value[index + 2])) ||
                !continuation(static_cast<unsigned char>(value[index + 3]))) {
                return false;
            }
            index += 3;
            continue;
        }
        if (first == 0xF4) {
            if (index + 3 >= value.size() ||
                static_cast<unsigned char>(value[index + 1]) < 0x80 ||
                static_cast<unsigned char>(value[index + 1]) > 0x8F ||
                !continuation(static_cast<unsigned char>(value[index + 2])) ||
                !continuation(static_cast<unsigned char>(value[index + 3]))) {
                return false;
            }
            index += 3;
            continue;
        }
        return false;
    }
    return true;
}

bool valid_text(std::string_view name, const std::string& value,
                std::size_t limit, std::string& error) {
    if (value.size() > limit) {
        error = std::string(name) + " is too long";
        return false;
    }
    if (value.find('\0') != std::string::npos) {
        error = std::string(name) + " contains a NUL character";
        return false;
    }
    if (!valid_utf8(value)) {
        error = std::string(name) + " is not valid UTF-8";
        return false;
    }
    return true;
}

}  // namespace

bool sfx_is_known_path_token(std::string_view name) {
    return std::find(kPathTokens.begin(), kPathTokens.end(), name) !=
           kPathTokens.end();
}

bool sfx_validate_path_template(std::string_view value, std::string& error) {
    if (value.empty()) return true;
    std::string literal;
    literal.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') {
            literal.push_back(value[index]);
            continue;
        }
        const auto close = value.find('%', index + 1);
        if (close == std::string_view::npos) {
            error = "unterminated % in the destination template";
            return false;
        }
        const std::string_view name = value.substr(index + 1, close - index - 1);
        if (!sfx_is_known_path_token(name)) {
            error = "unknown destination template token: %" + std::string(name) + "%";
            return false;
        }
        index = close;
    }
    // A traversal in the literal portion cannot be justified, and catching it
    // at creation time is better than at the user's machine.
    if (literal.find("..") != std::string::npos) {
        error = "the destination template must not contain '..'";
        return false;
    }
    return true;
}

bool sfx_validate_config(const SfxConfig& config, std::string& error) {
    if (!valid_text("title", config.title, kMaxTextLength, error) ||
        !valid_text("window_title", config.window_title, kMaxTextLength, error) ||
        !valid_text("description", config.description, kMaxTextLength, error) ||
        !valid_text("banner_text", config.banner_text, kMaxTextLength, error) ||
        !valid_text("license_text", config.license_text, kMaxTextLength, error) ||
        !valid_text("default_path", config.default_path, kMaxPathLength, error) ||
        !valid_text("run_program", config.run_program, kMaxPathLength, error) ||
        !valid_text("run_working_dir", config.run_working_dir, kMaxPathLength, error) ||
        !valid_text("run_arguments", config.run_arguments, kMaxPathLength, error)) {
        return false;
    }
    if (static_cast<unsigned>(config.theme) > 2 ||
        static_cast<unsigned>(config.mode) > 2 ||
        static_cast<unsigned>(config.overwrite) > 2 ||
        static_cast<unsigned>(config.elevation) > 2) {
        error = "SFX configuration contains an invalid enum value";
        return false;
    }
    if (config.threads > kSfxMaxThreads) {
        error = "threads is out of range";
        return false;
    }
    if (config.allow_file_selection) {
        error = "allow_file_selection is not supported; use --include instead";
        return false;
    }
    if (!sfx_validate_path_template(config.default_path, error)) return false;
    if (!valid_relative_path_text(config.run_program, "run_program", error) ||
        !valid_relative_path_text(config.run_working_dir, "run_working_dir", error)) {
        return false;
    }
    if (config.require_accept && config.license_text.empty()) {
        error = "require_accept is set but no license text was supplied";
        return false;
    }
    if (!config.run_arguments.empty() && config.run_program.empty()) {
        error = "run_arguments was given without run_program";
        return false;
    }
    // Unattended, elevated, and running a program afterwards is exactly the
    // shape of a silent privileged execution chain. Each part is reasonable;
    // together they are not something Axiom will build.
    if (config.mode == SfxMode::very_silent &&
        config.elevation != SfxElevation::none &&
        !config.run_program.empty()) {
        error = "very_silent with elevation and run_program is refused";
        return false;
    }
    if (config.propagate_exit_code && !config.wait_for_exit) {
        error = "propagate_exit_code requires wait_for_exit";
        return false;
    }
    if (config.create_subfolder && !config.allow_path_change &&
        config.default_path.empty()) {
        error = "create_subfolder needs a default_path when the path is fixed";
        return false;
    }
    return true;
}

std::vector<std::uint8_t> encode_sfx_config(const SfxConfig& config) {
    std::string error;
    if (!sfx_validate_config(config, error)) {
        throw std::invalid_argument("invalid SFX configuration: " + error);
    }
    std::vector<std::uint8_t> out;
    out.insert(out.end(), kConfigMagic.begin(), kConfigMagic.end());
    put_u16(out, kConfigVersion);

    put_text(out, Tag::title, config.title);
    put_text(out, Tag::window_title, config.window_title);
    put_text(out, Tag::description, config.description);
    put_text(out, Tag::banner_text, config.banner_text);
    put_scalar(out, Tag::theme, static_cast<std::uint32_t>(config.theme));
    put_text(out, Tag::default_path, config.default_path);
    put_scalar(out, Tag::allow_path_change, config.allow_path_change ? 1 : 0);
    put_scalar(out, Tag::create_subfolder, config.create_subfolder ? 1 : 0);
    put_text(out, Tag::license_text, config.license_text);
    put_scalar(out, Tag::require_accept, config.require_accept ? 1 : 0);
    put_scalar(out, Tag::mode, static_cast<std::uint32_t>(config.mode));
    put_scalar(out, Tag::overwrite, static_cast<std::uint32_t>(config.overwrite));
    put_scalar(out, Tag::restore_mtime, config.restore_mtime ? 1 : 0);
    put_scalar(out, Tag::open_destination, config.open_destination ? 1 : 0);
    put_scalar(out, Tag::auto_close, config.auto_close ? 1 : 0);
    put_scalar(out, Tag::threads, config.threads);
    put_scalar(out, Tag::allow_file_selection, config.allow_file_selection ? 1 : 0);
    put_text(out, Tag::run_program, config.run_program);
    put_text(out, Tag::run_arguments, config.run_arguments);
    put_text(out, Tag::run_working_dir, config.run_working_dir);
    put_scalar(out, Tag::wait_for_exit, config.wait_for_exit ? 1 : 0);
    put_scalar(out, Tag::propagate_exit_code, config.propagate_exit_code ? 1 : 0);
    put_scalar(out, Tag::elevation, static_cast<std::uint32_t>(config.elevation));
    if (out.size() > kSfxMaxConfigSize) {
        throw std::invalid_argument("SFX configuration is too large");
    }
    return out;
}

std::optional<SfxConfig> decode_sfx_config(std::span<const std::uint8_t> blob) {
    if (blob.size() > kSfxMaxConfigSize) return std::nullopt;
    Reader reader{blob};
    if (!reader.remaining(kConfigMagic.size() + 2)) return std::nullopt;
    if (!std::equal(kConfigMagic.begin(), kConfigMagic.end(), blob.begin())) {
        return std::nullopt;
    }
    reader.offset += kConfigMagic.size();
    if (reader.u16() != kConfigVersion) return std::nullopt;

    SfxConfig config;
    std::array<bool, 24> seen{};
    auto scalar_in_range = [](std::uint32_t value, std::uint32_t limit) {
        return value <= limit;
    };

    while (reader.offset < blob.size()) {
        if (!reader.remaining(6)) return std::nullopt;
        const std::uint16_t tag = reader.u16();
        const std::uint32_t length = reader.u32();
        if (length > kMaxTextLength || !reader.remaining(length)) return std::nullopt;
        if (tag < seen.size()) {
            if (seen[tag]) return std::nullopt;
            seen[tag] = true;
        }
        const auto payload = blob.subspan(reader.offset, length);
        const std::string text(reinterpret_cast<const char*>(payload.data()),
                               payload.size());
        std::uint32_t number = 0;
        if (length == 4) {
            for (unsigned index = 0; index < 4; ++index) {
                number |= static_cast<std::uint32_t>(payload[index]) << (index * 8);
            }
        }
        reader.offset += length;

        switch (static_cast<Tag>(tag)) {
            case Tag::title: config.title = text; break;
            case Tag::window_title: config.window_title = text; break;
            case Tag::description: config.description = text; break;
            case Tag::banner_text: config.banner_text = text; break;
            case Tag::default_path: config.default_path = text; break;
            case Tag::license_text: config.license_text = text; break;
            case Tag::run_program: config.run_program = text; break;
            case Tag::run_arguments: config.run_arguments = text; break;
            case Tag::run_working_dir: config.run_working_dir = text; break;
            case Tag::theme:
                if (length != 4 || !scalar_in_range(number, 2)) return std::nullopt;
                config.theme = static_cast<SfxTheme>(number);
                break;
            case Tag::mode:
                if (length != 4 || !scalar_in_range(number, 2)) return std::nullopt;
                config.mode = static_cast<SfxMode>(number);
                break;
            case Tag::overwrite:
                if (length != 4 || !scalar_in_range(number, 2)) return std::nullopt;
                config.overwrite =
                    static_cast<ExtractOptions::Overwrite>(number);
                break;
            case Tag::elevation:
                if (length != 4 || !scalar_in_range(number, 2)) return std::nullopt;
                config.elevation = static_cast<SfxElevation>(number);
                break;
            case Tag::threads:
                if (length != 4 || number > kSfxMaxThreads) return std::nullopt;
                config.threads = number;
                break;
            case Tag::allow_path_change:
            case Tag::create_subfolder:
            case Tag::require_accept:
            case Tag::restore_mtime:
            case Tag::open_destination:
            case Tag::auto_close:
            case Tag::allow_file_selection:
            case Tag::wait_for_exit:
            case Tag::propagate_exit_code: {
                if (length != 4 || number > 1) return std::nullopt;
                const bool flag = number != 0;
                switch (static_cast<Tag>(tag)) {
                    case Tag::allow_path_change: config.allow_path_change = flag; break;
                    case Tag::create_subfolder: config.create_subfolder = flag; break;
                    case Tag::require_accept: config.require_accept = flag; break;
                    case Tag::restore_mtime: config.restore_mtime = flag; break;
                    case Tag::open_destination: config.open_destination = flag; break;
                    case Tag::auto_close: config.auto_close = flag; break;
                    case Tag::allow_file_selection:
                        config.allow_file_selection = flag; break;
                    case Tag::wait_for_exit: config.wait_for_exit = flag; break;
                    default: config.propagate_exit_code = flag; break;
                }
                break;
            }
            default:
                // An unknown tag means the author configured something this
                // stub cannot honour. Refusing is safer than proceeding.
                return std::nullopt;
        }
    }

    std::string error;
    if (!sfx_validate_config(config, error)) return std::nullopt;
    return config;
}

SfxConfigTextResult parse_sfx_config_text(std::string_view text) {
    SfxConfigTextResult result;
    SfxConfig& config = result.value;
    std::size_t line_number = 0;

    if (text.size() > kSfxMaxConfigSize) {
        result.ok = false;
        result.error = "SFX configuration is too large";
        result.line = 1;
        return result;
    }

    // Notepad, PowerShell's Set-Content, and Visual Studio all write a UTF-8
    // BOM by default. Without this the first key parses as "\xEF\xBB\xBFtitle".
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);

    auto fail = [&](std::string message) {
        result.ok = false;
        result.error = std::move(message);
        result.line = line_number;
        return result;
    };

    std::size_t position = 0;
    std::unordered_set<std::string> seen_keys;
    while (position <= text.size()) {
        const auto newline = text.find('\n', position);
        const std::string_view raw = text.substr(
            position, newline == std::string_view::npos ? std::string_view::npos
                                                        : newline - position);
        position = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
        ++line_number;

        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            if (line.back() != ']') return fail("unterminated section header");
            continue;  // section headers are decorative
        }

        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            return fail("expected key = value");
        }
        const std::string key = trim(std::string_view(line).substr(0, equals));
        const std::string value = trim(std::string_view(line).substr(equals + 1));
        if (key.empty()) return fail("configuration key is empty");
        if (!seen_keys.insert(key).second) {
            return fail("duplicate configuration key: " + key);
        }

        auto boolean = [&](bool& target) {
            const auto parsed = parse_bool(value);
            if (!parsed) {
                fail("expected a boolean for " + key);
                return false;
            }
            target = *parsed;
            return true;
        };

        if (key == "title") config.title = unescape(value);
        else if (key == "window_title") config.window_title = unescape(value);
        else if (key == "description") config.description = unescape(value);
        else if (key == "banner_text") config.banner_text = unescape(value);
        else if (key == "default_path") config.default_path = value;
        else if (key == "license_text") config.license_text = unescape(value);
        else if (key == "run_program") config.run_program = value;
        else if (key == "run_arguments") config.run_arguments = value;
        else if (key == "run_working_dir") config.run_working_dir = value;
        else if (key == "allow_path_change") { if (!boolean(config.allow_path_change)) return result; }
        else if (key == "create_subfolder") { if (!boolean(config.create_subfolder)) return result; }
        else if (key == "require_accept") { if (!boolean(config.require_accept)) return result; }
        else if (key == "restore_mtime") { if (!boolean(config.restore_mtime)) return result; }
        else if (key == "open_destination") { if (!boolean(config.open_destination)) return result; }
        else if (key == "auto_close") { if (!boolean(config.auto_close)) return result; }
        else if (key == "allow_file_selection") { if (!boolean(config.allow_file_selection)) return result; }
        else if (key == "wait_for_exit") { if (!boolean(config.wait_for_exit)) return result; }
        else if (key == "propagate_exit_code") { if (!boolean(config.propagate_exit_code)) return result; }
        else if (key == "theme") {
            if (value == "auto") config.theme = SfxTheme::automatic;
            else if (value == "light") config.theme = SfxTheme::light;
            else if (value == "dark") config.theme = SfxTheme::dark;
            else return fail("theme expects auto, light, or dark");
        } else if (key == "mode") {
            if (value == "interactive") config.mode = SfxMode::interactive;
            else if (value == "silent") config.mode = SfxMode::silent;
            else if (value == "very_silent") config.mode = SfxMode::very_silent;
            else return fail("mode expects interactive, silent, or very_silent");
        } else if (key == "overwrite") {
            if (value == "replace") config.overwrite = ExtractOptions::Overwrite::overwrite;
            else if (value == "skip") config.overwrite = ExtractOptions::Overwrite::skip;
            else if (value == "fail") config.overwrite = ExtractOptions::Overwrite::fail;
            else return fail("overwrite expects replace, skip, or fail");
        } else if (key == "elevation") {
            if (value == "none") config.elevation = SfxElevation::none;
            else if (value == "auto") config.elevation = SfxElevation::automatic;
            else if (value == "require") config.elevation = SfxElevation::require;
            else return fail("elevation expects none, auto, or require");
        } else if (key == "threads") {
            std::uint32_t parsed = 0;
            if (value.empty()) return fail("threads expects a whole number");
            for (const char character : value) {
                if (character < '0' || character > '9') {
                    return fail("threads expects a whole number");
                }
                parsed = parsed * 10 + static_cast<std::uint32_t>(character - '0');
                if (parsed > kSfxMaxThreads) return fail("threads is out of range");
            }
            config.threads = parsed;
        } else {
            return fail("unknown key: " + key);
        }
    }

    line_number = 0;
    std::string error;
    if (!sfx_validate_config(config, error)) return fail(error);
    return result;
}

}  // namespace axiom::sfx
