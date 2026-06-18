# Axiom Archive Format

This document specifies the on-disk layout of a multi-file Axiom archive
(`.axar`). It is distinct from the single-stream `.axc` payload produced by
`axiom::compress`, which an archive embeds once per solid block.

All integers are little-endian. Offsets and sizes are absolute byte positions in
the archive file unless stated otherwise.

## Goals

- Hold many files and directories with their relative paths and metadata.
- Compress with cross-file redundancy (files are grouped into *solid blocks*)
  while keeping each block **independently decompressible** for selective
  extraction and bounded-memory decode.
- Single-pass, bounded-memory writing (one solid block in memory at a time).
- Localizable integrity: a per-block checksum (from the embedded `.axc`) plus a
  per-file CRC-32, so damage can be attributed to a file rather than the whole
  archive.

## Layout

```
+--------------------+
| Header (16 bytes)  |
+--------------------+
| Solid block 0      |  each block is a complete axiom::compress() archive
| Solid block 1      |  of that block's concatenated file bytes
| ...                |
+--------------------+
| Central directory  |  block table + file/dir entries
+--------------------+
| Footer (24 bytes)  |
+--------------------+
```

### Header (16 bytes, at offset 0)

| field     | type     | notes                                |
|-----------|----------|--------------------------------------|
| magic     | u8[8]    | `"AXIOMAR\0"`                        |
| version   | u16      | format version, currently `4`        |
| flags     | u16      | required-feature flags (see below), `0` today; a reader rejects any bit it does not understand |
| reserved  | u32      | must be `0`                          |

### Solid blocks

The concatenated bytes of the archived files are split into solid blocks whose
uncompressed size is approximately `block_size` (default 4 MiB). A file may
straddle a block boundary; a file larger than `block_size` spans several blocks.
Each block's bytes are compressed with `axiom::compress`, producing a
self-contained `.axc` payload (with its own header and CRC-32 — this is the
per-block integrity check). Blocks are written back-to-back after the header. The
`.axc` stream is its own versioned format (currently version `4`) and is described
in [ARCHITECTURE.md](ARCHITECTURE.md); the container does not interpret a block's
internals beyond its declared size and checksum.

### Central directory (at `directory_offset`)

The directory is **vint-encoded** (LEB128 unsigned varints: 7 bits per byte, high
bit = "more follow") and **extensible**: every record that can grow carries a
length so a reader can consume optional fields it knows and skip the rest. This is
how later features (strong hashes, high-precision times, links, attributes, owner)
are added without changing the layout.

```
vint                     block_count
block_count × BlockRec
vint                     entry_count
entry_count × EntryRec
vint                     archive_extra_count
archive_extra_count × (vint type, vint len, u8[len])   archive-level TLV (reserved)
```

The trailing **archive-level extra records** are reserved for service data added
later — comment, recovery-record parameters, encryption parameters, volume info —
and are skipped by length today, the same way block- and entry-level extras are.

`BlockRec`:

| field            | type | notes                                   |
|------------------|------|-----------------------------------------|
| compressed_offset| vint | absolute offset of the block's `.axc`   |
| compressed_size  | vint | byte length of the block's `.axc`       |
| uncompressed_size| vint | bytes the block expands to              |
| extra_len        | vint | length of a reserved block extra area (`0` today) |
| extra            | u8[] | `extra_len` bytes, skipped if unknown   |

`EntryRec` — each entry is a **length-prefixed record** so unknown future entry
types can be skipped whole:

```
vint     record_len            (length of the record body that follows)
--- record body (record_len bytes) ---
vint     type                  0 = file, 1 = directory (future: symlink, hardlink, …)
vint     path_len
u8[]     path                  relative, UTF-8, '/'-separated, no '..'
  if type == file:
    vint   size                uncompressed size
    vint   first_block         index of the block holding the first byte
    vint   offset              byte offset of the file within first_block
--- zero or more TLV extra records, until the body ends ---
vint     record_type
vint     payload_len
u8[]     payload               payload_len bytes
```

**Extra-record types** (current):

| type | name      | payload                                   |
|------|-----------|-------------------------------------------|
| 1    | mtime     | i64 modification time, seconds since epoch (8-byte LE) |
| 2    | crc32     | CRC-32 of the file's bytes (4-byte LE)    |
| 3    | blake3    | BLAKE3-256 digest of the file's bytes (32 bytes) — the strong content hash, verified by `test` |
| 4    | win_attrs | Windows file attributes bitmask (`FILE_ATTRIBUTE_*`, u32 LE) |
| 5    | win_times | Windows creation/access/write times, 100-ns FILETIME ticks since 1601 UTC (3 × u64 LE); full precision, supersedes `mtime` on restore |

Readers consume the extra records they understand and **skip the rest by
`payload_len`**. A file's bytes are recovered by reading `size` bytes starting at
(`first_block`, `offset`), continuing into consecutive blocks (using each block's
`uncompressed_size`) when the file straddles a boundary.

### Footer (24 bytes, at end of file)

| field          | type  | notes                       |
|----------------|-------|-----------------------------|
| directory_offset | u64 | absolute offset of directory|
| directory_size   | u64 | byte length of directory    |
| magic            | u8[8]| `"AXIOMAR\0"`               |

A reader opens the file, reads the trailing 24 bytes, validates the magic, then
seeks to `directory_offset` to read the directory without scanning the blocks.

## Feature flags

`flags` is a bitfield reserved for forward-compatible extensions (e.g. stronger
hashes, POSIX modes, symlink records, encryption). A reader must reject an
archive whose `flags` contains a bit it does not understand. New optional
sections are added behind new flags so that archives produced by older writers
remain readable and readers reject only what they genuinely cannot interpret.

## Resource limits (decompression bombs)

Declared sizes in the header, directory, and embedded `.axc` blocks are
untrusted: an overlapping match can expand a tiny payload to fill an arbitrary
declared size. The decoder defends against this before allocating:

- `decompress` rejects any stream whose declared original size exceeds a
  caller-supplied limit (default 4 GiB) up front, so peak memory is bounded by
  that limit rather than by the attacker's header.
- The archive reader rejects a block whose declared uncompressed size is
  implausible, and decodes each block bounded to the size the directory promised,
  confirming the result matches afterward.
- Pre-reservations are capped, so a malformed size cannot force a huge
  allocation even transiently.

## Compatibility policy

The format is **pre-release and free to change**, so writers and readers are
deliberately strict: a reader accepts **only the exact current version** it was
built for (`.axar` container = `3`, embedded `.axc` stream = `4`) and rejects
anything else with a clear error. There is **no backward or forward compatibility
yet** — an archive must be produced and read by the same build generation.

The `version` field and the reserved `flags` bitfield exist so that, once the
format stabilizes, an incompatible structural change can bump `version` while
additive optional features (stronger hashes, POSIX modes, symlink records,
encryption) ride on `flags` — at which point readers can start accepting older
archives and rejecting only the flags they genuinely cannot interpret.

## Limits

What the format and the current implementation do and do not handle:

**Supported**

- Many regular files and directories, recursive, with relative `/`-separated
  UTF-8 paths; empty files and empty directories are preserved.
- Solid (cross-file) compression with per-block *and* per-file CRC-32, random
  access list/extract, selective single-file extraction, atomic writes,
  `--overwrite fail|skip|all`, mtime restore, and threaded encode/decode.

**Not stored / not supported**

- **Metadata:** mtime (seconds), plus on Windows the file attributes and
  full-precision creation/access/write times. POSIX permission bits and ownership
  are not yet stored (a later phase); NTFS alternate data streams are not yet
  captured.
- **No symlinks or special files** — symlinks are *skipped* when adding.
- **No encryption** and **no append/update in place**; writing needs a seekable
  output (the directory and footer are written last).
- **Integrity:** per-block CRC-32 (inside each `.axc`) plus a per-file CRC-32 and
  a per-file **BLAKE3-256** content hash, all checked by `test`. (Encryption and
  authenticity signatures are later phases.)

**Size and resource ceilings**

- Match-finder positions are 32-bit, so a single block (and any single file
  larger than a block is split across blocks anyway) is bounded near **4 GiB**;
  total archive size across blocks is u64.
- The level-1 `fast_lz` block codec: match **distance ≤ 16 MiB** (24-bit), match
  **length ≤ 273**.
- Stored **path length ≤ 65,535 bytes**.
- File and block counts are u64 but bounded by memory; pre-reservations are
  capped so a malformed count cannot force a huge allocation.
- `decompress` rejects any declared output size above a caller limit (**default
  4 GiB**) before allocating (decompression-bomb guard).

**Extraction-safety caveat**

- Path containment is **lexical** — solid against `..` and absolute paths, but a
  *pre-existing symlink already in the destination directory* is not yet guarded
  (`openat`/`O_NOFOLLOW` is planned). Extract only into trusted directories.

## Not yet specified (planned)

- POSIX permission bits / Windows attributes, symlink and special-file records.
- Append/update in place and a seekable streaming-write mode for non-seekable
  outputs.
