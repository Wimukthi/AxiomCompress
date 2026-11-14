#pragma once

// File search for the Find dialog: a compiled query, a compact index of what a
// search area holds, and a walk that fills the index from folders and archives
// on several threads at once.
//
// The dialog walks once and keeps the index, so refining the query is a pass
// over memory rather than another walk of the disk.

#include "axiom/archive.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace axiom::search {

enum class EntryKind : std::uint8_t {
    file,
    folder,
    archive,  // a file whose name marks it as an archive Axiom can open
    link,     // a symbolic or hard link inside an archive
};

inline constexpr std::uint32_t kNoArchive = 0xFFFFFFFFu;

// What a content search learned about an entry.
enum class ContentState : std::uint8_t {
    unchecked,  // not read: no content search, or its name did not qualify
    found,
    absent,
    skipped,    // too large, or it could not be read
};

// Where entries live. A filesystem folder has `filesystem`; a folder inside an
// archive has `archive` and its '/'-separated `archive_directory`.
struct Folder {
    std::wstring display;  // what the Location column shows
    std::filesystem::path filesystem;
    std::uint32_t archive = kNoArchive;
    std::string archive_directory;
};

struct Entry {
    std::wstring name;
    std::uint32_t folder = 0;  // into Index::folder()
    EntryKind kind = EntryKind::file;
    bool hidden = false;
    ContentState content = ContentState::unchecked;
    // What an archive records about an entry inside it, when the format has it.
    bool has_packed = false;
    bool has_crc32 = false;
    std::uint32_t crc32 = 0;
    std::uint64_t packed = 0;   // compressed bytes
    std::uint64_t size = 0;
    std::int64_t modified = 0;  // seconds since the Unix epoch; 0 when unknown
    std::string archive_path;   // the entry's UTF-8 path inside its archive, if any
};

// What one walker thread hands over at a time. Folder and archive numbers are
// local to the batch until Index::append renumbers them.
struct Batch {
    std::vector<Folder> folders;
    std::vector<std::filesystem::path> archives;
    std::vector<Entry> entries;
    std::size_t read_errors = 0;
    std::size_t skipped_encrypted = 0;
    std::size_t content_read = 0;     // files whose contents were searched
    std::size_t content_skipped = 0;  // files too large or unreadable to search
};

class Index {
public:
    void append(Batch&& batch);

    const std::vector<Entry>& entries() const noexcept { return entries_; }
    const Folder& folder(std::uint32_t id) const { return folders_[id]; }
    const std::filesystem::path& archive(std::uint32_t id) const { return archives_[id]; }
    std::size_t read_errors() const noexcept { return read_errors_; }
    std::size_t skipped_encrypted() const noexcept { return skipped_encrypted_; }
    std::size_t content_read() const noexcept { return content_read_; }
    std::size_t content_skipped() const noexcept { return content_skipped_; }

    // The full display path of an entry: its folder's location and its name.
    std::wstring display_path(const Entry& entry) const;

private:
    std::vector<Folder> folders_;
    std::vector<std::filesystem::path> archives_;
    std::vector<Entry> entries_;
    std::size_t read_errors_ = 0;
    std::size_t skipped_encrypted_ = 0;
    std::size_t content_read_ = 0;
    std::size_t content_skipped_ = 0;
};

struct QueryOptions {
    bool match_case = false;
    bool whole_name = false;   // a plain term must be the whole name
    bool search_path = false;  // a plain term may match the location too
    bool files = true;         // files and links
    bool folders = true;
    bool archives = true;
};

// The query language of the file list's instant filter, compiled once:
// whitespace-separated terms that must all match, "quoted phrases", a leading
// '-' to exclude, '*' and '?' wildcards, and the fields name:, ext:, path:,
// size: and date: (or modified:), which take <, <=, >, >= or =, and type: (or
// kind:) with file, folder, archive or link. Match case and Whole name apply to
// plain terms and to name: and path:.
class Query {
public:
    // nullopt, with `error` set, when a field's value cannot be read.
    static std::optional<Query> compile(std::wstring_view text, const QueryOptions& options,
                                        std::wstring* error = nullptr);

    bool matches(const Index& index, const Entry& entry) const;
    // The same test for an entry not yet in an index, given its folder's text.
    bool matches(const Entry& entry, std::wstring_view folder_display) const;

private:
    enum class Field : std::uint8_t { plain, name, ext, path, size, date, kind };
    enum class Compare : std::uint8_t { equal, less, less_equal, greater, greater_equal };
    struct Term {
        Field field = Field::plain;
        bool negate = false;
        bool wildcard = false;
        std::wstring text;  // folded unless matching case
        Compare compare = Compare::equal;
        std::uint64_t number = 0;  // a size in bytes, or a date as yyyymmdd
        EntryKind kind = EntryKind::file;
    };

    bool matches_with(const Entry& entry, const std::function<std::wstring()>& path) const;
    bool term_matches(const Term& term, const Entry& entry,
                      const std::function<std::wstring()>& path) const;
    bool text_matches(const Term& term, std::wstring_view value, bool whole) const;

    std::vector<Term> terms_;
    QueryOptions options_;
    bool needs_date_ = false;
};

// Text to find inside files, in the two encodings text files on Windows use:
// UTF-8 (which covers ASCII) and UTF-16LE. Without match_case, letters fold:
// all of them in UTF-16, and the ASCII ones in UTF-8, where other letters must
// match as written.
class ContentPattern {
public:
    static std::optional<ContentPattern> compile(std::wstring_view text, bool match_case);

    // Bytes a scan keeps between reads so a match across them is still seen.
    std::size_t overlap() const noexcept { return overlap_; }
    bool find(const std::uint8_t* data, std::size_t size) const;

private:
    bool find_utf8(const std::uint8_t* data, std::size_t size) const;
    bool find_utf16(const std::uint8_t* data, std::size_t size, std::size_t parity) const;

    bool match_case_ = false;
    std::vector<std::uint8_t> utf8_;
    std::array<std::size_t, 256> utf8_shift_{};
    std::vector<std::uint16_t> utf16_;
    std::array<std::size_t, 256> utf16_shift_{};
    std::size_t overlap_ = 0;
};

// One file's contents, fed in pieces.
class ContentScan {
public:
    explicit ContentScan(const ContentPattern& pattern) : pattern_(pattern) {}
    // True once the text has been seen; later calls do nothing.
    bool feed(std::span<const std::uint8_t> bytes);
    bool found() const noexcept { return found_; }

private:
    const ContentPattern& pattern_;
    std::vector<std::uint8_t> carry_;
    std::vector<std::uint8_t> seam_;
    bool found_ = false;
};

// Folders a search leaves out, with everything beneath them: ';'-separated
// wildcards matched against a folder's name or, for a pattern that holds a path
// separator, against its whole path ('\' and '/' alike). Letters match in
// either case.
class FolderExcludes {
public:
    explicit FolderExcludes(std::wstring_view patterns = {});
    bool empty() const noexcept { return names_.empty() && paths_.empty(); }
    bool excludes(std::wstring_view name, std::wstring_view path) const;

private:
    std::vector<std::wstring> names_;
    std::vector<std::wstring> paths_;  // with '/' for every separator
};

struct WalkOptions {
    bool recurse = true;
    std::wstring exclude_folders;  // FolderExcludes patterns
    bool include_hidden = true;    // hidden and system files and folders
    bool search_archives = false;  // list the contents of archives found on the way
    std::wstring archive_patterns; // ';'-separated wildcards; empty means every archive
    bool skip_encrypted = true;
    unsigned threads = 0;          // 0 picks a count for the machine

    // Search the contents of each file, in folders and inside archives, that
    // `content_filter` accepts (by name and type) and that is no larger than
    // content_max_size; the result lands in Entry::content.
    std::optional<ContentPattern> content;
    const Query* content_filter = nullptr;
    std::uint64_t content_max_size = 64ull << 20;
};

// Visit everything beneath each root folder, not the roots themselves, on a pool
// of threads, handing over batches as they fill (from those threads: `sink` must
// be safe to call concurrently, and may run until walk() returns). A folder that
// cannot be read is counted and skipped. A link or junction is listed but never
// followed. Returns early, with whatever was found, once `stop` is requested.
void walk(const std::vector<std::filesystem::path>& roots, const WalkOptions& options,
          std::stop_token stop, const std::function<void(Batch&&)>& sink);

// Search the contents of an archive's entries that are already in `batch`, from
// `first` on, as walk() does for archives it meets; for an archive that is open
// in the browser, whose entries did not come from a walk.
void search_archive_contents(Batch& batch, const std::filesystem::path& archive,
                             std::size_t first, const WalkOptions& options,
                             std::stop_token stop);

// A size such as "64 MB", "1.5G" or "512" in bytes, as size: reads it.
std::optional<std::uint64_t> parse_size_text(std::wstring_view text);

// Add an archive's already-read entries beneath `directory` ("" for its root):
// direct children only unless `recurse`. Folders the entry list only implies are
// added as folders, as the archive browser shows them. Folders `excludes`
// names are left out with their contents.
void add_archive_entries(Batch& batch, const std::filesystem::path& archive,
                         const std::vector<ArchiveEntry>& entries, std::string_view directory,
                         bool recurse, const FolderExcludes* excludes = nullptr);

// The orders the Find results can be listed in.
enum class SortKey : std::uint8_t { name, location, type, size, packed, modified, crc32 };

// Names as Explorer orders them: letters in either case, and runs of digits by
// their value, so "file2" comes before "file10".
int compare_names(std::wstring_view left, std::wstring_view right);

// Put `matches`, indexes into index.entries(), in order by `key`. Ties go by
// name, then location. Type orders folders first, then by extension. Items
// without a packed size or CRC-32 come after those with one.
void sort_matches(const Index& index, std::vector<std::uint32_t>& matches, SortKey key,
                  bool ascending);
// The same when the first `sorted` matches are already in that order and the
// rest were appended since: sorts only those and merges them in.
void merge_matches(const Index& index, std::vector<std::uint32_t>& matches, std::size_t sorted,
                   SortKey key, bool ascending);

// Whether a file name marks an archive the browser opens: .axar, .zip and its
// relatives, .7z, .rar and RAR volumes, the tar family, .iso and .cab.
bool looks_like_archive_name(const std::filesystem::path& name);

// The Find dialog's "Archive types" list: ';' or ','-separated wildcards
// matched against the file name; an empty list matches every archive.
bool matches_archive_patterns(std::wstring_view patterns, const std::filesystem::path& path);

}  // namespace axiom::search
