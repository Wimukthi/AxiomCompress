# Axiom archive format

This specifies the on-disk layout of a multi-file Axiom archive (`.axar`) and
the single-stream `.axc` payload it embeds once per solid block.

All integers are little-endian. Offsets and sizes are absolute byte positions
in the archive file unless stated otherwise.

Documented limits and validation rules are **part of the format**. Rejecting
malformed data is intended behavior, not an implementation detail.

> **Status: pre-release.** The format is free to change. The AXAR container is
> version `4`; see [Compatibility](#compatibility).

## Contents

- [Overview](#overview)
- [Header](#header)
- [Solid blocks](#solid-blocks)
- [Embedded AXC streams](#embedded-axc-streams)
- [Central directory](#central-directory)
- [Encryption](#encryption)
- [Recovery service](#recovery-service)
- [Split and recovery volumes](#split-and-recovery-volumes)
- [Footer](#footer)
- [Resource limits](#resource-limits)
- [Compatibility](#compatibility)
- [Capabilities and limits](#capabilities-and-limits)

## Overview

```text
+--------------------+
| Header (16 bytes)  |
+--------------------+
| Solid block 0      |  each block is a complete axiom::compress() stream
| Solid block 1      |  over that block's concatenated file bytes
| ...                |
+--------------------+
| Central directory  |  block table + file/directory entries + archive TLV
+--------------------+
| Recovery service   |  optional Reed-Solomon parity + locator
+--------------------+
| Footer (24 bytes)  |
+--------------------+
```

In plain terms: the header identifies the file, solid blocks hold the
compressed bytes, the central directory says which files exist and where their
bytes live, optional recovery data protects against damage, and the footer
points back to the directory.

### Design goals

- Hold many files and directories with relative paths and metadata.
- Compress with cross-file redundancy — files are grouped into *solid blocks* —
  while keeping each block **independently decompressible** for selective
  extraction and bounded-memory decode.
- Single-pass, bounded-memory writing: one solid block in memory at a time.
- Localizable integrity: per-block checks from the embedded `.axc`, plus
  per-file CRC-32 and BLAKE3-256 content hashes.

## Header

16 bytes at offset 0.

| Field | Type | Notes |
|---|---|---|
| `magic` | `u8[8]` | `"AXIOMAR\0"` |
| `version` | `u16` | Format version, currently `4` |
| `flags` | `u16` | Required-feature flags. Bit `0x0001` = encrypted directory, which means a plaintext encryption preamble follows the header. A reader must reject any bit it does not understand |
| `reserved` | `u32` | Must be `0` |

## Solid blocks

The concatenated bytes of the archived files are split into solid blocks whose
uncompressed size is approximately the writer's effective `block_size`. Writers
choose that size from the compression preset, any explicit `--block-size`, and
the thread count; this is writer policy, not a different on-disk layout.

A file may straddle a block boundary, and a file larger than `block_size` spans
several blocks.

Each block's bytes are compressed with `axiom::compress`, producing a
self-contained `.axc` payload with its own header and CRC-32 — that is the
per-block integrity check. Blocks are written back-to-back after the header.
The container does not interpret a block's internals beyond its declared size
and checksum.

Before building solid blocks, writers group regular files by broad type and
then by extension. This improves cross-file matching without changing AXAR
directory semantics. Directories and links retain their scan order.

## Embedded AXC streams

Versions 5 and later keep the original fixed AXC fields and add a bounded
transform-metadata area before the codec payload.

| Field | Type | Notes |
|---|---|---|
| `magic` | `u8[8]` | `"AXIOMC1\0"` |
| `version` | `u16` | `5` through `10` |
| `codec` | `u8` | Inner store/LZ/parallel codec identifier |
| `flags` | `u8` | Bit `0x01` means transform metadata is present |
| `original_size` | `u64` | Restored byte count |
| `payload_size` | `u64` | Inner codec payload only |
| `crc32` | `u32` | CRC-32 of the original bytes, after inverse transforms |
| `transform_metadata_size` | `u32` | Bytes before the codec payload |
| `transform_metadata` | `u8[]` | Transform ranges; empty when `flags` is zero |
| `payload` | `u8[]` | Bytes consumed by the selected inner codec |

Version 4 streams remain readable; their payload begins at the old 32-byte
fixed header.

### Transform metadata

Begins with a vint range count. Each range stores:

- a transform id — `1` = x86/x64 relative-branch conversion, `2` = byte delta,
  `3` = 16-bit numeric prediction with zigzag and byte-plane shuffle;
- one parameter byte — the delta stride, zero for x86, or the numeric
  predictor's row-width exponent;
- vint `offset`, `size`, and logical `source_offset`.

Ranges are ordered, non-overlapping, and bounded by `original_size`.
Independent source offsets let files and file fragments reset a filter safely
when several inputs share a solid block or one file spans blocks.

Writers content-check PE, PCM WAV, and uncompressed BMP inputs and run a small
trial encode before enabling a candidate. Unsupported, overlapping,
out-of-range, or implausibly numerous ranges are rejected before decode
allocation.

### Version history

| Version | Adds |
|---:|---|
| 4 | Canonical fixed 32-byte header |
| 5 | Transform-present flag and bounded transform-range section |
| 6 | Codec id `6`: sequence-oriented LZ77 payload |
| 7 | Parallel-block codec id `7`: hybrid split/context payload; transform id `3` |
| 8 | Contextual distance-footer coding; static match-byte and full-previous literal modes |
| 9 | Parallel-block codec id `10`: parser-checkpoint context-split representation |
| 10 | Outer codec ids `8` (Zstandard), `9` (LZMA2), `10` (Deflate) |

**Version 6** stores the sequence count and final literal count, followed by
entropy-coded literal-length, match-length, and offset-code streams. Values
`0..15` are direct; larger lengths use a logarithmic code plus packed low bits.
Offset codes `0..3` select an MTF recent-distance entry; higher codes select a
distance slot plus packed low bits. Literals are split into eight lanes by the
high three bits of the previous output byte, either raw or XORed with the
current most-recent-match prediction. Counts, packed-bit padding, lane
consumption, match references, and trailing bytes are all validated exactly.

**Version 7** retains the legacy command, literal-length, match-length,
distance-slot, and distance-footer streams for its first five streams. A
literal-mode byte and eight previous-byte context lanes replace the flat
literal stream.

**Version 8** codes the low distance-footer bits contextually by distance slot,
and adds two static literal modes: match-byte (first literal after a match,
XORed with the rep0 byte, in one of eight lanes chosen by that byte) and
full-previous (all 256 preceding-byte values mapped to at most 16
encoder-chosen clusters, one static stream each).

**Version 9** adds a zero-output command containing four descriptor bytes:
values `0..3` copy a distance from the decoder's current four-entry
recent-distance table, and value `4` reads one explicit distance from the
normal distance-slot stream. The four selected values replace the table
atomically. Checkpoint distances must be nonzero and no greater than the output
position; descriptor, stream-consumption, and footer padding checks remain
exact. Matches retain the full block window but may not cross the static
encoder-selected checkpoint boundaries.

Every representation competes in a per-block bake-off and is emitted only when
its complete payload is strictly smaller than every alternative.

### External-codec envelope (version 10)

Codec ids `8`, `9`, and `10` share one bounded payload:

| Field | Type | Notes |
|---|---|---|
| `magic` | `u8[4]` | `"AXEC"` |
| `payload_version` | `u8` | `1` |
| `property_size` | `u8` | `0` for Zstandard/Deflate; `1` for LZMA2 |
| `reserved` | `u16` | Must be zero |
| `chunk_size` | `u32` | 256 KiB–4 MiB for Zstandard/Deflate; up to 512 MiB for LZMA2 |
| `chunk_count` | `u32` | Exactly `ceil(original_size / chunk_size)` |
| `properties` | `u8[]` | LZMA2 dictionary property byte when present; the decoded dictionary size must not exceed `chunk_size` |
| `chunks` | `record[]` | Exactly `chunk_count` records |

Each chunk record is `raw_size:u32`, `encoded_size:u32`, `flags:u8`, three zero
reserved bytes, then `encoded_size` bytes. Flag bit 0 means the chunk is stored
verbatim; all other bits are invalid. `raw_size` must equal the remaining
bounded chunk geometry, stored chunks require `encoded_size == raw_size`, and
the sum of raw sizes must equal the outer AXC `original_size`.

Compressed chunks are independent Zstandard frames, LZMA2 streams, or zlib
Deflate streams. A reader allocates exactly the validated raw chunk size,
requires the backend to produce that exact size and consume the complete
encoded chunk, and rejects trailing bytes. Writers store a chunk when the
selected codec would expand it.

The chunk boundary is also the cooperative pause/cancel and progress boundary.
Transform metadata, CRC-32, encryption, recovery, signing, and all AXAR
directory semantics are unchanged.

## Central directory

At `directory_offset`. The directory is **vint-encoded** — LEB128 unsigned
varints, 7 bits per byte, high bit means "more follow" — and **extensible**:
every record that can grow carries a length, so a reader consumes the optional
fields it knows and skips the rest.

```text
vint                     block_count
block_count × BlockRec
vint                     entry_count
entry_count × EntryRec
vint                     archive_extra_count
archive_extra_count × (vint type, vint len, u8[len])
```

### Archive-level extra records

Archive-wide service data. Unknown types are skipped by length.

| Type | Name | Payload |
|---:|---|---|
| 1 | comment | Free-form UTF-8 archive comment (the whole payload) |
| 2 | lock | None — its presence marks the archive read-only |
| 3 | encryption | KDF params + salt + key-check token (see [Encryption](#encryption)) |
| 4 | signature | Signer public key (32 bytes) + Monocypher EdDSA signature (64 bytes) |

Recovery data is deliberately **outside** this TLV, so repair can locate it
even when the protected directory itself is damaged.

### BlockRec

| Field | Type | Notes |
|---|---|---|
| `compressed_offset` | vint | Absolute offset of the block's `.axc` |
| `compressed_size` | vint | Byte length of the block's `.axc` |
| `uncompressed_size` | vint | Bytes the block expands to |
| `extra_len` | vint | Length of a reserved block extra area (`0` today) |
| `extra` | `u8[]` | `extra_len` bytes, skipped if unknown |

### EntryRec

Each entry is a length-prefixed record, so unknown future entry types can be
skipped whole.

```text
vint     record_len            length of the record body that follows
--- record body (record_len bytes) ---
vint     type                  0 = file, 1 = directory, 2 = symlink, 3 = hardlink
vint     path_len
u8[]     path                  relative, UTF-8, '/'-separated, no '..'
  if type == file:
    vint   size                uncompressed size
    vint   first_block         index of the block holding the first byte
    vint   offset              byte offset of the file within first_block
  if type == symlink or hardlink:
    vint   target_len
    u8[]   target              symlink: the link target, verbatim
                               hardlink: the archive path of the file whose bytes
                               are shared (always an earlier entry); no content
--- zero or more TLV extra records, until the body ends ---
vint     record_type
vint     payload_len
u8[]     payload
```

#### Entry extra records

| Type | Name | Payload |
|---:|---|---|
| 1 | mtime | `i64` modification time, seconds since epoch (8-byte LE) |
| 2 | crc32 | CRC-32 of the file's bytes (4-byte LE) |
| 3 | blake3 | BLAKE3-256 digest of the file's bytes (32 bytes) — the strong content hash, verified by `test` |
| 4 | win_attrs | Windows file attributes bitmask (`FILE_ATTRIBUTE_*`, `u32` LE) |
| 5 | win_times | Windows creation/access/write times, 100 ns FILETIME ticks since 1601 UTC (3 × `u64` LE); full precision, supersedes `mtime` on restore |
| 6 | ads_stream | One NTFS named alternate data stream: `vint name_len`, the UTF-8 stream name, then the stream bytes (rest of the record). A file may carry several; each ≤ 1 MiB — larger streams are skipped at capture time |
| 7 | posix | POSIX mode, uid, and gid (3 × `u32` LE); ignored on Windows |

Readers consume the records they understand and **skip the rest by
`payload_len`**. A file's bytes are recovered by reading `size` bytes starting
at (`first_block`, `offset`), continuing into consecutive blocks using each
block's `uncompressed_size` when the file straddles a boundary.

## Encryption

When an archive is created with a password, every solid block is encrypted and
the `encryption` archive-extra record records how to derive the key.

At a glance:

- The password is never stored.
- Argon2id derives one archive key from the password and a per-archive salt.
- Each compressed solid block is sealed independently.
- Block-only encryption leaves the central directory readable.
- `--encrypt-names` also seals the directory, so listing needs the password.

```text
vint     kdf_algorithm     2 = Argon2id
vint     mem_blocks        Argon2 memory cost, in 1 KiB blocks
vint     passes            Argon2 time cost
vint     lanes             Argon2 parallelism
vint     salt_len          (16)
u8[]     salt              per-archive random salt
vint     check_len
u8[]     key_check         a fixed plaintext sealed under the key (salt as AD)
```

**Key.** `Argon2id(password, salt, params)` → 32 bytes, derived once per
archive.

**Blocks.** Each block's compressed `.axc` bytes are sealed with
XChaCha20-Poly1305 (Monocypher). The stored block is
`nonce(24) ‖ tag(16) ‖ ciphertext`, and the block's index (8-byte LE) is the
AEAD associated data — so a block is valid only at its own position, with no
reordering or cross-archive transplant. `compressed_size` covers the whole
sealed blob; `uncompressed_size` is the plaintext block size as before.

**Wrong-password check.** `key_check` is a known constant sealed under the key.
A reader re-derives the key and opens it first, rejecting a wrong password
before any block is read.

**Editing.** Block-only encrypted archives can be edited with the password:
`add`/`update`/`sync` copy existing sealed blocks verbatim and seal new ones
under the same key, while `delete`/`repack` decrypt the survivors and re-seal
them. A wrong password is rejected via the key-check before anything is
written.

**Encrypted directory.** With `--encrypt-names`, the whole central directory is
additionally sealed, hiding names, sizes, and hashes. Header `flags` bit
`0x0001` is set, and the KDF parameters move to a **plaintext preamble**
immediately after the 16-byte header — a `u32` length, then the vint-encoded
KDF params, salt, and key_check — because they must be read before the sealed
directory can be opened. The directory blob at `directory_offset` is then
`nonce ‖ tag ‖ ciphertext` with a fixed `"AXDIR"` associated-data tag, and it
carries no `encryption` archive-extra. Editing a directory-encrypted archive is
not supported.

## Recovery service

An archive created with `--recovery N`, or updated with `axiomc recovery`,
places a systematic Reed-Solomon service after the central directory. The
protected range is `[0, directory_offset + directory_size)`: header, optional
encryption preamble, all stored solid blocks, and the complete directory.

| Field | Type | Notes |
|---|---|---|
| `magic` | `u8[8]` | `"AXIOMRR\0"` |
| `version` | `u16` | Recovery-service version, currently `1` |
| `percent` | `u16` | Requested redundancy percentage, `1..100` |
| `data_shards` | `u16` | Systematic data-shard count |
| `parity_shards` | `u16` | Reed-Solomon parity-shard count |
| `shard_size` | `u64` | Bytes per shard; the final data shard is zero-padded |
| `protected_size` | `u64` | End of the protected archive range |
| `directory_offset` | `u64` | Copied directory location, for repair |
| `directory_size` | `u64` | Copied directory length, for repair |
| `checksums` | `u32[]` | CRC-32 for every data shard, then every parity shard |
| `parity` | `u8[]` | `parity_shards × shard_size` bytes |

The body is followed by a fixed 24-byte locator immediately before the normal
footer: `u64 service_offset`, `u64 service_size`, `u8[8] "AXIOMRR\0"`. The
normal footer remains last, so listing and extraction need no special path when
the archive is intact.

Repair validates each shard CRC, treats failures as erasures, reconstructs up
to `parity_shards` unavailable shards, and atomically rewrites the protected
data with fresh parity.

## Split and recovery volumes

Volume sets wrap the exact bytes of a completed `.axar`; individual members are
not parseable archives. For `name.axar`, data volumes are `name.part001.axar`,
`name.part002.axar`, … and optional recovery volumes are `name.rev001`,
`name.rev002`, … . Every member begins with this 80-byte header:

| Field | Type | Notes |
|---|---|---|
| `magic` | `u8[8]` | `"AXIOMVL\0"` |
| `version` | `u16` | Volume format version, currently `1` |
| `kind` | `u16` | `0` data, `1` recovery |
| `index` | `u32` | Zero-based index within its kind |
| `data_count` | `u32` | Number of data volumes |
| `recovery_count` | `u32` | Number of parity volumes |
| `shard_size` | `u64` | Target data-volume payload size |
| `archive_size` | `u64` | Exact original `.axar` size |
| `archive_digest` | `u8[32]` | BLAKE3-256 of the original archive |
| `payload_crc` | `u32` | CRC-32 of the zero-padded shard |
| `reserved` | `u32` | `0` |

Data payloads are consecutive archive byte ranges; only the last may be shorter
than `shard_size`. Recovery payloads are full Reed-Solomon parity shards.

Joining validates set identity and payload CRCs, reconstructs unavailable
shards when enough members survive, truncates to `archive_size`, verifies
`archive_digest`, and installs the output atomically. A set is limited to 255
total volumes.

When every data volume is present, readers concatenate the payload ranges as a
logical random-access archive and can list, test, or extract directly — no
temporary joined file, and the set stays read-only. Joining is required for
modification and for recovery from missing or corrupt data parts.

## Footer

24 bytes at end of file.

| Field | Type | Notes |
|---|---|---|
| `directory_offset` | `u64` | Absolute offset of the directory |
| `directory_size` | `u64` | Byte length of the directory |
| `magic` | `u8[8]` | `"AXIOMAR\0"` |

A reader opens the file, reads the trailing 24 bytes, validates the magic, then
seeks to `directory_offset` — no scanning of the blocks.

## Resource limits

Declared sizes in the header, directory, and embedded `.axc` blocks are
untrusted: an overlapping match can expand a tiny payload to fill an arbitrary
declared size. The decoder defends before allocating:

- `decompress` rejects any stream whose declared original size exceeds a
  caller-supplied limit (**default 4 GiB**) up front, so peak memory is bounded
  by that limit rather than by the attacker's header.
- The archive reader rejects a block whose declared uncompressed size is
  implausible, decodes each block bounded to the size the directory promised,
  and confirms the result matches afterwards.
- Pre-reservations are capped, so a malformed count or size cannot force a huge
  allocation even transiently.

### Size ceilings

- Match-finder positions are 32-bit, so a single block is bounded near **4 GiB**
  (a file larger than a block is split across blocks anyway). Total archive size
  across blocks is `u64`.
- The level-1 `fast_lz` block codec: match **distance ≤ 16 MiB** (24-bit),
  match **length ≤ 273**.
- Stored **path length ≤ 65,535 bytes**.
- File and block counts are `u64` but bounded by memory.

## Compatibility

The format is **pre-release and free to change**.

| Layer | Written | Accepted |
|---|---|---|
| AXAR container | `4` | `4` |
| AXC, native Axiom method | `9` | `4`–`10` |
| AXC, external codec methods | `10` | `4`–`10` |
| Recovery service | `1` | `1` |
| Volume header | `1` | `1` |

Older readers reject AXC `9` and `10`, so archives written with an external
method require Axiom 0.7.0.0 or newer. Unknown AXAR versions, AXC versions,
required flags, codecs, transforms, and transform parameters are all rejected
with a clear error.

The `version` field and the reserved `flags` bitfield exist so that, once the
format stabilizes, an incompatible structural change can bump `version` while
additive optional features ride on `flags` — at which point readers can accept
older archives and reject only the flags they genuinely cannot interpret.

## Capabilities and limits

### Supported

- Many regular files and directories, recursive, with relative `/`-separated
  UTF-8 paths. Empty files and empty directories are preserved.
- **Symbolic links** — stored as links, with the target recorded verbatim and
  *not* followed, and recreated on extract. Creating a symlink on extract can
  require privilege (Windows without Developer Mode); when the OS refuses, that
  link is skipped and the rest of the archive still extracts.
- **Hard links** — files sharing one identity (Windows volume + file index, or
  POSIX dev + inode) are stored **once**. The first occurrence holds the bytes,
  later ones are hardlink entries referencing it, and extract re-links them
  with `create_hard_link`, falling back to an independent copy across volumes.
  Detection costs nothing for the common single-link file, since only files
  with link count > 1 are probed.
- **Solid compression** with per-block and per-file CRC-32, random-access
  listing, selective file and directory extraction, atomic writes,
  `--overwrite fail|skip|all`, mtime restore, and threaded encode/decode.
- **Add and update** into an existing archive: existing files are not
  recompressed — their solid blocks are copied verbatim, new files are appended
  as new blocks, and the directory is rebuilt. An added path replaces the
  existing entry of the same name, and the replaced bytes become dead space
  until a repack.
- **File-manager operations:** map a filesystem file or directory to an
  explicit archive destination, extract selected entries (a selected directory
  includes its subtree), and rename or move files and whole subtrees without
  recompressing block data. Hard-link targets are rewritten when their
  canonical path moves; a selectively extracted hardlink is materialized as a
  regular file when its canonical entry was not selected.
- **Update / fresh / sync** — refresh by modification time.
- **Delete / repack** — rebuild keeping only surviving entries, re-solidifying
  their files into fresh blocks so removed and replaced data is physically
  reclaimed. A directory path removes its whole subtree; a hard link whose
  target is removed is dropped.
- **Comment and lock** — a free-form UTF-8 comment and a one-way read-only
  flag. Both live in the archive-level TLV and survive edits; reads ignore the
  lock.
- **Encryption** — per-block XChaCha20-Poly1305 with an Argon2id password key.
- **Signatures** — type-4 archive metadata stores a Curve25519 EdDSA public key
  and signature using Monocypher's BLAKE2b-based primitive. The signed digest
  covers exact header, preamble, and block bytes plus canonical directory
  semantics. `test` rejects an invalid signature, and any edit removes the
  stale signature. This primitive is **not** wire-compatible with standard
  SHA-512 Ed25519.
- **SFX packaging** — an intact `.axar` or `.zip` is appended to Axiom's
  read-only SFX PE image, followed by `"AXIOMSFX"` and a `u64` payload length.
  The image ships as the non-executable `AxiomSfx.bin` and is read only during
  SFX creation. This wrapper does not change either archive format.
- **Recovery records** and **split/recovery volumes**, as specified above.

### Metadata

mtime in seconds, plus on Windows the file attributes, full-precision
creation/access/write times, and **NTFS alternate data streams** (named streams
≤ 1 MiB each; larger ones are skipped). POSIX mode, uid, and gid are stored and
restored best-effort on POSIX hosts; ownership requires privilege.

### Not supported

- **No special files** — devices, FIFOs, and sockets are skipped. Only regular
  files, directories, symlinks, and hard links are stored.
- **No in-place append.** Editing rewrites the whole file via a temporary plus
  atomic rename, and writing needs a seekable output because the directory and
  footer are written last.
- **No editing of directory-encrypted archives.**

### Extraction safety

Path containment is **lexical** — solid against `..` and absolute paths.

It is also **symlink-safe**: lexical containment only proves a path *spells* no
escape, so before materializing any entry the extractor requires every existing
directory component, from the destination root down to that entry's parent, to
be a **real directory and not a redirecting link**. This covers symlinks on
every platform and, on Windows, **NTFS junctions and mount points** — directory
reparse points, which need no privilege to create and which
`std::filesystem::is_symlink` does not report.

It rejects both a pre-existing link in the destination and one an archive
plants and then tries to write through: in-order extraction means the later
entry's parent chain now contains the link and is refused.

A restored symlink's **target** is still stored verbatim and may point
anywhere. The guarantee is that no archive entry is ever written *through* one.

## Planned

- POSIX special-file records for devices, FIFOs, and sockets.
- Append and update in place, plus a seekable streaming-write mode for
  non-seekable outputs.
