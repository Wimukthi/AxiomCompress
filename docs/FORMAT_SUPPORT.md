# Archive format support roadmap

Axiom uses one internal provider interface for every archive format. The GUI and
CLI should ask the active provider for capabilities, then enable only the
commands that are safe for that format.

## Current support

| Format | Browse | Extract | Test | Create | Add/update/sync | Delete | Move/rename | Packed sizes | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| AXAR | Yes | Yes | Yes | Yes | Yes | Yes | Yes | Estimated | Native format with encryption, recovery records, split volumes, comments, locking, signatures, metadata, links, and SFX packaging. |
| ZIP | Yes | Yes | Yes | Yes | Yes, plaintext only | Yes, plaintext only | Yes, plaintext only | Yes | Stored/Deflate ZIP archives. New encrypted ZIPs use WinZip AES-256 file-data encryption. ZIP edits are atomic rewrites. |
| 7z | Windows | Windows | Windows | No | No | No | No | No | Read-only system provider backed by Windows `tar.exe`/libarchive. Compatibility depends on the OS tool. |
| RAR/RAR5 | Windows | Windows | Windows | No | No | No | No | No | Read-only system provider. RAR creation is intentionally unsupported. |
| TAR family | Windows | Windows | Windows | No | No | No | No | No | Covers `.tar`, `.tar.gz`, `.tgz`, `.tar.xz`, `.txz`, `.tar.bz2`, `.tbz2`, `.tar.zst`, and `.tzst` when supported by Windows `tar.exe`. |
| ISO | Windows | Windows | Windows | No | No | No | No | No | Read-only ISO image browsing/extraction through the system provider. |
| CAB | Windows | Windows | Windows | No | No | No | No | No | Read-only CAB browsing/extraction through the system provider. |

ZIP stores exact compressed sizes per central-directory entry. AXAR uses solid
blocks, so per-file Packed values are proportional estimates and the GUI marks
them with `≈`. ZIP can create AES-256 encrypted archives and can read/test/extract
encrypted stored/Deflate entries when a password is supplied. Existing encrypted
ZIPs are not updated, deleted from, or renamed in place yet. ZIP intentionally
does not expose AXAR-only services: archive comments, locking, recovery records,
split volumes, signatures, SFX packaging, encrypted names, and Axiom metadata
remain disabled when ZIP is selected.

The Windows system provider is intentionally read-only. It uses signature checks
where possible, falls back to extensions for wrapped/compressed TAR names, and
routes extraction through a temporary staging directory before copying files into
the requested destination. It does not currently provide archive passwords,
comments, packed sizes, write operations, or fine-grained byte progress.

## Full-support targets

AXAR and ZIP are the supported creation targets. Other formats should stay
read-only unless there is a clear compatibility and maintenance reason to expand
them.

### 1. Direct TAR backend, optional

If Axiom later needs first-class TAR creation/editing, TAR is the most realistic
next full-support provider.

- It has a simple sequential container structure and no codec licensing issue.
- It maps cleanly to Axiom's provider model.
- Edits can be implemented as atomic rewrites, like ZIP.
- It can preserve useful metadata through ustar/pax records.

Initial TAR scope:

- `.tar`
- ustar and pax path/name records
- regular files and directories first
- symlinks where safe to extract
- create, add/update/sync, delete, move/rename by full rewrite
- no sparse files in the first implementation

### 2. TAR plus external compression, optional

After plain TAR is stable, add compressed TAR variants:

- `.tar.gz` / `.tgz`
- `.tar.zst` / `.tzst`
- `.tar.xz` when a codec backend is chosen

The first compressed-TAR implementation should be view/extract/test/create. In
place add/update/delete/move should come later because every edit requires
decompressing and recompressing the complete stream.

## View/extract/test-only targets

These formats should start as read-only providers. Full writing either has
licensing, compatibility, security, or complexity tradeoffs that are not worth
taking in the first pass.

| Format | First scope | Reason |
|---|---|---|
| 7z | Browse, extract, test | Implemented on Windows through the system provider; direct SDK integration can improve password/progress support later. |
| RAR | Browse, extract, test | Implemented on Windows through the system provider; creation remains proprietary and unsupported. |
| ISO | Browse, extract/test | Implemented on Windows through the system provider. Creation is a separate authoring workflow. |
| CAB | Browse, extract/test | Implemented on Windows through the system provider. |
| GZip/BZip2/XZ single streams | Extract/test, optional create | These are compressed streams, not multi-file archives. Surface them as single-file operations or as TAR codecs. |

## GUI behavior

The Add to archive dialog should derive visible controls from the selected
provider:

- AXAR: all native options.
- ZIP: compression level, update mode, and optional file-data password
  encryption. Hide or disable AXAR-only features, including encrypted names.
- TAR: metadata-focused options; no compression level for plain `.tar`.
- Compressed TAR: show the outer compression codec and level; explain that edits
  rebuild the complete stream.
- Read-only providers: never appear as creation targets.

The browser should use the same provider flags for commands, drag/drop, archive
information, and context menus. Unsupported actions should stay disabled instead
of showing late failure dialogs.
