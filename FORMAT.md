# Axiom Archive Format

This document specifies the on-disk layout of a multi-file Axiom archive
(`.axar`). It is distinct from the single-stream `.axc` payload produced by
`axiom::compress`, which an archive embeds once per solid block.

All integers are little-endian. Offsets and sizes are absolute byte positions in
the archive file unless stated otherwise.

## How to read this document

The first half explains the normal archive path: header, solid blocks, central
directory, entries, and footer. The later sections cover optional services:
encryption, recovery records, split volumes, signatures, SFX packaging, and
compatibility rules.

Plain-language layout:

1. The header identifies the file as an Axiom archive.
2. Solid blocks store the compressed file bytes.
3. The central directory says which files exist and where their bytes live.
4. Optional recovery data can protect the archive from damage.
5. The footer points back to the central directory.

Readers should treat all documented limits and validation rules as part of the
format. Rejecting malformed data is intentional behavior, not just an
implementation detail.

## Goals

- Hold many files and directories with their relative paths and metadata.
- Compress with cross-file redundancy (files are grouped into *solid blocks*)
  while keeping each block **independently decompressible** for selective
  extraction and bounded-memory decode.
- Single-pass, bounded-memory writing (one solid block in memory at a time).
- Localizable integrity: per-block checks from the embedded `.axc`, plus
  per-file CRC-32 and BLAKE3-256 content hashes.

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
| Recovery service   |  optional Reed-Solomon parity + locator
+--------------------+
| Footer (24 bytes)  |
+--------------------+
```

### Header (16 bytes, at offset 0)

| field     | type     | notes                                |
|-----------|----------|--------------------------------------|
| magic     | u8[8]    | `"AXIOMAR\0"`                        |
| version   | u16      | format version, currently `4`        |
| flags     | u16      | required-feature flags; bit `0x0001` = encrypted directory (a plaintext encryption preamble follows the header). A reader rejects any bit it does not understand |
| reserved  | u32      | must be `0`                          |

### Solid blocks

The concatenated bytes of the archived files are split into solid blocks whose
uncompressed size is approximately the writer's effective `block_size`. Current
writers choose that size from the compression preset, any explicit
`--block-size`, and the selected thread count; this is a writer policy, not a
different on-disk layout. A file may straddle a block boundary; a file larger
than `block_size` spans several blocks.
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
archive_extra_count × (vint type, vint len, u8[len])   archive-level TLV
```

The trailing **archive-level extra records** carry archive-wide service data;
unknown types are skipped by length, the same way block- and entry-level extras are.

| type | name       | payload                                            |
|------|------------|----------------------------------------------------|
| 1    | comment    | free-form UTF-8 archive comment (the whole payload) |
| 2    | lock       | none — its presence marks the archive read-only     |
| 3    | encryption | KDF params + salt + key-check token (see *Encryption*) |
| 4    | signature  | signer public key (32 bytes) + Monocypher EdDSA signature (64 bytes) |

Recovery data is deliberately outside the central-directory TLV. This lets repair
locate it even when the protected directory itself is damaged.

### Encryption

When an archive is created with a password, every solid block is encrypted and the
`encryption` archive-extra record records how to derive the key:

At a glance:

- The password is never stored.
- Argon2id derives one archive key from the password and per-archive salt.
- Each compressed solid block is sealed independently.
- Block-only encryption leaves the central directory readable.
- `--encrypt-names` also seals the central directory, so listing requires the
  password.

```
vint     kdf_algorithm     2 = Argon2id
vint     mem_blocks        Argon2 memory cost, in 1 KiB blocks
vint     passes            Argon2 time cost
vint     lanes             Argon2 parallelism
vint     salt_len          (16)
u8[]     salt              per-archive random salt
vint     check_len
u8[]     key_check         a fixed plaintext sealed under the key (salt as AD)
```

- **Key:** Argon2id(password, salt, params) → 32 bytes. Derived once per archive.
- **Blocks:** each block's compressed `.axc` bytes are sealed with **XChaCha20-Poly1305**
  (Monocypher). The stored block is `nonce(24) ‖ tag(16) ‖ ciphertext`; the block's
  index (8-byte LE) is the AEAD associated data, so a block is valid only at its own
  position (no reordering or cross-archive transplant). `compressed_size` covers the
  whole sealed blob; `uncompressed_size` is the plaintext block size as before.
- **Wrong-password check:** `key_check` is a known constant sealed under the key; a
  reader re-derives the key and opens it first, rejecting a wrong password before any
  block is read.
- **Editing:** block-only encrypted archives can be edited with the password —
  `add`/`update`/`sync` copy the existing sealed blocks verbatim and seal new ones
  under the same key; `delete`/`repack` decrypt the surviving blocks and re-seal them.
  A wrong password is rejected (via the key-check) before anything is written.
- **Encrypted directory (`--encrypt-names`, RAR's `-hp`):** the whole central
  directory is additionally sealed, hiding names, sizes, and hashes — listing then
  needs the password. The header `flags` set bit `0x0001`, and the KDF parameters move
  to a **plaintext preamble** right after the 16-byte header (a `u32` length then the
  vint-encoded `kdf params + salt + key_check`), since they must be read before the
  sealed directory can be opened. The directory blob at `directory_offset` is then
  `nonce ‖ tag ‖ ciphertext` with a fixed `"AXDIR"` associated-data tag, and it
  carries no `encryption` archive-extra (the preamble has the parameters). Editing a
  directory-encrypted archive is not supported yet.

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
vint     type                  0 = file, 1 = directory, 2 = symlink, 3 = hardlink
vint     path_len
u8[]     path                  relative, UTF-8, '/'-separated, no '..'
  if type == file:
    vint   size                uncompressed size
    vint   first_block         index of the block holding the first byte
    vint   offset              byte offset of the file within first_block
  if type == symlink or hardlink:
    vint   target_len          length of the target
    u8[]   target              symlink: the link target, verbatim (relative or absolute)
                               hardlink: the archive path of the file whose bytes are
                               shared (always an earlier entry); carries no content
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
| 6    | ads_stream | one NTFS named alternate data stream: `vint name_len`, the UTF-8 stream name, then the stream bytes (rest of the record). A file may carry several; each ≤ 1 MiB (larger streams are skipped at capture time) |
| 7    | posix      | POSIX mode, uid, and gid (3 x u32 LE); ignored on Windows |

Readers consume the extra records they understand and **skip the rest by
`payload_len`**. A file's bytes are recovered by reading `size` bytes starting at
(`first_block`, `offset`), continuing into consecutive blocks (using each block's
`uncompressed_size`) when the file straddles a boundary.

### Optional recovery service

An archive created with `--recovery N`, or updated with `axiomc recovery`, places
a systematic Reed-Solomon service after the central directory. The protected byte
range is `[0, directory_offset + directory_size)`: header, optional encryption
preamble, all stored solid blocks, and the complete central directory.

| field | type | notes |
|---|---|---|
| magic | u8[8] | `"AXIOMRR\0"` |
| version | u16 | recovery-service version, currently `1` |
| percent | u16 | requested redundancy percentage, `1..100` |
| data_shards | u16 | systematic data-shard count |
| parity_shards | u16 | Reed-Solomon parity-shard count |
| shard_size | u64 | bytes per shard; final data shard is zero-padded |
| protected_size | u64 | end of the protected archive range |
| directory_offset | u64 | copied directory location for repair |
| directory_size | u64 | copied directory length for repair |
| checksums | u32[] | CRC-32 for every data shard, then every parity shard |
| parity | u8[] | `parity_shards * shard_size` bytes |

The body is followed by a fixed 24-byte locator immediately before the normal
archive footer: `u64 service_offset`, `u64 service_size`, `u8[8] "AXIOMRR\0"`.
The normal footer remains last, so listing and extraction need no special path
when the archive is intact. Repair validates each shard CRC, treats failures as
erasures, reconstructs up to `parity_shards` unavailable shards, and atomically
rewrites the protected data and fresh recovery parity.

### Split and recovery volumes

Volume sets wrap the exact bytes of a completed `.axar`; they are not individually
parseable archives. For `name.axar`, data volumes are `name.part001.axar`,
`name.part002.axar`, … and optional recovery volumes are `name.rev001`,
`name.rev002`, … . Every member begins with this 80-byte header:

| field | type | notes |
|---|---|---|
| magic | u8[8] | `"AXIOMVL\0"` |
| version | u16 | volume format version, currently `1` |
| kind | u16 | `0` data, `1` recovery |
| index | u32 | zero-based index within its kind |
| data_count | u32 | number of data volumes |
| recovery_count | u32 | number of parity volumes |
| shard_size | u64 | target data-volume payload size |
| archive_size | u64 | exact original `.axar` size |
| archive_digest | u8[32] | BLAKE3-256 of the original archive |
| payload_crc | u32 | CRC-32 of the zero-padded shard |
| reserved | u32 | `0` |

Data payloads are consecutive archive byte ranges; only the last may be shorter
than `shard_size`. Recovery payloads are full Reed-Solomon parity shards. Joining
validates set identity and payload CRCs, reconstructs unavailable shards when
enough members survive, truncates to `archive_size`, verifies `archive_digest`,
and installs the output atomically. A set is limited to 255 total volumes.

When every data volume is present, readers concatenate those payload ranges as a
logical random-access archive and can list, test, or extract it directly. This
does not create a temporary joined file and the set remains read-only. Joining is
required for modification and for recovery from missing or corrupt data parts.

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
built for (`.axar` container = `4`, embedded `.axc` stream = `4`) and rejects
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
- **Symbolic links** — stored as links (the target is recorded verbatim, the link
  is *not* followed) and recreated on extract. Creating a symlink on extract can
  require privilege (Windows without Developer Mode); when the OS refuses, that
  link is skipped and the rest of the archive still extracts.
- **Hard links** — files sharing one identity (Windows volume+file index, or POSIX
  dev+inode) are stored **once**: the first occurrence holds the bytes, later ones
  are hardlink entries referencing it, and extract re-links them with
  `create_hard_link` (falling back to an independent copy across volumes). Detection
  costs nothing for the common single-link file (only files with link count > 1 are
  probed).
- Solid (cross-file) compression with per-block *and* per-file CRC-32, random
  access listing, selective file/directory extraction, atomic writes,
  `--overwrite fail|skip|all`, mtime restore, and threaded encode/decode.
- **Add / update** files into an existing archive (`a` on an existing `.axar`):
  existing files are not recompressed — their solid blocks are copied verbatim and
  new files are appended as new blocks, then the directory is rebuilt. An added path
  replaces the existing entry of the same name (the replaced bytes become dead space
  until a repack).
- **File-manager operations:** callers can map a filesystem file/directory to an
  explicit archive destination, extract selected entries (a selected directory
  includes its subtree), and rename/move files or whole directory subtrees without
  recompressing their block data. Hard-link targets are rewritten when their
  canonical path moves; a selectively extracted hardlink is materialized as a
  regular file when its canonical entry was not selected.
- **Update / fresh / sync** (`u`, `f`, `s`): refresh by modification time. `update`
  adds new files and replaces archived copies older than the disk file; `fresh`
  replaces only files already in the archive; `sync` mirrors the inputs — update,
  then delete any archived entry no longer present on disk.
- **Delete / repack** (`delete <path>…`, `repack`): rebuild the archive keeping only
  the surviving entries, re-solidifying their files into fresh blocks so removed and
  replaced data is physically reclaimed. A directory path removes its whole subtree;
  a hard link whose target is removed is dropped. `repack` keeps everything, purely
  reclaiming dead space.
- **Comment** (`comment`) and **lock** (`lock`): a free-form UTF-8 archive comment,
  and a one-way read-only flag after which every edit operation refuses. Both live in
  the archive-level TLV and survive edits; reads (list/test/extract) ignore the lock.
- **Encryption** (`-p`/`--password`): per-block XChaCha20-Poly1305 with an Argon2id
  password key. Wrong password and tampering are rejected. Block-only encryption
  leaves names listable and supports edits with the password. `--encrypt-names`
  additionally seals the central directory, requiring the password to list it.

- **Authenticity signatures:** type-4 archive metadata stores a Curve25519 EdDSA
  public key and signature using Monocypher's BLAKE2b-based primitive. The signed
  digest covers exact header/preamble/block bytes and canonical directory semantics.
  `test` rejects an invalid signature and any edit removes the stale signature.
  This primitive is not wire-compatible with standard SHA-512 Ed25519.
- **SFX packaging:** an intact `.axar` or `.zip` is appended to `Axiom.exe`, followed
  by `"AXIOMSFX"` and a u64 payload length. The stub identifies the embedded provider,
  verifies where supported, and extracts the payload. This wrapper does not change
  either archive format.
- **Recovery records:** optional Reed-Solomon parity protects the archive through
  the end of its central directory and supports atomic repair.
- **Split/recovery volumes:** exact archive bytes can be divided into checked data
  volumes and reconstructed from optional `.rev` parity volumes.

**Additional behavior and current limits**

- **Metadata stored:** mtime (seconds), plus on Windows the file attributes,
  full-precision creation/access/write times, and **NTFS alternate data streams**
  (named streams ≤ 1 MiB each; larger ones are skipped). POSIX mode/uid/gid are
  stored and restored best-effort on POSIX hosts; ownership requires privilege.
- **No special files** (devices, FIFOs, sockets) — only regular files,
  directories, symlinks, and hard links are stored; everything else is skipped.
- **Encryption**: block contents are always sealed; with `--encrypt-names` the
  central directory is sealed too (names/sizes hidden, listing needs the password).
  Block-only encrypted archives can be edited with the password; editing a
  directory-encrypted one is not supported yet.
- Editing (`add`/replace/`delete`/`repack`) rewrites the whole file via a temp +
  atomic rename; there is no true zero-copy in-place append yet. Writing needs a
  seekable output (the directory and footer are written last).
- **Integrity:** per-block CRC-32 (inside each `.axc`) plus a per-file CRC-32 and
  a per-file **BLAKE3-256** content hash, all checked by `test`. Encrypted blocks add
  an authenticated AEAD tag. Optional signatures cover stored block bytes and
  canonical directory metadata.

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

**Extraction safety**

- Path containment is **lexical** — solid against `..` and absolute paths.
- **Symlink-safe**: lexical containment only proves a path *spells* no escape, so
  before materializing any entry the extractor requires every existing directory
  component from the destination root down to that entry's parent to be a **real
  directory, not a redirecting link**. This covers symlinks on every platform and,
  on Windows, **NTFS junctions / mount points** (directory reparse points, which
  need no privilege to create and which `std::filesystem::is_symlink` does not
  report). It rejects both a *pre-existing* link in the destination and one an
  archive plants and then tries to write through (in-order extraction means the
  later entry's parent chain now contains the link and is refused). A restored
  symlink's **target** is still stored verbatim and may point anywhere — the
  guarantee is that no archive entry is written *through* it.

## Not yet specified (planned)

- POSIX special-file records (devices, FIFOs, and sockets).
- Append/update in place and a seekable streaming-write mode for non-seekable
  outputs.
