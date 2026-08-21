# Changelog

Everything worth knowing about each AxiomCompress release, newest first.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Version numbers are four parts, `major.minor.patch.build`, explained in
[docs/VERSIONING.md](docs/VERSIONING.md).

Entries are condensed from the
[GitHub releases](https://github.com/Wimukthi/AxiomCompress/releases).

> **The archive format is still pre-release** and can change between minor
> versions. Where a release changes what Axiom writes, the entry says so.

## [Unreleased]

## [0.10.2.0] - 2026-08-21

Progress stability and file-selection reliability release.

### Fixed

- Made **Add to archive** accept folders as well as files from every entry
  point. When no live filesystem selection is available, it now offers native
  **Folder...** and **Files...** choices; folders are added recursively and the
  file picker retains multi-selection. Vanished selections and a deleted open
  archive are rejected before an operation starts.
- Stopped the per-file progress presentation changing at stage boundaries.
  Operation stages do not all report per-file byte totals, so the old caption
  disappeared from transient snapshots even though the corresponding bar
  intentionally remained visible.
- Made synchronization plan and compression-result rows independently sticky.
  A plan can arrive before compression statistics; the old count-based layout
  populated the plan's hidden second row and revealed it only after another row
  appeared.
- Made benchmark report text use the palette's guaranteed foreground color and
  refresh its brushes when the application, Windows, or high-contrast theme
  changes.

### Changed

- Removed the redundant numeric captions beneath the overall and per-file
  progress bars, along with the **Details** button and its elapsed,
  checkpoint-age, and archive-read diagnostic labels.
- Added native horizontal scrolling to the benchmark report alongside its
  vertical scrollbar, including mouse-wheel, keyboard, page, and thumb-track
  navigation.

## [0.10.1.0] - 2026-08-21

Native UI clarity and window-persistence release.

### Fixed

- Main-window and dialog dimensions now survive display changes. Centered child
  windows retain their saved size and maximized state, with dialog dimensions
  DPI-scaled and kept inside the active monitor.

### Changed

- Expanded native, task-oriented tooltips across non-obvious archive, settings,
  progress, color, column, and update controls. Editable combo boxes now retain
  their tooltip over the embedded text field, including when controls are
  disabled, resized for DPI, or restyled for a theme change.

## [0.10.0.0] - 2026-08-21

AXAR deduplication GUI and tooltip consistency release.

### Added

- Exposed live AXAR deduplication in the Windows Add to archive dialog, including
  validated minimum, average, and maximum content-defined chunk sizes. The
  normal Information dialog now reports live-deduplication and snapshot state.

### Changed

- Unified Windows tooltips behind the XactCopy Native tooltip-manager pattern.
  Tooltips now follow dialog DPI and theme changes, cover labels reliably, and
  remain available on disabled controls so unavailable options can explain why.

## [0.9.2.3] - 2026-08-21

Live content deduplication and progress reporting release.

### Added

- Added an opt-in AXAR v5 live-deduplication profile (`axiomc a --dedup`) with
  persisted content-defined chunk geometry, keyed identities for encrypted
  archives, mutable add/update/sync/delete/move workflows, and repack garbage
  collection. Snapshot repositories now share the same versioned chunk engine.
- Coalesced exact whole-file duplicates during an ordinary archive's initial
  create pass instead of waiting for a later add or repack.

### Changed

- Simplified the operation progress window. It now shows six lines and two
  bars instead of eighteen labelled readouts, and keeps elapsed time, time
  since the last update, and archive bytes read behind a **Details** button.
  The add/update/remove/unchanged plan counters get their own row rather than
  borrowing the compressed-size and ratio fields.

### Fixed

- Showed live progress while dragged archive entries are copied from Axiom's
  staging directory to the drop target. The copy is driven by shell callbacks
  on Axiom's own UI thread, so the progress window it owned could neither
  repaint nor accept a Cancel click, and a large drag looked like it had hung.
  That window now runs on its own UI thread, so progress advances throughout
  and Cancel works during the copy.
- Stopped reporting a percentage and byte counts that measured different things
  during a multi-phase operation. The bar and percentage span the whole
  operation; the counters and the time remaining are labelled as belonging to
  the current stage.
- Gave the command line the same throughput measurement the window uses. It was
  dividing completed bytes by total elapsed time, which lagged any speed change
  for the rest of the run and reported 0 B/s while the first solid block was
  still being assembled.
- Replaced a sub-second time remaining that displayed as "0s left" with
  "finishing", and stopped animating an indeterminate per-file bar when the
  backend reports no per-item size.
- Stopped the per-file progress bar blinking out during an operation. Whether a
  progress report carries a per-item size is not stable across snapshots, so
  the bar is now shown or hidden once per operation instead of being decided
  frame by frame.
- Matched the two progress bars in height, and removed the band of empty dialog
  above the buttons during operations that report no compression figures or
  plan counts.
- Fixed a duplicated status line left behind when the progress window grew to
  make room for the compression figures. The rows below the new one moved
  without the vacated band being repainted, so the status text appeared twice.

### Validation

- Release MSVC solution build and the complete codec/archive/safety/fuzz suite
  passed with no warnings or errors.
- CLI create, integrity-test, repack, and post-repack integrity checks passed
  for the new live-deduplication profile.

## [0.9.2.2] - 2026-08-06

Large-window and large-solid-block release.

### Added

- Added native Axiom and LZMA2 dictionary/window controls through the 4 GiB
  user-facing limit, with the 32-bit wire ceiling validated consistently by
  the library, CLI, GUI, and external-codec reader.
- Added the AXAR large-solid profile for LZMA2 blocks from above 4 GiB through
  64 GiB, using temporary-file staging and bounded AXEC subframes with a
  required compatibility flag.
- Added boundary, malformed-geometry, stable-final-chunk, and CLI smoke
  regression coverage for the new limits.

### Fixed

- Kept one LZMA2 property across every external-codec frame, including a short
  final frame, preventing release-runtime aborts during large-dictionary
  streams.
- Rejected oversized size arguments and out-of-range match distances before
  narrowing or allocation.
- Included both full and Mini SFX runtime modules in installer and portable
  packages so every documented `--stub` tier works after installation.

### Validation

- Release MSVC solution build and complete codec/archive/safety/fuzz suite
  passed, including the 4 GiB boundary coverage.
- Release CLI 4 GiB LZMA2 creation smoke test passed and left no temporary
  artifact.

## [0.9.2.0] - 2026-08-05

Reliability and interface refinement release.

### Added

- Added high-confidence early stopping to compression preview curves when all
  tested levels have converged.
- Added a compact settings-style navigation layout and an icons-only toolbar
  display mode with hover highlighting.

### Fixed

- Hardened AXAR snapshot-manifest and chunk-table count validation so malformed
  archives cannot trigger multi-gigabyte vector allocations.
- Added regression coverage for the malformed allocation cases found by CI
  fuzzing.

### Validation

- Release MSVC build and complete codec/archive/safety regression suite passed.
- Rebuilt Windows AddressSanitizer/libFuzzer targets, ran both fuzzers for 30
  seconds, and replayed the exact CI crash artifact without findings.

## [0.9.1.0] - 2026-08-03

Performance maintenance release.

### Changed

- Optimized LZ77 candidate analysis, parser ring indexing, tree candidate
  scheduling, and the level-9 greedy search while preserving the AXC format.
- Added opt-in codec phase profiling for identifying ratio-neutral hot paths.

### Validation

- The full levels 1-9 regression matrix passed all 108 compression and
  decompression round trips. Archive bytes were unchanged at 17 of 18 corpus
  and level pairs; the remaining level-9 Silesia result differed by 71 bytes.

## [0.9.0.0] — 2026-08-02

Configurable, signable self-extracting archives and AXAR v5 capabilities.

### Added

- A native SFX v2 wrapper anchored to the end of the PE image, with a bounded
  descriptor, payload integrity hash, configuration TLV, and Authenticode-safe
  payload location. Existing v1 SFX files remain readable.
- Full and Mini SFX runtimes with GUI/console operation, silent and scripted
  modes, destination templates, license acceptance, selective extraction,
  run-after-extract controls, elevation policy, free-space checks, logging, and
  stable exit codes.
- AXAR v5 fidelity, password-slot encryption, append-generation, seekable
  extraction, and snapshot repository profiles, with malformed-input and fuzz
  coverage for the new parsing surfaces.
- Reproducible vendored-dependency lock verification across Windows, Linux, and
  macOS, including the refreshed 7-Zip, miniz, minizip-ng, and Monocypher
  components.

### Changed

- SFX creation, extraction, GUI controls, and CLI configuration now share the
  same validated options and read-only decode facade.
- AXAR v4 remains readable, while optional and additive v5 features reject
  unknown required capabilities instead of attempting an unsafe interpretation.

### Fixed

- Cross-platform dependency-lock checks now hash canonical text consistently,
  and all SFX sources are included in the Linux and Windows fuzz builds.
- Portable CLI builds now include the shared SFX configuration parser.
- The Authenticode regression test no longer depends on hosted Windows
  certificate-store services.

## [0.8.0.0] — 2026-07-30

Faster archive update, freshen, and synchronize.

### Changed

- Synchronization is now one planned transaction: unchanged compressed blocks
  are copied directly, changed and new files are compressed once, removed files
  are omitted in the same rewrite, and recovery data is rebuilt once after the
  final contents are known.
- Archive comments, locking, and signing fold into synchronization
  finalization instead of triggering extra rewrites.
- Recovery-parity generation is parallelized with bounded memory use.

### Added

- Path-aware update and synchronization for AXAR and ZIP, so GUI operations
  preserve the intended archive-relative paths.
- A fast no-op path when the source and archive already match.
- Explicit compare, copy, recovery, and commit progress phases, plus added,
  updated, removed, and unchanged counts.
- Round-trip coverage for the single-pass synchronization transaction.

The AXAR on-disk format is unchanged; existing archives remain compatible.

## [0.7.1.0] — 2026-07-29

### Changed

- Retuned the built-in file-type compression profiles around measured
  speed/ratio Pareto points. Text, structured data, and binary profiles use the
  level-7 tree matcher; mixed folders use level 5; already-compressed media
  keeps the level-1 fast-rejection path. Levels 8–9 remain explicit choices.
- Expanded the live compression-estimation graph in Add to archive and reduced
  excess width in the profile and compression-option controls.

### Fixed

- Modal dialog shutdown transfers activation directly back to the main window,
  removing close-time flicker and preventing the main window from dropping
  behind an underlying Explorer window. Applied to Settings, About, Find,
  Benchmark, information/estimate, archive-feature, password, message, and SFX
  dialogs.

## [0.7.0.0] — 2026-07-29

Selectable block compression methods.

### Added

- AXAR block methods: Axiom adaptive, Zstandard 1.5.7, LZMA2 (LZMA SDK 26.02),
  Deflate, and Store.
- The bounded AXC v10 external-codec envelope, with independent chunks, strict
  geometry validation, exact input consumption, stored fallbacks, and
  cooperative pause/cancel checkpoints.
- Format-aware compression profiles and method-specific controls for native
  levels, LZMA2 dictionary and fast-bytes settings, and HC4/BT4 selection.
- A live Add-to-archive compression prognosis: one shared sample produces every
  level point, with predicted size, savings, uncertainty, and click-to-select.

### Changed

- Hardened GUI validation for paths, numeric fields, output/input collisions,
  signing keys, split-volume limits, and benchmark inputs.
- Polished dark/light rendering, tooltips, DPI behavior, and dialog sizing.

### Compatibility

Axiom adaptive continues to emit AXC v9. Zstandard, LZMA2, and Deflate emit
AXC v10 and require 0.7.0.0 or newer to decode. Encryption, signatures,
recovery records, volumes, metadata, and SFX packaging are codec-independent.

## [0.6.0.0] — 2026-07-28

### Added

- A dedicated native SFX module embedded into Axiom, producing self-extracting
  archives without shipping or materializing a separate stub executable.
- Unified **Information** for files, folders, archives, and multiple
  selections, including background compression estimates.
- Full file-list column customization: visibility, ordering, persisted widths,
  and direct header reordering.
- **Tools > Delete Axiom temporary files**, with active-item protection,
  optional secure wiping, background execution, and reclaimed-space reporting.

### Changed

- Reorganized the native menu structure, and improved menu switching,
  customizable shortcuts, toolbar configuration, selection behavior, and
  repainting.
- Automatic update checks are silent on startup, with reliable retry after a
  failed check.
- Improved settings layout, dialog theming, parent-window activation, and
  file-browser responsiveness.

## [0.5.1.0] — 2026-07-26

### Changed

- Integrated the shared `Wimukthi.Win32Theme` framework for native light/dark
  mode, High Contrast, title bars, and standard-control theming, while
  preserving Axiom's custom accent palette, owner-drawn controls, menus, and
  icon styling.
- Pinned the shared theme dependency for reproducible CI and release builds.

### Added

- Complete third-party notices, and packaged Wimukthi.Win32Theme and
  Darkmodelib licensing plus corresponding Darkmodelib source.

### Fixed

- Windows fuzz build, by compiling the direct `7z.dll` adapter and linking its
  COM dependencies.
- Operation-progress window theme refresh when Windows or Axiom appearance
  settings change.

## [0.5.0.0] — 2026-07-26

### Changed

- Replaced the 7-Zip console-process backend with direct, in-process `7z.dll`
  integration. Portable and installer packages ship `7z.dll` without `7z.exe`,
  and no separate 7-Zip installation is required.

### Added

- Structured listing, testing, extraction, encrypted-archive passwords,
  progress, pause/cancel, and split-volume handling for the read-only
  7z/RAR/ISO/CAB providers.
- An app-wide custom accent color picker, and polished keyboard focus
  rendering.

### Fixed

- Hybrid ISO/UDF browsing now shows complete disc contents.

## [0.4.1.0] — 2026-07-24

GUI rendering and theme handling only; the archive format and codec are
unchanged.

### Fixed

- Checkbox and radio-button rendering, with stable antialiased indicators in
  both themes.
- Background-color mismatch between owner-drawn and native control labels.
- Live Dark↔Light Settings transitions, including duplicate combo arrows, ghost
  borders, and hidden-page drawing artifacts.

### Changed

- Brought the light theme to visual parity across the main window, dialogs,
  settings navigation, tabs, buttons, scrollbars, toolbars, and icons, while
  preserving native high-contrast colors and behavior.

## [0.4.0.2] — 2026-07-22

### Added

- AXC v9 decoder support and an opt-in parser-checkpoint swarm candidate.
  Independent DP tiles retain the full block window and are written only when
  their complete payload is smaller.
- Swarm parsing extended to levels 1, 8, and 9, with deterministic
  encoder-selected candidates and legacy representations kept as bake-off
  fallbacks.
- A fully in-memory, deterministic GUI benchmark with automatic sizing,
  repeated phases, continuous mode, and rolling throughput, variation,
  stability, and verification telemetry.
- File-type-oriented compression profiles and selectable estimation levels.
- macOS in the build-and-test matrix.

### Changed

- Reduced the level-9 DP working set with compact recent-distance state, 8-byte
  parse decisions, and a live cost-frontier ring. The recorded enwik9 run cut
  peak commit by 39.8% and wall time by 9.9% with identical archive bytes.
- Logical processors are discovered across Windows processor groups while
  automatic block geometry stays based on physical cores.

### Fixed

- POSIX filesystem, timestamp, process, large-file, and ARM64 BLAKE3
  portability issues.

## [0.4.0.0] — 2026-07-16

AXC format version 8, and the complete published benchmark suites.

### Added

- Entropy-coded distance footer bits, with a raw-stream bake-off fallback.
- Per-lane literal model selection, including clustered order-1, match-byte,
  and full-previous-byte contexts.
- Adaptive block geometry and tar-aware file transforms, selected only when
  they make the archive smaller.
- Executable, delta, and 16-bit numeric transforms with deterministic bounded
  decoding.
- Silesia and enwik8 cross-codec ratio and throughput graphs, with raw CSV
  results committed under `bench/results/`.

### Security

- Transform metadata range counts are validated before allocation, preventing
  malformed archives from requesting excessive memory.
- Windows and Linux fuzz builds cover the transform and split-ZIP sources plus
  minizip-ng.

## [0.3.0.0] — 2026-07-15

### Added

- AXC v7 hybrid blocks, stronger literal contexts, recent-distance coding, and
  validated numeric and tar-member transforms.
- An operation-scoped work-stealing executor, so independent block, layout, and
  entropy work can share all available physical cores without a fixed thread
  ceiling.
- WinRAR RAR5 in the round-trip-verified cross-codec benchmark harness.

### Changed

- The GUI benchmark runs compression, decompression, and verification entirely
  in memory after custom input is preloaded.
- Reworked operation dialogs into persistent telemetry fields.

### Fixed

- False `0 B/s` reporting while many small files are read into the first solid
  block.

## [0.2.2.x] — 2026-07-04 … 2026-07-14

The 0.2.2 series was a rapid iteration on the GUI, the provider layer, and
packaging.

### Added

- Read-only view/extract/test for 7z, RAR/RAR5, TAR-family, ISO, and CAB
  through the system archive provider, with per-format associations and
  embedded archive icons (0.2.2.0).
- A bundled 7-Zip backend, so read-only formats work without user-installed
  tools, plus fast native ISO9660/Joliet browsing (0.2.2.3).
- A customizable keyboard shortcut system with a Settings > Shortcuts page, and
  restored Back/Forward browser history including selection, scroll, and tree
  state (0.2.2.12).
- Recovery parity progress reporting (0.2.2.34).
- An Explorer Axiom submenu with per-command icons, and richer `axiomc` startup
  diagnostics — version, build date, author, and CPU features (0.2.2.35).
- Adaptive compression estimation in the Information dialog, and direct
  read-only opening, testing, and extraction of complete AXAR and ZIP
  split-volume sets without reconstructing a temporary archive (0.2.2.38).

### Changed

- Split the archive container and the Win32 main window into focused
  translation units (0.2.2.3).

### Fixed

- GUI crash when browsing filenames not representable in the active ANSI code
  page (0.2.2.0).
- ZIP self-extracting archive selection, so enabling SFX preserves the ZIP
  format (0.2.2.38).
- Archive creation hardened around Unicode paths, unreadable or disappearing
  inputs, atomic output replacement, and split-archive validation (0.2.2.38).

## [0.2.1.0] — 2026-07-04

### Added

- The advanced I/O buffer setting, wired across compression, extraction,
  testing, update/rewrite, SFX creation, and embedded SFX extraction.
- Live Apply behavior for settings, so appearance and advanced options take
  effect immediately.

## [0.2.0.0] — 2026-07-02

### Added

- The archive provider layer groundwork, and miniz-backed ZIP support.
- The custom directory tree pane.

### Changed

- Improved archive information and capability UI, archive comment display, and
  provider-aware archive dialogs.

## [0.1.2.0] — 2026-07-01

### Added

- The detailed benchmark window foundation, with live compression and
  decompression metrics instead of a plain text log.
- Browse buttons for GUI fields that take file or folder paths.

## [0.1.1.0] — 2026-07-01

### Changed

- Improved default CPU scaling for compression and decompression without
  changing the archive format. `--threads 0` still means all detected hardware
  threads, and default block sizing now creates enough independent codec work
  to feed them.
- Removed major serial bottlenecks with bulk file reads and by reusing
  worker-computed block CRCs for parallel payloads.

## [0.1.0.2] — 2026-06-28

### Added

- The native Win32 benchmark dialog, wired into Tools > Benchmark.
- The CLI ASCII splash and interactive prompt when `axiomc` runs with no
  arguments.

### Changed

- Improved compression throughput without changing the archive format,
  including incompressible-data fast paths and lower single-worker overhead.

## [0.1.0.1] — 2026-06-27

First published release: the Inno Setup installer and portable zip, carrying
`Axiom.exe`, `axiomc.exe`, documentation, App Paths registration,
update/repair/remove maintenance handling, and dynamic light/dark setup
styling.

[Unreleased]: https://github.com/Wimukthi/AxiomCompress/compare/0.10.2.0...HEAD
[0.10.2.0]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.10.2.0
[0.10.1.0]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.10.1.0
[0.10.0.0]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.10.0.0
[0.9.2.3]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.9.2.3
[0.9.2.2]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.9.2.2
[0.9.1.0]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.9.1.0
[0.9.0.0]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.9.0.0
[0.8.0.0]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.8.0.0
[0.7.1.0]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.7.1.0
[0.7.0.0]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.7.0.0
[0.6.0.0]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.6.0.0
[0.5.1.0]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.5.1.0
[0.5.0.0]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.5.0.0
[0.4.1.0]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.4.1.0
[0.4.0.2]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.4.0.2
[0.4.0.0]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.4.0.0
[0.3.0.0]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.3.0.0
[0.2.2.x]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.2.2.38
[0.2.1.0]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.2.1.0
[0.2.0.0]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.2.0.0
[0.1.2.0]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.1.2.0
[0.1.1.0]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.1.1.0
[0.1.0.2]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.1.0.2
[0.1.0.1]: https://github.com/Wimukthi/AxiomCompress/releases/tag/0.1.0.1
