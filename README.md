# AxiomCompress

[![CI](https://github.com/Wimukthi/AxiomCompress/actions/workflows/ci.yml/badge.svg)](https://github.com/Wimukthi/AxiomCompress/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/Wimukthi/AxiomCompress)](https://github.com/Wimukthi/AxiomCompress/releases/latest)
[![License: GPL v3](https://img.shields.io/badge/license-GPLv3-blue.svg)](LICENSE)
[![Platform: Windows x64](https://img.shields.io/badge/platform-Windows%20x64-lightgrey.svg)](#installation)

Axiom packs files into a single smaller file, the way WinRAR or 7-Zip do. It
comes with a Windows app, a command-line tool, and its own archive format.

What makes it different is where it spends effort. Packing an archive is
allowed to be slow and to work hard for a smaller result. Unpacking is not:
the decoder does no searching, keeps to a memory limit it reads from the file
itself, and runs at roughly the same speed no matter how hard the packer
worked. An archive made at the highest setting opens as fast as one made at
the lowest.

> **This is pre-release software.** The archive format can still change between
> minor versions, so an archive written today may not open in a much later
> build. Keep independent backups of anything you care about.

![The Axiom archive browser showing an open archive in dark mode](docs/images/axiom-gui.png)

| Creating an archive | The command line |
|---|---|
| [![The Add to archive dialog, showing compression settings and a live size preview](docs/images/axiom-add-to-archive.png)](docs/images/axiom-add-to-archive.png) | [![The interactive Axiom command shell listing an archive](docs/images/axiom-cli.png)](docs/images/axiom-cli.png) |

## What you get

**A Windows app that behaves like a file manager.** Browse folders and
archives in the same window, drag files in and out, and run every archive
command from the toolbar or menus. It is written directly against Win32 — no
Qt, .NET, WinUI, or embedded browser — with dark mode and per-monitor DPI
support.

**A command-line tool** that drives the identical engine, for scripts and
scheduled backups.

**Five ways to compress.** Axiom's own adaptive method is the default.
Zstandard, LZMA2, Deflate, and Store are also available when you want a
known quantity or need to skip compression entirely.

**Files that are checked, not just stored.** Every block carries a CRC-32, and
every file carries both a CRC-32 and a BLAKE3-256 hash. The `test` command
verifies all of them.

**Optional protection, when you want it.** Password encryption (Argon2id key
derivation with XChaCha20-Poly1305), optionally hiding file names as well;
recovery data that can repair a damaged archive; split volumes for transport;
and signatures that prove who made an archive.

**Self-extracting output.** Turn an archive into a standalone `.exe` that
unpacks itself, so the recipient needs nothing installed.

**It reads what you already have.** Full read and write for ZIP. Read-only
support for 7z, RAR, ISO/UDF, CAB, and the TAR family on Windows.

## Installation

Download from the [latest release](https://github.com/Wimukthi/AxiomCompress/releases/latest):

| File | Use it if |
|---|---|
| `AxiomSetup-<version>-win-x64.exe` | You want a normal install, with a Start Menu entry |
| `Axiom-<version>-win-x64.zip` | You want to unzip and run, with no installer |

If you use the zip, keep `AxiomSfx.bin`, `AxiomSfxMini.bin`, and the
`backends\` folder next to `Axiom.exe` and `axiomc.exe`. The two `.bin` files
are not programs you can run — they are the extractor code that gets built
into a self-extracting archive. The `backends\7zip\` folder is what lets Axiom
read 7z, RAR, ISO, and CAB files.

To build it yourself, see [Building](#building).

## Quick start

### The Windows app

Run `Axiom.exe`. The main window browses your folders. Open an archive and it
browses that instead, in the same list. Drag files in to add them, drag entries
out to extract them, and use the toolbar or the **Archive** menu for everything
else.

Full walkthrough: [docs/GUI_GUIDE.md](docs/GUI_GUIDE.md).

### The command line

Four commands cover most of what people do:

```powershell
axiomc a archive.axar mydir file.txt   # create an archive, or add to one
axiomc l archive.axar                  # list what's inside
axiomc t archive.axar                  # check it is undamaged
axiomc x archive.axar restored         # extract it
```

Some common variations:

```powershell
axiomc a --level 9 archive.axar mydir                                   # smallest result
axiomc a --method lzma2 --codec-level 7 archive.axar mydir               # use LZMA2 instead
axiomc a -p "correct horse battery staple" --encrypt-names private.axar secrets
axiomc a --recovery 10 backup.axar "D:\Work"                             # add repair data
axiomc sfx archive.axar installer.exe                                    # make a self-extractor
```

Running `axiomc` with no arguments opens an interactive prompt. Full reference:
[CLI_GUIDE.md](CLI_GUIDE.md).

## Compression levels

`--level 1..9` is the one dial most people need. It trades time for size. The
default is 5, `--fast` is 1, and `--max` is 9. Extraction speed is the same at
every level.

| Level | What it does differently | Good for |
|---:|---|---|
| 1 | Checks a few places for repeated data and moves on | The lowest CPU cost |
| 2–3 | Searches a short list of candidates, weighing cost before committing | Fast routine backups |
| 4–5 | Searches a longer list | General use (default) |
| 6 | Searches a much longer list | A better result without extra memory |
| 7 | Sorted-tree search, looks one byte ahead before choosing, 8 MiB window | Long-range repetition, without the cost of level 8 |
| 8 | Sorted-tree search plus a full cheapest-path calculation, 32 MiB window | A high ratio at moderate cost |
| 9 | The same, deeper, with measured rather than estimated costs, 64 MiB window | The smallest archive |

Levels 8 and 9 don't just take the first good match they find — they work out
the cheapest way to encode the whole block and follow that. It costs time and
memory; it wins several percent.

Individual flags (`--chain-depth`, `--nice`, `--bt`, `--window`, `--optimal…`)
override whatever the level chose. A level is a starting point, not a lock.

### About the large size limits

The window and dictionary controls for Axiom's own method and for LZMA2 accept
values up to 4 GiB. (The number actually written into the file is 4 GiB−1, the
largest a 32-bit field holds. Choosing 4 GiB gets you that.)

Separately, an AXAR archive using LZMA2 can group up to 64 GiB of data into a
single solid block. That mode writes the raw data to a temporary file and
compresses it in bounded pieces, so a 64 GiB block does not need 64 GiB of
memory. It cannot currently be combined with encryption or recovery records,
and older versions of Axiom will refuse to open such an archive rather than
misread it. Details: [FORMAT.md](FORMAT.md) and the
[CLI guide](CLI_GUIDE.md#block-size).

## How fast, and how small

Measured on the **Silesia corpus** packed into one 211.9 MB tar file, so every
compressor sees byte-identical input. AMD Ryzen 9 5950X (16 cores / 32
threads), warm cache, Release build. Every row was decompressed and compared
against the original before it was recorded.

> These are the **0.8.0.0** figures, measured 2026-07-31.

| Compressor and setting | Compressed | Ratio | Pack | Unpack |
|---|---:|---:|---:|---:|
| zstd -3 | 66.2 MB | 3.20x | 0.12 s | 0.17 s |
| **Axiom -5** (default) | 56.5 MB | 3.75x | 2.91 s | 0.24 s |
| WinRAR -m5 128M | 53.2 MB | 3.99x | 3.23 s | 0.45 s |
| zstd -19 | 52.8 MB | 4.01x | 16.78 s | 0.17 s |
| **Axiom -9** | 51.4 MB | 4.12x | 16.02 s | 0.24 s |
| LZMA2 -mx9 | 48.7 MB | 4.35x | 35.02 s | 1.21 s |

Axiom at level 9 lands between zstd's high-ratio settings and LZMA2. It is 2.8%
smaller than zstd -19 at about the same packing time. It is 5.6% larger than
LZMA2 -mx9, but packs 2.2x faster and unpacks 5.1x faster.

Full tables, charts, and the enwik8 results:
[docs/PERFORMANCE.md](docs/PERFORMANCE.md). To measure on your own machine:
[docs/BENCHMARKING.md](docs/BENCHMARKING.md).

## Which formats work

| Format | Browse | Extract | Test | Create and edit |
|---|:--:|:--:|:--:|---|
| AXAR | ✓ | ✓ | ✓ | Everything |
| ZIP | ✓ | ✓ | ✓ | Everything for unencrypted; new archives can use AES-256 |
| 7z, RAR, ISO, CAB | Windows | Windows | Windows | — |
| TAR family | Windows | Windows | Windows | — |

AXAR and ZIP are the only formats Axiom can create. The read-only formats go
through the bundled `7z.dll` engine and Windows' own `tar.exe`. Details and
roadmap: [docs/FORMAT_SUPPORT.md](docs/FORMAT_SUPPORT.md).

## Building

You need a C++20 compiler. Windows builds also need the sibling
[`Wimukthi.Win32Theme`](https://github.com/Wimukthi/Wimukthi.Win32Theme)
repository checked out next to `AxiomCompress`, or its path passed explicitly.

### Visual Studio / MSBuild

```powershell
.\tools\build_msvc.ps1 -Configuration Release
.\tools\test_msvc.ps1  -Configuration Release
```

Add `-ThemeRoot <path>` if the theme framework isn't in the sibling directory.
Binaries land in `out\Release\`.

A normal GUI build bumps the fourth part of the version number in
`src\gui\axiom_gui.rc`, which leaves a change in your working tree. To build
without that:

```powershell
.\tools\build_msvc.ps1 -Configuration Release -AutoIncrementVersion:$false
```

### CMake

```powershell
cmake --preset default
cmake --build --preset default
ctest --preset default
```

Use `--preset vs2022` if Ninja isn't available, and
`-DWIMUKTHI_WIN32_THEME_ROOT=<path>` to point at the theme dependency
elsewhere. Non-Windows builds produce the library, the CLI, and the tests only.

### What gets built

| Target | Output | What it is |
|---|---|---|
| `AxiomLib` / `axiom` | `axiom.lib` | The archive and codec engine |
| `AxiomC` / `axiomc` | `axiomc.exe` | The command-line tool |
| `AxiomGui` / `axiom_gui` | `Axiom.exe` | The Windows app |
| `AxiomSfxDecodeLib` / `axiom_sfx_decode` | `AxiomSfxDecodeLib.lib` | Read-only AXAR/ZIP runtime for self-extractors |
| `AxiomSfx` / `axiom_sfx_module` | `AxiomSfx.bin` | The full self-extractor runtime |
| `AxiomSfxMini` / `axiom_sfx_mini_module` | `AxiomSfxMini.bin` | The console-only self-extractor runtime |
| `AxiomRoundtrip` / `axiom_roundtrip` | `axiom_roundtrip.exe` | The test suite |

## Testing

```powershell
.\tools\test_msvc.ps1 -Configuration Release   # or: ctest --preset default
.\tools\test_sfx_runtime.ps1 -BuildRoot .\out -Configuration Release
.\tools\test_sfx_footprint.ps1 -BuildRoot .\out -Configuration Release
.\tools\build_fuzz.ps1 -Target all
.\tools\run_fuzz.ps1 -Seconds 60 -Target all
```

The round-trip suite covers the codec, the container, the safety rules, and an
in-process mutation fuzzer. CI builds and tests on Windows, Linux, and macOS,
and runs both decode-surface fuzz targets on Linux and Windows for every push.
The scheduled run fuzzes for longer.

## Documentation

| Document | What's in it |
|---|---|
| [docs/GUI_GUIDE.md](docs/GUI_GUIDE.md) | Using the Windows app |
| [CLI_GUIDE.md](CLI_GUIDE.md) | Every `axiomc` command and option |
| [docs/GLOSSARY.md](docs/GLOSSARY.md) | What the terminology means, in plain language |
| [docs/FORMAT_SUPPORT.md](docs/FORMAT_SUPPORT.md) | What Axiom can do with each archive format |
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the codec and container are built, and why |
| [FORMAT.md](FORMAT.md) | The `.axar` and `.axc` byte-level specification |
| [docs/SFX_ARCHITECTURE.md](docs/SFX_ARCHITECTURE.md) | How the self-extractor works |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | Published benchmark results |
| [docs/BENCHMARKING.md](docs/BENCHMARKING.md) | How to measure a change yourself |
| [docs/INSTALLER.md](docs/INSTALLER.md) | Building the release packages |
| [docs/VERSIONING.md](docs/VERSIONING.md) | Version numbering and release steps |
| [docs/GAP_ANALYSIS_LZMA2.md](docs/GAP_ANALYSIS_LZMA2.md) | Research notes on the remaining LZMA2 ratio gap |
| [docs/STYLE.md](docs/STYLE.md) | How this documentation is written |
| [CHANGELOG.md](CHANGELOG.md) | Release history |

## License

AxiomCompress is licensed under the
[GNU General Public License v3](LICENSE). Bundled components keep their own
licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
