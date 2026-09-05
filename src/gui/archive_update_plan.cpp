#include "gui/archive_update_plan.hpp"

#include "archive/container_internal.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace axiom::gui {
namespace {

enum class EntryClass {
    file,
    directory,
    symlink,
};

struct SourceItem {
    axiom::ScanItem scanned;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
};

EntryClass entry_class(const axiom::ScanItem& item) {
    if (item.is_directory) return EntryClass::directory;
    if (item.is_symlink) return EntryClass::symlink;
    return EntryClass::file;
}

EntryClass entry_class(const ArchiveEntry& entry) {
    if (entry.is_directory) return EntryClass::directory;
    if (entry.is_symlink) return EntryClass::symlink;
    return EntryClass::file;
}

bool same_axar_entry_class(const axiom::ScanItem& source,
                           const ArchiveEntry& archived) {
    // Archived hard links are file-like and are intentionally represented as
    // ordinary files by a new filesystem scan.
    return entry_class(source) == entry_class(archived);
}

void mark_conflict(std::unordered_map<std::string, std::wstring>& conflicts,
                   std::string_view path, std::wstring reason) {
    auto [found, inserted] = conflicts.emplace(std::string(path), reason);
    if (!inserted && found->second.find(reason) == std::wstring::npos) {
        found->second += L"; ";
        found->second += reason;
    }
}

SourceItem inspect_source_item(axiom::ScanItem item) {
    SourceItem result;
    result.scanned = std::move(item);
    if (result.scanned.is_directory || result.scanned.is_symlink ||
        result.scanned.is_reparse_point) {
        return result;
    }

    std::error_code error;
    const auto size = std::filesystem::file_size(result.scanned.absolute, error);
    if (error) {
        throw std::filesystem::filesystem_error(
            "cannot determine source file size", result.scanned.absolute, error);
    }
    result.size = static_cast<std::uint64_t>(size);

    error.clear();
    const auto stamp = std::filesystem::last_write_time(result.scanned.absolute, error);
    if (!error) {
        try {
            result.mtime = axiom::to_unix_seconds(stamp);
        } catch (...) {
            result.mtime = 0;
        }
    }
    return result;
}

void count_item(ArchiveUpdatePlan& plan, const ArchiveUpdatePlanItem& item) {
    switch (item.action) {
        case ArchiveUpdatePlanAction::add:
            ++plan.added;
            plan.added_bytes += item.source_size;
            break;
        case ArchiveUpdatePlanAction::replace:
            ++plan.replaced;
            plan.replacement_source_bytes += item.source_size;
            plan.replacement_archive_bytes += item.archive_size;
            break;
        case ArchiveUpdatePlanAction::remove:
            ++plan.removed;
            plan.removed_bytes += item.archive_size;
            break;
        case ArchiveUpdatePlanAction::unchanged:
            ++plan.unchanged;
            break;
        case ArchiveUpdatePlanAction::ignored:
            ++plan.ignored;
            break;
        case ArchiveUpdatePlanAction::conflict:
            ++plan.conflicts;
            break;
    }
}

}  // namespace

ArchiveUpdatePlan build_archive_update_plan(
    const std::vector<ArchiveInput>& inputs,
    const std::vector<ArchiveEntry>& existing,
    ArchiveUpdateMode mode,
    ArchiveFormat format,
    std::filesystem::path archive_path) {
    ArchiveUpdatePlan plan;
    plan.mode = mode;
    plan.format = format;
    plan.archive_path = std::move(archive_path);

    std::vector<axiom::ScanItem> scanned;
    for (const auto& input : inputs) {
        axiom::scan_input_at(input, scanned, nullptr);
    }

    std::vector<SourceItem> source;
    source.reserve(scanned.size());
    for (auto& item : scanned) {
        source.push_back(inspect_source_item(std::move(item)));
    }

    std::unordered_map<std::string, std::vector<std::size_t>> incoming;
    incoming.reserve(source.size());
    for (std::size_t index = 0; index < source.size(); ++index) {
        incoming[source[index].scanned.archive_path].push_back(index);
    }

    std::unordered_map<std::string, const ArchiveEntry*> archived;
    archived.reserve(existing.size());
    for (const auto& entry : existing) {
        archived.emplace(entry.path, &entry);
    }

    std::unordered_map<std::string, std::wstring> conflicts;
    conflicts.reserve(source.size());
    for (const auto& [path, indices] : incoming) {
        if (indices.size() > 1) {
            mark_conflict(conflicts, path,
                          L"Multiple source items map to this archive destination");
        }
    }

    for (const auto& [path, indices] : incoming) {
        const auto& item = source[indices.front()].scanned;
        if (format == ArchiveFormat::zip && item.is_symlink) {
            mark_conflict(conflicts, path,
                          L"ZIP updates do not support symbolic links");
        }
        if (format == ArchiveFormat::axar) {
            if (const auto old = archived.find(path);
                old != archived.end() &&
                !same_axar_entry_class(item, *old->second)) {
                mark_conflict(conflicts, path,
                              L"Source and archive entry types differ");
            }
        }
        if (!item.is_directory) {
            for (const auto& [other, ignored] : incoming) {
                (void)ignored;
                if (other != path && axiom::is_same_or_child(other, path)) {
                    mark_conflict(conflicts, path,
                                  L"A non-directory destination has children");
                    break;
                }
            }
        }
    }

    for (const auto& [path, indices] : incoming) {
        const auto& item = source[indices.front()].scanned;
        for (const auto& old : existing) {
            if (old.path == path) continue;
            if (!old.is_directory && axiom::is_same_or_child(path, old.path)) {
                if (format == ArchiveFormat::axar ||
                    incoming.find(old.path) == incoming.end()) {
                    mark_conflict(conflicts, path,
                                  L"Destination is beneath an archived file");
                }
            }
            if (!item.is_directory && axiom::is_same_or_child(old.path, path)) {
                mark_conflict(conflicts, path,
                              L"A non-directory destination has archived children");
            }
        }
    }

    plan.items.reserve(source.size() +
                       (mode == ArchiveUpdateMode::synchronize
                            ? existing.size() : 0));
    std::unordered_set<std::string> wanted;
    wanted.reserve(source.size());
    for (const auto& item : source) {
        const auto& scanned_item = item.scanned;
        wanted.insert(scanned_item.archive_path);
        ArchiveUpdatePlanItem row;
        row.archive_path = scanned_item.archive_path;
        row.source_path = scanned_item.absolute;
        row.source_directory = scanned_item.is_directory;
        row.source_symlink = scanned_item.is_symlink;
        row.source_size = item.size;
        row.source_mtime = item.mtime;

        const auto old = archived.find(scanned_item.archive_path);
        const bool in_archive = old != archived.end();
        if (in_archive) {
            row.archive_directory = old->second->is_directory;
            row.archive_symlink = old->second->is_symlink;
            row.archive_size = old->second->size;
            row.archive_mtime = old->second->mtime;
        }

        if (const auto conflict = conflicts.find(scanned_item.archive_path);
            conflict != conflicts.end()) {
            row.action = ArchiveUpdatePlanAction::conflict;
            row.reason = conflict->second;
            plan.items.push_back(std::move(row));
            continue;
        }

        if (mode == ArchiveUpdateMode::add_or_replace) {
            row.action = in_archive ? ArchiveUpdatePlanAction::replace
                                    : ArchiveUpdatePlanAction::add;
            row.reason = in_archive ? L"Replace the existing entry"
                                    : L"New archive entry";
        } else if (scanned_item.is_directory || scanned_item.is_symlink) {
            if (in_archive) {
                row.action = ArchiveUpdatePlanAction::unchanged;
                row.reason = scanned_item.is_directory
                    ? L"Directory already exists"
                    : L"Link already exists";
            } else if (mode == ArchiveUpdateMode::fresh_existing) {
                row.action = ArchiveUpdatePlanAction::ignored;
                row.reason = L"Freshen does not add missing entries";
            } else {
                row.action = ArchiveUpdatePlanAction::add;
                row.reason = L"Missing from archive";
            }
        } else if (!in_archive) {
            if (mode == ArchiveUpdateMode::fresh_existing) {
                row.action = ArchiveUpdatePlanAction::ignored;
                row.reason = L"Freshen does not add missing files";
            } else {
                row.action = ArchiveUpdatePlanAction::add;
                row.reason = L"Missing from archive";
            }
        } else if (item.mtime > old->second->mtime) {
            row.action = ArchiveUpdatePlanAction::replace;
            row.reason = L"Source is newer than the archived file";
        } else {
            row.action = ArchiveUpdatePlanAction::unchanged;
            row.reason = item.mtime == old->second->mtime
                ? L"Modification time matches the archive"
                : L"Source is not newer than the archived file";
        }
        plan.items.push_back(std::move(row));
    }

    if (mode == ArchiveUpdateMode::synchronize) {
        for (const auto& old : existing) {
            if (wanted.find(old.path) != wanted.end()) continue;
            ArchiveUpdatePlanItem row;
            row.action = ArchiveUpdatePlanAction::remove;
            row.archive_path = old.path;
            row.archive_directory = old.is_directory;
            row.archive_symlink = old.is_symlink;
            row.archive_size = old.size;
            row.archive_mtime = old.mtime;
            row.reason = L"Missing from the complete source";
            plan.items.push_back(std::move(row));
        }
    }

    std::sort(plan.items.begin(), plan.items.end(), [](const auto& left,
                                                       const auto& right) {
        if (left.archive_path != right.archive_path) {
            return left.archive_path < right.archive_path;
        }
        if (left.source_path != right.source_path) {
            return left.source_path.native() < right.source_path.native();
        }
        return static_cast<int>(left.action) < static_cast<int>(right.action);
    });
    for (const auto& item : plan.items) count_item(plan, item);
    return plan;
}

bool equivalent_archive_update_plans(const ArchiveUpdatePlan& left,
                                     const ArchiveUpdatePlan& right) {
    return left.mode == right.mode && left.format == right.format &&
           left.archive_path == right.archive_path && left.items == right.items;
}

}  // namespace axiom::gui
