# Axiom GUI Handoff

This document is the portable starting point for continuing the Windows GUI in
a fresh Codex session. Read it together with `README.md`, `ARCHITECTURE.md`, and
`FORMAT.md` before editing code.

## Git Checkpoint

- Branch: `gui/file-browser`
- Completed native GUI checkpoint: `8749014`
- Backend merged from `main` through `58a50db`; inspect the current tip with
  `git log --oneline --decorate -8` because subsequent integration commits may
  advance both branches.

On another PC, a separate worktree is optional. If only the GUI is being worked
on there, clone the repository and check out `gui/file-browser` normally.

```powershell
git clone <repository-url> AxiomCompress
Set-Location AxiomCompress
git switch gui/file-browser
git status
```

Do not copy a live `.git` directory through OneDrive while Git is active on two
PCs. Prefer a Git remote, commit on one PC, push, and pull on the other.

## Product Constraints

- Windows-only frontend written in Visual C++ and native Win32.
- Do not introduce MFC, WinForms, WPF, Qt, Electron, or another UI framework.
- WinRAR and 7-Zip are interaction references, but Axiom keeps its own visual
  identity and archive architecture.
- Dark mode and per-monitor DPI behavior are foundation requirements, not later
  polish. New controls must be checked in dark and light themes at 100%, 150%,
  and 200% scaling.
- Keep compression work outside the UI thread. Preserve cooperative pause and
  cancellation through `axiom::OperationControl`.
- The archive format is still evolving on `main`. Use public archive APIs and
  capability flags instead of binding the GUI to container internals.

## Current GUI Architecture

- `src/gui/main.cpp`: narrow `wWinMain` entry point and initial-path parsing.
- `src/gui/main_window.cpp`: application shell, browser presentation, command
  routing, navigation, drag/drop, and operation orchestration.
- `src/gui/browser_model.*`: filesystem/archive locations, browser snapshots,
  navigation history, archive catalog, and future archive capability flags.
- `src/gui/custom_menu.*`: owner-drawn NativePad-style menu bar, popup menus,
  context menus, keyboard navigation, theme handling, and DPI scaling.
- `src/gui/toolbar_icons.*`: DPI-aware Fluent icon rasterization and cache.
  `toolbar_icon_masks.inc` is generated from the pinned MIT-licensed SVG assets
  under `assets/icons/fluent`.
- `src/gui/archive_dialogs.*`: native dark create, extract, and settings dialogs.
- `src/gui/archive_feature_dialogs.*`: capability-gated compression, update,
  security, comment, lock, recovery, volume, signature, and SFX option surfaces.
- `src/gui/message_dialog.*`, `about_dialog.*`, `update_checker.*`: DPI-aware
  native custom messages/about and the NativePad-style update flow.
- `src/gui/operation_runner.*`: worker lifetime, progress message delivery,
  pause/cancel control, and completion/error results.
- `src/gui/operation_progress_window.*`: separate modeless progress window for
  create, extract, and test operations. It shows stage, path, bytes/items,
  percentage, speed, ETA, output size, and live compression ratio, with
  Pause/Resume and Cancel controls.
- `src/gui/directory_watcher.*`: filesystem refresh through
  `ReadDirectoryChangesW`.
- `src/gui/settings_store.*`: per-user settings under
  `HKCU\Software\AxiomCompress\GUI`.

The main window currently provides drive/directory navigation, editable address
bar, back/forward/up/refresh, shell icons, multi-selection, sortable and
resizable columns, archive hierarchy browsing, incoming shell drops, owner-drawn
menus/context menus, the icon toolbar, custom dialogs, and create/open/test/extract
plus add/update/fresh/sync/delete/repack/comment/lock workflows.

The backend now exposes the APIs needed for full file-manager drag/drop:

- `ArchiveInput` + path-aware `add_to_archive` maps dropped filesystem objects to
  the current archive directory.
- `extract_entries` extracts only selected files/directories; a directory includes
  its subtree and an isolated hardlink is materialized safely.
- `ArchiveMove` + `move_archive_entries` renames/moves archive subtrees without
  recompressing block data and rewrites hard-link targets.
- `archive_encryption_mode` distinguishes plaintext, editable data-only encryption,
  and currently read-only encrypted-directory archives.

The next GUI slice is an OLE `IDropTarget`/`IDataObject` layer over those APIs.
Incoming `WM_DROPFILES` still routes non-archive files to create-archive. Outbound
drag should selectively extract to managed temporary storage before publishing
`CF_HDROP`; cleanup must not race Explorer's asynchronous consumption. Also wire
filename encryption and password-authenticated block-only archive editing. Recovery
UI remains disabled: the Reed–Solomon core exists, but recovery records do not.

## Build And Verification

The checked-in Visual Studio projects currently target `v145`, C++20, and x64.

```powershell
.\tools\build_msvc.ps1 -Configuration Release
.\tools\test_msvc.ps1 -Configuration Release
```

Expected GUI executable:

```text
out\Release\Axiom.exe
```

Before making changes in a new session:

1. Run `git status --short --branch` and inspect recent commits.
2. Build and run the Release tests once to establish that machine's baseline.
3. Launch `out\Release\Axiom.exe` and inspect both themes and at least two
   display scale factors before changing layout or painting code.
4. Re-run the build and tests after each coherent change.

## Integration Rules

- Treat new archive capabilities from `main` as API integration work. Inspect
  the current public headers before changing GUI assumptions.
- Rebase or merge deliberately; do not overwrite the GUI branch with the main
  worktree or copy source trees over each other.
- Keep GUI-only commits on `gui/file-browser` until an integration checkpoint is
  ready. The branch can later be merged into `main` normally.
- Preserve command IDs as the shared routing layer for menus, toolbar buttons,
  keyboard shortcuts, and context menus.
- Keep non-obvious Win32 behavior documented with concise comments, especially
  owner drawing, DPI transitions, worker/UI message ownership, and COM shell
  operations.

## Next Session Startup Prompt

Use this prompt after opening the cloned repository in Codex:

```text
Continue the Axiom native Win32 GUI on branch gui/file-browser. Read README.md,
ARCHITECTURE.md, FORMAT.md, and GUI_HANDOFF.md, then inspect git status and the
recent GUI commits. Build and test Release before editing. Preserve the native
Visual C++/Win32-only architecture, flawless dark mode, per-monitor DPI support,
the shared command routing, and worker-thread pause/cancel behavior. Do not alter
the compression format unless I explicitly ask. First report the current GUI
state and any drift from this handoff, then continue the requested GUI task.
```
