#pragma once

#include "axiom/archive.hpp"
#include "gui/archive_feature_options.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace axiom::gui {

enum class ArchiveUpdatePlanAction {
    add,
    replace,
    remove,
    unchanged,
    ignored,
    conflict,
};

struct ArchiveUpdatePlanItem {
    ArchiveUpdatePlanAction action = ArchiveUpdatePlanAction::unchanged;
    std::string archive_path;
    std::filesystem::path source_path;
    std::wstring reason;
    bool source_directory = false;
    bool source_symlink = false;
    bool archive_directory = false;
    bool archive_symlink = false;
    std::uint64_t source_size = 0;
    std::uint64_t archive_size = 0;
    std::int64_t source_mtime = 0;
    std::int64_t archive_mtime = 0;

    bool operator==(const ArchiveUpdatePlanItem&) const = default;
};

struct ArchiveUpdatePlan {
    ArchiveUpdateMode mode = ArchiveUpdateMode::update_newer;
    ArchiveFormat format = ArchiveFormat::axar;
    std::filesystem::path archive_path;
    std::vector<ArchiveUpdatePlanItem> items;
    std::vector<std::wstring> additional_effects;
    std::wstring notice;
    std::size_t added = 0;
    std::size_t replaced = 0;
    std::size_t removed = 0;
    std::size_t unchanged = 0;
    std::size_t ignored = 0;
    std::size_t conflicts = 0;
    std::uint64_t added_bytes = 0;
    std::uint64_t replacement_source_bytes = 0;
    std::uint64_t replacement_archive_bytes = 0;
    std::uint64_t removed_bytes = 0;

    bool has_changes() const {
        return added != 0 || replaced != 0 || removed != 0;
    }

    bool can_apply() const {
        return conflicts == 0 && (has_changes() || !additional_effects.empty());
    }
};

// Scans the mapped source exactly as the archive providers do, then creates a
// deterministic, read-only comparison. The result is safe to compare with a
// second scan immediately before mutation.
ArchiveUpdatePlan build_archive_update_plan(
    const std::vector<ArchiveInput>& inputs,
    const std::vector<ArchiveEntry>& existing,
    ArchiveUpdateMode mode,
    ArchiveFormat format,
    std::filesystem::path archive_path = {});

bool equivalent_archive_update_plans(const ArchiveUpdatePlan& left,
                                     const ArchiveUpdatePlan& right);

}  // namespace axiom::gui
