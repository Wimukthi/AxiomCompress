# AxiomCompress

AxiomCompress is an experimental archival compressor. The benchmark target is
7-Zip (`7z` + LZMA2) in high-ratio solid mode.

What it does today:

- **Multi-file `.axar` archives** — solid blocks for cross-file redundancy,
  independently decodable for selective extraction; BLAKE3 + CRC integrity,
  Windows metadata/ADS, links, archive editing, comments/locking, and optional
  Monocypher encryption of data and names. See [FORMAT.md](FORMAT.md).
- **One speed/ratio knob:** `--level 1..9`. Level 1 is a fast LZ4-style path
  (`fast_lz`); levels 2–6 a lazy hash-chain matcher; 7–9 a cyclic-window
  binary-tree matcher, with the optimal parser available via `--optimal`.
- **Repeat-offset (rep0–rep3) matches**, split LZ77 streams (with a position-slot
  distance variant), and entropy via canonical Huffman, **4-lane interleaved
  order-0 rANS**, and an adaptive **order-1** coder — chosen per substream.
- **Threaded** independent-block compression and decompression, with progress
  reporting and cooperative pause/cancel (`OperationControl`).
- **CLI** (`axiomc`) and a **Windows GUI** (`Axiom.exe`).
- Coverage-guided libFuzzer+ASan targets and a Release-mode test suite.

It does not yet match 7-Zip's ratio — the open gap is the entropy stage (a
context-modeled coder; see [ROADMAP.md](ROADMAP.md)) — but it decodes fast and the
decoder stays simple and bounded by design.

## Build With Visual C++

Open `AxiomCompress.sln` in Visual Studio and build `Release|x64`.
The checked-in projects target the installed Visual C++ toolset `v145`.

From PowerShell on this machine, MSBuild is available through the Visual Studio install path:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" AxiomCompress.sln /p:Configuration=Release /p:Platform=x64
```

Or use the helper script:

```powershell
.\tools\build_msvc.ps1 -Configuration Release
.\tools\test_msvc.ps1 -Configuration Release
```

The solution contains:

- `AxiomLib`: static library for the compression engine.
- `AxiomC`: command-line compressor/decompressor.
- `AxiomGui`: Windows-only native Win32 GUI frontend for `.axar` archives.
- `AxiomRoundtrip`: executable round-trip tests.

## Build With CMake

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

## CLI

Multi-file archives (`.axar`) — files and directories with paths, metadata, and
per-file integrity. See [FORMAT.md](FORMAT.md) for the on-disk layout.

```powershell
axiomc a archive.axar mydir file.txt        # create (recurses directories)
axiomc a --optimal --block-size 16M archive.axar mydir
axiomc l archive.axar                        # list contents
axiomc t archive.axar                        # verify integrity
axiomc x archive.axar dest-dir               # extract (paths contained to dest-dir)
axiomc x --overwrite skip archive.axar dest-dir
axiomc a -p "password" archive.axar private-dir
axiomc a -p "password" --encrypt-names hidden.axar private-dir
axiomc l -p "password" hidden.axar
```

Single-stream mode (one input stream to one `.axc` blob):

```powershell
axiomc c input.bin output.axc                # default: level 5 (balanced)
axiomc c --fast input.bin output.axc         # level 1 (fastest)
axiomc c --max input.bin output.axc          # level 9 (maximum ratio)
axiomc c --level 7 input.bin output.axc      # pick a point on the curve
axiomc c --level 3 --threads 8 input.bin output.axc
axiomc d output.axc restored.bin
```

## GUI

`out\Release\Axiom.exe` is a native Win32 frontend for `.axar` archives. It
supports adding files/folders, creating archives, opening/listing archives,
testing integrity, and extracting to a chosen folder. The GUI is Windows-only,
per-monitor DPI aware, and has a dark-mode foundation (system theme detection,
dark title bar, custom dark-painted table/progress controls, and scaled
layout/fonts). Operations run on a worker thread so the window stays responsive;
create/test/extract report byte progress to the status strip and progress bar,
including throughput, ETA, output size, and live compression ratio when available.
Operations can be paused or cancelled cooperatively. Archive creation, extraction,
feature options, custom messages/about, and application settings use native dark,
DPI-scaled dialogs.

The main window is a file-manager browser with drive and directory navigation,
an editable address bar, history, shell icons, multi-selection, sortable and
resizable columns, and hierarchical archive browsing. Archive presentation is
isolated behind a provider/catalog layer with explicit capability flags, so
archive editing, comments, locking, links, and data encryption are wired through
public archive APIs. Filename encryption and encrypted-archive editing have backend
support and are the next GUI integration step. Recovery records, volumes,
authenticity, and SFX remain capability-gated until their archive APIs land.
Its owner-drawn dark menu bar exposes File, Commands, Tools, Options, and Help
menus without falling back to a light system menu, and routes menu and keyboard
commands through the same command IDs used by the toolbar.
Filesystem folders refresh automatically through `ReadDirectoryChangesW`; dropped
archives open in the browser, while dropped files and folders currently enter the
create-archive workflow. The backend now exposes destination-aware add, selective
extract, and metadata-only move APIs for the pending Explorer-style archive
drag/drop layer. Window placement, the last location, sorting, and application
defaults persist per user under `HKCU\Software\AxiomCompress\GUI`.

### Effort levels (`--level 1..9`, default 5)

One knob trades speed for ratio. `--fast` = 1, `--max` = 9. **Level 1** is the
dedicated fast path (`fast_lz` 2-way row hash, rANS literals); **levels 2–6** use
the lazy hash-chain matcher (rising chain depth + entropy effort); **7–9** switch
to the cyclic-window binary tree with growing windows.

| level | matcher | profile |
|---|---|---|
| 1 (`--fast`) | `fast_lz` 2-way row hash | fastest; rANS literals, no lazy |
| 3 | hash, chain 32 | fast; lazy + rANS-literal entropy |
| 5 (default) | hash, chain 128 | balanced; lazy + full entropy bake-off |
| 6 | hash, chain 256 | best hash-chain ratio |
| 7 | bt, 8M window | long-range matches, still parallel |
| 8 | bt, 32M window | wider window |
| 9 (`--max`) | bt, full window | maximum ratio, slowest |

Individual flags override the preset (in any order): `--chain-depth N`, `--nice N`,
`--lazy` / `--no-lazy`, `--fast-entropy`, `--bt`, `--window SIZE`, `--block-size`,
`--threads`, `--parallel`, `--optimal[-depth N][-candidates N]`. `--window` sets
the bt match window and bounds its memory (≈ `2 × min(window, block) × 4` bytes).
The optimal parser (`--optimal`) is not folded into a level — its per-byte DP is
impractical on a large single block, so combine it with a smaller `--block-size`.

## Benchmark

The standing benchmark is **enwik8** (100 MB of English Wikipedia text).
`tools\bench_enwik8.ps1` downloads it on first run, sweeps the match finders and
window sizes, prints a 7-Zip reference, and verifies every row by round-trip
before reporting a ratio:

```powershell
.\tools\bench_enwik8.ps1                         # full sweep
.\tools\bench_enwik8.ps1 -Quick                  # skip the slow full-window rows
.\tools\bench_enwik8.ps1 -Axiomc out\Release\axiomc.exe
```

For ad-hoc inputs, the Python harness auto-detects `7z` on `PATH`, `SEVENZIP`, and
the standard `C:\Program Files\7-Zip\7z.exe` install path. It accepts either a
file or folder; folders are packed into a deterministic benchmark byte stream so
Axiom and 7-Zip compress the same input until Axiom has a native multi-file
archive.

```powershell
python bench/bench_7zip.py --axiom out/Release/axiomc.exe --input path/to/corpus
python bench/bench_7zip.py --axiom out/Release/axiomc.exe --input path/to/corpus --axiom-threads 8 --axiom-block-size 4M
python bench/bench_7zip.py --axiom out/Release/axiomc.exe --input path/to/corpus --axiom-parallel --axiom-threads 8
python bench/bench_7zip.py --axiom out/Release/axiomc.exe --input path/to/corpus --axiom-optimal --axiom-threads 8
python bench/bench_7zip.py --axiom out/Release/axiomc.exe --input path/to/corpus --axiom-optimal-depth 64 --axiom-optimal-candidates 8 --axiom-threads 8
```

## Performance

Speed and ratio are selected with `--level` (above). Levels share most building
blocks — repeat-offset matches, split streams, 4-lane rANS, parallel independent
blocks, eight-byte-at-a-time match comparison — but differ in matcher and entropy
effort. Higher levels search harder (deeper chains → cyclic-window binary tree →
optimal parser) and run the full entropy bake-off; the fast levels (1–3) use a
shallow/row-hash matcher and code literals with rANS instead of the slow
bit-serial order-1 coder, which is what keeps their **decode** fast.

Compressed ratio by level on **enwik8** (100 MB text; exact, reproducible):

| level | 1 | 3 | 5 (default) | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|
| ratio | 2.68× | 3.01× | 3.09× | 3.10× | 3.17× | 3.33× | 3.47× |

Throughput is machine-dependent; ratios above are exact. On an Intel i7-13620H
laptop (MSVC Release), **level 1** runs ~140 MB/s compress and ~476 MB/s decode
(to NUL) — within ~1.2×/1.8× of 7-Zip `-mx1` (162 / 860 MB/s) at a ~10% ratio
deficit. Higher levels trade compress speed for ratio down to ~1 MB/s at level 9;
decode stays fast on levels 1–3 (rANS literals) and is slower on 4–9 (order-1
literals). 7-Zip `-mx9` still leads on ratio (4.03×) — the remaining gap is the
entropy stage, not the matcher. See
[ROADMAP.md](ROADMAP.md#benchmark-snapshot-enwik8-100-mb-vs-7-zip-lzma2) for the
full analysis, and `tools\bench_enwik8.ps1` to reproduce.

## Testing and fuzzing

`AxiomRoundtrip` is the test suite: codec roundtrips, a multi-file archive
roundtrip, and deterministic safety tests (truncation, bad magic, zip-slip
rejection, decompression-bomb rejection, integrity checks), plus an in-process
mutation-fuzz pass. It runs in every configuration (checks abort on failure
rather than being compiled out).

```powershell
.\tools\test_msvc.ps1 -Configuration Release    # or: ctest --preset default
```

Coverage-guided fuzzing runs libFuzzer under AddressSanitizer over the two
untrusted-input surfaces — the single-stream decoder and the archive container
parser. MSVC ships the libFuzzer runtime, so no external toolchain is needed:

```powershell
.\tools\build_fuzz.ps1 -Target all
.\tools\run_fuzz.ps1 -Seconds 60 -Target all
```

CI (`.github/workflows/ci.yml`) builds and tests on Windows and Linux and runs
both fuzz targets on every push, with a longer nightly run.

## Roadmap

1. ~~Add binary-tree match finding~~ — done: cyclic-window bt4 with node
   deletion (`--bt`), memory bounded by `--window`. Next: make it the default
   where it wins (see [ROADMAP.md](ROADMAP.md)).
2. Add file classification and solid block grouping.
3. Add executable, delta, text, JSON/XML, and binary record transforms.
4. Add long-distance matching across entire solid groups.
5. Add context models for literals, lengths, and distances (the largest
   remaining gap to LZMA: adaptive, context-mixed entropy coding).
