#pragma once

// File search for the Find dialog: a compiled query, a compact index of what a
// search area holds, and a walk that fills the index from folders and archives
// on several threads at once.
//
// The dialog walks once and keeps the index, so refining the query is a pass
// over memory rather than another walk of the disk.

#include "axiom/archive.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
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
};

class Index {
public:
    void append(Batch&& batch);

    const std::vector<Entry>& entries() const noexcept { return entries_; }
    const Folder& folder(std::uint32_t id) const { return folders_[id]; }
    const std::filesystem::path& archive(std::uint32_t id) const { return archives_[id]; }
    std::size_t read_errors() const noexcept { return read_errors_; }
    std::size_t skipped_encrypted() const noexcept { return skipped_encrypted_; }

    // The full display path of an entry: its folder's location and its name.
    std::wstring display_path(const Entry& entry) const;

private:
    std::vector<Folder> folders_;
    std::vector<std::filesystem::path> archives_;
    std::vector<Entry> entries_;
    std::size_t read_errors_ = 0;
    std::size_t skipped_encrypted_ = 0;
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

    bool term_matches(const Term& term, const Index& index, const Entry& entry) const;
    bool text_matches(const Term& term, std::wstring_view value, bool whole) const;

    std::vector<Term> terms_;
    QueryOptions options_;
    bool needs_date_ = false;
};

struct WalkOptions {
    bool recurse = true;
    bool include_hidden = true;    // hidden and system files and folders
    bool search_archives = false;  // list the contents of archives found on the way
    std::wstring archive_patterns; // ';'-separated wildcards; empty means every archive
    bool skip_encrypted = true;
    unsigned threads = 0;          // 0 picks a count for the machine
};

// Visit everything beneath each root folder, not the roots themselves, on a pool
// of threads, handing over batches as they fill (from those threads: `sink` must
// be safe to call concurrently, and may run until walk() returns). A folder that
// cannot be read is counted and skipped. A link or junction is listed but never
// followed. Returns early, with whatever was found, once `stop` is requested.
void walk(const std::vector<std::filesystem::path>& roots, const WalkOptions& options,
          std::stop_token stop, const std::function<void(Batch&&)>& sink);

// Add an archive's already-read entries beneath `directory` ("" for its root):
// direct children only unless `recurse`. Folders the entry list only implies are
// added as folders, as the archive browser shows them.
void add_archive_entries(Batch& batch, const std::filesystem::path& archive,
                         const std::vector<ArchiveEntry>& entries, std::string_view directory,
                         bool recurse);

// Whether a file name marks an archive the browser opens: .axar, .zip and its
// relatives, .7z, .rar and RAR volumes, the tar family, .iso and .cab.
bool looks_like_archive_name(const std::filesystem::path& name);

// The Find dialog's "Archive types" list: ';' or ','-separated wildcards
// matched against the file name; an empty list matches every archive.
bool matches_archive_patterns(std::wstring_view patterns, const std::filesystem::path& path);

}  // namespace axiom::search
