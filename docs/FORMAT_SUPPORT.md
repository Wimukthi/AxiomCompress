# What Axiom can do with each archive format

Axiom talks to every archive format through one internal interface. Before
enabling a command, the app and the CLI ask that interface what the format can
actually do — so a command that wouldn't work is greyed out, rather than
failing after you've clicked it.

## The short version

| Format | Browse | Extract | Test | Create | Add / update / sync | Delete | Move | Per-file packed size |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|---|
| AXAR | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | Estimated |
| ZIP | ✓ | ✓ | ✓ | ✓ | Unencrypted only | Unencrypted only | Unencrypted only | Exact |
| 7z | Win | Win | Win | — | — | — | — | Exact |
| RAR / RAR5 | Win | Win | Win | — | — | — | — | Exact |
| ISO | Win | Win | Win | — | — | — | — | Partial |
| CAB | Win | Win | Win | — | — | — | — | Partial |
| TAR family | Win | Win | Win | — | — | — | — | — |

"Win" means Windows only, through the bundled `7z.dll` engine or Windows' own
`tar.exe`.

## AXAR

Axiom's own format, and the only one where everything is available.

You can choose Axiom's adaptive method, Zstandard, LZMA2, Deflate, or Store for
the compressed data, and separately turn on encryption, recovery records, split
volumes, comments, locking, signatures, extended metadata, links, and
self-extracting output.

New AXAR archives can also select live content-defined deduplication. Repeated
chunks are stored once, ordinary add/update/sync/delete/move operations remain
available, and `repack` garbage-collects chunks no longer referenced by the
live directory. Snapshot repositories use the same chunk engine but retain
named historical directories instead of allowing ordinary mutation.

Changing the compression method normally changes only the compressed payload
inside each solid block, nothing else. Axiom's own payloads use AXC v9; the
bundled Zstandard, LZMA2, and Deflate payloads use the bounded AXC v10 envelope.
Store skips compression entirely.

The one exception is the large-solid LZMA2 profile, which stages data on disk
and currently cannot be combined with encryption or recovery records.

### Why per-file sizes are estimates

Files share solid blocks — that sharing is what makes AXAR small. There is
therefore no honest answer to "how many bytes does this one file take", so
Axiom shows a proportional estimate and marks it `≈` in the app. The
archive-level size and ratio are exact.

### Size limits

Axiom windows and LZMA2 dictionaries go up to the 4 GiB user-facing setting;
the value actually stored is 4 GiB−1, which is the largest a 32-bit field
holds. LZMA2 chunks use the same ceiling.

Choosing an AXAR solid block above 4 GiB and up to 64 GiB switches on the
large-solid profile. It stages the raw block on disk and compresses it in
independently decoded pieces, 512 MiB by default. Such an archive requires a
reader that understands the profile, and can't currently carry encryption or
recovery records. Existing AXAR v4 and v5 archives are unaffected.

## ZIP

Full read and write for stored and Deflate entries, with WinZip AES-256 for new
encrypted archives, SFX packaging, and standard split volumes.

Edits are atomic rewrites. Entries you didn't touch are cloned across intact —
their Deflate data, metadata, and CRCs are preserved rather than recompressed.
Ordinary ZIPs, split sets, and ZIP payloads embedded in SFX files all use the
same minizip-ng container path. Entry data, Deflate, and encryption are streamed
in bounded chunks. Creating a split ZIP writes its volumes directly into a
staged set and installs the set transactionally; it does not create a complete
ordinary ZIP first.

Current limits:

- An **existing** encrypted ZIP can be listed, tested, and extracted with its
  password, but not updated, deleted from, or renamed in place.
- Split `.z01`, `.z02`, …, `.zip` sets can be created, browsed, tested, and
  extracted — including AES-256 entries — but not edited. Recreate the set to
  change what's in it.
- ZIP has no equivalent of AXAR's archive comments, locking, recovery records
  and volumes, signatures, encrypted file names, or Axiom metadata, so those
  are unavailable.
- ZIP creation is limited to Store and Deflate, which is what keeps the result
  readable everywhere else.

## The read-only formats

The Windows system provider is deliberately read-only. It loads the bundled
`7z.dll` engine directly for 7z, RAR/RAR5, ISO/UDF, and CAB, and uses Windows'
`tar.exe` for `.tar`, `.tar.gz`, `.tgz`, `.tar.xz`, `.txz`, `.tar.bz2`,
`.tbz2`, `.tar.zst`, and `.tzst`.

Format detection reads file signatures where it can, falling back to the
extension for wrapped and compressed TAR names. Extraction goes through a
temporary staging directory before anything is copied into the destination you
asked for. The `7z.dll` path exposes structured metadata and progress callbacks
directly — no helper process, no parsing console text.

Encrypted 7z archives prompt for a password. Numbered 7z split volumes are read
as one logical stream.

ISO images get special handling. A plain ISO9660 or Joliet image uses Axiom's
own directory reader, so it displays immediately. A hybrid image uses the
authoritative UDF catalog through `7z.dll`, which means a bridge-only tree is
never mistaken for the complete disc.

None of these formats ever appears as something you can create.

## Roadmap

AXAR and ZIP are the supported creation targets. Other formats stay read-only
unless there's a clear compatibility or maintenance reason to expand them.

### Possible: a direct TAR backend

If Axiom needs first-class TAR creation and editing, TAR is the most realistic
next candidate. It is a simple sequential container with no codec licensing
problem, it maps cleanly onto the existing provider model, its edits can be
atomic rewrites exactly like ZIP's, and ustar/pax records carry useful
metadata.

The initial scope would be plain `.tar`; ustar and pax path and name records;
regular files and directories first; symlinks where extraction is safe; create,
add/update/sync, delete, and move by full rewrite; no sparse files.

### Possible: TAR plus external compression

Once plain TAR is stable: `.tar.gz` / `.tgz`, `.tar.zst` / `.tzst`, and
`.tar.xz` once a codec backend is chosen. The first version should be
view, extract, test, and create only — in-place add, update, delete, and move
require decompressing and recompressing the whole stream, which is a different
kind of commitment.

### Staying read-only

| Format | Why |
|---|---|
| 7z | Already implemented through `7z.dll` with structured properties and callbacks, and no helper process |
| RAR | Creation is proprietary and will remain unsupported |
| ISO | Creating one is a disc-authoring workflow, not an archiving one |
| CAB | Already implemented through `7z.dll` |
| GZip / BZip2 / XZ single streams | These are compressed streams, not multi-file archives. They belong as single-file operations or as TAR codecs |

## How the interface works

Which controls you see should follow from the format you picked, not from
guesswork:

- **AXAR** — method-aware level and codec controls, the live level-curve
  preview, and every container option.
- **ZIP** — a Store/Deflate level preview, update mode, and optional password
  encryption of file data. AXAR-only features are hidden or disabled, including
  encrypted names.
- **TAR**, if it is ever implemented — metadata-focused options, and no
  compression level at all for plain `.tar`.
- **Compressed TAR**, if it is ever implemented — show the outer codec and
  level, and be explicit that an edit rebuilds the whole stream.
- **Read-only providers** — never offered as a creation target, even though the
  Open dialog will happily browse them.

The browser uses the same capability flags for its commands, drag and drop,
archive information, and context menus. An action that isn't supported stays
disabled, rather than producing a failure dialog after the fact.

## Extending this

The provider layer is plug-in shaped, but it is **not** externally pluggable,
and that is on purpose. A new format should land as a compiled-in provider
first. That lets the capability model, password prompts, drag-and-drop
behaviour, and command enabling settle before anyone commits to a public C ABI,
a DLL loading policy, a sandboxing story, and a trust model for third-party
parsers.
