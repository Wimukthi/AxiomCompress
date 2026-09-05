#include "gui/file_manager_model.hpp"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <optional>
#include <unordered_set>

namespace axiom::gui {
namespace fs = std::filesystem;
namespace {

std::wstring fold(std::wstring_view value) {
    std::wstring result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return result;
}

std::wstring trim(std::wstring_view value) {
    std::size_t first = 0;
    while (first < value.size() && std::iswspace(value[first])) ++first;
    std::size_t last = value.size();
    while (last > first && std::iswspace(value[last - 1])) --last;
    return std::wstring(value.substr(first, last - first));
}

std::wstring widen_utf8(std::string_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        char32_t codepoint = 0;
        std::size_t count = 0;
        if (first < 0x80) {
            codepoint = first;
            count = 1;
        } else if ((first & 0xE0) == 0xC0) {
            codepoint = first & 0x1F;
            count = 2;
        } else if ((first & 0xF0) == 0xE0) {
            codepoint = first & 0x0F;
            count = 3;
        } else if ((first & 0xF8) == 0xF0) {
            codepoint = first & 0x07;
            count = 4;
        } else {
            codepoint = 0xFFFD;
            count = 1;
        }
        if (count > 1) {
            if (index + count > value.size()) {
                codepoint = 0xFFFD;
                count = 1;
            } else {
                for (std::size_t offset = 1; offset < count; ++offset) {
                    const auto continuation =
                        static_cast<unsigned char>(value[index + offset]);
                    if ((continuation & 0xC0) != 0x80) {
                        codepoint = 0xFFFD;
                        count = 1;
                        break;
                    }
                    codepoint = (codepoint << 6) | (continuation & 0x3F);
                }
            }
        }
        const bool overlong = (count == 2 && codepoint < 0x80) ||
                              (count == 3 && codepoint < 0x800) ||
                              (count == 4 && codepoint < 0x10000);
        if (overlong || codepoint > 0x10FFFF ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            codepoint = 0xFFFD;
        }
        if constexpr (sizeof(wchar_t) == 2) {
            if (codepoint <= 0xFFFF) {
                result.push_back(static_cast<wchar_t>(codepoint));
            } else {
                codepoint -= 0x10000;
                result.push_back(static_cast<wchar_t>(0xD800 + (codepoint >> 10)));
                result.push_back(static_cast<wchar_t>(0xDC00 + (codepoint & 0x3FF)));
            }
        } else {
            result.push_back(static_cast<wchar_t>(codepoint));
        }
        index += count;
    }
    return result;
}

std::vector<std::wstring> filter_terms(std::wstring_view query) {
    std::vector<std::wstring> result;
    std::wstring current;
    bool quoted = false;
    for (const wchar_t ch : query) {
        if (ch == L'"') {
            quoted = !quoted;
        } else if (!quoted && std::iswspace(ch)) {
            if (!current.empty()) {
                result.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) result.push_back(std::move(current));
    return result;
}

bool wildcard_match(std::wstring_view pattern, std::wstring_view text) {
    std::size_t pattern_index = 0;
    std::size_t text_index = 0;
    std::size_t star = std::wstring_view::npos;
    std::size_t retry = 0;
    while (text_index < text.size()) {
        if (pattern_index < pattern.size() &&
            (pattern[pattern_index] == L'?' ||
             std::towlower(pattern[pattern_index]) == std::towlower(text[text_index]))) {
            ++pattern_index;
            ++text_index;
        } else if (pattern_index < pattern.size() && pattern[pattern_index] == L'*') {
            star = pattern_index++;
            retry = text_index;
        } else if (star != std::wstring_view::npos) {
            pattern_index = star + 1;
            text_index = ++retry;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern.size() && pattern[pattern_index] == L'*') {
        ++pattern_index;
    }
    return pattern_index == pattern.size();
}

bool text_match(std::wstring_view pattern, std::wstring_view value) {
    if (pattern.find_first_of(L"*?") != std::wstring_view::npos) {
        return wildcard_match(pattern, value);
    }
    return fold(value).find(fold(pattern)) != std::wstring::npos;
}

enum class Comparison { equal, less, less_equal, greater, greater_equal };

struct ComparedValue {
    Comparison comparison = Comparison::equal;
    std::wstring_view value;
};

ComparedValue split_comparison(std::wstring_view value) {
    if (value.starts_with(L">=")) return {Comparison::greater_equal, value.substr(2)};
    if (value.starts_with(L"<=")) return {Comparison::less_equal, value.substr(2)};
    if (value.starts_with(L">")) return {Comparison::greater, value.substr(1)};
    if (value.starts_with(L"<")) return {Comparison::less, value.substr(1)};
    if (value.starts_with(L"=")) return {Comparison::equal, value.substr(1)};
    return {Comparison::equal, value};
}

template <typename Value>
bool compare_value(Value left, Value right, Comparison comparison) {
    switch (comparison) {
        case Comparison::equal: return left == right;
        case Comparison::less: return left < right;
        case Comparison::less_equal: return left <= right;
        case Comparison::greater: return left > right;
        case Comparison::greater_equal: return left >= right;
    }
    return false;
}

std::optional<std::uint64_t> parse_size(std::wstring_view text) {
    const std::wstring owned = trim(text);
    if (owned.empty()) return std::nullopt;
    wchar_t* end = nullptr;
    const long double number = std::wcstold(owned.c_str(), &end);
    if (end == owned.c_str() || !std::isfinite(number) || number < 0.0L) {
        return std::nullopt;
    }
    std::wstring suffix = fold(trim(std::wstring_view(
        end, static_cast<std::size_t>(owned.c_str() + owned.size() - end))));
    std::uint64_t multiplier = 1;
    if (suffix.empty() || suffix == L"b") {
        multiplier = 1;
    } else if (suffix == L"k" || suffix == L"kb" || suffix == L"kib") {
        multiplier = 1ull << 10;
    } else if (suffix == L"m" || suffix == L"mb" || suffix == L"mib") {
        multiplier = 1ull << 20;
    } else if (suffix == L"g" || suffix == L"gb" || suffix == L"gib") {
        multiplier = 1ull << 30;
    } else if (suffix == L"t" || suffix == L"tb" || suffix == L"tib") {
        multiplier = 1ull << 40;
    } else {
        return std::nullopt;
    }
    const long double bytes = number * static_cast<long double>(multiplier);
    // Compare with the exact exclusive 2^64 bound. Converting UINT64_MAX to
    // long double rounds it up on platforms where long double is IEEE binary64.
    const long double exclusive_limit =
        std::ldexp(1.0L, std::numeric_limits<std::uint64_t>::digits);
    if (bytes >= exclusive_limit) return std::nullopt;
    return static_cast<std::uint64_t>(bytes);
}

bool valid_iso_date(std::wstring_view value) {
    if (value.size() != 10 || value[4] != L'-' || value[7] != L'-') return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 4 || index == 7) continue;
        if (!std::iswdigit(value[index])) return false;
    }
    const int month = std::stoi(std::wstring(value.substr(5, 2)));
    const int day = std::stoi(std::wstring(value.substr(8, 2)));
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

bool kind_match(const BrowserItem& item, std::wstring_view value) {
    const std::wstring expected = fold(value);
    if (expected == L"folder" || expected == L"directory") {
        return item.kind == BrowserItemKind::directory ||
               item.kind == BrowserItemKind::drive ||
               item.kind == BrowserItemKind::parent;
    }
    if (expected == L"archive") {
        return item.kind == BrowserItemKind::archive ||
               fold(item.type).find(L"archive") != std::wstring::npos;
    }
    if (expected == L"file") {
        return item.kind == BrowserItemKind::file ||
               item.kind == BrowserItemKind::symlink ||
               item.kind == BrowserItemKind::hardlink;
    }
    if (expected == L"link") {
        return item.kind == BrowserItemKind::symlink ||
               item.kind == BrowserItemKind::hardlink;
    }
    if (expected == L"drive") return item.kind == BrowserItemKind::drive;
    return text_match(value, item.type);
}

bool field_match(const BrowserItem& item, std::wstring_view field,
                 std::wstring_view value) {
    const std::wstring key = fold(field);
    if (key == L"name") return text_match(value, item.name);
    if (key == L"type" || key == L"kind") return kind_match(item, value);
    if (key == L"ext" || key == L"extension") {
        std::wstring extension = fs::path(item.name).extension().wstring();
        if (!value.empty() && value.front() != L'.' && value.front() != L'*') {
            extension = extension.empty() ? extension : extension.substr(1);
        }
        return text_match(value, extension);
    }
    if (key == L"path") {
        const std::wstring path = !item.filesystem_path.empty()
            ? item.filesystem_path.wstring() : widen_utf8(item.archive_path);
        return text_match(value, path);
    }
    if (key == L"size") {
        const ComparedValue compared = split_comparison(value);
        const auto bytes = parse_size(compared.value);
        return bytes && compare_value(item.size, *bytes, compared.comparison);
    }
    if (key == L"date" || key == L"modified") {
        const ComparedValue compared = split_comparison(value);
        if (!valid_iso_date(compared.value) || item.modified.size() < 10) return false;
        return compare_value(item.modified.substr(0, 10),
                             std::wstring(compared.value), compared.comparison);
    }
    return false;
}

bool term_match(const BrowserItem& item, std::wstring_view term) {
    const std::size_t separator = term.find(L':');
    if (separator != std::wstring_view::npos && separator != 0) {
        const std::wstring field = fold(term.substr(0, separator));
        if (field == L"name" || field == L"type" || field == L"kind" ||
            field == L"ext" || field == L"extension" || field == L"path" ||
            field == L"size" || field == L"date" || field == L"modified") {
            return field_match(item, field, term.substr(separator + 1));
        }
    }
    if (text_match(term, item.name) || text_match(term, item.type)) return true;
    if (!item.filesystem_path.empty() && text_match(term, item.filesystem_path.wstring())) {
        return true;
    }
    return !item.archive_path.empty() &&
           text_match(term, widen_utf8(item.archive_path));
}

bool reserved_device_name(std::wstring_view name) {
    std::wstring base = fold(name.substr(0, name.find(L'.')));
    while (!base.empty() && (base.back() == L' ' || base.back() == L'.')) base.pop_back();
    if (base == L"con" || base == L"prn" || base == L"aux" || base == L"nul" ||
        base == L"conin$" || base == L"conout$") {
        return true;
    }
    if (base.size() == 4 && (base.starts_with(L"com") || base.starts_with(L"lpt")) &&
        base[3] >= L'1' && base[3] <= L'9') {
        return true;
    }
    return false;
}

bool ends_with_folded(std::wstring_view value, std::wstring_view suffix) {
    return value.size() >= suffix.size() &&
           fold(value.substr(value.size() - suffix.size())) == fold(suffix);
}

std::wstring strip_archive_suffix(std::wstring name) {
    const std::wstring folded = fold(name);
    if (ends_with_folded(folded, L".axar")) {
        const std::wstring_view without_extension(
            folded.data(), folded.size() - std::wstring_view(L".axar").size());
        const std::size_t part = without_extension.rfind(L".part");
        if (part != std::wstring_view::npos) {
            const std::wstring_view number = without_extension.substr(part + 5);
            if (!number.empty() &&
                std::all_of(number.begin(), number.end(), [](wchar_t ch) {
                    return std::iswdigit(ch) != 0;
                })) {
                return name.substr(0, part);
            }
        }
    }
    const std::size_t axar_part = folded.rfind(L".axar.part");
    if (axar_part != std::wstring::npos) return name.substr(0, axar_part);
    const std::size_t rar_part = folded.rfind(L".part");
    if (rar_part != std::wstring::npos && ends_with_folded(folded, L".rar")) {
        return name.substr(0, rar_part);
    }
    const fs::path numeric_path(name);
    const std::wstring numeric_extension = numeric_path.extension().wstring();
    if (numeric_extension.size() > 1 &&
        std::all_of(numeric_extension.begin() + 1, numeric_extension.end(),
                    [](wchar_t ch) { return std::iswdigit(ch) != 0; })) {
        const fs::path without_volume = numeric_path.parent_path() / numeric_path.stem();
        return without_volume.has_extension()
            ? without_volume.stem().wstring()
            : without_volume.filename().wstring();
    }
    constexpr std::wstring_view compound_suffixes[] = {
        L".tar.gz", L".tar.xz", L".tar.bz2", L".tar.zst"};
    for (const auto suffix : compound_suffixes) {
        if (ends_with_folded(folded, suffix)) {
            return name.substr(0, name.size() - suffix.size());
        }
    }
    const fs::path path(name);
    return path.has_extension() ? path.stem().wstring() : name;
}

}  // namespace

bool browser_item_matches_filter(const BrowserItem& item, std::wstring_view query) {
    if (item.kind == BrowserItemKind::parent) return true;
    const auto terms = filter_terms(query);
    for (std::wstring term : terms) {
        bool negate = false;
        if (term.size() > 1 && term.front() == L'-') {
            negate = true;
            term.erase(term.begin());
        }
        const bool matched = term_match(item, term);
        if (matched == negate) return false;
    }
    return true;
}

std::wstring file_manager_leaf_name_error(std::wstring_view name) {
    if (name.empty()) return L"The name cannot be empty.";
    if (name == L"." || name == L"..") return L"Choose a normal file or folder name.";
    if (name.size() > 255) return L"The name is longer than 255 characters.";
    if (name.back() == L' ' || name.back() == L'.') {
        return L"Windows names cannot end with a space or period.";
    }
    constexpr std::wstring_view invalid = L"<>:\"/\\|?*";
    for (const wchar_t ch : name) {
        if (ch < 32 || invalid.find(ch) != std::wstring_view::npos) {
            return L"The name contains a character Windows cannot use: < > : \" / \\ | ? *";
        }
    }
    if (reserved_device_name(name)) {
        return L"That name is reserved by Windows. Choose another name.";
    }
    return {};
}

std::wstring extraction_folder_name(const fs::path& archive) {
    std::wstring name = strip_archive_suffix(archive.filename().wstring());
    while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
    if (name.empty() || !file_manager_leaf_name_error(name).empty()) return L"Extracted";
    return name;
}

std::vector<fs::path> batch_extraction_destinations(
    const fs::path& root, const std::vector<fs::path>& archives) {
    std::vector<fs::path> result;
    result.reserve(archives.size());
    std::unordered_set<std::wstring> used;
    for (const auto& archive : archives) {
        const std::wstring base = extraction_folder_name(archive);
        std::wstring candidate = base;
        for (std::size_t index = 2; !used.insert(fold(candidate)).second; ++index) {
            candidate = base + L" (" + std::to_wstring(index) + L")";
        }
        result.push_back(root / candidate);
    }
    return result;
}

}  // namespace axiom::gui
