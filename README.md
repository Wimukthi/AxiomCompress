# AxiomCompress

AxiomCompress is an experimental archive compressor for Windows and the command
line. Its main archive format is `.axar`; the lower-level single-stream format is
`.axc`.

The current goal is 7-Zip-class solid compression with a simple, bounded decoder.
The project is not yet at 7-Zip's maximum ratio, but it already has a complete
archive container, a native Win32 GUI, a scriptable CLI, integrity checks,
encryption, recovery data, split volumes, signing, and SFX packaging.

## Start here

Most users need one of these paths:

| Goal | Use |
|---|---|
| Create/open archives visually | `out\Release\Axiom.exe` |
| Script backups or tests | `out\Release\axiomc.exe` |
| Learn every CLI command | [CLI_GUIDE.md](CLI_GUIDE.md) |
| Understand the archive layout | [FORMAT.md](FORMAT.md) |
| Understand the codec design | [ARCHITECTURE.md](ARCHITECTURE.md) |
| Benchmark speed and ratio changes | [docs/BENCHMARKING.md](docs/BENCHMARKING.md) |
| Build an installer | [docs/INSTALLER.md](docs/INSTALLER.md) |

## What Axiom supports today

- Multi-file `.axar` archives with files, folders, metadata, NTFS alternate data
  streams, symlinks, hardlinks, comments, locking, and archive editing.
- Solid blocks for cross-file compression while keeping each block independently
  decodable for selective extraction.
- Per-block CRC checks, per-file CRC-32, and per-file BLAKE3-256 hashes.
- Optional password encryption using Monocypher Argon2id and
  XChaCha20-Poly1305.
- Optional encrypted filenames/directories.
- Recovery records, repair, split volumes, and `.rev` recovery volumes.
- Monocypher EdDSA archive signatures.
- Native Windows SFX output as a single merged `.exe`.
- Cooperative progress, pause, resume, and cancel through shared worker-thread
  operation control.
- Coverage-guided fuzz targets and Release-mode round-trip tests.

## Build

### Visual Studio / MSBuild

Open `AxiomCompress.sln` in Visual Studio and build `Release|x64`.
The checked-in Visual Studio projects target the installed Visual C++ toolset
`v145`.

From PowerShell:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" AxiomCompress.sln /p:Configuration=Release /p:Platform=x64
```

Or use the helper scripts:

```powershell
.\tools\build_msvc.ps1 -Configuration Release
.\tools\test_msvc.ps1 -Configuration Release
```

The solution contains:

| Project | Purpose |
|---|---|
| `AxiomLib` | Static library for the archive and codec engine |
| `AxiomC` | CLI executable, `axiomc.exe` |
| `AxiomGui` | Native Win32 GUI executable, `Axiom.exe` |
| `AxiomRoundtrip` | Test executable |

`AxiomGui.vcxproj` uses source-backed versioning like NativePad. Normal GUI
builds increment the fourth version component in `src\gui\axiom_gui.rc`. For a
diagnostic build that must not modify source, pass:

```powershell
.\tools\build_msvc.ps1 -Configuration Release -AutoIncrementVersion:$false
```

Details: [docs/VERSIONING.md](docs/VERSIONING.md).

Packaged releases pin the exact resource version before tagging, then build with
`-AutoIncrementVersion:$false` so the installer, zip asset, About dialog, and
GitHub release tag all use the same four-part version.

### CMake

```powershell
cmake --preset default
cmake --build --preset default
ctest --preset default
```

If Ninja is not installed but Visual Studio 2022 is available:

```powershell
cmake --preset vs2022
cmake --build --preset vs2022 --config Release
```

## GUI

The GUI executable is:

```text
out\Release\Axiom.exe
```

It is a native Visual C++ / Win32 application. It does not use Qt, .NET, WinUI, or
web UI layers.

### Main window

The main window behaves like a file manager:

- Browse filesystem folders and `.axar` archives.
- Open `.zip` archives for browsing, testing, extraction, and normal edit
  operations.
- Use the editable address dropdown for paths, drives, shell locations, recent
  folders, and history.
- Sort and resize columns.
- Select multiple files.
- Drag files from Explorer into an archive.
- Drag files out of an archive to Explorer.
- Move entries between folders inside an archive.
- Open dropped archives directly in the browser.

Archives are presented through a provider/catalog layer. The UI asks the archive
engine what each archive can do, then enables or disables commands from those
capability flags.

That provider layer is internal for now. It is designed so future built-in
providers such as ZIP or TAR can expose view/extract/test/update capabilities
without turning Axiom into an external plug-in host before the API is stable.

Current provider support:

| Format | Browse | Extract | Test | Create/update/delete/move |
|---|---:|---:|---:|---:|
| AXAR | Yes | Yes | Yes | Yes |
| ZIP | Yes | Yes, stored/deflated entries | Yes | Yes, with limits for encrypted ZIPs |
| 7z, RAR, TAR family, ISO, CAB | Yes on Windows | Yes on Windows | Yes on Windows | No |

ZIP create/update/delete/move support rewrites the ZIP atomically through the
archive provider layer and populates the Packed column from the ZIP central
directory. AXAR populates Packed with a proportional estimate because solid
blocks can contain data from several files; estimated values are marked with
`≈`. New encrypted ZIPs use WinZip AES-256 file-data encryption. ZIP file names
remain visible, and existing encrypted ZIPs are read/test/extract only for now.
AXAR-only features such as archive comments, locking, recovery records, split
volumes, signatures, SFX packaging, encrypted names, and Axiom metadata remain
disabled when ZIP is selected.

On Windows, Axiom also exposes read-only archive providers for common formats.
ISO browsing uses Axiom's native ISO9660/Joliet directory reader so large images
display immediately; ISO extraction/test still use the bundled 7-Zip backend.
The bundled 7-Zip backend also handles `.7z`, `.rar`, and `.cab`; Windows
`tar.exe` handles `.tar`, `.tar.gz`, `.tgz`, `.tar.xz`, `.txz`, `.tar.bz2`,
`.tbz2`, `.tar.zst`, and `.tzst`. These formats never appear as Add-to-archive
creation targets.

See [docs/FORMAT_SUPPORT.md](docs/FORMAT_SUPPORT.md) for the planned split
between full read/write formats and view/extract-only formats.

### Archive operations

The GUI supports:

- Add files/folders.
- Extract.
- Test integrity.
- Delete entries.
- Edit archive comments.
- Lock archives.
- Repair damaged archives when recovery data is available.
- Open numbered split volumes and reconstruct the complete archive.
- Sign and verify archives.
- Create native SFX executables.

Long-running operations run on worker threads. The main window remains
responsive, and progress includes byte counts, throughput, ETA, output size, and
compression ratio when available. Pause/resume and cancel use the same
`OperationControl` path as the CLI/library.

### Add to Archive dialog

Archive creation uses one resizable tabbed dialog:

| Tab | Contains |
|---|---|
| Compression | Level, dictionary size, word size, solid block size, threads |
| General | Update mode, archive comment, metadata notes |
| Security | Password, filename encryption, show-password toggle |
| Recovery & volumes | Recovery record, split volume size, recovery volumes |
| SFX & signing | Self-extracting output and archive signing |

The output path and effective-output preview stay visible across tabs. If SFX is
enabled, the final output is the merged `.exe`; Axiom does not leave a separate
archive next to it.

### Dark mode and DPI

The GUI is designed to stay native while still looking correct in dark mode:

- Dark title bars, menus, dialogs, list views, progress controls, combo boxes, and
  custom message boxes.
- Per-monitor DPI-scaled fonts, icons, spacing, and dialog layouts.
- Owner-drawn controls where common controls do not dark-theme correctly.
- Shared command IDs for menus, toolbar buttons, keyboard shortcuts, and context
  actions.

### Settings

Settings are stored per user under:

```text
HKCU\Software\AxiomCompress\GUI
```

The Settings dialog has these pages:

- General
- Compression
- Paths
- File list
- Viewer
- Security
- Integration
- Updates
- Advanced

The Integration page can register per-user file associations for AXAR,
ZIP/JAR/WAR/APK, 7z, RAR, TAR-family, ISO, and CAB files. Read-only formats use
embedded Axiom format icons and open into the browser for viewing, testing, and
extraction.

Settings are wired only where the engine or GUI has real behavior. Unsupported
future options are disabled instead of being stored as silent no-ops.

### Benchmark window

`Tools > Benchmark...` opens a 7-Zip-style benchmark window for measuring Axiom
compression and extraction throughput. It can use generated corpora or a custom
input folder/file.

## CLI quick start

Use `axiomc.exe` for scripts and automation:

```powershell
axiomc a archive.axar mydir file.txt
axiomc l archive.axar
axiomc t archive.axar
axiomc x archive.axar restored
```

Common archive commands:

```powershell
axiomc a --level 9 archive.axar mydir
axiomc a -p "password" archive.axar private-dir
axiomc a -p "password" --encrypt-names hidden.axar private-dir
axiomc a archive.zip mydir
axiomc a -p "password" encrypted.zip private-dir
axiomc recovery archive.axar 10
axiomc repair archive.axar
axiomc split archive.axar 100M 3
axiomc join archive.part001.axar restored.axar
axiomc keygen secret.key public.key
axiomc sign archive.axar secret.key
axiomc verify archive.axar public.key
axiomc sfx archive.axar archive.exe
```

For `.zip` output, `-p/--password` creates WinZip AES-256 file-data encrypted
entries. ZIP file names remain visible; use `.axar --encrypt-names` when names
and directory metadata must be hidden.

Single-stream `.axc` mode:

```powershell
axiomc c input.bin output.axc
axiomc c --fast input.bin output.axc
axiomc c --max input.bin output.axc
axiomc d output.axc restored.bin
```

Launching `axiomc` without arguments opens an interactive prompt. Full command
reference: [CLI_GUIDE.md](CLI_GUIDE.md).

## Compression levels

One level chooses the speed/ratio tradeoff:

| Level | Matcher | Intended use |
|---|---|---|
| 1 / `--fast` | Fast row hash | Fastest compression |
| 2-3 | Shallow hash chain | Fast backups |
| 4-5 | Lazy hash chain | Balanced default use |
| 6 | Deeper hash chain | Better ratio without tree memory |
| 7 | Binary tree, 8 MiB window | Long-range matches |
| 8 | Binary tree, 32 MiB window | Wider long-range search |
| 9 / `--max` | Binary tree, 64 MiB window | Maximum preset |

The default is level 5.

Advanced flags can override the preset:

```text
--chain-depth N
--nice N
--lazy / --no-lazy
--fast-entropy
--bt
--window SIZE
--block-size SIZE
--threads N
--parallel
--optimal
--optimal-depth N
--optimal-candidates N
```

## Benchmarking

The standard text benchmark is `enwik8`:

```powershell
.\tools\bench_enwik8.ps1
.\tools\bench_enwik8.ps1 -Quick
.\tools\bench_enwik8.ps1 -Axiomc out\Release\axiomc.exe
```

For custom files or folders:

```powershell
python bench/bench_7zip.py --axiom out/Release/axiomc.exe --input path/to/corpus
python bench/bench_7zip.py --axiom out/Release/axiomc.exe --input path/to/corpus --axiom-threads 0
python bench/bench_7zip.py --axiom out/Release/axiomc.exe --input path/to/corpus --axiom-optimal --axiom-threads 0
```

`--threads 0` is the default and means all detected hardware threads. Supplying
`--block-size` is useful for controlled experiments, but it disables the default
auto block sizing that keeps enough independent work available for the selected
thread count.

To compare two Axiom builds across compression levels, use the level comparator:

```powershell
.\tools\bench_axiom_levels.ps1 `
  -BaselineAxiomc D:\baselines\axiomc.exe `
  -CurrentAxiomc .\out\Release\axiomc.exe `
  -CorpusDir D:\tests\axiom-perf\corpora `
  -OutputDir D:\tests\axiom-perf\results `
  -Levels 1,2,3,4,5,6,7,8,9 `
  -Repeats 3
```

For a quick smoke test, let the script generate deterministic sample corpora:

```powershell
.\tools\bench_axiom_levels.ps1 `
  -BaselineAxiomc D:\baselines\axiomc.exe `
  -GenerateSampleCorpora `
  -SampleSizeMiB 8
```

The script verifies round-trips and writes raw, summary, and delta CSV files.
Positive compression/decompression deltas mean the current build is faster;
positive ratio deltas mean the current build compressed smaller. For custom
profile sweeps and repeatable tuning workflow details, see
[docs/BENCHMARKING.md](docs/BENCHMARKING.md).

## Performance snapshot

Ratios below are for `enwik8` (100 MB English Wikipedia text). They are exact for
the current codec; throughput depends on hardware and build settings.

| Level | 1 | 3 | 5 default | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|
| Ratio | 2.68x | 3.01x | 3.09x | 3.10x | 3.17x | 3.33x | 3.47x |

The 0.1.1.0 CPU-scaling pass keeps the archive format unchanged and improves
throughput by feeding all available workers by default. Local D:\tests corpus
measurements from the release candidate:

| Corpus / level | Compress before | Compress 0.1.1.0 | Decode-to-NUL 0.1.1.0 | Ratio |
|---|---:|---:|---:|---:|
| mixed-64m, level 1 | 211.9 MiB/s | 907.2 MiB/s | 2204.0 MiB/s | 386.14x |
| mixed-64m, level 8 | 126.2 MiB/s | 255.0 MiB/s | 2059.3 MiB/s | 440.51x |
| long-distance-112m, level 1 | 127.4 MiB/s | 217.3 MiB/s | 1048.8 MiB/s | 1.17x |
| mixed-512m, level 1 | 229.8 MiB/s | 1098.1 MiB/s | 3853.7 MiB/s | 409.09x |
| mixed-512m, level 8 | 127.3 MiB/s | 246.3 MiB/s | 2729.8 MiB/s | 515.30x |

Throughput depends on CPU, memory bandwidth, storage, corpus shape, and Release
build settings. Higher levels still trade compression speed for ratio; the
remaining gap to 7-Zip maximum ratio is mainly in the entropy stage.

## Testing and fuzzing

Run the Release tests:

```powershell
.\tools\test_msvc.ps1 -Configuration Release
```

Or with CMake:

```powershell
ctest --preset default
```

Build and run fuzz targets:

```powershell
.\tools\build_fuzz.ps1 -Target all
.\tools\run_fuzz.ps1 -Seconds 60 -Target all
```

CI builds and tests on Windows and Linux, then runs both fuzz targets on every
push.

## Packaging

Axiom uses Inno Setup 6 for release installers:

```powershell
.\installer\build-installer.ps1
```

The script builds Release x64, runs the Release round-trip test, reads the app
version from `src\gui\axiom_gui.rc`, and writes:

```text
installer\output\AxiomSetup-<version>-win-x64.exe
```

GitHub releases also carry a portable zip asset named
`Axiom-<version>-win-x64.zip` containing `Axiom.exe`, `axiomc.exe`, the bundled
read-only archive backend, the license, and the user/developer docs.

Details: [docs/INSTALLER.md](docs/INSTALLER.md).

## License

AxiomCompress is licensed under the GNU General Public License version 3. See
[LICENSE](LICENSE).

Vendored third-party components keep their license notices under
`src/third_party`, `third_party`, and the installed `backends` folder. Current
third-party components include:

- miniz 3.1.1 for ZIP read/write support, under the MIT license.
- 7-Zip console backend for read-only 7z/RAR/ISO/CAB support, under LGPL/BSD
  terms with the upstream unRAR restriction for some RAR code.
- Monocypher for cryptographic primitives, under the BSD 2-Clause license.
- BLAKE3 for hashing and integrity primitives, under CC0/Apache-2.0 licensing.
- A generated subset of Microsoft Fluent UI System Icons for the native GUI,
  under the MIT license.
