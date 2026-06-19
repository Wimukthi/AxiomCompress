# RAR Feature-Parity Plan

A phased plan to take the `.axar` archive to **feature parity with RAR**. This is
about *archive features* (metadata, integrity, encryption, recovery, volumes,
in-place editing, SFX), **not** matching RAR's compression ratio — Axiom keeps its
own codec. Companion to [FORMAT.md](FORMAT.md) and [ROADMAP.md](ROADMAP.md).

## Decisions (locked)

- **Scope:** full parity, including the niche items (SFX, NTFS alternate data
  streams, authenticity signatures).
- **Dependencies:** **Monocypher** is the vendored crypto backend
  (XChaCha20-Poly1305, Argon2id, and Phase-5 signing primitives). Recovery uses a
  tested portable systematic Reed–Solomon implementation over GF(2^8); its scalar
  backend can later be replaced or accelerated without changing the recovery format.
- **Platform priority:** **Windows-first** metadata fidelity (matches the GUI);
  Unix `mode`/owner stored as pass-through and given full POSIX restore later.
- **Compatibility:** still pre-release, so the container is redesigned **once**
  now (no incremental version churn); later features add header/record types, not
  format breaks.

## Gap analysis vs RAR

| RAR feature | `.axar` today | Phase |
|---|---|---|
| Solid archives, random-access list/selective extract | Complete | — |
| Per-file integrity | CRC-32 + **BLAKE3** complete | 1 |
| Windows attrs, high-precision mtime/ctime/atime | Complete | 1 |
| NTFS alternate data streams | Complete (streams ≤ 1 MiB) | 1 |
| Symlinks / hardlinks | Complete; special files remain | 1 |
| Symlink-safe extraction | Complete, including Windows reparse ancestors | 1 |
| Add / update / delete / sync / mapped move | Complete | 2 |
| Archive comment, lock flag, quick-open | Complete | 2 |
| Encryption (data + filenames) | Complete; encrypted-directory edits remain | 3 |
| Recovery record / recovery volumes | RS codec foundation only | 4 |
| Multi-volume (split) archives | Not started | 4 |
| Authenticity / digital signature | Complete (Monocypher EdDSA) | 5 |
| Self-extracting (SFX) | Complete (native Axiom stub) | 5 |
| Full POSIX mode/ownership restore | Complete, best-effort ownership | 5 |
| 64-bit everything (>4 GiB blocks) | 32-bit block pos | 0 |

## Phase 0 — Extensible container (foundation)

Redesign the container around a **RAR5-style typed-header model** so every later
feature is an additive header/record type, never a format break.

- **Block types:** `MAIN`, `FILE`, `SERVICE`, `ENCRYPTION`, `END` (with a
  "volume continues" flag). Each block: type + flags + size, vint-encoded.
- **TLV "extra area"** per record for optional fields (hash, timestamps, links,
  encryption, owner, streams…). Readers skip unknown records by length.
- **Capability flags** in `MAIN`; a reader rejects only flags it cannot interpret.
- **64-bit** sizes and positions everywhere; lift the ~4 GiB per-block ceiling
  (widen the match-finder position type for huge single blocks, or cap block size
  and rely on multi-block spanning — decide during implementation).
- Port `create`/`list`/`extract`/`test` onto the new container; embedded `.axc`
  blocks are unchanged.
- *Verify:* existing round-trip + safety corpus on the new format; fuzz the new
  header parser (it is the new untrusted surface).

## Phase 1 — Integrity & Windows metadata fidelity

- ✅ **BLAKE3** per-file hash (CRC-32 remains for the fast path / block check).
- ✅ **Windows metadata:** file attributes (read-only/hidden/system/archive),
  high-precision **mtime/ctime/atime** (100 ns, UTC), and **NTFS alternate data
  streams** captured and restored.
- ✅ **Links:** symlinks and hardlinks (dedupe by file identity → link record).
  Junction-specific records and device/FIFO records remain Phase 5 work.
- ✅ **Symlink-safe extraction:** existing ancestor components are rejected when
  they are symlinks or, on Windows, directory reparse points.
- Unix `mode`/uid/gid capture and restore remains Phase 5 work.
- *Verify:* a metadata edge-case corpus (attrs, ADS, symlink loops, hardlink
  fan-out, sub-second times, long/unicode paths) round-trips in CI.

## Phase 2 — In-place archive editing

- ✅ `add` (and same-path *replace*) — existing blocks copied verbatim, new files
  appended as new blocks, directory rebuilt; `axiomc a` on an existing archive.
- ✅ `delete` (dir = subtree) and `repack` — rebuild keeping live entries,
  re-solidifying their files so replaced/removed data is physically reclaimed.
- ✅ `update`/`fresh` (mtime-based) and `sync` (mirror a directory: update + delete
  the missing). CLI `u` / `f` / `s`.
- ✅ File-manager APIs: explicit source-to-archive destination mapping, selective
  file/directory extraction, and metadata-only file/directory moves with hard-link
  target rewriting. These are backend APIs for GUI/OLE drag-and-drop; they do not
  change the on-disk format.
- Strategy: append new solid blocks + rewrite the directory; delete/repack rebuild
  affected runs (RAR's model, done eagerly here). True zero-copy in-place append is
  a later optimization — today every edit writes a fresh file via temp + rename.
- ✅ **Archive comment** and **lock/read-only** flag — archive-level TLV records
  (`comment`, `lock`); CLI `comment` / `lock`. Lock is one-way; edits refuse.
- ✅ **Quick-open** — *N/A by design*: the format already keeps a self-locating
  central directory at the end (footer → directory_offset), so `list`/`test` read it
  directly without scanning blocks. RAR needs quick-open only because it lacks a
  central directory.

## Phase 3 — Encryption (Monocypher)

Backend: **Monocypher** (vendored single-file, audited) instead of libsodium —
chosen for clean two-compiler portability; provides XChaCha20-Poly1305 + Argon2id
(and its Curve25519/BLAKE2b EdDSA primitive for Phase 5). Windows CSPRNG via
`BCryptGenRandom`.

- ✅ **Data:** per-block **XChaCha20-Poly1305** AEAD; the block index is the AD
  (anti-reordering). Stored as `nonce ‖ tag ‖ ciphertext`.
- ✅ **Key derivation:** **Argon2id** with a per-archive random salt; KDF params in
  the `encryption` archive-extra record; a sealed key-check token rejects a wrong
  password up front. Key wiped after use.
- ✅ CLI `-p`/`--password`; wired through list/extract/test and block-only encrypted
  edits. *Verified:* roundtrip, wrong-password, tamper (AEAD failure), and
  no-plaintext-on-disk tests; archive parser fuzzed with the encryption record.
- ✅ **Editing encrypted archives** — add/update/sync seal new blocks under the same
  key; delete/repack decrypt + re-seal; wrong password rejected before writing.
- ✅ **Header/filename encryption** (`--encrypt-names`, RAR `-hp`) — the central
  directory is sealed (names/sizes/hashes hidden); KDF params move to a plaintext
  header preamble; listing requires the password. (Editing a directory-encrypted
  archive is still refused — a small follow-up.)

Phase 3 is functionally complete; the only open item is editing directory-encrypted
archives.

## Phase 4 — Recovery records & multi-volume

- ✅ **Foundation:** portable systematic Reed–Solomon encode/reconstruct over
  GF(2^8), with randomized erasure tests. It is not connected to `.axar` yet.
- **Recovery record:** Reed-Solomon redundancy sized as a percentage of archive
  data (RAR's `rr`), stored in a `SERVICE` header; repair during `test`/`extract`
  with a damaged-archive test corpus.
- **Multi-volume:** split into volumes of a target size using the `END`
  "volume continues" flag and a volume-naming scheme; optional `.rev` recovery
  volumes for cross-volume reconstruction.
- *Verify:* inject byte/sector damage and whole-volume loss, then repair.

## Phase 5 — Authenticity, SFX, full POSIX

- ✅ **Authenticity:** Monocypher Curve25519/BLAKE2b EdDSA archive
  signing/verification with raw key management out of band. Standard Ed25519 wire
  compatibility would require adding Monocypher's optional Ed25519 module.
- ✅ **SFX:** the native GUI executable is the self-extractor stub; an intact archive
  and fixed length trailer are appended, detected on launch, verified, and extracted.
- ✅ **Full POSIX metadata restore:** mode/uid/gid capture, chmod, and best-effort
  lchown (including link ownership). Device/FIFO/socket records remain unsupported.

## Cross-cutting (every phase)

- **Spec:** keep [FORMAT.md](FORMAT.md) authoritative for each header/record type.
- **Surfaces:** CLI flags and GUI controls for each feature.
- **Tests:** round-trip + targeted unit tests + libFuzzer coverage on every new
  untrusted-input surface (headers, encrypted streams, recovery data).
- **Build:** keep the checked-in MSVC projects and CMake source manifests aligned;
  test Monocypher and Reed–Solomon on Windows and Linux CI.

## Sequencing & risk

`0 → 1 → 2` are additive, lower-risk, and deliver most of the day-to-day
"feels like RAR" value. `3` (encryption) and `4` (recovery/volumes) are the heavy,
dependency-bearing, correctness-critical items and land after the format is stable
so they bolt on cleanly. `5` rounds out full parity. Each phase is independently
shippable and independently testable.
