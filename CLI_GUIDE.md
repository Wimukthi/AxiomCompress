# Axiom Command-Line Guide

`axiomc` is Axiom's command-line archiver. It works with `.axar` multi-file
archives and `.axc` single-stream compressed files. This guide documents the
commands implemented by the current CLI. See [FORMAT.md](FORMAT.md) for the binary
format.

## How to use this guide

If you only need the normal archive workflow, read these sections first:

1. [Getting started](#getting-started)
2. [Create or add files](#create-or-add-files-a-add)
3. [List, test, and extract](#list-test-and-extract)
4. [Compression options](#compression-options)

The later sections cover maintenance features such as comments, locking,
recovery records, split volumes, signing, SFX creation, and the lower-level
single-stream `.axc` commands.

Important rule: paths after an archive name are usually paths **inside the
archive**, not Windows filesystem paths. Use `axiomc l archive.axar` to see the
exact archive paths before deleting, moving, or selectively extracting entries.

## Getting started

After a Visual C++ Release build, the executable is
`out\Release\axiomc.exe`. From the repository root in PowerShell:

```powershell
$axiom = ".\out\Release\axiomc.exe"
& $axiom a backup.axar Documents
& $axiom l backup.axar
& $axiom t backup.axar
& $axiom x backup.axar restored
```

If `axiomc.exe` is on `PATH`, use `axiomc` directly. Quote paths containing
spaces:

```powershell
axiomc a "D:\Backups\Work files.axar" "D:\Work files"
```

Options may appear before or after positional paths, although putting options
first is easier to read.

## Interactive shell

Launching `axiomc` without arguments opens an interactive prompt instead of
printing usage and closing:

```powershell
axiomc
```

The prompt shows the Axiom ASCII logo, then accepts the same commands documented
below without the leading `axiomc`:

```text
axiom> a backup.axar "D:\Work files"
axiom> l backup.axar
axiom> t backup.axar
axiom> x backup.axar restored
axiom> exit
```

You can also enter the prompt explicitly with `axiomc shell` or
`axiomc --interactive`. Command-line mode remains script-friendly: when arguments
are supplied, Axiom runs the command and exits normally without the splash.

Prompt helpers:

- `help` prints the full command help.
- `pwd` prints the current working directory.
- `cd <dir>` changes the working directory.
- `clear` clears the console.
- `exit` or `quit` closes the prompt.

## Command overview

| Command | Purpose |
|---|---|
| `a`, `add` | Create an archive, or add/replace entries |
| `u`, `update` | Add new entries and replace newer entries |
| `f`, `fresh` | Replace newer existing entries without adding new ones |
| `s`, `sync` | Mirror source inputs, including deletion |
| `delete`, `rm` | Delete entries or directory subtrees |
| `repack` | Rebuild an archive and reclaim dead space |
| `comment` | Display, set, or clear the archive comment |
| `lock` | Permanently mark an archive read-only |
| `recovery`, `rr` | Display, add, replace, or remove a recovery record |
| `repair` | Repair protected archive damage |
| `split` | Create numbered data and optional recovery volumes |
| `join` | Join or reconstruct a volume set |
| `x`, `extract` | Extract an archive |
| `l`, `list` | List archive contents and state |
| `t`, `test` | Decode and verify archive integrity |
| `keygen` | Generate a signing key pair |
| `sign` | Sign an archive |
| `verify` | Verify an archive signature |
| `sfx` | Build a self-extracting Windows executable |
| `c`, `compress` | Compress one stream to `.axc` |
| `d`, `decompress` | Decompress one `.axc` stream |

## Common workflows

### Create, verify, and extract a backup

```powershell
axiomc a backup.axar "D:\Work"
axiomc t backup.axar
axiomc x backup.axar "D:\Restore-test"
```

### Create an encrypted archive

```powershell
axiomc a -p "correct horse battery staple" private.axar "D:\Private"
```

Add `--encrypt-names` when filenames, sizes, and directory metadata should also
be hidden:

```powershell
axiomc a -p "correct horse battery staple" --encrypt-names private-hidden.axar "D:\Private"
```

### Add recovery data before storing an important archive

```powershell
axiomc a important.axar "D:\Important"
axiomc recovery important.axar 10
axiomc t important.axar
```

### Split an archive for transfer

```powershell
axiomc split important.axar 100M 3
axiomc join important.part001.axar restored.axar
```

## Create or add files: `a`, `add`

```text
axiomc a [compression-options] <archive.axar> <input>...
axiomc add [compression-options] <archive.axar> <input>...
```

If the archive does not exist, Axiom creates it. If it exists, Axiom adds new
entries and replaces entries with the same archive path.

```powershell
# Create an archive from a directory.
axiomc a photos.axar "D:\Photos"

# Create one from several files and directories.
axiomc a project.axar README.md src include assets

# Add another file later.
axiomc a project.axar CHANGELOG.md
```

Input directories are recursive. An input is stored relative to its parent, so
adding `D:\Data\Reports` creates a top-level `Reports/` entry.

## Update existing archives

### Update: `u`, `update`

```text
axiomc u [compression-options] <archive.axar> <input>...
```

Update adds files not already present and replaces archived files only when the
source modification time is newer.

```powershell
axiomc u project.axar src assets README.md
```

### Freshen: `f`, `fresh`

```text
axiomc f [compression-options] <archive.axar> <input>...
```

Freshen replaces newer files already in the archive. It never adds new files.

```powershell
axiomc f project.axar src
```

### Synchronize: `s`, `sync`

```text
axiomc s [compression-options] <archive.axar> <input>...
```

Synchronize mirrors the supplied inputs: it adds missing files, replaces newer
files, and removes archived entries no longer in the source set.

```powershell
axiomc s mirror.axar "D:\Current project"
```

`sync` is destructive. List and test the result before deleting an older backup.

## Delete entries and reclaim space

### Delete: `delete`, `rm`

```text
axiomc delete [compression-options] <archive.axar> <archive-path>...
axiomc rm [compression-options] <archive.axar> <archive-path>...
```

The paths after the archive name are paths inside the archive, not filesystem
paths. Use `list` to obtain them. Archive paths use forward slashes.

```powershell
axiomc l project.axar
axiomc delete project.axar "project/build.log" "project/cache"
```

Deleting a directory removes its complete subtree. The archive is rebuilt and
live data is recompressed.

### Repack: `repack`

```text
axiomc repack [compression-options] <archive.axar>
```

Repack rebuilds all live entries and reclaims dead space left by earlier
replacements.

```powershell
axiomc repack --level 7 project.axar
```

## Comments and locking

```powershell
# Show the comment.
axiomc comment project.axar

# Set or replace it.
axiomc comment project.axar "Nightly backup before migration"

# Clear it with an empty text argument.
axiomc comment project.axar ""

# Permanently mark the archive read-only.
axiomc lock project.axar
```

Locking is one-way in the current format. A locked archive can still be listed,
tested, extracted, and verified, but cannot be edited, repacked, re-signed, or
have its comment changed. There is no unlock command.

## List, test, and extract

### List: `l`, `list`

```text
axiomc l [-p <password>] <archive.axar>
```

```powershell
axiomc l project.axar
axiomc l -p "correct horse battery staple" private.axar
```

The listing identifies directories, symbolic links, hard links, comments, lock
and encryption state, entry count, and total uncompressed bytes.

### Test: `t`, `test`

```text
axiomc t [--threads N] [-p <password>] <archive.axar>
```

Test decodes data and verifies integrity without writing extracted files.

```powershell
axiomc t --threads 0 project.axar
axiomc t -p "correct horse battery staple" private.axar
```

### Extract: `x`, `extract`

```text
axiomc x [options] <archive.axar> [destination]
```

The destination defaults to the current directory.

```powershell
axiomc x project.axar restored
axiomc x project.axar
```

Overwrite behavior is explicit:

```powershell
# Stop if a destination exists. This is the default.
axiomc x --overwrite fail project.axar restored

# Keep existing files and extract the others.
axiomc x --overwrite skip project.axar restored

# Replace existing files.
axiomc x --overwrite all project.axar restored
```

For encrypted archives:

```powershell
axiomc x -p "correct horse battery staple" private.axar restored
```

Axiom contains extracted paths within the destination and rejects unsafe path
traversal and reparse-point ancestors.

## Encryption

Encrypt archive data blocks:

```powershell
axiomc a -p "correct horse battery staple" private.axar SecretFiles
```

Encrypt the central directory too, hiding filenames, sizes, and hashes:

```powershell
axiomc a -p "correct horse battery staple" --encrypt-names hidden.axar SecretFiles
```

`--encrypt-header` is an alias for `--encrypt-names`. Supply the password to
commands that read protected content:

```powershell
axiomc l -p "correct horse battery staple" hidden.axar
axiomc t -p "correct horse battery staple" hidden.axar
axiomc x -p "correct horse battery staple" hidden.axar restored
axiomc verify -p "correct horse battery staple" hidden.axar public.key
```

Encryption uses Argon2id and XChaCha20-Poly1305. Command-line passwords may be
visible in shell history and process inspection. The current CLI has no
interactive password prompt; use a controlled account and clear sensitive history
where required.

Directory-encrypted archive editing remains restricted. Create a new archive if
an edit command reports that this mode cannot be updated safely.

## Compression levels

Use `--level N`, where `N` is 1 through 9. The default is 5. `--fast` selects
level 1 and `--max` selects level 9.

```powershell
axiomc a --level 3 fast-backup.axar Data
axiomc a --level 7 high-ratio.axar Data
axiomc a --max maximum-ratio.axar Data
```

| Level | Main profile | Typical use |
|---:|---|---|
| 1 | Dedicated fast LZ path | Minimum CPU time |
| 2-3 | Shallow hash-chain search | Fast backups |
| 4-5 | Balanced hash-chain search | General use |
| 6 | Deep hash-chain search | Better ratio without tree memory |
| 7 | Binary tree, 8 MiB window/block | High ratio |
| 8 | Binary tree, 32 MiB window/block | Very high ratio |
| 9 | Binary tree, 64 MiB window / 16 MiB block | Maximum preset; bounded high-effort search |

Explicit tuning options override the level regardless of argument order.

## Advanced compression options

These options apply to commands that create or recompress data: `add`, `update`,
`fresh`, `sync`, `delete`, `repack`, `sign`, and single-stream `compress`.

### Threads: `--threads N`

`0` means all available hardware threads.

```powershell
axiomc a --threads 8 archive.axar Data
```

### Solid block size: `--block-size SIZE`

For `.axar`, this is the target solid-block size. Larger blocks can find
redundancy across more files, but increase memory use and selective-extraction
work while reducing independent blocks available for parallel compression.

```powershell
axiomc a --block-size 32M archive.axar Data
```

### Dictionary size: `--window SIZE`

This is Axiom's dictionary/match window. Larger windows find more distant
repetition and require more memory, especially with `--bt`.

```powershell
axiomc a --bt --window 64M --block-size 64M archive.axar Data
```

The binary-tree matcher uses approximately `2 x min(window, block) x 4` bytes for
tree links, in addition to input, output, and other codec memory.

### Word size equivalent: `--nice N`

`--nice` is the closest Axiom equivalent to 7-Zip's word-size/fast-bytes setting.
The matcher stops deep searching after finding a match of this length. Larger
values can improve ratio but spend more time searching. The current maximum match
length is 273 bytes.

```powershell
axiomc a --nice 192 archive.axar Data
```

### Search depth: `--chain-depth N`

Controls how many previous candidates the matcher examines. Larger values trade
speed for ratio.

```powershell
axiomc a --chain-depth 256 archive.axar Data
```

### Matcher and parser selection

| Option | Effect |
|---|---|
| `--fast-lz` | Select the byte-token fast profile; disable tree/optimal modes |
| `--bt` | Select the cyclic binary-tree matcher |
| `--optimal` | Enable the dynamic-programming parser |
| `--optimal-depth N` | Set optimal search depth and enable it |
| `--optimal-candidates N` | Set parser candidates and enable it |
| `--lazy` | Check whether delaying a match produces a better match |
| `--no-lazy` | Disable that extra match check |
| `--fast-entropy` | Use cheaper entropy-coder selection |
| `--parallel` | Force the independent parallel-block path |

The optimal parser has significant per-byte CPU and memory cost. Give it a
bounded block size:

```powershell
axiomc a --level 7 --optimal --block-size 16M archive.axar Data
```

### Size syntax

Size options accept integer bytes or a binary suffix:

| Input | Meaning |
|---|---:|
| `65536` | 65,536 bytes |
| `64K` | 64 KiB |
| `16M` | 16 MiB |
| `1G` | 1 GiB |

Suffixes are case-insensitive. Fractional values such as `1.5G` are not accepted.

## Recovery records and volumes

Create an archive with recovery data sized as a percentage of its protected
contents:

```powershell
axiomc a --recovery 10 backup.axar "D:\Work"
```

Inspect, add, replace, or remove the recovery record later:

```text
axiomc recovery <archive.axar> [percent]
axiomc rr <archive.axar> [percent]
```

```powershell
axiomc recovery backup.axar       # show the current record
axiomc recovery backup.axar 15    # rebuild it at 15%
axiomc recovery backup.axar 0     # remove it
```

The valid percentage is `1..100`; `0` means remove. Normal archive edits preserve
an existing recovery percentage and regenerate its parity after the edit.

If `test` reports damage and the recovery locator is still readable, repair the
archive atomically and test again:

```powershell
axiomc repair backup.axar
axiomc t backup.axar
```

`repair` returns exit code 3 when the archive has no recovery record. Damage that
exceeds the parity-shard count cannot be repaired, so a recovery record is not a
substitute for a second independent backup.

Split a completed archive into fixed-size transport volumes:

```text
axiomc split <archive.axar> <size> [recovery-volume-count]
```

```powershell
axiomc split backup.axar 100M
axiomc split backup.axar 100M 3
```

The second example produces `backup.part001.axar`, … plus `backup.rev001`,
`backup.rev002`, and `backup.rev003`. The source `backup.axar` is preserved by
the CLI. At most 255 data and recovery volumes may exist in one set.

Join a complete set, or reconstruct missing/corrupt data volumes when enough
`.rev` members survive:

```text
axiomc join <any-volume> <output.axar>
```

```powershell
axiomc join backup.part001.axar restored.axar
axiomc t restored.axar
```

Any surviving data or recovery volume may identify the set. Joining checks each
payload CRC and the BLAKE3 digest of the complete reconstructed archive.

## Signing and verification

Generate a key pair:

```text
axiomc keygen <secret.key> <public.key>
```

```powershell
axiomc keygen release-secret.key release-public.key
```

The secret key is 64 bytes and must be protected. The public key is 32 bytes and
can be distributed.

Sign an archive:

```text
axiomc sign [compression-options] <archive.axar> <secret.key>
```

```powershell
axiomc sign release.axar release-secret.key
axiomc sign -p "correct horse battery staple" private.axar release-secret.key
```

Verify cryptographic validity using the key embedded in the signature:

```powershell
axiomc verify release.axar
```

Require a particular trusted public key:

```powershell
axiomc verify release.axar release-public.key
```

`verify` uses distinct exit codes:

| Exit code | Meaning |
|---:|---|
| 0 | Valid; if a key was supplied, it is trusted |
| 1 | Invalid signature or another runtime error |
| 2 | Invalid command-line usage |
| 3 | Archive is not signed |
| 4 | Valid signature, but a different key |

```powershell
axiomc verify release.axar release-public.key
if ($LASTEXITCODE -ne 0) {
    throw "Signature verification failed: $LASTEXITCODE"
}
```

## Self-extracting archives

```text
axiomc sfx <archive.axar> <output.exe> [stub.exe]
```

By default, Axiom uses `Axiom.exe` beside `axiomc.exe` as the extraction stub:

```powershell
axiomc sfx release.axar release-setup.exe
```

Supply a specific compatible stub if necessary:

```powershell
axiomc sfx release.axar release-setup.exe "D:\Axiom\Axiom.exe"
```

The output is one standalone Windows executable. It contains the native Axiom GUI
stub, the intact `.axar` payload, and an Axiom SFX trailer; the source archive is
not referenced when the resulting `.exe` runs. Use the GUI and CLI executables
from the same build.

When opened, the self-extractor shows a native extraction dialog with:

- an editable destination and folder browser;
- file/folder counts, unpacked size, encryption state, and signature state;
- replace, skip, or stop-on-conflict policies;
- automatic or explicit extraction thread count;
- optional modification-time restoration and destination-folder opening; and
- worker-thread progress with Pause/Resume and Cancel.

Encrypted payloads request their password before exposing archive metadata.
Signed payloads are verified before extraction; an invalid signature blocks the
operation. File integrity is checked by the normal extraction path.

When **Create a self-extracting Windows executable** is enabled in the GUI's
archive-creation options, the `.axar` path is an intermediate build artifact. It
is removed after the merged `.exe` succeeds, so only the executable remains. The
standalone `axiomc sfx` command preserves its input archive because that command
converts an existing archive supplied by the caller.

You can also supply a destination on the command line, which pre-fills the dialog:

```powershell
.\release-setup.exe "D:\Applications\Release"
```

## Single-stream `.axc` files

```text
axiomc c [compression-options] <input> <output.axc>
axiomc d [--threads N] [-p <password>] <input.axc> <output>
```

```powershell
axiomc c --level 5 database.bin database.axc
axiomc d database.axc restored-database.bin
```

`.axc` is one compressed stream. It does not contain the multi-file directory,
comments, file metadata, or editing features of `.axar`.

## Practical recipes

Fast daily backup:

```powershell
axiomc a --level 3 --threads 0 daily.axar "D:\Work"
axiomc t daily.axar
```

Balanced encrypted backup with hidden names:

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

Create a signed self-extractor:

```powershell
axiomc keygen release-secret.key release-public.key
axiomc a --level 5 release.axar publish
axiomc sign release.axar release-secret.key
axiomc verify release.axar release-public.key
axiomc sfx release.axar release.exe
```

## Errors and safe automation

Successful commands return 0. Invalid syntax or unknown options return 2. Runtime
failures return 1 and print an `axiomc:` message to standard error. `verify` also
uses exit codes 3 and 4 as documented above.

```powershell
axiomc t backup.axar
if ($LASTEXITCODE -ne 0) {
    throw "Backup integrity verification failed"
}
```

For important archives:

1. Write a new path instead of overwriting the previous known-good backup.
2. Run `axiomc t` on the new archive.
3. If signed, run `axiomc verify` with the trusted public key.
4. Perform a test extraction for critical restore workflows.
5. Only then rotate the older backup.

Recovery records and recovery volumes tolerate bounded corruption or loss, but
keep independent archive copies on separate storage for important data.
