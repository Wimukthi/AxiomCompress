# AxiomCompress — Roadmap & Handoff

A snapshot of where the project stands and what to pick up next. Companion to
[ARCHITECTURE.md](ARCHITECTURE.md) and [FORMAT.md](FORMAT.md).

## Current state

- **Codec**: greedy LZ77 with repeat-offset (rep0–rep3) matches and **lazy
  matching**; rep-aware, two-pass measured-cost optimal parser; split streams;
  LZMA-style position-slot distance coding; entropy via canonical Huffman, a
  **4-lane interleaved order-0 rANS**, and a Fenwick-backed adaptive **order-1**
  coder, chosen either by full bake-off or a **cheap order-0/order-1 estimate**
  (fast mode); optional **binary-tree** match finder (`--bt`) over a **cyclic
  window with node deletion** (memory bounded to `--window`); word-at-a-time match
  comparison; threaded parallel-block codec.
- **Fast path** (`--level 1`): a dedicated throughput codec — a fixed-probe
  **2-way row-hash** matcher feeding LZ77 sequences that are split-stream
  entropy-coded with **rANS literals** (not the slow bit-serial order-1 coder),
  with a standalone LZ4-style nibble format (`fast_lz`) kept as a fallback for
  near-incompressible blocks. Decode uses wide (word-at-a-time) match copy. Built
  for throughput on Axiom's non-LZMA architecture.
- **Effort control**: `--level 1..9` (default 5) picks a coherent speed/ratio
  profile — level 1 is the fast path, levels 2–6 tune the hash chain (depth, lazy,
  entropy effort), 7–9 use the binary tree with growing windows, 9 adds the
  optimal parser. Individual flags override the preset.
- **Front-ends & archiver**: multi-file `.axar` container via CLI and the native
  Windows **`Axiom.exe`** archive manager. The archive library supports create,
  list/test, selective extract, destination-aware add, update/fresh/sync,
  delete/repack, metadata-only move/rename, comments, locking, Windows metadata,
  links, and Monocypher data/name encryption. Operations report progress and honor
  cooperative pause/cancel through `OperationControl`.
- **Safety**: lexical containment plus symlink/reparse-point ancestor rejection,
  atomic writes, CRC-32 + BLAKE3 integrity, authenticated encrypted blocks,
  decompression-bomb guards, and bounded reserves.
- **Quality**: Release-mode test suite (roundtrip + safety + in-process mutation
  fuzz), coverage-guided libFuzzer+ASan targets, GitHub Actions CI (Windows MSVC
  + Linux clang, build/test/fuzz).

## Benchmark snapshot (enwik8, 100 MB, vs 7-Zip LZMA2)

The standing corpus is **enwik8** (first 10⁸ bytes of an English Wikipedia dump).
Reproduce with `tools\bench_enwik8.ps1` (downloads the corpus on first run,
verifies every row by round-trip). 16-core machine, `g++ 15.2 -O2`. **Ratios are
exact; compress throughput is approximate** — the dev machine syncs this path in
the background, so absolute MB/s vary run to run (the relative shape is stable).

Level-1 throughput baseline (MSVC `/O2 /GL /LTCG`, **Intel i7-13620H laptop**,
10C/16T, enwik8 in `%LOCALAPPDATA%\axiom-build`/`axiom-bench`, 2026-06-17).
Decode is measured to NUL (matches 7-Zip `t`). The laptop thermally throttles
under sustained 16-thread bursts, so numbers are cooldown-spaced single runs;
build output is kept off the OneDrive-synced tree (syncing it steals cores).

| config | ratio | compress | decode (to NUL) |
|---|---:|---:|---:|
| Axiom `--level 1` (this work) | 2.680x | 140.0 MB/s | 476.2 MB/s |
| Axiom `--level 1` (session start) | 2.809x | ~117 MB/s | ~352 MB/s |
| 7-Zip `-mx1` | 2.993x | 161.9 MB/s | 859.8 MB/s |

Three level-1 changes, in order of impact, traded ~5% ratio for large throughput:

1. **rANS literals on the fast levels** (was order-1). Order-1 wins a little ratio
   on text but decodes bit-serially (~12 MB/s) and dominated decode time; rANS
   literals decode ~2.3× faster (single-stream 100→230 MB/s). −1.6% ratio.
2. **2-way row hash** (was 4-way): ~+40% matcher throughput for −3.3% ratio.
3. **Dropped the redundant standalone-nibble matcher pass**: `compress_block` no
   longer runs the row-hash matcher twice per block (the nibble format is now a
   fallback only when a block barely compresses). +24% compress, ratio-neutral.

Gaps to mx1 narrowed from ~1.4×/2.4× to **~1.16× compress, ~1.8× decode**, at a
ratio ~10% behind. Level 1 stays on Axiom's non-LZMA fast architecture: fixed-probe
row hash, repeat-offset sequence tokens, independent blocks, split-stream entropy,
4-lane byte rANS. Further gaps vs mx1 are now the matcher + rANS encode (compress)
and the remaining rANS decode + token replay (decode). Note: the rANS *slot-table*
size is **not** the decode bottleneck — a paired 4 KiB-vs-32 KiB-table measurement
showed 0% difference; the cost is the per-symbol dependency chain and the replay.

The per-level table below is the earlier g++ pass (approximate throughput); the
level-1 row now reflects the `fast_lz` path (see the MSVC Release baseline above
for level-1's measured numbers).

| `--level` | matcher | ratio | compress | decompress |
|---|---|---|---|---|
| 1 (`--fast`) | row-hash `fast_lz` | 2.81× | ~120 MB/s | ~340 MB/s |
| 3 | hash, chain 32, lazy | 3.03× | ~50 MB/s | ~240 MB/s |
| 5 (default) | hash, chain 128, lazy | 3.09× | ~17 MB/s | ~240 MB/s |
| 6 | hash, chain 256, lazy | 3.10× | ~8 MB/s | ~240 MB/s |
| 7 | bt, 8M window | 3.17× | ~10 MB/s | ~240 MB/s |
| 8 | bt, 32M window | 3.33× | ~4 MB/s | ~240 MB/s |
| 9 (`--max`) | bt, full window | **3.47×** | ~1.1 MB/s | ~90 MB/s |
| 7-Zip `-mx1` | — | 2.99× | ~63 MB/s | ~127 MB/s |
| 7-Zip `-mx9` | — | 4.03× | ~1.5 MB/s | ~98 MB/s |

Reading the table:

- **One knob spans the curve:** level 1 is ~5–6× faster than the default at −7%
  ratio; the default (level 5) is both faster and slightly higher-ratio than the
  pre-level default (3.085× vs 3.041×) thanks to lazy matching.
- **Levels 1–6 are the hash chain** (depth + lazy + entropy effort); **7–9 are the
  cyclic-window binary tree** with growing windows. Level 8 already beats 7-Zip
  `-mx1`'s ratio; level 9 reaches 3.47×.
- **Best Axiom ratio is ~14% behind `-mx9`** at comparable (slow) compress speed.
  The remaining gap is the entropy stage, not the matcher (see Next step 2).
- **The optimal parser is opt-in, not a level.** `--optimal` adds the two-pass
  measured-cost parser, but its per-byte DP makes it impractical on a 100 MB
  single block; pair it with a smaller `--block-size` (e.g. `--optimal
  --block-size 16M`) for bounded-memory ultra runs.
- Decompress splits by parallelism, not matcher: parallel-block archives (levels
  1–8) decode multi-threaded (~240 MB/s); the single big block at level 9 decodes
  on one thread (~90 MB/s).

Isolated entropy decode throughput (one 32 MiB stream): order-0 arithmetic
20 MB/s, **rANS 172 MB/s**, adaptive order-1 ~12 MB/s (bit-serial).

## Next steps (in priority order)

### 1. Auto-select the level / matcher from cheap statistics
Speed and ratio are now a single `--level 1..9` knob (default 5), built on the
landed match-finder and entropy work: cyclic-window bt4 with node deletion,
lazy matching, a cheap order-0/order-1 entropy chooser, and word-at-a-time match
comparison. The default moved off the deep hash chain to a lazy chain-128 profile
that is faster *and* slightly higher ratio than the old default (3.085× vs 3.041×
on enwik8), and `--bt` is reachable at levels 7–9.

What remains is making effort *adaptive* rather than user-specified: sample a
block's statistics (entropy, match density) to auto-pick the matcher and a level,
so the default does well across content types without the user choosing. Validate
the level table on a mixed-content corpus (Silesia) so it is not over-fit to text;
the current table was tuned on enwik8.

### 2. Context-modeled binary range coder (LZMA-style)
The adaptive order-1 coder is the slowest decode path (~12 MB/s, bit-serial) and
the entropy stage is the remaining ratio gap vs LZMA. A binary range coder with
bit-level context models (literal-under-match-state, position-aligned distance
bits) is the standard answer and would help **both** decode speed and ratio.
Large subsystem; design deliberately.

## Archive feature parity (RAR)

A separate, phased effort is bringing `.axar` to **feature parity with RAR**.
Phases 1–3 now cover strong hashes, Windows metadata/ADS, links, safe extraction,
editing, comments/lock, and Monocypher data/name encryption. A portable tested
Reed–Solomon core is the Phase-4 foundation, but recovery records, volumes,
signatures, POSIX ownership, and SFX still remain. Full status is in
[RAR_PARITY_PLAN.md](RAR_PARITY_PLAN.md). The items below are folded into its
Phase 0–1.

## Deferred / smaller items

- **Archive recovery:** define recovery service records, connect the Reed–Solomon
  core to create/test/repair, then add split and recovery volumes.
- **Encrypted-directory editing:** data-only encrypted archives are editable;
  archives with sealed names remain read-only.
- **Metadata:** POSIX modes/ownership and special files remain; Windows attributes,
  high-precision times, ADS, symlinks, and hardlinks are implemented.
- **CLI polish**: real `-h`/`--help`/`--version` (today help prints only on
  misuse, to stderr, exit 2).
- **Archive optimization:** edits currently use a temporary file + replacement
  rename; true in-place append remains an optimization. A stable `libaxiom` C ABI
  for bindings is also pending.
- **Encoder speed**: the default still re-runs several candidate codecs; a
  cheap-statistics codec chooser would cut compress time further.

## Notes for whoever picks this up

- Build: `cmake --preset default` (CI uses CMake; the checked-in `.sln` pins MSVC
  toolset v145, which hosted runners lack).
- Fuzz locally: `tools\build_fuzz.ps1` then `tools\run_fuzz.ps1` (MSVC ships the
  libFuzzer runtime; no external clang needed).
- Any match-finder change is low-risk to *correctness*: emitted matches are
  byte-validated, so the CRC/roundtrip catches errors as ratio regressions, never
  as corrupt archives.
