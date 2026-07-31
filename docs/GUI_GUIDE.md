# Axiom for Windows

`Axiom.exe` is a native Win32 archive manager. It is plain Visual C++ and Win32
— no Qt, .NET, WinUI, or embedded browser — and it drives the same library the
[command-line tool](../CLI_GUIDE.md) uses.

![The Axiom archive browser in dark mode](images/axiom-gui.png)

## Main window

The window behaves like a file manager. It browses filesystem folders and
archives through the same view:

- Open `.axar` and `.zip` archives for browsing, testing, extraction, and
  editing; open 7z, RAR, ISO, CAB, and TAR-family archives read-only.
- Navigate with the editable address dropdown — paths, drives, shell locations,
  favorites, recent folders, and history.
- Sort, resize, reorder, show, and hide columns; widths and order persist.
- Drag files in from Explorer, drag entries out to Explorer, and move entries
  between folders inside an archive.
- Drop an archive onto the window to open it.

The status bar reports the selection, unpacked and packed totals, and the
active archive.

### Menus

| Menu | Contains |
|---|---|
| File | Open, single-stream compress/decompress, Information, Exit |
| Edit | Select all, Find, Delete, copy path, copy CRC-32 |
| Archive | Add, Extract, Test, Update, Freshen, Synchronize, Repack, Split, Join, comment, lock, recovery, repair, sign, verify, SFX |
| View | Navigation, refresh, tree pane, favorites, column layout |
| Tools | Benchmark, Generate signing key, Delete Axiom temporary files, Settings |
| Help | Check for updates, About Axiom |

### Packed sizes

ZIP reports exact per-entry compressed sizes from its central directory. AXAR
files share solid blocks, so per-file Packed values are proportional estimates
and are marked with `≈`. Archive-level size and ratio are always exact.

## Archive operations

Add, extract, test, delete entries, edit the archive comment, lock, repair from
recovery data, split and join volumes, sign and verify, and build a
self-extracting `.exe`.

Long operations run on worker threads. The window stays responsive, and
progress shows byte counts, throughput, ETA, output size, and compression ratio
where available. Pause, resume, and cancel use the same cooperative
`OperationControl` path as the CLI, so cancelling leaves no partial output.

Update, freshen, and synchronize run as a single planned transaction: unchanged
compressed blocks are copied directly, changed and new files are compressed
once, removed files are dropped in the same rewrite, and recovery data is
rebuilt once at the end. Progress reports the compare, copy, compress,
recovery, and commit phases separately. If the source already matches the
archive, nothing is rewritten.

Complete numbered volume sets open directly; reconstruction is only needed when
data parts are missing or damaged.

## Add to archive

![The Add to archive dialog](images/axiom-add-to-archive.png)

Archive creation is one resizable tabbed dialog. The output path stays visible
across all tabs.

| Tab | Contains |
|---|---|
| Compression | Method, level, dictionary and word size, solid block size, threads, threading model |
| General | Update mode, archive comment, metadata notes |
| Security | Password, filename encryption, show-password toggle |
| Recovery & volumes | Recovery record percentage, split volume size, recovery volumes |
| SFX & signing | Self-extracting output and archive signing |

For AXAR the Method list offers Axiom adaptive, Zstandard, LZMA2, Deflate, and
Store. Choosing a method rebuilds the level list and enables only the controls
that mean something: Axiom exposes its threading model, LZMA2 exposes
dictionary and word size plus HC4/BT4, and Zstandard and Deflate keep their
native levels. ZIP creation is deliberately limited to Deflate and Store.

If SFX is enabled, the output path becomes the final merged `.exe` — Axiom does
not leave a separate archive beside it.

### Compression profiles

Five built-in profiles cover text and source, executables, structured data,
already-compressed media, and mixed folders. They are tuned as practical
defaults: level 7 for text, structured data, and binaries; level 1 fast
rejection for pre-compressed media; and balanced level 5 for mixed folders.
Levels 8 and 9 stay explicit choices rather than hiding inside a profile.

Saved user profiles also preserve the method, native codec level, LZMA2 match
finder, dictionary and word size, solid block size, thread count, and threading
model.

### Compression preview

For AXAR and ZIP, the right side of the Compression tab shows a live prediction
across every level the selected method supports. All points come from the same
sampled source regions, so the curve compares codecs rather than sampling
noise.

Blue is predicted compressed bytes, green is predicted saving, and the neutral
band is the uncertainty range. Click a point to select that level. Changing a
setting that affects the source cancels and re-runs the estimate after a short
pause; changing only the level moves the marker without rescanning.

## Settings

![The Axiom settings dialog](images/axiom-settings.png)

Settings are stored per user under `HKCU\Software\AxiomCompress\GUI`, across
eleven pages:

| Page | Covers |
|---|---|
| General | Theme, accent color, button icons, startup location, confirmations |
| Compression | Default method, level, and threading model |
| Paths | Default output, extraction, and temporary locations |
| File list | Column visibility and order, sorting, display options |
| Viewer | How entries open for preview |
| Security | Password handling and encryption defaults |
| Integration | Per-user file associations and the Explorer submenu |
| Updates | Automatic update checks |
| Shortcuts | Rebindable keyboard shortcuts |
| Toolbar | Which buttons appear, and in what order |
| Advanced | Diagnostics and low-level behavior |

The General page can follow the Windows accent, use an Axiom preset, or take a
custom color from a DPI-aware picker. The accent applies to selections,
progress indicators, buttons, and optional accent-colored icons.

The Integration page registers per-user associations for AXAR, ZIP/JAR/WAR/APK,
7z, RAR, TAR-family, ISO, and CAB. Read-only formats use embedded Axiom icons
and open into the browser for viewing, testing, and extraction.

Options are wired only where the engine or GUI actually has the behavior.
Unsupported future options are disabled rather than silently stored.

## Benchmark

![The Axiom benchmark window](images/axiom-benchmark.png)

`Tools > Benchmark…` measures Axiom compression and extraction throughput. It
uses either a generated corpus or a folder or file you choose.

Generated data is created directly in RAM and custom input is preloaded once
before timing, so every compression, decompression, and byte-for-byte
verification pass runs entirely in memory — storage throughput never
contaminates the codec result.

The default synthetic corpus uses deterministic literals and log-distributed
backward matches to exercise the match finder across the active window, rather
than timing a trivially repeated string. Automatic sizing fits the selected
level and available memory, and each measured phase repeats enough to reduce
timer noise. Continuous mode runs until **Stop**, keeping recent-pass detail
plus lifetime throughput, rolling variation, and a stability indicator.

The window measures the native Axiom method only. For cross-codec comparisons
see [BENCHMARKING.md](BENCHMARKING.md).

## Appearance and DPI

System light/dark detection, High Contrast handling, native title bars, and
standard control theming come from the shared `Wimukthi.Win32Theme` framework.
Axiom keeps its own accent palette and owner drawing on top:

- Dark title bars, menus, dialogs, list views, progress controls, combo boxes,
  and custom message boxes.
- Per-monitor DPI-scaled fonts, icons, spacing, and dialog layout.
- Owner-drawn controls where common controls do not dark-theme correctly.
- Shared command IDs across menus, toolbar, shortcuts, and context actions.

## Keyboard shortcuts

All shortcuts are rebindable on the Settings > Shortcuts page. Defaults:

| Action | Shortcut |
|---|---|
| Add to archive | `Ctrl+N` |
| Extract | `Ctrl+E` |
| Test archive | `Ctrl+T` |
| Update / Freshen / Synchronize | `Ctrl+U` / `Ctrl+Shift+U` / `Ctrl+Alt+U` |
| Repack | `Ctrl+Shift+R` |
| Split / Join volumes | `Ctrl+Shift+S` / `Ctrl+J` |
| Edit comment | `Ctrl+M` |
| Lock archive | `Ctrl+Shift+L` |
| Recovery record | `Ctrl+Shift+Y` |
| Repair archive | `Ctrl+Shift+P` |
| Generate signing key | `Ctrl+Shift+K` |
| Sign / Verify | `Ctrl+Shift+G` / `Ctrl+Shift+V` |
| Create self-extracting archive | `Ctrl+Shift+X` |
| Compress / decompress single stream | `Ctrl+Alt+Z` / `Ctrl+Alt+X` |
| Information | `Alt+Enter` |
| Find files | `Ctrl+F` |
| Benchmark | `Ctrl+B` |
| Delete Axiom temporary files | `Ctrl+Shift+Delete` |
| Settings | `Ctrl+,` |
| Select all / Delete | `Ctrl+A` / `Delete` |
| Copy path / Copy CRC-32 | `Ctrl+Shift+C` / `Ctrl+Alt+C` |
| Back / Forward / Up | `Alt+Left` / `Alt+Right` / `Alt+Up` |
| Focus address bar | `Ctrl+L` |
| Refresh | `F5` |
| Show/hide tree pane | `F9` |
| Add / remove favorite | `Ctrl+D` / `Ctrl+Shift+D` |
| Check for updates | `Ctrl+Alt+Shift+U` |
| About Axiom | `F1` |

## Command line entry points

`Axiom.exe` accepts a few startup commands, which is how the Explorer
integration drives it:

```text
Axiom.exe <archive>              open the archive in the browser
Axiom.exe --add <path>...        open Add to archive for those paths
Axiom.exe --extract <archive>    open the extraction flow
Axiom.exe --test <archive>       open the test flow
```

Concurrent `--add` invocations from an Explorer multi-selection are coalesced
into one dialog.

## Housekeeping

`Tools > Delete Axiom temporary files` clears Axiom's staging directory, skips
anything an active operation is using, optionally wipes securely, runs in the
background, and reports the space reclaimed.

Update checks are silent on startup and retry cleanly after a failed check.
`Help > Check for updates` runs one on demand.
