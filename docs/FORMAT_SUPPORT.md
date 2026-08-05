# Archive format support

Axiom uses one internal provider interface for every archive format. The GUI
and CLI ask the active provider for its capabilities and enable only the
commands that are safe for that format.

## Current support

| Format | Browse | Extract | Test | Create | Add/update/sync | Delete | Move | Packed sizes |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|---|
| AXAR | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | Estimated |
| ZIP | ✓ | ✓ | ✓ | ✓ | Plaintext only | Plaintext only | Plaintext only | Exact |
| 7z | Win | Win | Win | — | — | — | — | Exact |
| RAR / RAR5 | Win | Win | Win | — | — | — | — | Exact |
| ISO | Win | Win | Win | — | — | — | — | Partial |
| CAB | Win | Win | Win | — | — | — | — | Partial |
| TAR family | Win | Win | Win | — | — | — | — | — |

"Win" means Windows only, through the bundled `7z.dll` engine or Windows
`tar.exe`.

### AXAR

The native container. Selectable Axiom, Zstandard, LZMA2, Deflate, or Store
block streams; encryption, recovery records, directly readable read-only split
volumes, comments, locking, signatures, metadata, links, and SFX packaging.

Codec selection normally changes only the independently decoded AXC payload
inside each solid block. Native payloads use AXC v9, and bundled
Zstandard/LZMA2/Deflate payloads use the bounded AXC v10 external-codec
envelope. Store bypasses entropy coding. The large-solid LZMA2 profile is the
explicit exception: it uses disk-staged AXEC subframes and currently does not
combine with encryption or recovery records.

Because files share solid blocks, per-file Packed values are proportional
estimates, marked `≈` in the GUI. Archive-level size and ratio are exact.

Native Axiom windows and LZMA2 dictionaries are supported up to the 4 GiB
user-facing setting; their encoded 32-bit ceiling is 4 GiB−1. LZMA2 AXEC
chunks use the same ceiling. Selecting an AXAR solid block above 4 GiB through
64 GiB enables the required large-solid profile, which stages the raw block on
disk and uses independently decoded bounded chunks (512 MiB by default).
Large-solid archives require a reader that understands the profile and
currently cannot combine it with archive encryption or recovery records.
Existing AXAR v4/v5 archives are unaffected.

### ZIP

Stored and Deflate entries, WinZip AES-256 creation, SFX packaging, and
standard split volumes. Edits are atomic rewrites that clone unchanged
plaintext entries, preserving their Deflate data, metadata, and CRCs.

Current limits:

- Existing encrypted ZIPs can be listed, tested, and extracted with a password,
  but not updated, deleted from, or renamed in place.
- Split `.z01`, `.z02`, …, `.zip` sets can be created, browsed, tested, and
  extracted, including AES-256 entries, but not edited — recreate the set to
  change its contents.
- ZIP does not expose AXAR-only services: archive comments, locking, recovery
  records and volumes, signatures, encrypted names, and Axiom metadata.
- ZIP creation is limited to the interoperable Store and Deflate methods.

### Read-only formats

The Windows system provider is intentionally read-only. It loads the bundled
`7z.dll` engine directly for 7z, RAR/RAR5, ISO/UDF, and CAB, and uses Windows
`tar.exe` for `.tar`, `.tar.gz`, `.tgz`, `.tar.xz`, `.txz`, `.tar.bz2`,
`.tbz2`, `.tar.zst`, and `.tzst`.

It uses signature checks where possible, falls back to extensions for
wrapped and compressed TAR names, and routes extraction through a temporary
staging directory before copying into the requested destination. The DLL path
exposes structured metadata and progress callbacks without a helper process.

Encrypted 7z archives prompt for a password, and numbered 7z split volumes are
read as one logical stream. Pure ISO9660/Joliet images use Axiom's native
directory reader for immediate display; hybrid images use the authoritative UDF
catalog through `7z.dll`, so bridge-only trees are not mistaken for the
complete disc.

These formats never appear as creation targets.

## Roadmap

AXAR and ZIP are the supported creation targets. Other formats stay read-only
unless there is a clear compatibility and maintenance reason to expand them.

### Possible: direct TAR backend

If Axiom needs first-class TAR creation and editing, TAR is the most realistic
next full-support provider: a simple sequential container, no codec licensing
issue, a clean map onto the provider model, edits implementable as atomic
rewrites like ZIP, and useful metadata through ustar/pax records.

Initial scope would be `.tar`; ustar and pax path/name records; regular files
and directories first; symlinks where safe to extract; create, add/update/sync,
delete, and move/rename by full rewrite; no sparse files.

### Possible: TAR plus external compression

After plain TAR is stable: `.tar.gz` / `.tgz`, `.tar.zst` / `.tzst`, and
`.tar.xz` once a codec backend is chosen. The first implementation should be
view/extract/test/create only — in-place add, update, delete, and move require
decompressing and recompressing the complete stream.

### Staying read-only

| Format | Reason |
|---|---|
| 7z | Implemented through `7z.dll` with structured properties and callbacks; no helper process |
| RAR | Creation is proprietary and will remain unsupported |
| ISO | Creation is a separate authoring workflow, not an archiving one |
| CAB | Implemented through `7z.dll` |
| GZip / BZip2 / XZ single streams | These are compressed streams, not multi-file archives. Surface them as single-file operations or as TAR codecs |

## Provider contract

Visible controls should derive from the selected provider:

- **AXAR** — method-aware level and codec controls, the live level-curve
  preview, and all container options.
- **ZIP** — Store/Deflate level preview, update mode, and optional file-data
  password encryption. AXAR-only features hidden or disabled, including
  encrypted names.
- **TAR** (if implemented) — metadata-focused options; no compression level for
  plain `.tar`.
- **Compressed TAR** (if implemented) — show the outer codec and level, and
  explain that edits rebuild the complete stream.
- **Read-only providers** — never appear as creation targets, even though the
  Open dialog can browse them.

The browser uses the same capability flags for commands, drag and drop, archive
information, and context menus. Unsupported actions stay disabled rather than
producing a late failure dialog.
