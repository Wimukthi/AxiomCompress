#include "archive/file_search.hpp"

#include "archive/container_internal.hpp"
#include "core/path_text.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <cwctype>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace axiom::search {

namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)
// Lowercase for every UTF-16 unit, from the invariant locale: towlower in the C
// locale folds ASCII only, so a capital A-umlaut never met a small one. Built once.
const std::array<wchar_t, 65536>& lowercase_table() {
    static const std::array<wchar_t, 65536> table = [] {
        std::array<wchar_t, 65536> units{};
        for (std::size_t k = 0; k < units.size(); ++k) units[k] = static_cast<wchar_t>(k);
        std::array<wchar_t, 65536> lower = units;
        // Surrogates are not characters on their own; leave them as they are.
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, units.data(), 0xD800,
                      lower.data(), 0xD800, nullptr, nullptr, 0);
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, units.data() + 0xE000, 0x2000,
                      lower.data() + 0xE000, 0x2000, nullptr, nullptr, 0);
        return lower;
    }();
    return table;
}

wchar_t fold_char(wchar_t value) {
    return lowercase_table()[static_cast<std::uint16_t>(value)];
}
#else
wchar_t fold_char(wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); }
#endif

std::wstring folded(std::wstring_view text) {
    std::wstring result(text);
    for (wchar_t& c : result) c = fold_char(c);
    return result;
}

// UTF-8 to wide text without the locale: std::filesystem's conversion throws in
// the C locale on POSIX, and archive paths are UTF-8 everywhere. Bytes that are
// not valid UTF-8 become U+FFFD.
std::wstring widen_utf8(std::string_view text) {
    std::wstring result;
    result.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::uint32_t code = 0xFFFD;
        std::size_t length = 1;
        if (lead < 0x80) {
            code = lead;
        } else if ((lead >> 5) == 0x6) {
            length = 2;
        } else if ((lead >> 4) == 0xE) {
            length = 3;
        } else if ((lead >> 3) == 0x1E) {
            length = 4;
        }
        if (length > 1) {
            bool valid = index + length <= text.size();
            std::uint32_t value = lead & (0x7F >> length);
            for (std::size_t k = 1; valid && k < length; ++k) {
                const auto next = static_cast<unsigned char>(text[index + k]);
                valid = (next & 0xC0) == 0x80;
                value = (value << 6) | (next & 0x3F);
            }
            if (valid) {
                code = value;
            } else {
                length = 1;
            }
        }
        index += length;
        if constexpr (sizeof(wchar_t) == 2) {
            if (code > 0xFFFF) {
                code -= 0x10000;
                result.push_back(static_cast<wchar_t>(0xD800 + (code >> 10)));
                result.push_back(static_cast<wchar_t>(0xDC00 + (code & 0x3FF)));
                continue;
            }
        }
        result.push_back(static_cast<wchar_t>(code));
    }
    return result;
}

std::wstring path_text(const fs::path& path) {
#if defined(_WIN32)
    return path.wstring();
#else
    return widen_utf8(path.native());
#endif
}

bool wildcard_match(std::wstring_view pattern, std::wstring_view text, bool fold) {
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::wstring_view::npos;
    std::size_t retry = 0;
    const auto same = [&](wchar_t a, wchar_t b) { return fold ? fold_char(a) == fold_char(b) : a == b; };
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == L'?' || (pattern[p] != L'*' && same(pattern[p], text[t])))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == L'*') {
            star = p++;
            retry = t;
        } else if (star != std::wstring_view::npos) {
            p = star + 1;
            t = ++retry;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == L'*') ++p;
    return p == pattern.size();
}

// A substring search with the needle already folded (when folding).
bool contains(std::wstring_view haystack, std::wstring_view needle, bool fold) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    const std::size_t last = haystack.size() - needle.size();
    for (std::size_t start = 0; start <= last; ++start) {
        std::size_t k = 0;
        while (k < needle.size() &&
               (fold ? fold_char(haystack[start + k]) : haystack[start + k]) == needle[k]) {
            ++k;
        }
        if (k == needle.size()) return true;
    }
    return false;
}

bool equals(std::wstring_view left, std::wstring_view needle, bool fold) {
    if (left.size() != needle.size()) return false;
    for (std::size_t k = 0; k < left.size(); ++k) {
        if ((fold ? fold_char(left[k]) : left[k]) != needle[k]) return false;
    }
    return true;
}

std::vector<std::wstring> split_terms(std::wstring_view query) {
    std::vector<std::wstring> terms;
    std::wstring current;
    bool quoted = false;
    for (const wchar_t c : query) {
        if (c == L'"') {
            quoted = !quoted;
        } else if (!quoted && std::iswspace(c)) {
            if (!current.empty()) terms.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) terms.push_back(std::move(current));
    return terms;
}

std::optional<std::uint64_t> parse_size(std::wstring_view text) {
    std::wstring owned(text);
    while (!owned.empty() && std::iswspace(owned.back())) owned.pop_back();
    if (owned.empty()) return std::nullopt;
    wchar_t* end = nullptr;
    const long double number = std::wcstold(owned.c_str(), &end);
    if (end == owned.c_str() || !std::isfinite(number) || number < 0.0L) return std::nullopt;
    std::wstring_view unit(end);
    while (!unit.empty() && std::iswspace(unit.front())) unit.remove_prefix(1);  // "64 MB"
    const std::wstring suffix = folded(unit);
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
    if (bytes >= std::ldexp(1.0L, std::numeric_limits<std::uint64_t>::digits)) return std::nullopt;
    return static_cast<std::uint64_t>(bytes);
}

// yyyy-mm-dd as yyyymmdd.
std::optional<std::uint64_t> parse_date(std::wstring_view text) {
    if (text.size() != 10 || text[4] != L'-' || text[7] != L'-') return std::nullopt;
    std::uint64_t value = 0;
    for (std::size_t k = 0; k < text.size(); ++k) {
        if (k == 4 || k == 7) continue;
        if (text[k] < L'0' || text[k] > L'9') return std::nullopt;
        value = value * 10 + static_cast<std::uint64_t>(text[k] - L'0');
    }
    const auto month = (value / 100) % 100;
    const auto day = value % 100;
    if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;
    return value;
}

// The local calendar date of a Unix time, as yyyymmdd, as the Modified column shows it.
std::uint64_t local_date(std::int64_t seconds) {
    const std::time_t time = static_cast<std::time_t>(seconds);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &time) != 0) return 0;
#else
    if (localtime_r(&time, &local) == nullptr) return 0;
#endif
    return static_cast<std::uint64_t>(local.tm_year + 1900) * 10000 +
           static_cast<std::uint64_t>(local.tm_mon + 1) * 100 +
           static_cast<std::uint64_t>(local.tm_mday);
}

template <typename Value, typename Compare>
bool compare_values(Value left, Value right, Compare compare) {
    switch (compare) {
        case Compare::equal: return left == right;
        case Compare::less: return left < right;
        case Compare::less_equal: return left <= right;
        case Compare::greater: return left > right;
        case Compare::greater_equal: return left >= right;
    }
    return false;
}

std::wstring_view extension_of(std::wstring_view name) {
    const std::size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot == 0) return {};
    return name.substr(dot);
}

#if !defined(_WIN32)
std::int64_t unix_seconds(fs::file_time_type stamp) {
    using Clock = fs::file_time_type::clock;
    if constexpr (requires { Clock::to_sys(stamp); }) {
        return std::chrono::duration_cast<std::chrono::seconds>(
            Clock::to_sys(stamp).time_since_epoch()).count();
    } else {
        const auto system = std::chrono::system_clock::now() +
            std::chrono::duration_cast<std::chrono::system_clock::duration>(stamp - Clock::now());
        return std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch()).count();
    }
}
#endif

std::string normalize_archive_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    const auto first = path.find_first_not_of('/');
    if (first == std::string::npos) return {};
    path.erase(0, first);
    while (!path.empty() && path.back() == '/') path.pop_back();
    return path;
}

}  // namespace

// ---- index ---------------------------------------------------------------------

void Index::append(Batch&& batch) {
    const auto folder_base = static_cast<std::uint32_t>(folders_.size());
    const auto archive_base = static_cast<std::uint32_t>(archives_.size());
    for (auto& folder : batch.folders) {
        if (folder.archive != kNoArchive) folder.archive += archive_base;
        folders_.push_back(std::move(folder));
    }
    for (auto& archive : batch.archives) archives_.push_back(std::move(archive));
    entries_.reserve(entries_.size() + batch.entries.size());
    for (auto& entry : batch.entries) {
        entry.folder += folder_base;
        entries_.push_back(std::move(entry));
    }
    read_errors_ += batch.read_errors;
    skipped_encrypted_ += batch.skipped_encrypted;
    content_read_ += batch.content_read;
    content_skipped_ += batch.content_skipped;
}

std::wstring Index::display_path(const Entry& entry) const {
    const Folder& location = folders_[entry.folder];
    if (location.display.empty()) return entry.name;
    std::wstring result = location.display;
    const bool in_archive = location.archive != kNoArchive;
    // Archive paths always use '/'; a folder on disk uses the platform's own.
    const wchar_t separator =
        in_archive ? L'/' : static_cast<wchar_t>(fs::path::preferred_separator);
    if (result.back() != separator && result.back() != L'\\' && result.back() != L'/') {
        result.push_back(separator);
    }
    result += entry.name;
    return result;
}

// ---- query ---------------------------------------------------------------------

std::optional<Query> Query::compile(std::wstring_view text, const QueryOptions& options,
                                    std::wstring* error) {
    Query query;
    query.options_ = options;
    const auto fail = [&](std::wstring message) -> std::optional<Query> {
        if (error != nullptr) *error = std::move(message);
        return std::nullopt;
    };
    for (std::wstring token : split_terms(text)) {
        Term term;
        if (token.size() > 1 && token.front() == L'-') {
            term.negate = true;
            token.erase(token.begin());
        }
        std::wstring_view value = token;
        const std::size_t colon = token.find(L':');
        if (colon != std::wstring::npos && colon != 0) {
            const std::wstring key = folded(std::wstring_view(token).substr(0, colon));
            const std::wstring_view rest = std::wstring_view(token).substr(colon + 1);
            const auto comparison = [&]() {
                std::wstring_view v = rest;
                if (v.starts_with(L">=")) { term.compare = Compare::greater_equal; v.remove_prefix(2); }
                else if (v.starts_with(L"<=")) { term.compare = Compare::less_equal; v.remove_prefix(2); }
                else if (v.starts_with(L">")) { term.compare = Compare::greater; v.remove_prefix(1); }
                else if (v.starts_with(L"<")) { term.compare = Compare::less; v.remove_prefix(1); }
                else if (v.starts_with(L"=")) { term.compare = Compare::equal; v.remove_prefix(1); }
                return v;
            };
            if (key == L"name") {
                term.field = Field::name;
                value = rest;
            } else if (key == L"ext" || key == L"extension") {
                term.field = Field::ext;
                value = rest;
            } else if (key == L"path") {
                term.field = Field::path;
                value = rest;
            } else if (key == L"size") {
                term.field = Field::size;
                const auto bytes = parse_size(comparison());
                if (!bytes) return fail(L"size: needs a size such as 10M or >=1.5G");
                term.number = *bytes;
            } else if (key == L"date" || key == L"modified") {
                term.field = Field::date;
                const auto date = parse_date(comparison());
                if (!date) return fail(L"date: needs a date such as 2026-01-31 or >=2026-01-31");
                term.number = *date;
                query.needs_date_ = true;
            } else if (key == L"type" || key == L"kind") {
                term.field = Field::kind;
                const std::wstring kind = folded(rest);
                if (kind == L"file") term.kind = EntryKind::file;
                else if (kind == L"folder" || kind == L"directory") term.kind = EntryKind::folder;
                else if (kind == L"archive") term.kind = EntryKind::archive;
                else if (kind == L"link") term.kind = EntryKind::link;
                else return fail(L"type: needs file, folder, archive or link");
            }
        }
        if (term.field == Field::plain || term.field == Field::name || term.field == Field::ext ||
            term.field == Field::path) {
            std::wstring pattern(value);
            if (term.field == Field::ext && !pattern.empty() && pattern.front() != L'.' &&
                pattern.front() != L'*') {
                pattern.insert(pattern.begin(), L'.');
            }
            term.wildcard = pattern.find_first_of(L"*?") != std::wstring::npos;
            const bool fold = term.field == Field::ext || !options.match_case;
            term.text = fold ? folded(pattern) : pattern;
            if (term.text.empty() && term.field != Field::plain) {
                return fail(L"a field needs a value, such as name:report");
            }
        }
        query.terms_.push_back(std::move(term));
    }
    return query;
}

bool Query::text_matches(const Term& term, std::wstring_view value, bool whole) const {
    const bool fold = term.field == Field::ext || !options_.match_case;
    if (term.wildcard) return wildcard_match(term.text, value, fold);
    return whole ? equals(value, term.text, fold) : contains(value, term.text, fold);
}

bool Query::term_matches(const Term& term, const Entry& entry,
                         const std::function<std::wstring()>& path) const {
    switch (term.field) {
        case Field::plain:
            if (options_.whole_name) return text_matches(term, entry.name, true);
            if (text_matches(term, entry.name, false)) return true;
            return options_.search_path && text_matches(term, path(), false);
        case Field::name:
            return text_matches(term, entry.name, options_.whole_name);
        case Field::ext:
            return entry.kind != EntryKind::folder &&
                   text_matches(term, extension_of(entry.name), true);
        case Field::path:
            return text_matches(term, path(), false);
        case Field::size:
            return entry.kind != EntryKind::folder &&
                   compare_values(entry.size, term.number, term.compare);
        case Field::date:
            return entry.modified != 0 &&
                   compare_values(local_date(entry.modified), term.number, term.compare);
        case Field::kind:
            return entry.kind == term.kind;
    }
    return false;
}

bool Query::matches(const Index& index, const Entry& entry) const {
    return matches_with(entry, [&] { return index.display_path(entry); });
}

bool Query::matches(const Entry& entry, std::wstring_view folder_display) const {
    return matches_with(entry, [&] {
        std::wstring path(folder_display);
        if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
            path.push_back(path.find(L" :: /") != std::wstring::npos
                               ? L'/'
                               : static_cast<wchar_t>(fs::path::preferred_separator));
        }
        return path + entry.name;
    });
}

bool Query::matches_with(const Entry& entry, const std::function<std::wstring()>& path) const {
    switch (entry.kind) {
        case EntryKind::file:
        case EntryKind::link:
            if (!options_.files) return false;
            break;
        case EntryKind::folder:
            if (!options_.folders) return false;
            break;
        case EntryKind::archive:
            if (!options_.archives) return false;
            break;
    }
    for (const Term& term : terms_) {
        if (term_matches(term, entry, path) == term.negate) return false;
    }
    return true;
}

// ---- content -------------------------------------------------------------------

namespace {

std::uint8_t fold_byte(std::uint8_t value) {
    return value >= 'A' && value <= 'Z' ? static_cast<std::uint8_t>(value + ('a' - 'A')) : value;
}

std::uint16_t fold_unit(std::uint16_t value) {
    if (value >= 0xD800 && value <= 0xDFFF) return value;  // surrogates stand as written
    return static_cast<std::uint16_t>(fold_char(static_cast<wchar_t>(value)));
}

// Code points of the text, from UTF-16 or UTF-32 wchar_t.
std::vector<std::uint32_t> code_points(std::wstring_view text) {
    std::vector<std::uint32_t> points;
    for (std::size_t k = 0; k < text.size(); ++k) {
        std::uint32_t c = static_cast<std::uint32_t>(text[k]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (c >= 0xD800 && c <= 0xDBFF && k + 1 < text.size()) {
                const std::uint32_t low = static_cast<std::uint32_t>(text[k + 1]);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                    ++k;
                }
            }
        }
        points.push_back(c);
    }
    return points;
}

}  // namespace

std::optional<ContentPattern> ContentPattern::compile(std::wstring_view text, bool match_case) {
    if (text.empty()) return std::nullopt;
    ContentPattern pattern;
    pattern.match_case_ = match_case;
    for (const std::uint32_t c : code_points(text)) {
        if (c < 0x80) {
            pattern.utf8_.push_back(static_cast<std::uint8_t>(c));
        } else if (c < 0x800) {
            pattern.utf8_.push_back(static_cast<std::uint8_t>(0xC0 | (c >> 6)));
            pattern.utf8_.push_back(static_cast<std::uint8_t>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            pattern.utf8_.push_back(static_cast<std::uint8_t>(0xE0 | (c >> 12)));
            pattern.utf8_.push_back(static_cast<std::uint8_t>(0x80 | ((c >> 6) & 0x3F)));
            pattern.utf8_.push_back(static_cast<std::uint8_t>(0x80 | (c & 0x3F)));
        } else {
            pattern.utf8_.push_back(static_cast<std::uint8_t>(0xF0 | (c >> 18)));
            pattern.utf8_.push_back(static_cast<std::uint8_t>(0x80 | ((c >> 12) & 0x3F)));
            pattern.utf8_.push_back(static_cast<std::uint8_t>(0x80 | ((c >> 6) & 0x3F)));
            pattern.utf8_.push_back(static_cast<std::uint8_t>(0x80 | (c & 0x3F)));
        }
        if (c < 0x10000) {
            pattern.utf16_.push_back(static_cast<std::uint16_t>(c));
        } else {
            const std::uint32_t v = c - 0x10000;
            pattern.utf16_.push_back(static_cast<std::uint16_t>(0xD800 + (v >> 10)));
            pattern.utf16_.push_back(static_cast<std::uint16_t>(0xDC00 + (v & 0x3FF)));
        }
    }
    if (!match_case) {
        for (auto& b : pattern.utf8_) b = fold_byte(b);
        for (auto& u : pattern.utf16_) u = fold_unit(u);
    }
    // Horspool shift tables. UTF-16 units share a 256-entry table keyed by their
    // low byte, taking the smallest shift of the units that share a key, which
    // keeps every skip safe.
    const std::size_t m8 = pattern.utf8_.size();
    pattern.utf8_shift_.fill(m8);
    for (std::size_t k = 0; k + 1 < m8; ++k) pattern.utf8_shift_[pattern.utf8_[k]] = m8 - 1 - k;
    const std::size_t m16 = pattern.utf16_.size();
    pattern.utf16_shift_.fill(m16);
    for (std::size_t k = 0; k + 1 < m16; ++k) {
        auto& shift = pattern.utf16_shift_[pattern.utf16_[k] & 0xFF];
        shift = std::min(shift, m16 - 1 - k);
    }
    pattern.overlap_ = std::max(m8, 2 * m16 + 1);
    return pattern;
}

bool ContentPattern::find_utf8(const std::uint8_t* data, std::size_t size) const {
    const std::size_t m = utf8_.size();
    if (size < m) return false;
    std::size_t i = 0;
    while (i + m <= size) {
        std::size_t k = m;
        if (match_case_) {
            while (k > 0 && data[i + k - 1] == utf8_[k - 1]) --k;
        } else {
            while (k > 0 && fold_byte(data[i + k - 1]) == utf8_[k - 1]) --k;
        }
        if (k == 0) return true;
        const std::uint8_t last = match_case_ ? data[i + m - 1] : fold_byte(data[i + m - 1]);
        i += utf8_shift_[last];
    }
    return false;
}

bool ContentPattern::find_utf16(const std::uint8_t* data, std::size_t size,
                                std::size_t parity) const {
    const std::size_t m = utf16_.size();
    if (size < parity) return false;
    const std::size_t units = (size - parity) / 2;
    if (units < m) return false;
    const auto unit = [&](std::size_t j) {
        const std::size_t at = parity + 2 * j;
        const auto value = static_cast<std::uint16_t>(data[at] | (data[at + 1] << 8));
        return match_case_ ? value : fold_unit(value);
    };
    std::size_t i = 0;
    while (i + m <= units) {
        std::size_t k = m;
        while (k > 0 && unit(i + k - 1) == utf16_[k - 1]) --k;
        if (k == 0) return true;
        i += utf16_shift_[unit(i + m - 1) & 0xFF];
    }
    return false;
}

bool ContentPattern::find(const std::uint8_t* data, std::size_t size) const {
    return find_utf8(data, size) || find_utf16(data, size, 0) || find_utf16(data, size, 1);
}

bool ContentScan::feed(std::span<const std::uint8_t> bytes) {
    if (found_ || bytes.empty()) return found_;
    const std::size_t overlap = pattern_.overlap();
    // The carry is the last `overlap` bytes seen; only the seam between it and
    // this piece needs searching together.
    seam_.assign(carry_.begin(), carry_.end());
    const std::size_t head = std::min(bytes.size(), overlap);
    seam_.insert(seam_.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(head));
    if (!carry_.empty() && pattern_.find(seam_.data(), seam_.size())) return found_ = true;
    if (pattern_.find(bytes.data(), bytes.size())) return found_ = true;
    if (bytes.size() >= overlap) {
        carry_.assign(bytes.end() - static_cast<std::ptrdiff_t>(overlap), bytes.end());
    } else {
        carry_.insert(carry_.end(), bytes.begin(), bytes.end());
        if (carry_.size() > overlap) {
            carry_.erase(carry_.begin(), carry_.end() - static_cast<std::ptrdiff_t>(overlap));
        }
    }
    return false;
}

// ---- names ---------------------------------------------------------------------

bool looks_like_archive_name(const fs::path& name) {
    const std::wstring file = folded(path_text(name.filename()));
    const auto ends_with = [&](std::wstring_view suffix) {
        return file.size() > suffix.size() && file.ends_with(suffix);
    };
    static constexpr std::array<std::wstring_view, 18> kSuffixes{
        L".axar", L".zip", L".jar", L".war", L".apk", L".7z", L".rar", L".tar", L".tar.gz",
        L".tgz", L".tar.xz", L".txz", L".tar.bz2", L".tbz2", L".tar.zst", L".tzst", L".iso",
        L".cab"};
    for (const auto suffix : kSuffixes) {
        if (ends_with(suffix)) return true;
    }
    // RAR volumes: .r00 to .r99.
    const std::wstring_view extension = extension_of(file);
    return extension.size() == 4 && extension[1] == L'r' && std::iswdigit(extension[2]) &&
           std::iswdigit(extension[3]);
}

bool matches_archive_patterns(std::wstring_view patterns, const fs::path& path) {
    const std::wstring name = path_text(path.filename());
    bool saw_pattern = false;
    std::size_t start = 0;
    while (start <= patterns.size()) {
        std::size_t end = patterns.find_first_of(L";,", start);
        if (end == std::wstring_view::npos) end = patterns.size();
        std::wstring_view pattern = patterns.substr(start, end - start);
        while (!pattern.empty() && std::iswspace(pattern.front())) pattern.remove_prefix(1);
        while (!pattern.empty() && std::iswspace(pattern.back())) pattern.remove_suffix(1);
        if (!pattern.empty()) {
            saw_pattern = true;
            if (pattern == L"*" || pattern == L"*.*" || wildcard_match(pattern, name, true)) {
                return true;
            }
        }
        if (end == patterns.size()) break;
        start = end + 1;
    }
    return !saw_pattern;
}

// ---- excludes ------------------------------------------------------------------

namespace {

std::wstring with_forward_slashes(std::wstring_view text) {
    std::wstring result(text);
    std::replace(result.begin(), result.end(), L'\\', L'/');
    return result;
}

}  // namespace

FolderExcludes::FolderExcludes(std::wstring_view patterns) {
    std::size_t start = 0;
    while (start <= patterns.size()) {
        std::size_t end = patterns.find(L';', start);
        if (end == std::wstring_view::npos) end = patterns.size();
        std::wstring_view pattern = patterns.substr(start, end - start);
        while (!pattern.empty() && std::iswspace(pattern.front())) pattern.remove_prefix(1);
        while (!pattern.empty() && std::iswspace(pattern.back())) pattern.remove_suffix(1);
        // "node_modules\" names the folder as plainly as "node_modules".
        while (pattern.size() > 1 && (pattern.back() == L'\\' || pattern.back() == L'/')) {
            pattern.remove_suffix(1);
        }
        if (!pattern.empty()) {
            if (pattern.find_first_of(L"\\/") == std::wstring_view::npos) {
                names_.emplace_back(pattern);
            } else {
                paths_.push_back(with_forward_slashes(pattern));
            }
        }
        if (end == patterns.size()) break;
        start = end + 1;
    }
}

bool FolderExcludes::excludes(std::wstring_view name, std::wstring_view path) const {
    for (const auto& pattern : names_) {
        if (wildcard_match(pattern, name, true)) return true;
    }
    if (paths_.empty() || path.empty()) return false;
    std::wstring normalized = with_forward_slashes(path);
    while (normalized.size() > 1 && normalized.back() == L'/') normalized.pop_back();
    for (const auto& pattern : paths_) {
        if (wildcard_match(pattern, normalized, true)) return true;
    }
    return false;
}

// ---- sorting -------------------------------------------------------------------

int compare_names(std::wstring_view left, std::wstring_view right) {
    const auto digit = [](wchar_t c) { return c >= L'0' && c <= L'9'; };
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < left.size() && j < right.size()) {
        if (digit(left[i]) && digit(right[j])) {
            // Compare the runs by value: past the leading zeros, the longer run
            // is the larger number, and runs of one length go digit by digit.
            std::size_t a = i;
            std::size_t b = j;
            while (a < left.size() && left[a] == L'0') ++a;
            while (b < right.size() && right[b] == L'0') ++b;
            std::size_t a_end = a;
            std::size_t b_end = b;
            while (a_end < left.size() && digit(left[a_end])) ++a_end;
            while (b_end < right.size() && digit(right[b_end])) ++b_end;
            if (a_end - a != b_end - b) return a_end - a < b_end - b ? -1 : 1;
            for (; a < a_end; ++a, ++b) {
                if (left[a] != right[b]) return left[a] < right[b] ? -1 : 1;
            }
            i = a_end;
            j = b_end;
            continue;
        }
        const wchar_t a = fold_char(left[i]);
        const wchar_t b = fold_char(right[j]);
        if (a != b) return a < b ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < left.size()) return 1;
    if (j < right.size()) return -1;
    return 0;
}

namespace {

template <typename T>
int compare_numbers(T left, T right) {
    return left < right ? -1 : (right < left ? 1 : 0);
}

// Known values first, in order, then the unknown ones.
template <typename T>
int compare_known(bool left_known, T left, bool right_known, T right) {
    if (left_known != right_known) return left_known ? -1 : 1;
    return left_known ? compare_numbers(left, right) : 0;
}

struct MatchOrder {
    const Index& index;
    SortKey key;
    bool ascending;

    int compare_location(const Entry& a, const Entry& b) const {
        if (a.folder == b.folder) return 0;
        return compare_names(index.folder(a.folder).display, index.folder(b.folder).display);
    }

    int compare_key(const Entry& a, const Entry& b) const {
        const bool a_folder = a.kind == EntryKind::folder;
        const bool b_folder = b.kind == EntryKind::folder;
        switch (key) {
            case SortKey::name: return 0;
            case SortKey::location: return compare_location(a, b);
            case SortKey::type:
                if (a_folder != b_folder) return a_folder ? -1 : 1;
                return a_folder ? 0 : compare_names(extension_of(a.name), extension_of(b.name));
            case SortKey::size: return compare_known(!a_folder, a.size, !b_folder, b.size);
            case SortKey::packed: return compare_known(a.has_packed, a.packed, b.has_packed, b.packed);
            case SortKey::modified:
                return compare_known(a.modified != 0, a.modified, b.modified != 0, b.modified);
            case SortKey::crc32: return compare_known(a.has_crc32, a.crc32, b.has_crc32, b.crc32);
        }
        return 0;
    }

    bool operator()(std::uint32_t left, std::uint32_t right) const {
        const auto& entries = index.entries();
        const Entry& a = entries[left];
        const Entry& b = entries[right];
        int order = compare_key(a, b);
        if (order == 0) order = compare_names(a.name, b.name);
        if (order == 0 && key != SortKey::location) order = compare_location(a, b);
        if (order == 0) order = compare_numbers(left, right);
        return ascending ? order < 0 : order > 0;
    }
};

}  // namespace

void sort_matches(const Index& index, std::vector<std::uint32_t>& matches, SortKey key,
                  bool ascending) {
    std::sort(matches.begin(), matches.end(), MatchOrder{index, key, ascending});
}

void merge_matches(const Index& index, std::vector<std::uint32_t>& matches, std::size_t sorted,
                   SortKey key, bool ascending) {
    if (sorted >= matches.size()) return;
    const MatchOrder order{index, key, ascending};
    const auto middle = matches.begin() + static_cast<std::ptrdiff_t>(sorted);
    std::sort(middle, matches.end(), order);
    std::inplace_merge(matches.begin(), middle, matches.end(), order);
}

// ---- archives ------------------------------------------------------------------

void add_archive_entries(Batch& batch, const fs::path& archive,
                         const std::vector<ArchiveEntry>& entries, std::string_view directory,
                         bool recurse, const FolderExcludes* excludes) {
    const auto archive_id = static_cast<std::uint32_t>(batch.archives.size());
    batch.archives.push_back(archive);
    const std::wstring archive_display = path_text(archive) + L" :: /";
    const std::string base = normalize_archive_path(std::string(directory));
    if (excludes != nullptr && excludes->empty()) excludes = nullptr;

    std::unordered_map<std::string, std::uint32_t> folder_ids;
    const auto folder_for = [&](const std::string& parent) {
        const auto found = folder_ids.find(parent);
        if (found != folder_ids.end()) return found->second;
        Folder folder;
        folder.display = archive_display + widen_utf8(parent);
        folder.archive = archive_id;
        folder.archive_directory = parent;
        const auto id = static_cast<std::uint32_t>(batch.folders.size());
        batch.folders.push_back(std::move(folder));
        folder_ids.emplace(parent, id);
        return id;
    };

    // Whether a folder in the archive is excluded, itself or through a parent.
    std::unordered_map<std::string, bool> excluded_folders;
    const std::function<bool(const std::string&)> excluded = [&](const std::string& folder) {
        if (excludes == nullptr || folder.empty()) return false;
        if (const auto found = excluded_folders.find(folder); found != excluded_folders.end()) {
            return found->second;
        }
        const std::size_t slash = folder.find_last_of('/');
        bool result = slash != std::string::npos && excluded(folder.substr(0, slash));
        if (!result) {
            const std::wstring name = widen_utf8(slash == std::string::npos
                                                     ? std::string_view(folder)
                                                     : std::string_view(folder).substr(slash + 1));
            result = excludes->excludes(name, archive_display + widen_utf8(folder));
        }
        excluded_folders.emplace(folder, result);
        return result;
    };

    // Every path met so far and its entry (npos when it lies outside the scope),
    // so a folder the list only implies is added once.
    constexpr std::size_t kOutside = static_cast<std::size_t>(-1);
    std::unordered_map<std::string, std::size_t> seen;
    seen.reserve(entries.size() * 2);
    const auto within = [&](const std::string& path) {
        if (base.empty()) return !path.empty();
        return path.size() > base.size() && path.compare(0, base.size(), base) == 0 &&
               path[base.size()] == '/';
    };
    const auto relative_depth = [&](const std::string& path) {
        const std::size_t start = base.empty() ? 0 : base.size() + 1;
        return static_cast<std::size_t>(
            std::count(path.begin() + static_cast<std::ptrdiff_t>(start), path.end(), '/'));
    };
    const auto add = [&](const std::string& path, EntryKind kind, const ArchiveEntry* source) {
        if (!within(path) || (!recurse && relative_depth(path) != 0)) {
            seen.emplace(path, kOutside);
            return;
        }
        if (!seen.emplace(path, batch.entries.size()).second) return;
        const std::size_t slash = path.find_last_of('/');
        const std::string parent = slash == std::string::npos ? std::string{} : path.substr(0, slash);
        Entry entry;
        entry.name = widen_utf8(slash == std::string::npos ? std::string_view(path)
                                                           : std::string_view(path).substr(slash + 1));
        entry.folder = folder_for(parent);
        entry.kind = kind;
        entry.archive_path = path;
        if (source != nullptr) {
            entry.modified = source->mtime;
            if (kind != EntryKind::folder) {
                entry.size = source->size;
                entry.has_packed = source->packed_size.has_value();
                entry.packed = source->packed_size.value_or(0);
                entry.has_crc32 = source->has_crc32;
                entry.crc32 = source->crc32;
            }
        }
        batch.entries.push_back(std::move(entry));
    };
    for (const auto& source : entries) {
        const std::string path = normalize_archive_path(source.path);
        if (path.empty()) continue;
        // Parents first, so an implied folder is listed even if nothing names
        // it; an excluded one ends the walk up the path, and the entry with it.
        bool blocked = false;
        for (std::size_t slash = path.find('/'); slash != std::string::npos;
             slash = path.find('/', slash + 1)) {
            const std::string parent = path.substr(0, slash);
            if (excluded(parent)) {
                blocked = true;
                break;
            }
            if (!seen.contains(parent)) add(parent, EntryKind::folder, nullptr);
        }
        if (blocked || (source.is_directory && excluded(path))) continue;
        const EntryKind kind = source.is_directory ? EntryKind::folder
            : (source.is_symlink || source.is_hardlink) ? EntryKind::link
                                                        : EntryKind::file;
        if (const auto found = seen.find(path); found != seen.end()) {
            // An implied folder that the list also names: take the listed time.
            if (kind == EntryKind::folder && found->second != kOutside) {
                batch.entries[found->second].modified = source.mtime;
            }
            continue;
        }
        add(path, kind, &source);
    }
}

// ---- walk ----------------------------------------------------------------------

namespace {

constexpr std::size_t kBatchEntries = 4096;
constexpr auto kBatchInterval = std::chrono::milliseconds(60);

struct Task {
    fs::path path;
    bool archive = false;
};

// Whether a file's contents are wanted: a content search is on, its name and
// type pass the filter, and it is small enough.
bool wants_content(Batch& batch, const WalkOptions& options, Entry& entry,
                   std::wstring_view folder_display) {
    if (!options.content) return false;
    if (options.content_filter != nullptr &&
        !options.content_filter->matches(entry, folder_display)) {
        return false;
    }
    if (entry.size > options.content_max_size) {
        entry.content = ContentState::skipped;
        ++batch.content_skipped;
        return false;
    }
    return true;
}

void search_file_content(Batch& batch, const WalkOptions& options, const std::stop_token& stop,
                         Entry& entry, const fs::path& path) {
    if (!options.content) return;
    if (!wants_content(batch, options, entry, batch.folders[entry.folder].display)) return;
    thread_local std::vector<char> buffer(1u << 20);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        entry.content = ContentState::skipped;
        ++batch.content_skipped;
        return;
    }
    ContentScan scan(*options.content);
    while (input && !stop.stop_requested()) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = input.gcount();
        if (got <= 0) break;
        if (scan.feed({reinterpret_cast<const std::uint8_t*>(buffer.data()),
                       static_cast<std::size_t>(got)})) {
            break;
        }
    }
    entry.content = scan.found() ? ContentState::found : ContentState::absent;
    ++batch.content_read;
}

// Formats read through 7z.dll or tar: extract just the wanted entries to a
// private temporary folder, read them there, and remove it.
void read_by_extracting(const fs::path& archive, const ArchiveProvider& provider,
                        const std::unordered_map<std::string, std::size_t>& wanted,
                        std::uint64_t wanted_bytes, const ArchiveFileVisitor& visitor,
                        const std::shared_ptr<OperationControl>& operation) {
    // Bounded so one search cannot fill the disk with an archive's contents.
    constexpr std::uint64_t kMaxExtractedBytes = 1ull << 30;
    if (wanted_bytes > kMaxExtractedBytes) return;
    std::random_device random;
    const fs::path folder = fs::temp_directory_path() /
        ("axiom-find-" + std::to_string(random()) + std::to_string(random()));
    std::error_code error;
    fs::create_directories(folder, error);
    if (error) return;
    struct Remove {
        fs::path folder;
        ~Remove() {
            std::error_code ignored;
            fs::remove_all(folder, ignored);
        }
    } remove{folder};
    std::vector<std::string> paths;
    paths.reserve(wanted.size());
    for (const auto& [entry_path, k] : wanted) paths.push_back(entry_path);
    ExtractOptions options;
    options.operation = operation;
    options.overwrite = ExtractOptions::Overwrite::overwrite;
    options.mark_of_the_web = ExtractOptions::MarkOfTheWeb::off;
    options.restore_mtime = false;
    provider.extract_selected(archive, paths, folder, options);
    thread_local std::vector<char> buffer(1u << 20);
    for (const auto& entry_path : paths) {
        operation_checkpoint(operation);
        if (!visitor.select(entry_path, 0)) continue;
        std::ifstream input(folder / core::path_from_utf8(entry_path), std::ios::binary);
        if (!input) continue;  // stays unread, and is counted as skipped
        visitor.begin(entry_path);
        bool more = true;
        while (more && input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto got = input.gcount();
            if (got <= 0) break;
            more = visitor.bytes({reinterpret_cast<const std::uint8_t*>(buffer.data()),
                                  static_cast<std::size_t>(got)});
        }
        visitor.end(entry_path);
    }
}

// Search the contents of the archive's entries just added at `first`.
void search_archive_content(Batch& batch, const WalkOptions& options,
                            const std::stop_token& stop, const fs::path& path,
                            const ArchiveProvider& provider, std::size_t first) {
    std::unordered_map<std::string, std::size_t> wanted;
    std::uint64_t wanted_bytes = 0;
    for (std::size_t k = first; k < batch.entries.size(); ++k) {
        Entry& entry = batch.entries[k];
        if (entry.kind == EntryKind::folder || entry.kind == EntryKind::link) continue;
        if (!wants_content(batch, options, entry, batch.folders[entry.folder].display)) continue;
        wanted.emplace(entry.archive_path, k);
        wanted_bytes += entry.size;
    }
    if (wanted.empty()) return;

    auto operation = std::make_shared<OperationControl>();
    std::stop_callback cancel(stop, [&] { operation->request_cancel(); });
    std::optional<ContentScan> scan;
    std::size_t current = 0;
    ArchiveFileVisitor visitor;
    visitor.select = [&](const std::string& entry_path, std::uint64_t) {
        return wanted.contains(entry_path);
    };
    visitor.begin = [&](const std::string& entry_path) {
        current = wanted.at(entry_path);
        scan.emplace(*options.content);
    };
    visitor.bytes = [&](std::span<const std::uint8_t> bytes) { return !scan->feed(bytes); };
    visitor.end = [&](const std::string&) {
        Entry& entry = batch.entries[current];
        entry.content = scan->found() ? ContentState::found : ContentState::absent;
        ++batch.content_read;
    };
    const auto mark_unread = [&] {
        for (const auto& [entry_path, k] : wanted) {
            Entry& entry = batch.entries[k];
            if (entry.content == ContentState::unchecked) {
                entry.content = ContentState::skipped;
                ++batch.content_skipped;
            }
        }
    };
    try {
        if (provider.info().native) {
            read_axar_files(path, {}, visitor, operation);
        } else if (provider.info().format == ArchiveFormat::zip) {
            read_zip_files(path, visitor, operation);
        } else {
            read_by_extracting(path, provider, wanted, wanted_bytes, visitor, operation);
        }
    } catch (...) {
        // Cancelled, or the archive could not be read: what was not read is
        // counted as skipped below.
    }
    mark_unread();
}

class Walker {
public:
    Walker(const WalkOptions& options, std::stop_token stop, const std::function<void(Batch&&)>& sink)
        : options_(options), excludes_(options.exclude_folders), stop_(std::move(stop)), sink_(sink) {}

    void run(const std::vector<fs::path>& roots) {
        for (const auto& root : roots) queue_.push_back({root, false});
        unsigned threads = options_.threads;
        if (threads == 0) {
            // Directory listing scales to about eight threads on NVMe and on a
            // cached hard disk; more only adds contention.
            threads = std::clamp(std::thread::hardware_concurrency(), 2u, 8u);
        }
        std::vector<std::thread> pool;
        pool.reserve(threads);
        for (unsigned k = 0; k < threads; ++k) pool.emplace_back([this] { work(); });
        for (auto& thread : pool) thread.join();
    }

private:
    struct Local {
        Batch batch;
        std::chrono::steady_clock::time_point flushed = std::chrono::steady_clock::now();
        std::uint32_t current_folder = kNoArchive;  // id of the folder being listed
        fs::path current_path;
    };

    void flush(Local& local, bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && local.batch.entries.size() < kBatchEntries && now - local.flushed < kBatchInterval) {
            return;
        }
        if (!local.batch.entries.empty() || local.batch.read_errors != 0 ||
            local.batch.skipped_encrypted != 0) {
            sink_(std::move(local.batch));
        }
        local.batch = Batch{};
        local.flushed = now;
        local.current_folder = kNoArchive;
    }

    std::uint32_t folder_id(Local& local, const fs::path& directory) {
        if (local.current_folder == kNoArchive || local.current_path != directory) {
            Folder folder;
            folder.display = path_text(directory);
            folder.filesystem = directory;
            local.current_folder = static_cast<std::uint32_t>(local.batch.folders.size());
            local.current_path = directory;
            local.batch.folders.push_back(std::move(folder));
        }
        return local.current_folder;
    }

    void push(std::vector<Task>& found) {
        if (found.empty()) return;
        {
            std::lock_guard guard(lock_);
            for (auto& task : found) queue_.push_back(std::move(task));
        }
        found.clear();
        wake_.notify_all();
    }

    void work() {
        Local local;
        std::vector<Task> found;
        for (;;) {
            Task task;
            {
                std::unique_lock guard(lock_);
                wake_.wait(guard, stop_, [&] { return !queue_.empty() || busy_ == 0; });
                if (stop_.stop_requested() || queue_.empty()) break;
                task = std::move(queue_.back());
                queue_.pop_back();
                ++busy_;
            }
            if (task.archive) {
                list_archive(local, task.path);
            } else {
                list_directory(local, task.path, found);
            }
            push(found);
            flush(local, false);
            {
                std::lock_guard guard(lock_);
                --busy_;
            }
            wake_.notify_all();
        }
        flush(local, true);
        wake_.notify_all();
    }

    void consider_archive(const fs::path& path, std::vector<Task>& found) {
        if (!options_.search_archives || !looks_like_archive_name(path) ||
            !matches_archive_patterns(options_.archive_patterns, path)) {
            return;
        }
        found.push_back({path, true});
    }

    void list_archive(Local& local, const fs::path& path) {
        try {
            const auto* provider = archive_provider_for_path(path);
            if (provider == nullptr) return;
            ArchiveContents contents;
            try {
                // One directory read gives the entries and whether they are encrypted.
                contents = provider->open(path);
            } catch (...) {
                if (provider->capabilities(path).encrypted) {
                    ++local.batch.skipped_encrypted;
                } else {
                    ++local.batch.read_errors;
                }
                return;
            }
            if (options_.skip_encrypted && contents.capabilities.encrypted) {
                ++local.batch.skipped_encrypted;
                return;
            }
            const std::size_t first = local.batch.entries.size();
            add_archive_entries(local.batch, path, contents.entries, {}, options_.recurse,
                                &excludes_);
            local.current_folder = kNoArchive;  // archive folders were added after it
            if (options_.content) {
                search_archive_content(local.batch, options_, stop_, path, *provider, first);
            }
        } catch (...) {
            ++local.batch.read_errors;
        }
    }

    void list_directory(Local& local, const fs::path& directory, std::vector<Task>& found) {
#if defined(_WIN32)
        WIN32_FIND_DATAW data{};
        const fs::path pattern = directory / L"*";
        const HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                             FindExSearchNameMatch, nullptr,
                                             FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) ++local.batch.read_errors;
            return;
        }
        do {
            if (stop_.stop_requested()) break;
            if (data.cFileName[0] == L'.' &&
                (data.cFileName[1] == 0 || (data.cFileName[1] == L'.' && data.cFileName[2] == 0))) {
                continue;
            }
            const DWORD attributes = data.dwFileAttributes;
            const bool hidden = (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
            if (hidden && !options_.include_hidden) continue;
            const bool directory_entry = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (directory_entry && excluded(data.cFileName, directory)) continue;
            Entry entry;
            entry.name = data.cFileName;
            entry.folder = folder_id(local, directory);
            entry.hidden = hidden;
            const ULONGLONG ticks =
                (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                data.ftLastWriteTime.dwLowDateTime;
            constexpr ULONGLONG kUnixEpochTicks = 116444736000000000ull;
            entry.modified = ticks > kUnixEpochTicks
                ? static_cast<std::int64_t>((ticks - kUnixEpochTicks) / 10000000ull) : 0;
            const fs::path child = directory / data.cFileName;
            if (directory_entry) {
                entry.kind = EntryKind::folder;
                // A junction or directory link is listed but never followed.
                if (options_.recurse && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                    found.push_back({child, false});
                }
            } else {
                entry.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                entry.kind = looks_like_archive_name(child) ? EntryKind::archive : EntryKind::file;
                if (entry.kind == EntryKind::archive) consider_archive(child, found);
            }
            if (entry.kind != EntryKind::folder) {
                search_file_content(local.batch, options_, stop_, entry, child);
            }
            local.batch.entries.push_back(std::move(entry));
            if (local.batch.entries.size() >= kBatchEntries) flush(local, false);
        } while (FindNextFileW(find, &data));
        FindClose(find);
#else
        std::error_code error;
        fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, error);
        if (error) {
            ++local.batch.read_errors;
            return;
        }
        for (const fs::directory_iterator end; it != end; it.increment(error)) {
            if (error || stop_.stop_requested()) break;
            const auto& item = *it;
            const std::string name = item.path().filename().native();
            const bool hidden = !name.empty() && name.front() == '.';
            if (hidden && !options_.include_hidden) continue;
            std::error_code status_error;
            const auto status = item.symlink_status(status_error);
            if (status_error) continue;
            if (fs::is_directory(status) && excluded(widen_utf8(name), directory)) continue;
            Entry entry;
            entry.name = widen_utf8(name);
            entry.folder = folder_id(local, directory);
            entry.hidden = hidden;
            std::error_code time_error;
            const auto stamp = item.last_write_time(time_error);
            if (!time_error) entry.modified = unix_seconds(stamp);
            if (fs::is_directory(status)) {
                entry.kind = EntryKind::folder;
                if (options_.recurse) found.push_back({item.path(), false});
            } else {
                std::error_code size_error;
                entry.size = fs::is_regular_file(status) ? item.file_size(size_error) : 0;
                if (size_error) entry.size = 0;
                entry.kind = looks_like_archive_name(item.path()) ? EntryKind::archive : EntryKind::file;
                if (entry.kind == EntryKind::archive) consider_archive(item.path(), found);
            }
            if (entry.kind != EntryKind::folder) {
                search_file_content(local.batch, options_, stop_, entry, item.path());
            }
            local.batch.entries.push_back(std::move(entry));
            if (local.batch.entries.size() >= kBatchEntries) flush(local, false);
        }
#endif
    }

    // Whether a folder found in `parent` is left out, with its contents.
    bool excluded(std::wstring_view name, const fs::path& parent) const {
        if (excludes_.empty()) return false;
        std::wstring path = path_text(parent);
        if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path.push_back(L'/');
        path += name;
        return excludes_.excludes(name, path);
    }

    const WalkOptions& options_;
    const FolderExcludes excludes_;
    std::stop_token stop_;
    const std::function<void(Batch&&)>& sink_;
    std::mutex lock_;
    std::condition_variable_any wake_;
    std::vector<Task> queue_;
    std::size_t busy_ = 0;
};

}  // namespace

void walk(const std::vector<fs::path>& roots, const WalkOptions& options, std::stop_token stop,
          const std::function<void(Batch&&)>& sink) {
    Walker walker(options, std::move(stop), sink);
    walker.run(roots);
}

void search_archive_contents(Batch& batch, const fs::path& archive, std::size_t first,
                             const WalkOptions& options, std::stop_token stop) {
    if (!options.content) return;
    const auto* provider = archive_provider_for_path(archive);
    if (provider == nullptr) return;
    search_archive_content(batch, options, stop, archive, *provider, first);
}

std::optional<std::uint64_t> parse_size_text(std::wstring_view text) {
    return parse_size(text);
}

}  // namespace axiom::search
