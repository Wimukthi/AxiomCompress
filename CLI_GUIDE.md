# axiomc — command-line guide

`axiomc` is Axiom's command-line archiver. It handles `.axar` multi-file
archives and `.axc` single-stream files, and it drives the same engine as the
[Windows GUI](docs/GUI_GUIDE.md).

```text
axiomc <command> [options] <archive> [paths...]
```

Options may appear before or after positional paths.

> **Paths after an archive name are paths *inside* the archive**, not
> filesystem paths, and they use forward slashes. Run `axiomc l <archive>` to
> see the exact stored paths before deleting, moving, or selectively extracting.

## Contents

- [Getting started](#getting-started)
  - [Interactive shell](#interactive-shell)
  - [Command reference](#command-reference)
  - [Creating and updating](#creating-and-updating)
  - [Sequential output](#sequential-output)
  - [Reading](#reading)
  - [Capture reports and metadata fidelity](#capture-reports-and-metadata-fidelity)
  - [Maintenance](#maintenance)
  - [Recovery and volumes](#recovery-and-volumes)
  - [Signing](#signing)
  - [Self-extracting archives](#self-extracting-archives)
  - [Single-stream files](#single-stream-files)
- [Compression options](#compression-options)
- [Encryption](#encryption)
- [Exit codes](#exit-codes)
- [Recipes](#recipes)

## Getting started

After a Release build the executable is `out\Release\axiomc.exe`.

```powershell
axiomc a backup.axar Documents    # create
axiomc l backup.axar              # list
axiomc t backup.axar              # test
axiomc x backup.axar restored     # extract
```

Quote paths containing spaces:

```powershell
axiomc a "D:\Backups\Work files.axar" "D:\Work files"
```

Input directories are added recursively. An input is stored relative to its
parent, so adding `D:\Data\Reports` creates a top-level `Reports/` entry.

## Interactive shell

Running `axiomc` with no arguments opens a prompt instead of printing usage and
exiting. `axiomc shell` and `axiomc --interactive` do the same thing
explicitly.

```text
axiom> a backup.axar "D:\Work files"
axiom> l backup.axar
axiom> exit
```

Commands are typed without the leading `axiomc`. Built-ins:

| Command | Effect |
|---|---|
| `help` | Print the full command help |
| `pwd` | Print the working directory |
| `cd <dir>` | Change the working directory |
| `clear`, `cls` | Clear the console |
| `exit`, `quit` | Close the prompt |

With arguments supplied, `axiomc` stays script-friendly: it runs the command,
prints no splash, and exits.

## Command reference

| Command | Purpose |
|---|---|
| `a`, `add` | Create an archive, or add and replace entries |
| `u`, `update` | Add new entries and replace older ones |
| `f`, `fresh` | Replace existing entries only, never add |
| `s`, `sync` | Mirror the inputs, including deletions |
| `delete`, `rm` | Delete entries or directory subtrees |
| `repack` | Rebuild and reclaim dead space |
| `comment` | Show, set, or clear the archive comment |
| `lock` | Permanently mark an archive read-only |
| `password-add` | Add a password slot without rewriting block payloads |
| `password-change` | Replace the slot authenticated by the current password |
| `password-remove` | Remove a password slot |
| `snapshot` | Create, append, list, diff, restore, and prune deduplicated AXAR snapshots |
| `recovery`, `rr` | Show, add, replace, or remove a recovery record |
| `repair` | Repair damage using the recovery record |
| `split` | Create numbered data and recovery volumes |
| `join` | Join or reconstruct a volume set |
| `x`, `extract` | Extract |
| `l`, `list` | List contents and archive state |
| `capture-report` | Show source metadata or unreadable-input warnings persisted in an AXAR archive |
| `t`, `test` | Decode and verify integrity |
| `keygen` | Generate a signing key pair |
| `sign` | Sign an archive |
| `verify` | Verify an archive signature |
| `sfx` | Build a self-extracting Windows executable |
| `c`, `compress` | Compress one stream to `.axc` |
| `d`, `decompress` | Decompress one `.axc` stream |
| `help`, `-h`, `--help`, `/?` | Print help |

### Creating and updating

```text
axiomc a [options] <archive.axar> <input>...
axiomc u [options] <archive.axar> <input>...
axiomc f [options] <archive.axar> <input>...
axiomc s [options] <archive.axar> <input>...
```

| Command | Adds new | Replaces existing | Deletes missing |
|---|:--:|:--:|:--:|
| `a` (add) | ✓ | always | — |
| `u` (update) | ✓ | if source is newer | — |
| `f` (fresh) | — | if source is newer | — |
| `s` (sync) | ✓ | if source is newer | ✓ |

```powershell
axiomc a photos.axar "D:\Photos"
axiomc a project.axar README.md src include assets
axiomc u project.axar src assets README.md
axiomc s mirror.axar "D:\Current project"
```

`sync` is destructive — list and test the result before deleting an older
backup.

Update, freshen, and synchronize run as one planned transaction: unchanged
compressed blocks are copied without decompression, changed files are
compressed once, stale entries are dropped from the final directory, and any
existing recovery record is rebuilt once against the result. A no-change
synchronize exits without rewriting the archive.

When an added or updated file has the same size, CRC-32, and BLAKE3 content
identity as data already stored in the AXAR, Axiom reuses that existing data
range instead of appending another compressed copy. This also applies to
`fresh` and `sync`; the operation progress line reports the reused item count
and source bytes. Reuse is automatic, keeps the current AXAR format, and falls
back to normal compression when the source cannot be safely compared.

When the archive header feature set does not need to change, an update appends a
new AXAR generation. Existing block bytes and the previous footer remain in the
file, so a completed update does not depend on copying the old block region.
Header-changing operations and compacting operations such as delete/repack use
the atomic temporary-file path instead. A partially written append is ignored
by generation-aware readers, which fall back to the previous valid footer.

### Snapshot repositories

Snapshot repositories use the AXAR v5 chunk-table profile. Files are split at
content-defined boundaries and identical chunks are stored once, so a later
snapshot normally appends only changed chunks plus a new manifest. The current
snapshot is also the live AXAR directory: ordinary `l`, `t`, and `x` commands
continue to operate on it.

```powershell
axiomc snapshot create [options] repository.axar base "D:\Project"
axiomc snapshot add [options] repository.axar nightly "D:\Project"
axiomc snapshot list [-p "password"] repository.axar
axiomc snapshot diff [-p "password"] repository.axar base nightly
axiomc snapshot restore [--overwrite all] repository.axar base restored
axiomc snapshot prune repository.axar base
axiomc repack repository.axar
```

`snapshot prune` removes historical manifests but refuses to remove the
current snapshot. `repack` is the snapshot garbage collector: it retains chunks
reachable from the current and remaining manifests, remaps their block
addresses, and atomically replaces the repository. Use `--recovery N` on
creation or append to retain recovery data, and `--strict-metadata` on restore
when fidelity loss must fail rather than warn.

Snapshot chunk identifiers are keyed by the archive data key by default when
the repository is encrypted. Use `--plain-chunks` to make identities stable
across encrypted repositories, or `--keyed-chunks` to select the default
explicitly. The chunk geometry can be tuned with `--chunk-min SIZE`,
`--chunk-average SIZE`, and `--chunk-max SIZE`; values are bounded and must be
ordered. Ordinary `a`, `u`, `f`, `s`, `delete`, and `move` content mutations are
rejected for a snapshot repository so they cannot discard historical manifests;
use `snapshot add` for content and `snapshot prune`/`repack` for maintenance.

Snapshot commands accept the same password, thread, compression, encryption,
recovery, and progress options as their corresponding archive operation.

### Sequential output

Use a literal `-` as the archive operand to create an AXAR v5 archive on
standard output:

```text
axiomc a - <input>...
```

On Windows, redirect binary output through `cmd.exe` so the shell does not
reinterpret the byte stream:

```text
cmd.exe /c "axiomc.exe a - D:\Work > D:\Backups\work.axar"
```

The sequential profile supports normal compression and encryption, and is
useful for pipes or another process that owns the output stream. It reserves
the known metadata flags in the header because the stream cannot seek back to
patch them. Recovery records are rejected for this profile, and signing should
be performed after the stream has been materialized as a regular archive file.

### Reading

```text
axiomc l [-p <password>] <archive.axar>
axiomc t [--threads N] [-p <password>] <archive.axar>
axiomc x [options] <archive.axar> [destination]
```

Listing identifies directories, symbolic links, hard links, comments, lock and
encryption state, entry count, and total uncompressed bytes. `test` decodes and
verifies integrity without writing anything.

The extraction destination defaults to the current directory. Overwrite
behavior is explicit:

```powershell
axiomc x --overwrite fail project.axar restored   # stop on conflict (default)
axiomc x --overwrite skip project.axar restored   # keep existing files
axiomc x --overwrite all  project.axar restored   # replace existing files
```

Extracted paths are contained within the destination; unsafe traversal and
reparse-point ancestors are rejected.

Use repeatable `--include` options to extract only selected archive paths. A
directory selection includes its descendants:

```powershell
axiomc x --include src/app.exe --include src/config project.axar restored
```

For AXAR, selected extraction automatically uses the optional subframe map
when an archive block provides independently decodable frames. Legacy archives,
encrypted blocks, transformed blocks, and codecs without independent frames
fall back to safe whole-block reads. Interactive CLI progress shows the
physical `archive read` byte count during testing and extraction; this includes
directory reads and is useful for confirming that a narrow selection avoided
unrelated compressed frames.

### Capture reports and metadata fidelity

AXAR archives preserve sparse-file allocation maps when the source filesystem
supports them. The logical bytes remain fully compressed and checksummed; the
map controls whether extraction recreates zero-filled holes as unallocated
space. Use `--no-sparse` when a deliberately dense restore is preferred.

If creation is allowed to skip an unreadable input, or a sparse map cannot be
queried, Axiom persists an explicit capture warning in the archive and reports
it through the operation UI:

```powershell
axiomc capture-report backup.axar
axiomc capture-report -p secret backup.axar
```

The command exits `0` when capture is complete and `1` when warnings are
present. Add `--strict-metadata` to creation/update or extraction when any
capture or sparse-restoration loss should fail the operation:

```powershell
axiomc a --strict-metadata backup.axar "D:\Data"
axiomc x --strict-metadata backup.axar restored
```

Archives with sparse maps, capture reports, or extended ACL/xattr/reparse
metadata use AXAR v5; dense archives without those required features may remain
v4, and all existing v4 archives continue to open unchanged.

### Maintenance

```powershell
axiomc delete project.axar "project/build.log" "project/cache"
axiomc repack --level 7 project.axar
axiomc comment project.axar                       # show
axiomc comment project.axar "Nightly backup"      # set
axiomc comment project.axar ""                    # clear
axiomc lock project.axar
```

Deleting a directory removes its whole subtree, and the archive is rebuilt so
the space is physically reclaimed. `repack` keeps every entry and only reclaims
dead space left by earlier replacements; it also coalesces duplicate file
content into one stored data range.

Locking is one-way — there is no unlock command. A locked archive can still be
listed, tested, extracted, and verified, but not edited, repacked, re-signed,
or re-commented.

Password slot management is separate from content editing:

```powershell
axiomc password-add -p "current password" private.axar "backup password"
axiomc password-change -p "current password" private.axar "new password"
axiomc password-remove -p "current password" private.axar "backup password"
```

The current password must unlock the archive. Adding and removing slots does
not recompress or re-encrypt existing blocks. The last remaining slot cannot be
removed. Changing password slots invalidates an archive signature, so sign the
archive again when signature verification is part of your workflow.

### Recovery and volumes

Reed-Solomon recovery data can be added at creation time or later:

```powershell
axiomc a --recovery 10 backup.axar "D:\Work"
axiomc recovery backup.axar      # show the current record
axiomc recovery backup.axar 15   # rebuild at 15%
axiomc recovery backup.axar 0    # remove
```

Valid percentages are `1..100`; `0` removes. Normal edits preserve the existing
percentage and regenerate parity afterwards.

For append generations, recovery protects the new directory and its 64-byte
generation record as well as the preceding archive bytes. Recovery metadata is
written after the generation record and before the final legacy footer, so a
generation-aware reader can fall back to the previous footer if an update is
interrupted before the new footer is complete.

If `test` reports damage and the recovery locator is readable:

```powershell
axiomc repair backup.axar
axiomc t backup.axar
```

`repair` returns exit code 3 when there is no recovery record. Damage beyond
the parity-shard count cannot be repaired — a recovery record is not a
substitute for a second backup.

Split a completed archive into transport volumes:

```text
axiomc split <archive.axar> <size> [recovery-volume-count]
axiomc join <any-volume> <output.axar>
```

```powershell
axiomc split backup.axar 100M 3      # backup.part001.axar… + backup.rev001…003
axiomc join backup.part001.axar restored.axar
```

The source archive is preserved. A set is limited to 255 total volumes. Any
surviving data or recovery volume identifies the set, and joining checks every
payload CRC plus the BLAKE3 digest of the reconstructed archive.

When every data volume is present, ordinary read commands work directly on any
numbered volume — no `join` needed. The set is exposed read-only.

### Signing

```text
axiomc keygen <secret.key> <public.key>
axiomc sign   [options] <archive.axar> <secret.key>
axiomc verify [options] <archive.axar> [public.key]
```

```powershell
axiomc keygen release-secret.key release-public.key
axiomc sign release.axar release-secret.key
axiomc verify release.axar release-public.key
```

The secret key is 64 bytes and must be protected; the public key is 32 bytes
and can be distributed. Verifying without a key checks cryptographic validity
using the key embedded in the signature. Supplying a key additionally requires
that specific key. Signing an existing file archive appends a generation whose
signature covers the generation history and current directory semantics;
editing it later invalidates that signature. Sequential stdout archives must be
materialized before signing. See [exit codes](#exit-codes) for the distinct
results.

### Self-extracting archives

```text
axiomc sfx [--config <file>] [--stub full|mini] <archive.axar-or-zip> <output.exe> [compatible-stub]
```

```powershell
axiomc sfx release.axar release-setup.exe
axiomc sfx --config setup.ini release.axar release-setup.exe
axiomc sfx --stub mini build.axar unpack-build.exe
```

`--stub` chooses the extractor runtime. `full` (the default) shows dialogs;
`mini` is console-only and runs unattended, which suits an artifact unpacked by
a script. Both understand the same options and the same configuration, while
`mini` uses the decode-only AXAR/ZIP runtime and excludes archive writers and
encoder backends. The Release Mini module is about 713 KiB versus about 2.17 MiB
for the full module.

By default this uses the `AxiomSfx.bin` module beside `axiomc.exe`. That module
is not an executable and is read only while creating an SFX. Supplying an
explicit compatible PE image is available for development and compatibility
testing.

The output is one standalone Windows executable containing Axiom's read-only
extraction runtime, an Axiom SFX descriptor, and the intact `.axar` or `.zip`
payload. It does not contain the file-manager GUI, and the source archive is
not referenced when the `.exe` runs.

The result can be Authenticode-signed. Sign the **generated executable**, not
the stub — packaging onto an already-signed stub is refused, because appending
to it would invalidate that signature:

```powershell
axiomc sfx release.axar release-setup.exe
signtool sign /fd SHA256 /tr <timestamp-url> /td SHA256 release-setup.exe
```

Because Authenticode covers trailing data, the signature attests to the
archive payload as well as to the extractor. Executables produced before
0.8.0.0 used a layout that could not be signed at all; they are still read
correctly.

The generated extractor shows a native dialog with an editable destination and
folder browser; file and folder counts, unpacked size, encryption state, and
signature state; replace/skip/stop conflict policies; automatic or explicit
thread count; optional modification-time restore and destination-folder
opening; and progress with pause, resume, and cancel.

Encrypted payloads ask for their password before revealing metadata. Signed
payloads are verified first, and an invalid signature blocks extraction.

#### Running a generated extractor

```text
release-setup.exe [options] [destination]

  -o, --output <dir>       extract into <dir>
  -s, --silent             progress and errors only
      --very-silent        no window at all
  -y, --accept             accept the license, assume yes
  -p, --password <text>    password for an encrypted payload
      --password-stdin     read the password from standard input
      --overwrite <mode>   replace, skip, or fail
      --threads <n>        worker threads, 0 for automatic, otherwise 1..4096
      --include <pattern>  extract only matching entries, repeatable
      --no-run             do not run the configured program
      --list               print the contents and exit
      --test               verify integrity and exit
      --log <file>         append a UTF-8 transcript to <file>
  -?, --help               show usage
```

A bare positional destination still works, as it always has:

```powershell
.\release-setup.exe "D:\Applications\Release"
```

Exit codes are stable and scriptable: 0 success, 1 extraction failed, 2 usage,
3 cancelled, 4 wrong or missing password, 5 integrity or signature failure,
6 not enough free space, 7 elevation required and unavailable, 8 the
run-after-extract program failed to start. With `propagate_exit_code`, a code
above 8 is that program's own.

The extractor is a GUI binary, so it never flashes a console when
double-clicked, but it attaches to the console that launched it — and honours
redirection, so `release-setup.exe --list > contents.txt` works.

#### Configuring an extractor

`--config` takes an INI-style file. Keys are optional; unknown keys are an
error rather than being ignored.

```ini
title = Contoso Toolkit
window_title = Contoso Toolkit Setup
description = Installs the Contoso build tools.
default_path = %ProgramFiles%\Contoso\Toolkit
create_subfolder = false
allow_path_change = true

license_text = By continuing you agree to the terms.\nSee LICENSE.txt.
require_accept = true

mode = interactive
overwrite = replace
threads = 0
open_destination = true
auto_close = false
elevation = auto

run_program = setup\install.exe
run_arguments = /quiet
run_working_dir = setup
wait_for_exit = true
propagate_exit_code = true
```

`default_path` accepts `%ProgramFiles%`, `%ProgramFiles(x86)%`,
`%LOCALAPPDATA%`, `%APPDATA%`, `%USERPROFILE%`, `%DESKTOP%`, `%DOCUMENTS%`,
`%TEMP%`, plus `%SFXDIR%` and `%SFXNAME%` for the extractor's own folder and
name. Shell folders resolve through the Windows known-folder API, not through
environment variables, so `%ProgramFiles%` cannot be redirected by setting a
variable before launch. A template with an unknown token or a `..` component is
rejected when the SFX is built, not on the user's machine.

`threads` accepts zero for automatic selection or a value from 1 through 4096.
`run_program` and `run_working_dir` are relative to the extraction root. They
must not be absolute, drive-rooted, contain `..` or `:`, or pass through a
symlink/reparse point; the program must be a regular file produced by the
extraction and the working directory must be a real extracted directory.
`elevation = auto` decides by testing whether the destination is writable, then
relaunches through the `runas` verb if it is not. The parent waits for that
child and returns its exit code. A password passed with `--password` or
`--password-stdin` is never forwarded to the elevated instance, where it would
be visible in the process list; that combination is refused instead. An
interactively entered password is cleared and requested again by the elevated
full stub.

`--log` appends UTF-8 status output and the final exit code. It refuses to open
the SFX executable itself as the log target and is forwarded safely across an
elevation relaunch.

Some combinations are rejected outright at build time: `require_accept`
without license text, `run_arguments` without `run_program`, and the fully
unattended privileged chain of `mode = very_silent` with
`elevation = require` and a `run_program`.

`allow_file_selection` is retained as a reserved compatibility key but is not
implemented; use repeatable `--include` patterns for selective extraction.

`axiomc sfx` preserves its input archive, because it converts an archive the
caller already has. The GUI's **Create a self-extracting Windows executable**
option instead treats the intermediate `.axar` as a build artifact and removes
it once the merged `.exe` succeeds.

### Single-stream files

```text
axiomc c [options] <input> <output.axc>
axiomc d [--threads N] [-p <password>] <input.axc> <output>
```

```powershell
axiomc c --level 5 database.bin database.axc
axiomc d database.axc restored-database.bin
```

`.axc` is one compressed stream. It has no multi-file directory, comments, file
metadata, or editing features.

## Compression options

These apply to commands that create or recompress data: `add`, `update`,
`fresh`, `sync`, `delete`, `repack`, `sign`, and `compress`.

### Methods

| Method | `--method` | Method-specific controls |
|---|---|---|
| Axiom adaptive (default) | `axiom` | `--level 1..9`, window, nice length, threading model |
| Zstandard | `zstd` | native level `-5..22` |
| LZMA2 | `lzma2` | native level `0..9`, 4 KiB–4 GiB dictionary, 5–273 fast bytes, HC4/BT4 |
| Deflate | `deflate` | native level `0..9`; fixed 32 KiB window, 258-byte max match |
| Store | `store` | none |

```powershell
axiomc a --method zstd --codec-level 12 archive.axar Data
axiomc a --method lzma2 --codec-level 7 --window 128M --block-size 128M --lzma-mf bt4 archive.axar Data
axiomc a --method store archive.axar already-compressed-media
```

Without `--codec-level`, the portable `--level 1..9` maps to a suitable native
level. Zstandard maps levels 1–9 to native 1, 2, 3, 5, 7, 10, 14, 18, 22;
LZMA2 and Deflate use the portable value directly.

For LZMA2, `--window` and `--nice` become the dictionary and fast-bytes
settings. The dictionary is clamped to the independently decoded AXC chunk
(the effective `--block-size`) and to a stable bound for the input represented
by that payload. Values up to 4 GiB
are accepted; the on-stream ceiling is 4 GiB−1. To request a 4 GiB LZMA2
window for ordinary streams, select both `--window 4G` and `--block-size 4G`.
Large dictionaries can use several times their size in encoder memory, so a
small payload is automatically capped to its own size while multi-chunk
payloads retain one property across their final short chunk. `--lzma-mf hc4`
favors speed, `bt4` favors ratio.

ZIP creation supports only `deflate` and `store`. Omitting `--method` keeps the
established ZIP Deflate behavior.

Every external-codec payload is split into independently bounded chunks. Pause,
cancel, and progress checkpoints happen between chunks, and an incompressible
chunk is stored rather than expanded. Encryption, recovery, signing, metadata,
volumes, and SFX are container services and do not vary with the method for
ordinary blocks. The large-solid LZMA2 profile is the exception: it currently
does not allow encryption or recovery records because those paths require
whole-block buffers.

### Levels

`--level N` where N is 1–9, default 5. `--fast` is level 1 and `--max` is
level 9.

| Level | Matcher and parser | Typical use |
|---:|---|---|
| 1 | Dedicated fast LZ path | Minimum CPU time |
| 2–3 | Shallow hash chain, price-aware lazy | Fast backups |
| 4–5 | Balanced hash chain, price-aware lazy | General use |
| 6 | Deep hash chain | Better ratio, no tree memory |
| 7 | Binary tree + cost-aware lazy lookahead, 8 MiB window | Long-range redundancy without optimal-parse cost |
| 8 | Binary tree + single-pass optimal parse, 32 MiB window | High ratio at moderate cost |
| 9 | Deep binary tree + measured-cost optimal parse, 64 MiB window, 4 KiB matches | Maximum ratio |

Levels 8 and 9 run the dynamic-programming optimal parser with candidates from
the binary tree; on generic data they compress substantially smaller than 6–7.
Level 7 uses a cheaper one-byte, token-cost-aware lookahead — it can defer an
expensive match for a better match or repeat offset at the next position
without paying for a full DP parse.

Explicit tuning options override the level regardless of argument order.

### Tuning

| Option | Effect |
|---|---|
| `--threads N` | Worker count; `0` (default) uses all hardware threads |
| `--block-size SIZE` | Target solid-block size; disables automatic sizing |
| `--window SIZE` | Match window (LZMA2: dictionary size) |
| `--nice N` | Stop deep search at this match length; max match is 273 |
| `--chain-depth N` | How many previous candidates the matcher examines |
| `--lazy` / `--no-lazy` | Force the deferred-match check on or off |
| `--fast-entropy` | Cheaper entropy-coder selection |
| `--fast-lz` | Byte-token fast profile; disables tree and optimal modes |
| `--bt` | Cyclic binary-tree match finder |
| `--optimal` | Dynamic-programming parser |
| `--optimal-depth N` | Set optimal search depth, and enable it |
| `--optimal-candidates N` | Set parser candidates, and enable it |
| `--parallel` | Force the independent parallel-block path |
| `--swarm` | Cores cooperate inside each block (levels 1–6 and 8–9) |
| `--no-filters` | Disable the automatic file-aware transforms |
| `--no-sparse` | Do not capture sparse allocation maps; keep the archive on the v4 dense path when possible |
| `--strict-metadata` | Fail when source metadata or sparse allocation cannot be captured |
| `--recovery N` | Add `1..100`% Reed-Solomon recovery data |

#### Threads

`--threads 0` means "use the machine": compression, decompression, testing, and
extraction all expose every logical processor. Automatic block sizing still
targets physical cores, keeping the ratio-friendly geometry stable while SMT
siblings pick up nested parser and entropy tasks. Explicit counts are honored
as given, and tiny inputs stay serial.

Match discovery inside a level 8–9 block remains ordered, so Task Manager need
not show 100% — on memory-bound phases one busy thread per physical core can be
the fastest schedule.

#### Block size

For `.axar` this is the target solid-block size. Larger blocks find redundancy
across more files but cost memory and make selective extraction do more work.

Omitting it enables automatic sizing: the archive layer keeps the solid block
large enough for ratio, and the codec layer splits it into enough internal
blocks to feed the workers. Supplying `--block-size` disables that, which is
what you want for repeatable benchmarks and deliberate memory tuning.

For LZMA2, values larger than `4G` through `64G` select AXAR's large solid-block
profile. The raw solid block is staged in a temporary file and encoded as
independently decoded codec chunks of at most 512 MiB by default, so the outer
block size does not require a matching allocation or dictionary. If `--window`
is raised, the large-block writer raises its codec chunk up to that requested
dictionary (maximum 4 GiB−1); the raw staging buffer still grows only to the
current chunk's input size. This profile requires LZMA2, disables file-aware
filters, and cannot be combined with encryption or recovery records. The
resulting AXAR required feature flag makes older readers reject the archive
instead of attempting an unsafe legacy decode.

#### Window

`--window` is the dictionary/match window. Native Axiom and LZMA2 accept up to
4 GiB (4 GiB−1 is the largest encoded distance/property). Larger windows reach
more distant repetition and cost memory, especially with `--bt`: the
binary-tree matcher uses roughly `2 × min(window, block) × 4` bytes for tree
links, on top of input, output, and other codec memory. The level-1 `--fast-lz`
profile remains limited to a 16 MiB distance.

#### Threading model

The default model splits input into independent blocks and compresses one per
worker. This is fastest in general, but ratio eases as more cores drive the
automatic block size smaller, and a large block chosen for ratio leaves most
cores idle during the parse.

`--swarm` instead makes cores cooperate inside each block:

- **Levels 2–6** parse each block cooperatively — segments are indexed and
  parsed in parallel against the full block window, then merged into one token
  stream. Most useful with a large solid block, where it recovers single-block
  ratio without single-thread cost.
- **Level 1** trades its byte-token fast path for the cooperative hash parser.
  Expect better ratio and lower throughput — a ratio override, not a faster
  level 1.
- **Levels 8–9** parallelize the preliminary binary-tree candidate. The global
  optimal parser stays intact and the bake-off keeps a candidate only when it
  is smaller, so the effect is modest.
- **Level 7** ignores it — its lazy tree parse is path-dependent and not safely
  segmentable.

```powershell
axiomc a --level 6 --block-size 128M --window 128M --swarm archive.axar Data
```

Output is deterministic for a given input regardless of worker scheduling.

#### File-aware filters

AXAR creation groups similar file types before filling solid blocks. PE x86/x64
executables can use a reversible relative-branch filter; PCM WAV and
uncompressed BMP data can use a reversible delta filter. Direct `.axc`
compression also validates POSIX tar headers and can filter regular-file
members in place, and smooth 16-bit numeric data may use a predictor,
signed-residual zigzag, and byte-plane shuffle.

Detection inspects content rather than trusting extensions, and a fast trial
keeps a transform only when it predicts a net reduction after metadata. Use
`--no-filters` for an unfiltered comparison:

```powershell
axiomc c --level 9 --no-filters app.exe app-unfiltered.axc
```

### Size syntax

| Input | Meaning |
|---|---|
| `65536` | 65,536 bytes |
| `64K` | 64 KiB |
| `16M` | 16 MiB |
| `1G` | 1 GiB |

Suffixes are case-insensitive. Fractional values such as `1.5G` are not
accepted.

### Stream versions

Native Axiom streams are AXC version 9; Zstandard, LZMA2, and Deflate streams
are version 10. The current reader accepts versions 4 through 10. Archives
written with an external method therefore require Axiom 0.7.0.0 or newer.

## Encryption

```powershell
axiomc a -p "correct horse battery staple" private.axar SecretFiles
axiomc a -p "correct horse battery staple" --encrypt-names hidden.axar SecretFiles
```

`-p` encrypts archive data blocks. `--encrypt-names` (alias
`--encrypt-header`) additionally seals the central directory, hiding filenames,
sizes, and hashes — listing then requires the password.

AXAR uses Argon2id key derivation with XChaCha20-Poly1305. For `.zip` output,
`-p` creates WinZip AES-256 file-data encrypted entries, but ZIP filenames stay
visible — use `.axar --encrypt-names` when names must be hidden too.

New AXAR password archives use encryption-v2: a random archive data key is
wrapped by one or more independent password slots. This permits password
rotation and shared recovery passwords without touching the compressed block
bytes. A legacy v4 encrypted archive remains readable; its first password-slot
operation migrates the metadata to the v2 profile while preserving those block
bytes.

Supply the password to any command that reads protected content:

```powershell
axiomc l -p "correct horse battery staple" hidden.axar
axiomc x -p "correct horse battery staple" hidden.axar restored
```

Security notes:

- Command-line passwords can appear in shell history and process listings.
  There is no interactive prompt yet.
- Password slots are capped at 16, and the final slot cannot be removed.
- Password changes invalidate archive signatures. Recovery data is rebuilt when
  the password metadata changes.

## Exit codes

| Code | Meaning |
|---:|---|
| 0 | Success |
| 1 | Runtime failure; an `axiomc:` message is printed to stderr |
| 2 | Invalid command line |
| 3 | `verify`: archive is not signed. `repair`: no recovery record |
| 4 | `verify`: valid signature, but a different key than the one supplied |

```powershell
axiomc t backup.axar
if ($LASTEXITCODE -ne 0) { throw "Backup integrity verification failed" }
```

For archives that matter:

1. Write to a new path rather than overwriting the last known-good backup.
2. Run `axiomc t` on the new archive.
3. If signed, run `axiomc verify` with the trusted public key.
4. Test-extract for critical restore workflows.
5. Only then rotate the older backup.

Recovery records and recovery volumes tolerate bounded corruption, but they are
not a replacement for independent copies on separate storage.

## Recipes

Fast daily backup:

```powershell
axiomc a --level 3 --threads 0 daily.axar "D:\Work"
axiomc t daily.axar
```

Encrypted backup with hidden names:

```powershell
axiomc a --level 5 -p "correct horse battery staple" --encrypt-names `
    private.axar "D:\Private"
axiomc t -p "correct horse battery staple" private.axar
```

High-ratio archive with explicit 64 MiB tuning:

```powershell
axiomc a --level 7 --bt --window 64M --block-size 64M --nice 192 `
    high-ratio.axar "D:\Dataset"
```

Signed self-extractor:

```powershell
axiomc keygen release-secret.key release-public.key
axiomc a --level 5 release.axar publish
axiomc sign release.axar release-secret.key
axiomc verify release.axar release-public.key
axiomc sfx release.axar release.exe
```

Archive protected for long-term storage:

```powershell
axiomc a --level 7 --recovery 10 archive.axar "D:\Important"
axiomc split archive.axar 100M 3
axiomc t archive.part001.axar
```
