# Axiom archive format

This specifies the on-disk layout of a multi-file Axiom archive (`.axar`) and
the single-stream `.axc` payload it embeds once per solid block.

All integers are little-endian. Offsets and sizes are absolute byte positions
in the archive file unless stated otherwise.

Documented limits and validation rules are **part of the format**. Rejecting
malformed data is intended behavior, not an implementation detail.

> **Compatibility baseline.** AXAR v4 remains readable indefinitely. AXAR v5 is
> an additive container revision for required fidelity metadata, encryption-v2,
> snapshot repositories, and the large solid-block profile; readers reject
> required features they do not understand instead of silently losing them. The
> append-generation record described below is an additive v4/v5 extension and
> does not change the header version. The current v4 and v5 golden fixtures live
> under `tests/fixtures/`.

## Contents

- [Overview](#overview)
- [Header](#header)
- [Solid blocks](#solid-blocks)
- [Embedded AXC streams](#embedded-axc-streams)
- [Central directory](#central-directory)
- [Append generations](#append-generations)
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
| Generation record  |  optional 64-byte append-history extension
+--------------------+
| Recovery service   |  optional Reed-Solomon parity + locator
+--------------------+
| Footer (24 bytes)  |
+--------------------+
```

In plain terms: the header identifies the file, solid blocks hold the
compressed bytes, the central directory says which files exist and where their
bytes live, an optional generation record links an appended update to its
previous footer, optional recovery data protects against damage, and the footer
points back to the directory.

### Design goals

- Hold many files and directories with relative paths and metadata.
- Compress with cross-file redundancy — files are grouped into *solid blocks* —
  while keeping each block **independently decompressible** for selective
  extraction and bounded-memory decode.
- Single-pass, bounded-memory writing: one ordinary solid block in memory at a
  time; the large LZMA2 profile uses a temporary-file spool and bounded codec
  chunks.
- Localizable integrity: per-block checks from the embedded `.axc`, plus
  per-file CRC-32 and BLAKE3-256 content hashes.

## Header

16 bytes at offset 0.

| Field | Type | Notes |
|---|---|---|
| `magic` | `u8[8]` | `"AXIOMAR\0"` |
| `version` | `u16` | `4` for the baseline container, `5` when v5 fidelity metadata or encryption-v2 is present |
| `flags` | `u16` | Required-feature flags. Bit `0x0001` = encrypted directory, `0x0002` = sparse entry maps, `0x0004` = source-capture warning report, `0x0008` = extended metadata (ACL/xattr/reparse payloads), `0x0010` = encryption-v2 password slots, `0x0020` = snapshot chunk table and manifest, `0x0040` = large solid-block profile. A reader must reject any bit it does not understand |
| `reserved` | `u32` | Must be `0` |

## Solid blocks

The concatenated bytes of the archived files are split into solid blocks whose
uncompressed size is approximately the writer's effective `block_size`. Writers
choose that size from the compression preset, any explicit `--block-size`, and
the thread count; this is writer policy, not a different on-disk layout.

A file may straddle a block boundary, and a file larger than `block_size` spans
several blocks.

For ordinary blocks, the bytes are compressed with `axiom::compress`, producing
a self-contained `.axc` payload with its own header and CRC-32 — that is the
per-block integrity check. Blocks are written back-to-back after the header.
The container does not interpret a block's internals beyond its declared size
and checksum.

### Large LZMA2 solid blocks

When header flag `0x0040` is set, AXAR permits blocks up to **64 GiB**. This
profile currently requires LZMA2. The writer stages the raw block in a temporary
file and emits one AXC v10 external-codec envelope whose LZMA2 chunks are
**512 MiB by default**, or up to **4 GiB−1** when a larger dictionary is
explicitly requested. The outer solid-block size and the codec chunk size are
therefore separate limits: a 64 GiB solid block does not require a 64 GiB
allocation or a 64 GiB LZMA2 dictionary.

Every large block carries a complete external-chunk subframe map. Readers use
that map for extraction and testing, decoding only the chunk ranges needed by
the current file operation. A large block without a valid map is rejected.
The streamed profile disables file-aware transforms; its AXC CRC covers the
staged source bytes directly and its transform-metadata area is empty. The
profile is deliberately rejected when archive block encryption or recovery
records are requested; those paths still use whole-block buffers and need their
own streaming profiles. Older readers reject flag `0x0040` before parsing the
directory.

Before building solid blocks, writers group regular files by broad type and
then by extension. This improves cross-file matching without changing AXAR
directory semantics. Directories and links retain their scan order.

During an add/update/fresh/sync operation, a regular file whose size, CRC-32,
and BLAKE3 digest match an existing stored file may reuse that file's data
range. Repack performs the same coalescing for duplicate entries while it
rewrites blocks. This is represented entirely by ordinary directory addresses;
it does not add a flag or require a new AXAR version. If the source cannot be
hashed reliably, the writer uses the normal new-block path.

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
| `chunk_size` | `u32` | 256 KiB–4 MiB for Zstandard/Deflate; up to 4 GiB−1 for LZMA2. Large LZMA2 solid blocks default to 512 MiB and may raise this independently of the outer AXAR solid-block size |
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
selected codec would expand it. LZMA2 property `40` denotes the maximum
4 GiB−1 dictionary; the effective dictionary is bounded by the declared chunk
and by the stable input bound chosen for the payload. Ordinary payloads use
their total input size, while the disk-streamed large-solid profile uses its
  working-chunk bound, so a small payload does not force a multi-gigabyte
  allocation and all frames retain one property byte.

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
| 5 | capture_report | `vint warning_count`, followed by `vint path_len`, UTF-8 path, `vint message_len`, UTF-8 message for each incomplete source capture |
| 6 | encryption_v2 | Key identifier plus one or more password slots (see [Encryption](#encryption)) |
| 7 | chunk_table | Snapshot chunk-table version, keyed-ID flag, and chunk records: logical size, CRC-32, BLAKE3 identity, block index, and block offset |
| 8 | snapshot_manifest | Snapshot manifest version and bounded named snapshot records. Each record contains generation, creation time, and length-prefixed snapshot entry bodies |

Recovery data is deliberately **outside** this TLV, so repair can locate it
even when the protected directory itself is damaged.

### BlockRec

| Field | Type | Notes |
|---|---|---|
| `compressed_offset` | vint | Absolute offset of the block's `.axc` |
| `compressed_size` | vint | Byte length of the block's `.axc` |
| `uncompressed_size` | vint | Bytes the block expands to |
| `extra_len` | vint | Length of the optional block extra area |
| `extra` | `u8[]` | Block-level TLV records; skipped by readers that do not use them |

The current optional block extra profile is type `1`, **subframe map**. It is
not a new AXAR version or required header flag, and writers omit it when a
block is encrypted, transformed, or has no independently decodable frames.
Its payload is:

```text
vint     map_version             1
vint     frame_count
frame_count × {
  vint   uncompressed_offset     relative to the start of this solid block
  vint   uncompressed_size
  vint   compressed_offset       relative to the start of the AXC block
  vint   compressed_size         complete independently decodable frame
  vint   kind                    1 = stored, 2 = parallel AXC block, 3 = AXEC chunk
  vint   codec                   inner parallel codec, or outer AXC codec
  vint   lzma_property           LZMA2 property for kind 3; otherwise zero
}
```

The map ranges are sorted, non-empty, and cover the complete uncompressed
solid block without gaps. Compressed ranges are bounded and non-overlapping.
For a stored AXC block there is one range; for the parallel-block and external
codec envelopes each range points at the complete frame/chunk header as well as
its payload. AXAR readers may use these ranges to decode only the frames that
intersect a selected file. The public `ArchiveEntry::subframes` view converts
them to entry-relative ranges and absolute archive offsets for inspection.
Unknown block-extra TLVs remain length-delimited and are skipped, preserving
the old directory parser contract.

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
| 8 | sparse_map | Map version `vint 1`, then `vint extent_count`, followed by ordered `vint offset`, `vint length` allocated ranges. The complement of these ranges is logically zero-filled and is recreated as holes when supported |

| 9 | security_descriptor | Bounded self-relative Windows security descriptor; maximum 64 KiB |
| 10 | xattr | `vint name_len`, UTF-8 attribute name, then raw value; maximum 1 MiB per value and 128 values per entry |
| 11 | reparse | `vint tag`, `vint data_len`, and the bounded opaque Windows reparse buffer; restored only after content writes |
| 12 | chunk_refs | Snapshot-only: `vint profile_version 1`, `vint reference_count`, then chunk-table indices |

Readers consume the records they understand and **skip the rest by
`payload_len`**. A file's bytes are recovered by reading `size` bytes starting
at (`first_block`, `offset`), continuing into consecutive blocks using each
block's `uncompressed_size` when the file straddles a boundary.
Multiple file entries may intentionally contain the same (`first_block`,
`offset`) pair. Readers must treat data ranges as shareable rather than
assuming that every entry owns a unique range.

### Snapshot repositories and chunk deduplication

The snapshot profile sets required header flag `0x0020` and writes AXAR v5.
Instead of giving each snapshot file a single solid-block address, the archive
stores a content-defined sequence of independently addressable chunks. The
default boundaries target 256 KiB minimum, 1 MiB average, and 4 MiB maximum;
the exact geometry is writer policy and is recorded only through the resulting
chunk references. Each distinct chunk is compressed once and the current entry
stores an ordered list of chunk-table indices. A changed file therefore reuses
unchanged chunks from earlier snapshots while only new chunks are appended.

Archive extra type 7 is the chunk table. Each record contains the logical chunk
size, CRC-32, a 32-byte BLAKE3 identity, and the block/offset containing the
chunk bytes. With keyed identifiers enabled, the BLAKE3 identity is keyed by
the archive data key; the key is never stored in the identifier. Keyed IDs are
the default for encrypted snapshot repositories, while `--plain-chunks` may be
used when equality across encrypted repositories is an intentional requirement.
Every extracted chunk is checked against both its CRC and identity before it is
returned to the file stream.

Archive extra type 8 is the snapshot manifest. It stores a bounded unique name,
generation number, creation time, and a complete entry catalogue for each
snapshot. Manifest entry bodies use the same path, content, sparse, link, and
metadata semantics as the live directory, plus entry extra type 12. The live
directory is the newest snapshot, so ordinary listing, testing, and extraction
remain correct without selecting a historical snapshot.

Snapshot creation and addition are append-compatible. `prune` removes selected
historical manifests but never the current one; `repack` marks chunks reachable
from the current and retained manifests, copies only their blocks, remaps all
references, and thereby performs snapshot garbage collection. Content mutation
through ordinary add/update/fresh/sync/delete/move operations is rejected so a
legacy operation cannot silently discard snapshot history. Metadata-only
operations such as comments, locking, password-slot changes, and signing keep
the chunk table and manifests intact.

### Sparse-file allocation maps

AXAR always stores the complete logical byte stream, including zero bytes in
holes. When the source filesystem exposes reliable allocated ranges, the writer
adds entry extra type 8 and upgrades the header to v5. This keeps compression,
hashing, and old v4 directory addressing straightforward while allowing a
restore to recover the source file's physical allocation pattern.

The extent list is sorted, non-overlapping, non-empty, and bounded by the
entry's logical `size`. An empty list is valid and represents a file whose
entire logical range is a hole. If a sparse candidate cannot be queried, the
writer archives the bytes densely, records the reason in archive extra type 5,
and reports it through the operation warning channel. Strict metadata mode turns
that warning into a failed operation. Extraction similarly reports an inability
  to punch holes unless strict metadata mode is enabled.

## Append generations

An archive update can append a new generation without copying the previous block
region. The previous complete archive remains byte-for-byte at the start of the
file, including its directory, optional recovery data, and legacy footer. The
new generation appends any new solid blocks, a new directory, this extension,
optional recovery data, and a new legacy footer. Replaced data is therefore
retained as dead space until `repack` or another compacting rewrite.

The original v4/v5 layout without this record is the legacy starting generation.
The first appended generation is numbered `1`; later generations increment that
number. The fixed extension is immediately after the current directory and is
always exactly 64 bytes:

| Field | Type | Notes |
|---|---|---|
| `magic` | `u8[8]` | `"AXIOMGF\0"` |
| `version` | `u16` | Generation-extension version, currently `1` |
| `flags` | `u16` | Must be `0` |
| `size` | `u32` | Must be `64` |
| `generation` | `u64` | Non-zero monotonically increasing generation number |
| `previous_footer_offset` | `u64` | Absolute offset of the previous generation's 24-byte footer |
| `previous_directory_offset` | `u64` | Directory offset copied from that previous footer |
| `previous_directory_size` | `u64` | Directory size copied from that previous footer |
| `previous_generation_offset` | `u64` | Previous generation extension offset, or `0` for the legacy starting layout |
| `reserved` | `u64` | Must be `0` |

The generation record is part of the protected range when recovery data is
present: `[0, directory_offset + directory_size + 64)`. The final footer keeps
the legacy 24-byte shape, so a reader that understands AXAR v4/v5 can still
locate the current directory in an intact appended archive. A generation-aware
reader additionally scans backward for the newest valid footer when a write was
interrupted after the previous footer; an incomplete or invalid newer extension
is ignored and the previous complete generation is opened.

Signatures include the canonical generation fields and the current directory
semantics. A signature mutation appends a new generation; ordinary edits clear
the old signature before writing the replacement directory. Direct append is
used when header features remain compatible. Header-changing operations,
compaction, deletion, and other paths that need a complete rebuild retain the
atomic temporary-file replacement path.

## Encryption

When an archive is created with a password, every solid block is encrypted. AXAR
has two encryption profiles: legacy v1, retained for AXAR v4 compatibility, and
encryption-v2, used for new password archives. Readers must support both.

### Common block encryption

- The password is never stored.
- Each compressed solid block is sealed independently with XChaCha20-Poly1305.
- A block's index is the AEAD associated data, preventing block reordering or
  cross-archive transplant.
- Block-only encryption leaves the central directory readable. `--encrypt-names`
  also seals the directory, so listing needs a password.

### Legacy encryption v1 (AXAR v4)

Legacy encrypted archives store archive-extra type `3`:

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

`Argon2id(password, salt, params)` derives the 32-byte archive key. The same
key seals the solid blocks and, when requested, the directory. The `key_check`
value is a known plaintext sealed under that key; readers authenticate it
before reading block data, so a wrong password is rejected early.

### Encryption-v2 password slots (AXAR v5)

New password archives use header flag `0x0010` and archive-extra type `6` when
the directory is public. The type-6 payload is:

```text
u8[8]  magic                 "AXIOME2\\0"
u16    profile_version       2
u16    options               0 (no optional options are currently allowed)
u8[16] key_id                random archive key identifier
vint   slot_count            1..16

repeat slot_count times:
  u32   slot_id
  vint  kdf_algorithm
  vint  mem_blocks
  vint  passes
  vint  lanes
  vint  salt_len             16
  u8[]  salt
  vint  wrapped_key_len      72
  u8[]  wrapped_key
```

The writer generates a random 32-byte archive data key. Each password slot
derives its own Argon2 key from its own salt, then wraps that same data key with
XChaCha20-Poly1305. The key identifier and slot identifier are authenticated
associated data. Any slot password can therefore unlock the same archive, while
changing a password only replaces that slot's wrapper.

For an encrypted directory, the same payload is stored in a plaintext preamble
immediately after the 16-byte header: a little-endian `u32` payload length
followed by the type-6 payload. The directory is then sealed as
`nonce(24) ‖ tag(16) ‖ ciphertext` with the fixed directory associated-data
tag. In this form there is no type-6 directory record because the directory is
not readable until after the preamble has been processed. Header flags `0x0001`
and `0x0010` are both set.

**Blocks.** Each block's compressed `.axc` bytes is sealed under the random
archive data key. `compressed_size` covers the complete
`nonce(24) ‖ tag(16) ‖ ciphertext` blob, and `uncompressed_size` remains the
plaintext block size.

**Wrong-password check.** A reader tries the bounded slot list and accepts a
password only when one wrapped data key authenticates. Corrupt slot metadata,
duplicate slot IDs, unsupported options, and implausible KDF costs are rejected
before expensive key derivation or archive writes.

**Password management.** The CLI exposes three slot operations:

```powershell
axiomc password-add -p "current password" archive.axar "new password"
axiomc password-change -p "current password" archive.axar "replacement password"
axiomc password-remove -p "current password" archive.axar "password to remove"
```

`password-add` appends a slot, `password-change` replaces the slot identified
by the current password, and `password-remove` removes the slot identified by
the final password. The last remaining slot cannot be removed. A password
mutation authenticates the current password first, then rewrites only the
encryption metadata and directory. Existing compressed/encrypted block bytes
are copied verbatim; they are not decompressed, recompressed, or re-encrypted.
For an encrypted directory, its preamble and directory are rewritten and block
offsets may move, but the stored block payloads remain unchanged.

Mutating a legacy v1 encrypted archive migrates its encryption metadata to the
v2 profile while preserving the existing data key and block bytes. The archive
then uses AXAR v5 and can have multiple password slots. Existing v4 encrypted
archives remain readable without migration.

Password mutations remove an existing archive signature because the signed
password metadata changes; re-sign the archive after the operation. Any
existing recovery service is rebuilt against the new header/directory layout.

### Encrypted directory

With `--encrypt-names`, the whole central directory is sealed, hiding names,
sizes, and hashes. The v2 preamble is public because it contains only salts, KDF
costs, slot identifiers, and wrapped keys; it does not contain the password or
plaintext archive key. Both legacy v1 and v2 encrypted-directory archives can
be listed, tested, extracted, edited, signed, and verified when the correct
password is supplied.

## Recovery service

An archive created with `--recovery N`, or updated with `axiomc recovery`,
places a systematic Reed-Solomon service after the central directory. For a
legacy layout the protected range is `[0, directory_offset + directory_size)`.
For an appended generation it is
`[0, directory_offset + directory_size + 64)`, including the generation
extension: header, optional encryption preamble, all stored solid blocks, the
complete directory, and the generation history.

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

The legacy footer is 24 bytes at the end of each complete generation (and at the
end of an intact archive). It remains the final fixed record even when a
generation extension or recovery service precedes it.

| Field | Type | Notes |
|---|---|---|
| `directory_offset` | `u64` | Absolute offset of the directory |
| `directory_size` | `u64` | Byte length of the directory |
| `magic` | `u8[8]` | `"AXIOMAR\0"` |

A reader opens the file, reads the trailing 24 bytes, validates the magic, then
seeks to `directory_offset` — no scanning of the blocks.

Generation-aware readers additionally scan backward for the newest valid footer
if the physical tail is longer than a complete generation because an append was
interrupted. The older footer remains usable as the crash fallback.

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
- A subframe map is limited to `2^20` frames per solid block; every mapped
  range is checked for monotonic, bounded geometry before it can drive a read.
- A sparse map is limited to `2^20` extents per entry; capture-report records are
  limited to `2^16` warnings, with each path and message limited to `2^20` bytes.
- Encryption-v2 accepts at most `16` password slots. Passwords are limited to
  `2^20` bytes; each slot's Argon2 memory cost is `8 * lanes` through `2^21`
  1 KiB blocks and its pass count is `1..64`.

### Size ceilings

- Native Axiom match distances are 32-bit, so the full-window path supports a
  maximum encoded distance of **4 GiB−1**; ordinary native blocks are bounded
  near **4 GiB** (a file larger than a block is split across blocks anyway).
  LZMA2 dictionary properties and AXEC chunk sizes use the same **4 GiB−1**
  ceiling. The external LZMA2 large-block profile raises the AXAR grouping
  limit to **64 GiB** while keeping its default independently decoded codec
  chunk at **512 MiB**. Total archive size across blocks is `u64`.
- The level-1 `fast_lz` block codec: match **distance ≤ 16 MiB** (24-bit),
  match **length ≤ 273**.
- Stored **path length ≤ 65,535 bytes**.
- File and block counts are `u64` but bounded by memory.

## Compatibility

The format is **pre-release and free to change**.

| Layer | Written | Accepted |
|---|---|---|
| AXAR container | `4` or `5` | `4` and `5` |
| AXC, native Axiom method | `9` | `4`–`10` |
| AXC, external codec methods | `10` | `4`–`10` |
| Append-generation extension | `1` | `1` |
| Recovery service | `1` | `1` |
| Volume header | `1` | `1` |

Older readers reject AXC `9` and `10`, so archives written with an external
method require Axiom 0.7.0.0 or newer. Existing AXAR v4 archives remain
readable. An archive with encryption-v2, sparse maps, a capture report, or a
snapshot chunk table is AXAR v5 and requires
a reader that understands the corresponding required flags; a v4 reader must
reject it rather than silently restoring a dense file or hiding a capture loss.
An archive with extended ACL, xattr, or reparse metadata also uses AXAR v5 and
requires the `0x0008` required flag.
Snapshot repositories additionally require the `0x0020` flag. Readers that do
not implement the snapshot profile must reject the archive rather than treating
chunk references as ordinary contiguous block addresses.
Large-solid-block archives additionally require the `0x0040` profile. Readers
that do not implement bounded external LZMA2 subframes must reject them rather
than applying the ordinary block-size ceiling. AXC v10 readers that predate the
4 GiB LZMA2 geometry may likewise reject an archive whose declared LZMA2 chunk
exceeds their older 512 MiB safety bound; existing v4/v5 archives are not
changed.
An appended generation does not change the AXAR header version. Readers that do
not understand its 64-byte extension can still read the current directory from
the final legacy footer of an intact archive, but generation history and the
generation-aware crash fallback are unavailable to them.
Unknown AXAR versions, AXC versions, required flags, codecs, transforms, and
transform parameters are all rejected with a clear error.

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
- **Seekable selected extraction** — new plaintext AXAR blocks may carry a
  skippable subframe map. Selected restores fetch and decode only intersecting
  independent frames; legacy, encrypted, transformed, and serial blocks use
  the ordinary whole-block path. The operation telemetry exposes physical
  archive bytes read, including directory reads.
- **Sparse-file fidelity** — logical bytes remain fully checksummed while
  supported files preserve their allocated ranges on extraction. The archive
  exposes an explicit capture report for skipped/unavailable source metadata;
  strict metadata mode is available to callers that require lossless capture.
- **Add and update** into an existing archive: existing files are not
  recompressed — their solid blocks are copied verbatim, exact content matches
  reuse existing ranges, new files are appended as new blocks when needed, and
  the directory is rebuilt. When the header feature set remains compatible, the
  completed directory is appended as a new generation while the previous footer
  remains available for crash fallback. An added path replaces the existing
  entry of the same name, and replaced bytes become dead space until a repack.
- **File-manager operations:** map a filesystem file or directory to an
  explicit archive destination, extract selected entries (a selected directory
  includes its subtree), and rename or move files and whole subtrees without
  recompressing block data. Hard-link targets are rewritten when their
  canonical path moves; a selectively extracted hardlink is materialized as a
  regular file when its canonical entry was not selected.
- **Update / fresh / sync** — refresh by modification time.
- **Delete / repack** — rebuild keeping only surviving entries, re-solidifying
  their files into fresh blocks so removed and replaced data is physically
  reclaimed while duplicate content shares one range. A directory path removes
  its whole subtree; a hard link whose target is removed is dropped.
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
- **SFX packaging** — a 64-byte descriptor and an intact `.axar` or `.zip` are
  written at the end of Axiom's read-only SFX PE image. The descriptor begins
  with `"AXSFX2\0\0"` and carries a format version, payload offset and length,
  the first eight bytes of the payload's BLAKE3-256, and a CRC-32 over itself.
  It sits at the end of the *image*, computed from the PE section table, rather
  than at the end of the *file*, so an Authenticode certificate can be appended
  afterwards without displacing it — and, because Authenticode covers trailing
  data, signing a packaged SFX authenticates the payload as well as the stub.
  Readers fall back to the pre-0.8.0.0 layout, an `"AXIOMSFX"` magic and `u64`
  length in the last 16 bytes, so existing executables keep working. The image
  ships as the non-executable `AxiomSfx.bin` and is read only during SFX
  creation. This wrapper does not change either archive format.
- **Recovery records** and **split/recovery volumes**, as specified above.
- **Sequential AXAR v5 output** to a non-seekable stream. The writer reserves
  the known required-feature flags up front and emits a counted directory/footer
  without seeking; recovery records and streaming-time signatures are not part
  of this profile.

### Metadata

mtime in seconds, plus on Windows the file attributes, full-precision
creation/access/write times, and **NTFS alternate data streams** (named streams
≤ 1 MiB each; larger ones are skipped). POSIX mode, uid, and gid are stored and
restored best-effort on POSIX hosts; ownership requires privilege. Sparse
allocation maps and incomplete-capture warnings are v5 metadata, and can be
queried through the library's archive capture-report API.

Windows self-relative security descriptors, POSIX extended attributes, and
non-symlink reparse buffers are bounded v5 metadata. Reparse buffers are
materialized only after all ordinary descendants have been written, and an
unsupported or failed restore is reported (or fails in strict metadata mode).

### Not supported

- **No special files** — devices, FIFOs, and sockets are skipped. Only regular
  files, directories, symlinks, and hard links are stored.
- **No arbitrary in-place mutation.** Append-compatible updates retain prior
  generations and append a new directory, but compaction, deletion, header
  changes, and other rebuilds still use a temporary plus atomic rename. Recovery
  and signing are file-output services, not available on the non-seekable stream
  profile.
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
