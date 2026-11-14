#include "archive/file_search.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <ctime>
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

wchar_t fold_char(wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); }

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
    const std::wstring suffix = folded(std::wstring_view(end));
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
}

std::wstring Index::display_path(const Entry& entry) const {
    const Folder& location = folders_[entry.folder];
    if (location.display.empty()) return entry.name;
    std::wstring result = location.display;
    const bool in_archive = location.archive != kNoArchive;
    const wchar_t separator = in_archive ? L'/' : L'\\';
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

bool Query::term_matches(const Term& term, const Index& index, const Entry& entry) const {
    switch (term.field) {
        case Field::plain:
            if (options_.whole_name) return text_matches(term, entry.name, true);
            if (text_matches(term, entry.name, false)) return true;
            return options_.search_path &&
                   text_matches(term, index.display_path(entry), false);
        case Field::name:
            return text_matches(term, entry.name, options_.whole_name);
        case Field::ext:
            return entry.kind != EntryKind::folder &&
                   text_matches(term, extension_of(entry.name), true);
        case Field::path:
            return text_matches(term, index.display_path(entry), false);
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
        if (term_matches(term, index, entry) == term.negate) return false;
    }
    return true;
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

// ---- archives ------------------------------------------------------------------

void add_archive_entries(Batch& batch, const fs::path& archive,
                         const std::vector<ArchiveEntry>& entries, std::string_view directory,
                         bool recurse) {
    const auto archive_id = static_cast<std::uint32_t>(batch.archives.size());
    batch.archives.push_back(archive);
    const std::wstring archive_display = path_text(archive) + L" :: /";
    const std::string base = normalize_archive_path(std::string(directory));

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
    const auto add = [&](const std::string& path, EntryKind kind, std::uint64_t size,
                         std::int64_t modified) {
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
        entry.size = kind == EntryKind::folder ? 0 : size;
        entry.modified = modified;
        entry.archive_path = path;
        batch.entries.push_back(std::move(entry));
    };
    for (const auto& source : entries) {
        const std::string path = normalize_archive_path(source.path);
        if (path.empty()) continue;
        // Parents first, so an implied folder is listed even if nothing names it.
        for (std::size_t slash = path.find('/'); slash != std::string::npos;
             slash = path.find('/', slash + 1)) {
            const std::string parent = path.substr(0, slash);
            if (!seen.contains(parent)) add(parent, EntryKind::folder, 0, 0);
        }
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
        add(path, kind, source.size, source.mtime);
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

class Walker {
public:
    Walker(const WalkOptions& options, std::stop_token stop, const std::function<void(Batch&&)>& sink)
        : options_(options), stop_(std::move(stop)), sink_(sink) {}

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
            add_archive_entries(local.batch, path, contents.entries, {}, options_.recurse);
            local.current_folder = kNoArchive;  // archive folders were added after it
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
            local.batch.entries.push_back(std::move(entry));
            if (local.batch.entries.size() >= kBatchEntries) flush(local, false);
        }
#endif
    }

    const WalkOptions& options_;
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

}  // namespace axiom::search
