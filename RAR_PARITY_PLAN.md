# RAR Feature-Parity Plan

A phased plan to take the `.axar` archive to **feature parity with RAR**. This is
about *archive features* (metadata, integrity, encryption, recovery, volumes,
in-place editing, SFX), **not** matching RAR's compression ratio — Axiom keeps its
own codec. Companion to [FORMAT.md](FORMAT.md) and [ROADMAP.md](ROADMAP.md).

## Decisions (locked)

- **Scope:** full parity, including the niche items (SFX, NTFS alternate data
  streams, authenticity signatures).
- **Dependencies:** use vetted libraries — **libsodium** (AES-256 AEAD, Argon2id,
  Ed25519) for crypto, **ISA-L** (or equivalent) for Reed-Solomon recovery. No
  homemade cryptography or erasure coding.
- **Platform priority:** **Windows-first** metadata fidelity (matches the GUI);
  Unix `mode`/owner stored as pass-through and given full POSIX restore later.
- **Compatibility:** still pre-release, so the container is redesigned **once**
  now (no incremental version churn); later features add header/record types, not
  format breaks.

## Gap analysis vs RAR

| RAR feature | `.axar` today | Phase |
|---|---|---|
| Solid archives, random-access list/extract | ✅ | — |
| Per-file integrity | CRC-32 only → add **BLAKE3** | 1 |
| Windows attrs, high-precision mtime/ctime/atime | ❌ | 1 |
| NTFS alternate data streams | ❌ | 1 |
| Symlinks / junctions / hardlinks / special files | ❌ (skipped) | 1 |
| Symlink-safe extraction | lexical only | 1 |
| Add / update / delete / sync in place | ❌ | 2 |
| Archive comment, lock flag, quick-open | ❌ | 2 |
| Encryption (data + filenames) | ❌ | 3 |
| Recovery record / recovery volumes | ❌ | 4 |
| Multi-volume (split) archives | ❌ | 4 |
| Authenticity / digital signature | ❌ | 5 |
| Self-extracting (SFX) | ❌ | 5 |
| Full POSIX mode/ownership restore | pass-through | 5 |
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

- **BLAKE3** per-file hash (keep CRC-32 for the fast path / per-block check).
- **Windows metadata:** file attributes (read-only/hidden/system/archive),
  high-precision **mtime/ctime/atime** (100 ns, UTC), and **NTFS alternate data
  streams** captured and restored.
- **Links & special files:** symlinks and junctions (store reparse target),
  **hardlinks** (dedupe by file id → link record), device/fifo records.
- **Symlink-safe extraction:** `O_NOFOLLOW`/`openat`-style containment on POSIX and
  the Win32 equivalent — closes the last decoder-safety gap.
- Unix `mode`/uid/gid captured and stored as pass-through (full restore in Phase 5).
- *Verify:* a metadata edge-case corpus (attrs, ADS, symlink loops, hardlink
  fan-out, sub-second times, long/unicode paths) round-trips in CI.

## Phase 2 — In-place archive editing

- `add` / `update` (replace if newer) / `delete` / `sync` (mirror a directory).
- Strategy: append new solid blocks + rewrite the directory; `delete` tombstones
  entries, with `--repack` to recompact solid runs (RAR rebuilds affected runs —
  same model, done lazily).
- **Archive comment** and **lock/read-only** flag (service headers / `MAIN` flags).
- **Quick-open**: redundant file-header copies near the end for instant listing of
  huge archives without scanning.

## Phase 3 — Encryption (libsodium)

- **Data:** AES-256 AEAD (GCM, or CTR + HMAC-SHA-256) per block.
- **Header/filename encryption:** optionally encrypt the central directory behind
  an `ENCRYPTION` header so names are not visible.
- **Key derivation:** Argon2id with a per-archive random salt; constant-time
  password checks; clear, versioned KDF parameters in the header.
- Wire through the existing `OperationControl` and atomic-write paths.
- *Verify:* known-answer tests against libsodium primitives; wrong-password and
  tamper (AEAD failure) tests; ensure no plaintext leaks into temp files.

## Phase 4 — Recovery records & multi-volume (ISA-L)

- **Recovery record:** Reed-Solomon redundancy sized as a percentage of archive
  data (RAR's `rr`), stored in a `SERVICE` header; repair during `test`/`extract`
  with a damaged-archive test corpus.
- **Multi-volume:** split into volumes of a target size using the `END`
  "volume continues" flag and a volume-naming scheme; optional `.rev` recovery
  volumes for cross-volume reconstruction.
- *Verify:* inject byte/sector damage and whole-volume loss, then repair.

## Phase 5 — Authenticity, SFX, full POSIX

- **Authenticity:** Ed25519 archive signing/verification (libsodium), key
  management out of band.
- **SFX:** a Win32 self-extractor stub prepended to the archive that extracts
  itself; the reader already tolerates a prefix via the trailing footer/directory.
- **Full POSIX metadata restore** (mode/ownership/symlink perms) to round out the
  Windows-first work from Phase 1.

## Cross-cutting (every phase)

- **Spec:** keep [FORMAT.md](FORMAT.md) authoritative for each header/record type.
- **Surfaces:** CLI flags and GUI controls for each feature.
- **Tests:** round-trip + targeted unit tests + libFuzzer coverage on every new
  untrusted-input surface (headers, encrypted streams, recovery data).
- **Build:** integrate libsodium and ISA-L via the build system (CMake
  FetchContent or vcpkg) and add them to CI on Windows and Linux.

## Sequencing & risk

`0 → 1 → 2` are additive, lower-risk, and deliver most of the day-to-day
"feels like RAR" value. `3` (encryption) and `4` (recovery/volumes) are the heavy,
dependency-bearing, correctness-critical items and land after the format is stable
so they bolt on cleanly. `5` rounds out full parity. Each phase is independently
shippable and independently testable.
