# axiomc — command-line reference

`axiomc` is Axiom's command-line archiver. It handles `.axar` multi-file
archives and `.axc` single-stream files, and it drives the same engine as the
[Windows app](docs/GUI_GUIDE.md).

```text
axiomc <command> [options] <archive> [paths...]
```

Options can go before or after the paths.

> **Paths written after an archive name are paths *inside* the archive.** They
> are not paths on your disk, and they use forward slashes. Run
> `axiomc l <archive>` first to see exactly how a path is stored, before you
> delete, move, or selectively extract it.

Unfamiliar with a term used here? [docs/GLOSSARY.md](docs/GLOSSARY.md) explains
them in plain language.

## Contents

- [Getting started](#getting-started)
- [The interactive shell](#the-interactive-shell)
- [Every command at a glance](#every-command-at-a-glance)
- [Creating and updating archives](#creating-and-updating-archives)
- [Deduplicated archives](#deduplicated-archives)
- [Snapshot repositories](#snapshot-repositories)
- [Writing to standard output](#writing-to-standard-output)
- [Listing, testing, and extracting](#listing-testing-and-extracting)
- [Metadata capture and sparse files](#metadata-capture-and-sparse-files)
- [Maintenance](#maintenance)
- [Recovery records and volumes](#recovery-records-and-volumes)
- [Signing](#signing)
- [Self-extracting archives](#self-extracting-archives)
- [Single-file compression](#single-file-compression)
- [Compression options](#compression-options)
- [Encryption](#encryption)
- [Exit codes](#exit-codes)
- [Recipes](#recipes)

## Getting started

After a Release build, the executable is `out\Release\axiomc.exe`.

```powershell
axiomc a backup.axar Documents    # create
axiomc l backup.axar              # list
axiomc t backup.axar              # test
axiomc x backup.axar restored     # extract
```

Quote anything containing spaces:

```powershell
axiomc a "D:\Backups\Work files.axar" "D:\Work files"
```

Folders are added recursively. Each input is stored relative to its parent, so
adding `D:\Data\Reports` produces a top-level `Reports/` entry in the archive —
not `D/Data/Reports/`.

## The interactive shell

Running `axiomc` with no arguments opens a prompt instead of printing usage and
quitting. `axiomc shell` and `axiomc --interactive` do the same thing
explicitly.

```text
axiom> a backup.axar "D:\Work files"
axiom> l backup.axar
axiom> exit
```

Commands are typed without the leading `axiomc`. A few extras exist only here:

| Command | What it does |
|---|---|
| `help` | Print the full command help |
| `pwd` | Print the working directory |
| `cd <dir>` | Change the working directory |
| `clear`, `cls` | Clear the console |
| `exit`, `quit` | Close the prompt |

Give `axiomc` arguments and it stays script-friendly: it runs the command,
prints no banner, and exits.

## Every command at a glance

| Command | What it does |
|---|---|
| `a`, `add` | Create an archive, or add and replace entries in one |
| `u`, `update` | Add new entries and replace ones the source has changed |
| `f`, `fresh` | Replace changed entries only — never add anything new |
| `s`, `sync` | Mirror the inputs exactly, including deleting what's gone |
| `delete`, `rm` | Delete entries, or a whole directory subtree |
| `repack` | Rebuild the archive and reclaim wasted space |
| `comment` | Show, set, or clear the archive's comment |
| `lock` | Mark an archive permanently read-only |
| `password-add` | Add another password that can open the archive |
| `password-change` | Replace the password you supplied with a new one |
| `password-remove` | Remove one of the passwords |
| `snapshot` | Create, append, list, compare, restore, and prune snapshots |
| `recovery`, `rr` | Show, add, replace, or remove the recovery record |
| `repair` | Repair damage using the recovery record |
| `split` | Split an archive into numbered volumes |
| `join` | Rejoin or reconstruct a volume set |
| `x`, `extract` | Extract |
| `l`, `list` | List contents and archive state |
| `capture-report` | Show any metadata warnings recorded when the archive was made |
| `t`, `test` | Decompress everything and verify all checksums |
| `keygen` | Generate a signing key pair |
| `sign` | Sign an archive |
| `verify` | Verify an archive's signature |
| `sfx` | Build a self-extracting Windows executable |
| `c`, `compress` | Compress one file to `.axc` |
| `d`, `decompress` | Decompress one `.axc` file |
| `help`, `-h`, `--help`, `/?` | Print help |

## Creating and updating archives

```text
axiomc a [options] <archive.axar> <input>...
axiomc u [options] <archive.axar> <input>...
axiomc f [options] <archive.axar> <input>...
axiomc s [options] <archive.axar> <input>...
```

The four differ only in what they do about files that already exist, or no
longer do:

| Command | Adds new | Replaces existing | Deletes missing |
|---|:--:|:--:|:--:|
| `a` (add) | ✓ | always | — |
| `u` (update) | ✓ | if the source is newer | — |
| `f` (fresh) | — | if the source is newer | — |
| `s` (sync) | ✓ | if the source is newer | ✓ |

```powershell
axiomc a photos.axar "D:\Photos"
axiomc a project.axar README.md src include assets
axiomc u project.axar src assets README.md
axiomc s mirror.axar "D:\Current project"
```

`sync` deletes things. List and test the result before you delete an older
backup.

### How updates avoid redoing work

Update, freshen, and synchronize run as one planned transaction. Files that
haven't changed are copied across still compressed — Axiom never decompresses
and recompresses them. Changed files are compressed once. Entries that no
longer exist are simply left out of the new directory, and any recovery record
is rebuilt once at the end. A synchronize that finds nothing to do exits
without touching the archive.

There is a second saving on top of that. If a file you're adding has the same
size, CRC-32, and BLAKE3 hash as data already in the archive, Axiom points the
new entry at the existing bytes instead of storing a second compressed copy.
This applies to `add`, `fresh`, and `sync` too, and the progress line reports
how many items and source bytes were reused. It happens automatically, doesn't
change the archive format, and falls back to normal compression whenever the
source can't be compared safely.

### Appending instead of rewriting

When an update doesn't need to change anything in the archive header, Axiom
appends a new *generation* rather than rewriting the file. The existing
compressed blocks and the old footer stay exactly where they are, so a
completed update never depended on copying the whole archive.

Operations that must change the header, and compacting ones such as `delete`
and `repack`, use the older approach instead: write a temporary file, then
rename it into place atomically.

If an append is interrupted halfway, readers that understand generations ignore
the incomplete tail and fall back to the previous valid footer. The archive
survives.

## Deduplicated archives

Use `--dedup` when creating an AXAR whose live contents contain repeated or
mostly-similar files. Axiom cuts files at content-defined boundaries and stores
each distinct chunk once:

```powershell
axiomc a --dedup backup.axar "D:\Projects"
```

This is an archive profile chosen at creation time. Later `a`, `u`, `f`, `s`,
`delete`, and move operations detect it automatically; do not repeat `--dedup`.
New and changed files append only chunks that are not already in the archive.
Deleted and replaced chunks become unreachable but remain on disk until
`axiomc repack backup.axar` performs garbage collection.

Tune the stable per-archive chunk geometry with `--chunk-min SIZE`,
`--chunk-average SIZE`, and `--chunk-max SIZE`. The defaults are 256 KiB,
1 MiB, and 4 MiB. For encrypted archives, chunk identities are keyed with the
archive key by default; `--plain-chunks` deliberately exposes equality instead.

`--dedup` requires a seekable AXAR file and cannot be combined with stdout
archive output. It does not add history: use a snapshot repository when old
filesystem views must remain restorable.

## Snapshot repositories

A snapshot repository keeps several dated versions of the same folder in one
archive, without storing unchanged data more than once. Files are cut into
pieces at content-defined boundaries, and identical pieces are stored once — so
a later snapshot normally adds only what actually changed, plus a small index.

The newest snapshot is also the archive's live directory, which means ordinary
`l`, `t`, and `x` keep working on it.

```powershell
axiomc snapshot create [options] repository.axar base "D:\Project"
axiomc snapshot add [options] repository.axar nightly "D:\Project"
axiomc snapshot list [-p "password"] repository.axar
axiomc snapshot diff [-p "password"] repository.axar base nightly
axiomc snapshot restore [--overwrite all] repository.axar base restored
axiomc snapshot prune repository.axar base
axiomc repack repository.axar
```

`snapshot prune` removes old snapshots but refuses to remove the current one.
`repack` is the garbage collector: it keeps every piece still reachable from
the current and remaining snapshots, remaps their addresses, and atomically
replaces the repository.

Use `--recovery N` when creating or appending to keep recovery data, and
`--strict-metadata` on restore when losing metadata should fail rather than
warn.

When a repository is encrypted, the identifiers Axiom uses to recognise
identical pieces are keyed with the archive's own key by default, so an
observer can't learn which pieces are the same. Use `--plain-chunks` when you
deliberately want identities to match across separate encrypted repositories,
or `--keyed-chunks` to state the default explicitly.

Tune the piece geometry with `--chunk-min SIZE`, `--chunk-average SIZE`, and
`--chunk-max SIZE`. The values are bounded and must be in that order.

The ordinary content commands — `a`, `u`, `f`, `s`, `delete`, `move` — are
rejected on a snapshot repository, so a habit from ordinary archives can't
silently throw away your history. Use `snapshot add` for content and
`snapshot prune` / `repack` for maintenance.

Snapshot commands accept the same password, thread, compression, encryption,
recovery, and progress options as the archive command they correspond to.

## Writing to standard output

Use a literal `-` in place of the archive name to write an archive to standard
output:

```text
axiomc a - <input>...
```

On Windows, route binary output through `cmd.exe` so the shell doesn't
reinterpret the bytes:

```text
cmd.exe /c "axiomc.exe a - D:\Work > D:\Backups\work.axar"
```

This mode supports normal compression and encryption, and is useful for pipes
or when another process owns the output stream. Because the stream can't seek
backwards to patch its own header, it reserves the known metadata flags up
front.

Recovery records are refused here — a stream you can't seek in can't be
repaired or signed in place. Sign the archive after you've written it to a real
file.

## Listing, testing, and extracting

```text
axiomc l [-p <password>] <archive.axar>
axiomc t [--threads N] [-p <password>] <archive.axar>
axiomc x [options] <archive.axar> [destination]
```

`list` identifies directories, symbolic links, hard links, comments, lock and
encryption state, the entry count, and the total uncompressed size.

`test` decompresses everything and verifies every checksum without writing a
single file.

Extraction goes to the current directory unless you name another. What happens
to files that are already there is always explicit:

```powershell
axiomc x --overwrite fail project.axar restored   # stop on the first conflict (default)
axiomc x --overwrite skip project.axar restored   # keep what's already there
axiomc x --overwrite all  project.axar restored   # replace what's already there
```

Extracted paths are always kept inside the destination. Attempts to escape it —
whether by `..`, by an absolute path, or through a symbolic link or junction —
are rejected.

### Extracting only part of an archive

Repeat `--include` for each archive path you want. Naming a directory includes
everything under it:

```powershell
axiomc x --include src/app.exe --include src/config project.axar restored
```

For AXAR, this reads only the compressed pieces those files need, when the
archive records where its pieces start. Older archives, encrypted blocks,
filtered blocks, and methods without independently decodable pieces fall back
to reading whole blocks — still correct, just more work.

Interactive progress shows an `archive read` byte count during testing and
extraction. That number includes reading the directory, and it is the way to
confirm that a narrow selection really did skip the rest of the archive.

## Metadata capture and sparse files

A *sparse* file has large runs of zeros that the file system doesn't actually
store. AXAR always stores the complete logical byte stream — every zero is
compressed and checksummed — and separately records which parts of the file
were really allocated, so extraction can recreate the holes. Use `--no-sparse`
if you'd rather restore a plain dense file.

When Axiom is allowed to skip an unreadable input, or can't query a sparse
layout, it records the reason inside the archive and reports it:

```powershell
axiomc capture-report backup.axar
axiomc capture-report -p secret backup.axar
```

This exits `0` when capture was complete and `1` when there are warnings, so it
works in a script.

Add `--strict-metadata` to a create, update, or extract command when any loss
of metadata should fail the operation instead of warning:

```powershell
axiomc a --strict-metadata backup.axar "D:\Data"
axiomc x --strict-metadata backup.axar restored
```

Archives that carry sparse maps, capture reports, or extended ACL / xattr /
reparse metadata are written as AXAR v5. Plain dense archives without those
features can stay v4, and every existing v4 archive keeps opening unchanged.

## Maintenance

```powershell
axiomc delete project.axar "project/build.log" "project/cache"
axiomc repack --level 7 project.axar
axiomc comment project.axar                       # show it
axiomc comment project.axar "Nightly backup"      # set it
axiomc comment project.axar ""                    # clear it
axiomc lock project.axar
```

Deleting a directory removes its entire subtree, and the archive is rebuilt so
the space is physically reclaimed.

`repack` keeps every entry and only reclaims the dead space left by earlier
replacements and deletions. It also merges duplicate file contents into a
single stored copy.

Locking is one-way. There is no unlock command. A locked archive can still be
listed, tested, extracted, and verified, but not edited, repacked, re-signed,
or re-commented.

### Managing passwords

Password management is separate from editing content:

```powershell
axiomc password-add    -p "current password" private.axar "backup password"
axiomc password-change -p "current password" private.axar "new password"
axiomc password-remove -p "current password" private.axar "backup password"
```

The password you supply with `-p` must already open the archive. Adding and
removing passwords does not recompress or re-encrypt anything — Axiom encrypts
your data with a random internal key and wraps that key once per password, so
only the wrapper changes.

An archive can hold up to 16 passwords, and the last one cannot be removed.

Changing passwords invalidates an archive signature. Sign the archive again if
signature verification is part of your workflow.

## Recovery records and volumes

A recovery record is spare data stored inside the archive that can rebuild
damaged parts of it. You can add it when you create the archive or later:

```powershell
axiomc a --recovery 10 backup.axar "D:\Work"
axiomc recovery backup.axar      # show the current record
axiomc recovery backup.axar 15   # rebuild it at 15%
axiomc recovery backup.axar 0    # remove it
```

Valid percentages are `1..100`; `0` removes the record. Ordinary edits keep the
existing percentage and regenerate the parity afterwards.

If `test` reports damage and the recovery data can be found:

```powershell
axiomc repair backup.axar
axiomc t backup.axar
```

`repair` returns exit code 3 when there is no recovery record. Damage beyond
what the parity can cover cannot be repaired. **A recovery record is not a
second backup.**

### Splitting into volumes

```text
axiomc split <archive.axar> <size> [recovery-volume-count]
axiomc join <any-volume> <output.axar>
```

```powershell
axiomc split backup.axar 100M 3      # backup.part001.axar… plus backup.rev001…003
axiomc join backup.part001.axar restored.axar
```

The original archive is left alone. A set can hold at most 255 volumes in
total. Any surviving data or recovery volume identifies the whole set, and
joining checks every payload's CRC plus a BLAKE3 digest of the reassembled
archive.

When every data volume is present, you don't need to join at all — ordinary
read commands work directly on any numbered volume. The set is read-only in
that form.

## Signing

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

The secret key is 64 bytes and must be protected. The public key is 32 bytes
and is meant to be distributed.

Verifying without naming a key checks that the signature is cryptographically
valid, using the key embedded in the signature itself. Naming a key
additionally requires that it was *that* key — which is the check you actually
want if you're verifying a release.

Signing an existing archive appends a new generation whose signature covers the
generation history and the current directory. Editing the archive afterwards
invalidates it. An archive written to standard output has to be saved as a real
file before it can be signed.

See [exit codes](#exit-codes) for how the different verification outcomes are
reported.

## Self-extracting archives

```text
axiomc sfx [--config <file>] [--stub full|mini] <archive.axar-or-zip> <output.exe> [compatible-stub]
```

```powershell
axiomc sfx release.axar release-setup.exe
axiomc sfx --config setup.ini release.axar release-setup.exe
axiomc sfx --stub mini build.axar unpack-build.exe
```

The result is one standalone Windows executable holding Axiom's read-only
extraction code, a small descriptor, and your intact `.axar` or `.zip`. It does
not contain the file-manager app, and it doesn't refer back to the source
archive when it runs. `axiomc sfx` leaves your original archive in place.

`--stub` picks the extractor runtime:

| Stub | Size (Release 0.9.2.2) | Behaviour |
|---|---:|---|
| `full` (default) | 2.22 MiB | Shows dialogs. For something a person double-clicks |
| `mini` | 776 KiB | Console only, never prompts. For an artifact a script unpacks |

Both understand the same options and the same configuration file. `mini` uses a
decode-only runtime with no archive writers and no compression backends linked
in, which is where its size saving comes from.

By default this uses the `AxiomSfx.bin` (or `AxiomSfxMini.bin`) module sitting
next to `axiomc.exe`. Those modules are not programs — they are only read while
an SFX is being created. Supplying your own compatible PE image is possible,
but it exists for development and compatibility testing.

### Signing the result

The generated `.exe` can be Authenticode-signed. Sign the **finished
executable**, never the stub — packaging onto an already-signed stub is
refused, because appending to it would break that signature.

```powershell
axiomc sfx release.axar release-setup.exe
signtool sign /fd SHA256 /tr <timestamp-url> /td SHA256 release-setup.exe
```

Because Authenticode covers trailing data, the signature attests to the archive
payload as well as to the extractor. Executables produced before 0.8.0.0 used a
layout that couldn't be signed at all; they are still read correctly.

### What the extractor shows

The full stub shows a native dialog with an editable destination and a folder
browser; file and folder counts, unpacked size, encryption state, and signature
state; replace / skip / stop conflict policies; automatic or explicit thread
count; optional restore of modification times and opening the destination
afterwards; and progress with pause, resume, and cancel.

Encrypted payloads ask for the password before revealing any metadata. Signed
payloads are verified first, and an invalid signature blocks extraction
entirely.

### Running a generated extractor

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

A bare destination still works, as it always has:

```powershell
.\release-setup.exe "D:\Applications\Release"
```

Exit codes are stable and safe to script against:

| Code | Meaning |
|---:|---|
| 0 | Success |
| 1 | Extraction failed |
| 2 | Bad usage |
| 3 | Cancelled |
| 4 | Wrong or missing password |
| 5 | Integrity or signature check failed |
| 6 | Not enough free space |
| 7 | Elevation needed but unavailable |
| 8 | The run-after-extract program failed to start |
| >8 | With `propagate_exit_code`, that program's own exit code |

The extractor is a GUI binary, so double-clicking it never flashes a console
window. It does attach to the console that launched it, and it honours
redirection — `release-setup.exe --list > contents.txt` works.

### Configuring an extractor

`--config` takes an INI-style file. Every key is optional, but an unrecognised
key is an error rather than being quietly ignored.

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
`%TEMP%`, plus `%SFXDIR%` for the extractor's own folder and `%SFXNAME%` for
its name. Those shell folders resolve through the Windows known-folder API,
not through environment variables, so `%ProgramFiles%` can't be redirected by
setting a variable before launch. A template containing an unknown token or a
`..` component is rejected when the SFX is built, not on the recipient's
machine.

`threads` accepts `0` for automatic, or 1 through 4096.

`run_program` and `run_working_dir` are relative to the extraction root. They
must not be absolute or drive-rooted, must not contain `..` or `:`, and must
not pass through a symbolic link or junction. The program has to be a regular
file that the extraction actually produced, and the working directory has to be
a real extracted directory.

`elevation = auto` decides by testing whether the destination is writable, then
relaunches through the `runas` verb if it isn't. The parent process waits for
that child and returns its exit code, so a script never sees a false success. A
password passed with `--password` or `--password-stdin` is never forwarded to
the elevated instance, where it would show up in the process list — that
combination is refused instead. A password typed interactively is cleared, and
the elevated full stub asks for it again.

`--log` appends UTF-8 status output and the final exit code. It refuses to use
the SFX executable itself as the log target, and it is forwarded safely across
an elevation relaunch.

Three combinations are rejected at build time, because the extractor would
refuse them anyway: `require_accept` with no license text, `run_arguments` with
no `run_program`, and the fully unattended privileged chain of
`mode = very_silent` with `elevation = require` and a `run_program`.

`allow_file_selection` is kept as a reserved compatibility key but is not
implemented. Use repeatable `--include` patterns for selective extraction.

> The Windows app's **Create a self-extracting Windows executable** option
> treats the intermediate `.axar` as a build artifact and deletes it once the
> merged `.exe` succeeds. `axiomc sfx` keeps its input, because it is
> converting an archive you already had.

## Single-file compression

```text
axiomc c [options] <input> <output.axc>
axiomc d [--threads N] [-p <password>] <input.axc> <output>
```

```powershell
axiomc c --level 5 database.bin database.axc
axiomc d database.axc restored-database.bin
```

`.axc` is one compressed stream and nothing else. No file list, no comments, no
file metadata, no editing.

## Compression options

These apply to the commands that create or recompress data: `add`, `update`,
`fresh`, `sync`, `delete`, `repack`, `sign`, and `compress`.

### Methods

| Method | `--method` | Its own controls |
|---|---|---|
| Axiom adaptive (default) | `axiom` | `--level 1..9`, window, nice length, threading model |
| Zstandard | `zstd` | native level `-5..22` |
| LZMA2 | `lzma2` | native level `0..9`, 4 KiB–4 GiB dictionary, 5–273 fast bytes, HC4/BT4 |
| Deflate | `deflate` | native level `0..9`; fixed 32 KiB window, 258-byte maximum match |
| Store | `store` | none — data is copied verbatim |

```powershell
axiomc a --method zstd --codec-level 12 archive.axar Data
axiomc a --method lzma2 --codec-level 7 --window 128M --block-size 128M --lzma-mf bt4 archive.axar Data
axiomc a --method store archive.axar already-compressed-media
```

Without `--codec-level`, the portable `--level 1..9` maps to a sensible native
level: Zstandard gets 1, 2, 3, 5, 7, 10, 14, 18, 22 for levels 1–9, while LZMA2
and Deflate use the number directly.

For LZMA2, `--window` and `--nice` become the dictionary and fast-bytes
settings. The dictionary is clamped to the independently decoded chunk size
(effectively `--block-size`) and to a stable bound for the data that chunk
holds. Values up to 4 GiB are accepted; what goes on the wire is 4 GiB−1. To
request a full 4 GiB LZMA2 window for an ordinary stream, set both `--window 4G`
and `--block-size 4G`.

Large dictionaries can consume several times their own size in encoder memory,
so a small payload is automatically capped to its own size, and a multi-chunk
payload keeps one dictionary setting across all its chunks — including the
short final one. `--lzma-mf hc4` favours speed; `bt4` favours ratio.

ZIP archives can only use `deflate` and `store`. Leaving `--method` off keeps
the established ZIP Deflate behaviour.

Every payload from an external codec is split into independently bounded
chunks. Pause, cancel, and progress happen between chunks, and a chunk that
would grow is stored verbatim instead. Encryption, recovery, signing, metadata,
volumes, and SFX packaging are container services — for ordinary blocks they
don't vary with the method. The one exception is the large-solid LZMA2 profile,
which cannot currently be combined with encryption or recovery records because
those paths still need whole-block buffers.

### Levels

`--level N`, where N is 1 to 9. The default is 5. `--fast` means level 1 and
`--max` means level 9.

| Level | Match finder and parser | Typical use |
|---:|---|---|
| 1 | Dedicated fast path | The least CPU time |
| 2–3 | Shallow candidate list, cost-aware deferral | Fast backups |
| 4–5 | Balanced candidate list, cost-aware deferral | General use |
| 6 | Deep candidate list | Better ratio, no tree memory |
| 7 | Sorted tree plus cost-aware one-byte lookahead, 8 MiB window | Long-range repetition without full optimal-parse cost |
| 8 | Sorted tree plus single-pass cheapest-path parse, 32 MiB window | High ratio at moderate cost |
| 9 | Deeper tree, measured-cost cheapest-path parse, 64 MiB window, 4 KiB matches | The smallest archive |

Levels 8 and 9 compute the cheapest way to encode the whole block rather than
taking each good match as it appears. On general data they land substantially
below levels 6 and 7. Level 7 uses a cheaper trick: it looks one byte ahead and
compares token costs, so it can defer an expensive match for a better one at
the next position without paying for the full calculation.

Explicit tuning options override the level, whatever order the arguments are
in.

### Tuning

| Option | Effect |
|---|---|
| `--threads N` | Worker count; `0` (default) uses every hardware thread |
| `--block-size SIZE` | Target solid-block size; switches off automatic sizing |
| `--window SIZE` | Match window (for LZMA2, the dictionary size) |
| `--nice N` | Stop searching deeper once a match of this length is found; the maximum match is 273 |
| `--chain-depth N` | How many earlier candidates the match finder examines |
| `--lazy` / `--no-lazy` | Force the deferred-match check on or off |
| `--fast-entropy` | Cheaper entropy-coder selection |
| `--fast-lz` | Byte-token fast profile; disables the tree and optimal modes |
| `--bt` | Use the sorted binary-tree match finder |
| `--optimal` | Use the cheapest-path parser |
| `--optimal-depth N` | Set the optimal search depth, and enable it |
| `--optimal-candidates N` | Set the parser's candidate count, and enable it |
| `--parallel` | Force the independent parallel-block path |
| `--swarm` | Make cores cooperate inside each block (levels 1–6 and 8–9) |
| `--no-filters` | Turn off the automatic file-aware transforms |
| `--no-sparse` | Don't capture sparse allocation maps |
| `--strict-metadata` | Fail if source metadata or sparse layout can't be captured |
| `--recovery N` | Add `1..100`% recovery data |
| `--dedup` | Select live content-defined deduplication when creating a new AXAR |
| `--chunk-min SIZE` | Minimum content-defined chunk size |
| `--chunk-average SIZE` | Target content-defined chunk size |
| `--chunk-max SIZE` | Maximum content-defined chunk size |
| `--keyed-chunks` / `--plain-chunks` | Hide or expose equality in encrypted chunk identities |

#### Threads

`--threads 0` means "use the machine". Compression, decompression, testing, and
extraction all get every logical processor.

Automatic block sizing is a separate decision, and it targets *physical* cores.
That keeps blocks large enough to compress well, while SMT siblings pick up the
nested parser and entropy work. Explicit thread counts are honoured as given,
and tiny inputs stay single-threaded because starting threads would cost more
than it saves.

At levels 8 and 9, match discovery inside a block stays ordered, so Task
Manager may not show 100%. During memory-bound phases, one busy thread per
physical core is often the fastest schedule there is.

#### Block size

For `.axar` this is the target solid-block size. Larger blocks find repetition
across more files but cost memory, and they make extracting a single file do
more work.

Leave it off and Axiom sizes blocks automatically: the archive layer keeps the
solid block large enough for a good ratio, and the codec layer splits it
internally into enough pieces to keep the workers busy. Setting `--block-size`
disables that — which is exactly what you want for a repeatable benchmark or
for deliberate memory tuning, and not what you want for everyday use.

For LZMA2, values above `4G` and up to `64G` select AXAR's large solid-block
profile. The raw block is staged in a temporary file and encoded as
independently decoded chunks of at most 512 MiB by default, so the outer block
size does not require a matching allocation or dictionary. Raising `--window`
raises the chunk size up to the requested dictionary (at most 4 GiB−1), while
the staging buffer still grows only to the current chunk's input size.

This profile requires LZMA2, disables the file-aware filters, and cannot be
combined with encryption or recovery records. The resulting archive carries a
required feature flag, so an older reader rejects it outright rather than
attempting an unsafe decode.

#### Window

`--window` sets how far back the compressor can look. Axiom's own method and
LZMA2 both accept up to 4 GiB; 4 GiB−1 is the largest distance the format can
encode.

Larger windows reach more distant repetition and cost memory — noticeably so
with `--bt`, where the tree needs roughly
`2 × min(window, block) × 4` bytes for its links, on top of the input, output,
and everything else. The level-1 `--fast-lz` profile is limited to a 16 MiB
distance regardless.

#### Threading model

By default Axiom splits the input into independent blocks and gives one block
to each worker. That is fastest in general, but it has two costs: the ratio
eases as more cores drive the automatic block size down, and a single large
block chosen for ratio leaves most cores idle during the parse.

`--swarm` makes the cores cooperate inside each block instead:

- **Levels 2–6** parse each block cooperatively. Segments are indexed and
  parsed in parallel against the full block window, then merged into one token
  stream. This is most useful with a large solid block, where it recovers
  single-block ratio without single-thread timing.
- **Level 1** trades its byte-token fast path for the cooperative parser.
  Expect a better ratio and lower throughput — this is a ratio override, not a
  faster level 1.
- **Levels 8–9** parallelize only the preliminary candidate search. The global
  cheapest-path parser stays intact, and a candidate is kept only when it's
  smaller, so the effect is modest.
- **Level 7** ignores `--swarm` entirely. Its parse depends on the path taken
  so far and can't be safely segmented.

```powershell
axiomc a --level 6 --block-size 128M --window 128M --swarm archive.axar Data
```

Output is deterministic for a given input, whatever order the workers happen to
run in.

#### File-aware filters

Before filling solid blocks, AXAR creation groups files of similar type
together. Some types then get a reversible transform that makes them more
compressible: x86/x64 executables can have their jump addresses converted to a
relative form, and PCM WAV and uncompressed BMP data can be stored as
differences.

Direct `.axc` compression additionally validates POSIX tar headers and can
filter regular-file members inside a tar in place. Smooth 16-bit numeric data
can use a predictor, signed-residual zigzag, and byte-plane shuffle.

Detection inspects the actual content rather than trusting file extensions, and
a fast trial encode keeps a transform only when it predicts a net saving after
its own metadata. To compare without them:

```powershell
axiomc c --level 9 --no-filters app.exe app-unfiltered.axc
```

### Writing sizes

| You write | It means |
|---|---|
| `65536` | 65,536 bytes |
| `64K` | 64 KiB |
| `16M` | 16 MiB |
| `1G` | 1 GiB |

Suffixes are case-insensitive. Fractional values such as `1.5G` are not
accepted.

### Stream versions

Axiom's own streams are AXC version 9. Zstandard, LZMA2, and Deflate streams
are version 10. The current reader accepts versions 4 through 10.

An archive written with an external method therefore needs Axiom 0.7.0.0 or
newer to read.

## Encryption

```powershell
axiomc a -p "correct horse battery staple" private.axar SecretFiles
axiomc a -p "correct horse battery staple" --encrypt-names hidden.axar SecretFiles
```

`-p` encrypts the archive's data blocks. `--encrypt-names` (also spelled
`--encrypt-header`) additionally seals the index, hiding file names, sizes, and
hashes — after which even listing the archive needs the password.

AXAR uses Argon2id to turn the password into a key, and XChaCha20-Poly1305 to
encrypt. For `.zip` output, `-p` produces WinZip AES-256 encrypted entries, but
ZIP file names stay visible no matter what. If names must be hidden, use
`.axar --encrypt-names`.

New AXAR password archives encrypt your data with a random internal key, which
is then wrapped by one or more independent password slots. This is what lets
you rotate a password, or add a shared recovery password, without touching a
single compressed byte. Older v4 encrypted archives still open, and the first
password operation on one migrates its metadata to the new scheme while keeping
those block bytes as they are.

Supply the password to any command that reads protected content:

```powershell
axiomc l -p "correct horse battery staple" hidden.axar
axiomc x -p "correct horse battery staple" hidden.axar restored
```

Three things worth knowing:

- **A password on the command line can end up in your shell history and is
  visible in process listings.** There is no interactive prompt yet.
- An archive holds at most 16 passwords, and the last one can't be removed.
- Changing a password invalidates the archive's signature, and rebuilds the
  recovery data.

## Exit codes

| Code | Meaning |
|---:|---|
| 0 | Success |
| 1 | Something failed at runtime; an `axiomc:` message goes to stderr |
| 2 | The command line was invalid |
| 3 | `verify`: the archive isn't signed. `repair`: there is no recovery record |
| 4 | `verify`: the signature is valid, but by a different key than the one you supplied |

```powershell
axiomc t backup.axar
if ($LASTEXITCODE -ne 0) { throw "Backup integrity verification failed" }
```

### For archives that matter

1. Write to a new path rather than overwriting your last known-good backup.
2. Run `axiomc t` on the new archive.
3. If it's signed, run `axiomc verify` with the trusted public key.
4. For anything you'd genuinely need to restore, test-extract it.
5. Only then rotate out the older backup.

Recovery records and recovery volumes tolerate a bounded amount of corruption.
They are not a replacement for an independent copy on separate storage.

## Recipes

**Fast daily backup**

```powershell
axiomc a --level 3 --threads 0 daily.axar "D:\Work"
axiomc t daily.axar
```

**Encrypted backup with hidden file names**

```powershell
axiomc a --level 5 -p "correct horse battery staple" --encrypt-names `
    private.axar "D:\Private"
axiomc t -p "correct horse battery staple" private.axar
```

**High ratio, with explicit 64 MiB tuning**

```powershell
axiomc a --level 7 --bt --window 64M --block-size 64M --nice 192 `
    high-ratio.axar "D:\Dataset"
```

**A signed self-extractor**

```powershell
axiomc keygen release-secret.key release-public.key
axiomc a --level 5 release.axar publish
axiomc sign release.axar release-secret.key
axiomc verify release.axar release-public.key
axiomc sfx release.axar release.exe
```

**Something built to survive storage**

```powershell
axiomc a --level 7 --recovery 10 archive.axar "D:\Important"
axiomc split archive.axar 100M 3
axiomc t archive.part001.axar
```
