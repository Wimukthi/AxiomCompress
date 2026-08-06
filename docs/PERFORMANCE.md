# Published benchmark results

Measured results for Axiom against reference compressors, plus the engineering
log of optimization checkpoints.

To reproduce these on your own hardware, or to measure a change you are making,
see [BENCHMARKING.md](BENCHMARKING.md).

## How to read these numbers

**Ratio** is original size divided by compressed size. 4.00x means the archive
is a quarter of the original. Bigger is better.

**Ratios travel between machines. Timings do not.** Throughput depends on CPU,
memory bandwidth, storage, corpus shape, and build settings. If you need numbers
for your own hardware, run the harnesses in [BENCHMARKING.md](BENCHMARKING.md)
rather than scaling these.

**Timings are comparable within a snapshot, not across snapshots.** Between the
0.4.0.0 and 0.8.0.0 runs, reference binaries that had not changed at all moved
by 3–18% on identical input. That is machine state, not code. Attribute a speed
change to Axiom only from a controlled A/B run of two builds — which is exactly
what `tools\bench_axiom_levels.ps1` exists for.

> **Current snapshot: Axiom 0.8.0.0**, measured 2026-07-31.
>
> On enwik8, every Axiom archive is byte-for-byte the same size as in the
> 0.4.0.0 snapshot, at all nine levels. That is expected: 0.7.0.0 made the extra
> block methods opt-in through `--method`, and 0.7.1.0 retuned the *automatic*
> file-type profiles. Neither path changes what an explicit `--level N` emits.
>
> The exact rows behind every table and chart are versioned in
> [`../bench/results/`](../bench/results/).

## Test environment

| | |
|---|---|
| CPU | AMD Ryzen 9 5950X, 16 cores / 32 threads |
| Storage | NVMe, warm cache |
| Build | MSVC Release x64 |
| Protocol | Best-of-2 compression, best-of-3 decompression, every row round-trip verified |
| Axiom settings | Default `--threads 0` |
| References | zstd 1.5.7 (`-T0`), LZ4 1.10.0, 7-Zip 26.02 (`-mmt=on`) for LZMA2/bzip2/gzip, WinRAR 7.23 RAR5 (`-m3`, `-m5 -md128m`) |

## Silesia

The twelve Silesia files packed into one 211.9 MB uncompressed tar, so every
codec sees byte-identical input. Axiom's rows include its default validated-tar
member transforms — exactly what a normal `axiomc c --level N` run would do.

![Silesia compression ratio by codec](images/silesia-compression-ratio.svg)

![Silesia compression ratio versus compression throughput](images/silesia-speed-ratio.svg)

| Codec / level | Compressed | Ratio | Compress | Decompress |
|---|---:|---:|---:|---:|
| LZ4 -1 | 100.9 MB | 2.10x | 0.09 s | 0.09 s |
| LZ4 -9 (HC) | 78.0 MB | 2.72x | 0.37 s | 0.09 s |
| zstd -1 | 73.3 MB | 2.89x | 0.07 s | 0.15 s |
| zstd -3 | 66.2 MB | 3.20x | 0.12 s | 0.17 s |
| **Axiom -1** | 64.9 MB | 3.27x | 0.74 s | 0.22 s |
| gzip Deflate -9 | 64.7 MB | 3.28x | 87.90 s | 0.80 s |
| zstd -9 | 59.2 MB | 3.58x | 0.51 s | 0.16 s |
| **Axiom -2** | 59.1 MB | 3.59x | 1.57 s | 0.23 s |
| **Axiom -3** | 58.4 MB | 3.63x | 1.74 s | 0.23 s |
| **Axiom -4** | 56.8 MB | 3.73x | 2.31 s | 0.24 s |
| **Axiom -5** (default) | 56.5 MB | 3.75x | 2.91 s | 0.24 s |
| **Axiom -6** | 56.2 MB | 3.77x | 3.97 s | 0.24 s |
| **Axiom -7** | 54.9 MB | 3.86x | 5.05 s | 0.24 s |
| WinRAR -m3 | 54.2 MB | 3.91x | 1.94 s | 0.46 s |
| bzip2 -9 | 54.2 MB | 3.91x | 7.23 s | 2.15 s |
| WinRAR -m5 128M | 53.2 MB | 3.99x | 3.23 s | 0.45 s |
| zstd -19 | 52.8 MB | 4.01x | 16.78 s | 0.17 s |
| **Axiom -8** | 52.4 MB | 4.04x | 15.14 s | 0.25 s |
| zstd -22 --ultra | 52.3 MB | 4.05x | 84.06 s | 0.19 s |
| **Axiom -9** | 51.4 MB | 4.12x | 16.02 s | 0.24 s |
| LZMA2 -mx5 | 49.6 MB | 4.27x | 17.93 s | 0.78 s |
| LZMA2 -mx9 | 48.7 MB | 4.35x | 35.02 s | 1.21 s |

Raw data: [`silesia-0.8.0.0.csv`](../bench/results/silesia-0.8.0.0.csv).

### What the table says

- Axiom's fast presets trade zstd's throughput for a smaller result. Levels 2–7
  cover the range between zstd -3 and WinRAR normal.
- Axiom -9 is 2.8% smaller than zstd -19 at comparable encode time, and 1.7%
  smaller than zstd -22 while encoding 5.2x faster.
- WinRAR best is 3.4% larger and encodes 5.0x faster, but Axiom decodes 1.9x
  faster.
- Axiom -9 is 5.6% larger than LZMA2 -mx9, but encodes 2.2x faster and decodes
  5.1x faster.
- Levels 8 and 9 sit close together here: level 8 saves 5.5% of the encode time
  for a 2.0% larger archive. On enwik8 they converge completely, so level 8
  earns its place mainly through its smaller 32 MiB window rather than through
  speed.

Closing the remaining LZMA2 ratio gap is active work. The measured analysis is
in [GAP_ANALYSIS_LZMA2.md](GAP_ANALYSIS_LZMA2.md).

### A note on the input tar

Sizes here differ from `silesia-0.4.0.0.csv` by up to 3.8 KB — including for
reference codecs that have not changed at all. Both tars are exactly
211,948,032 bytes, but they were written by different `tar` builds, whose
headers differ. See [BENCHMARKING.md](BENCHMARKING.md#corpora).

Compare Axiom against the other codecs *within* a snapshot, not against a
previous one.

## enwik8

The same 22-profile protocol on enwik8 — 100 MB of English Wikipedia text, the
de-facto LZMA-class ratio benchmark — with default `--threads 0`.

![enwik8 compression ratio by codec](images/enwik8-compression-ratio.svg)

![enwik8 compression ratio versus compression throughput](images/enwik8-speed-ratio.svg)

| Axiom level | Compressed | Ratio | Compress | Decompress |
|---:|---:|---:|---:|---:|
| 1 | 37.3 MB | 2.68x | 0.26 s | 0.10 s |
| 2 | 32.8 MB | 3.05x | 0.75 s | 0.09 s |
| 3 | 32.4 MB | 3.09x | 1.08 s | 0.09 s |
| 4 | 32.1 MB | 3.12x | 1.80 s | 0.10 s |
| 5 (default) | 31.8 MB | 3.14x | 2.39 s | 0.10 s |
| 6 | 31.7 MB | 3.15x | 3.38 s | 0.10 s |
| 7 | 30.7 MB | 3.25x | 3.11 s | 0.11 s |
| 8 | 28.7 MB | 3.48x | 7.08 s | 0.11 s |
| 9 | 28.5 MB | 3.51x | 6.89 s | 0.10 s |

On enwik8, Axiom -9 encodes 5.2x faster and decodes 5.9x faster than
LZMA2 -mx9, while LZMA2 remains 12.9% smaller.

Levels 8 and 9 are indistinguishable in cost on this corpus. They landed within
3% of each other in both the 0.4.0.0 and the 0.8.0.0 run, in opposite
directions, so level 9's 0.9% smaller archive is effectively free on text.
Prefer level 8 here only when you need its smaller window for memory reasons.

Raw data: [`enwik8-0.8.0.0.csv`](../bench/results/enwik8-0.8.0.0.csv).

### Full-window diagnostic

A separate full-window sweep reaches 3.57x on enwik8, but only at 2.3 MB/s
against the level-9 preset's 14.3 MB/s. It is kept as a diagnostic rather than
promoted to a preset. The complete level/window sweep is in
[`enwik8-level-window-0.8.0.0.csv`](../bench/results/enwik8-level-window-0.8.0.0.csv).

## Charts

The SVGs above are generated from the versioned CSVs. The generator reads the
verified rows directly and rejects missing, unknown, or unverified data:

```powershell
python tools\generate_readme_charts.py
```

Regenerate them whenever the snapshot CSVs are refreshed, and update the tables
here in the same change, so the numbers and the charts can never disagree.

---

# Optimization log

The entries below are engineering checkpoints, not published comparisons. Each
one records a specific change, what it did to measured phase timings, and — the
part that matters most — proof that the archive bytes did not change.

Every entry ran on the same Release x64 host as the snapshot above. Unless a
row says otherwise, these are **directional, single-host measurements**. Repeat
the profiling workflow before making any cross-machine throughput claim.

## 2026-08-03 — All-level regression checkpoint

The current Release x64 build compared against the last pushed baseline
(`37dd990`) on the same host, with three repeats of every level on both
standing corpora. The full 108-run matrix: two builds, two corpora, nine levels,
compression and decompression, SHA-256 verified on every row.

| Corpus | Levels | Archive-byte delta | Compression speed | Decompression speed |
|---|---:|---:|---:|---:|
| enwik8 | 1–9 | 0 bytes at every level | −4.04% to +9.20% | −3.32% to +5.54% |
| Silesia tar | 1–9 | 0 bytes at levels 1–8; +71 bytes at level 9 | −10.12% to +23.64% | −1.78% to +0.69% |

The level-9 Silesia change is 71 bytes out of 211,948,032 input bytes. It is
reported as a ratio guard, not as a promoted improvement.

Raw, summary, and delta CSVs:
`D:\tests\axiom-perf\results\all-level-regression-20260803`.

**Next target.** The hash-chain greedy matcher used by levels 1–6, specifically
`encode_lz77_impl` and its `find_best` walk in `src/codec/lz77.cpp`. Level 6 is
the clearest signal — it is the only material compression regression in the
matrix while producing identical archive bytes. Profiling the Silesia level-6
path reports 70.629 s of aggregate greedy worker time, against 7.392 s for
candidate encoding and 1.143 s for entropy encoding.

The next pass should preserve candidate traversal order, chain depth,
nice-length cutoff, token decisions, and archive bytes, while removing only
redundant work from that walk. Exact archive-byte comparison and round-trip
hashes remain hard gates before any preset change is even considered.

## 2026-08-03 — Level-9 greedy depth

The level-9 preset's greedy tree search depth is now 384 instead of 512. This
shortens only the greedy cost-model pass; the optimal parser keeps its existing
depth, and the AXC format and decoder are unchanged.

With 32 threads and one 64 MiB block, the final five-corpus run produced:

| Corpus | Depth 512 control | Depth 384 default | Delta |
|---|---:|---:|---:|
| dickens | 2,888,466 | 2,888,466 | 0 |
| nci | 1,533,618 | 1,533,618 | 0 |
| mozilla | 15,405,821 | 15,405,795 | −26 |
| samba | 4,088,518 | 4,088,359 | −159 |
| webster | 8,538,616 | 8,538,595 | −21 |

The greedy phase was faster in every final profile: measured depth-384 greedy
times were 2.397 s, 6.528 s, 12.334 s, 4.597 s, and 11.385 s respectively. All
five compressed streams were decompressed and matched their source SHA-256
exactly.

## 2026-08-02 — Optimal-DP ring mask

The optimal parser's live-cost ring now uses the next power-of-two size above
the maximum transition distance. Frontier clearing keeps the same
`max_transition` safety invariant, while literal and match edges use a mask
instead of a compare-and-wrap branch. The larger ring costs only a few KiB and
does not alter DP reachability or decisions.

Two-run averages on the Dickens profile:

| Phase | Fused-cost baseline | Current | Delta |
|---|---:|---:|---:|
| Optimal LZ77 | 3.636 s | 3.480 s | −4.3% |
| Sum of profiled phases | 7.308 s | 7.143 s | −2.3% |

Both runs remained 2,888,466 bytes with SHA-256
`5BD317E7B2864CB03DE9E4385123180CF1FC6A6D77030257687E28E52205EE6F`.

## 2026-08-02 — Measured distance cost

Measured DP match transitions previously derived the distance slot and the
distance footer width through two helpers, recalculating the same bit width
twice. The parser now derives both values from one distance-width calculation.
The unmeasured cost model and the coded token stream are unchanged.

| Phase | Candidate-list baseline | Current | Delta |
|---|---:|---:|---:|
| Optimal LZ77 | 3.737 s | 3.636 s | −2.7% |
| Sum of profiled phases | 7.512 s | 7.308 s | −2.7% |

Both runs remained 2,888,466 bytes with the same SHA-256 as above.

## 2026-08-02 — Candidate-list initialization

`MatchList` keeps a fixed 64-entry backing array, but only its `count` entries
are ever read. The backing `Match` records therefore no longer carry default
initializers, which avoids a 512-byte clear for every position in the pipelined
tree matcher. Every inserted record is still fully assigned before it can be
observed.

| Phase | Initialized reference | Current | Delta |
|---|---:|---:|---:|
| Greedy LZ77 | 3.723 s | 3.373 s | −9.4% |
| Optimal LZ77 | 4.383 s | 3.737 s | −14.7% |
| Candidate encoding | 0.322 s | 0.316 s | −1.9% |
| Entropy encoding | 0.091 s | 0.086 s | −5.5% |
| Sum of profiled phases | 8.519 s | 7.512 s | −11.8% |

Both runs produced 2,888,466 bytes with the same SHA-256, identical to the
initialized reference.

## 2026-08-02 — Candidate-pipeline tile size

The tree matcher's producer/consumer candidate pipeline now batches 1 MiB of
positions per tile instead of 256 KiB. Tile production still advances the match
tree in the same order, so this is a scheduling change only — it changes no
match decisions, no AXC bytes, and no archive format.

Level 9, 32 threads, one 64 MiB block, on the 10,192,446-byte Silesia `dickens`
file. The reference 256 KiB profile against the 1 MiB two-run average:

| Tile size | Greedy LZ77 | Optimal LZ77 | Candidate encoding | Entropy encoding | Archive bytes |
|---:|---:|---:|---:|---:|---:|
| 256 KiB | 3.940 s | 4.301 s | 0.316 s | 0.090 s | 2,888,466 |
| 1 MiB | 3.723 s | 4.383 s | 0.322 s | 0.091 s | 2,888,466 |

The summed profiled phases improved by about 1.5%. Both 1 MiB runs produced
SHA-256 `5BD317E7B2864CB03DE9E4385123180CF1FC6A6D77030257687E28E52205EE6F`,
matching the 256 KiB output exactly.

## 2026-08-02 — CPU scaling cap

The first scaling sweep found that explicit SMT geometry was over-splitting
solid input: on the 16-core / 32-thread test machine, `--threads 32` created
smaller independent blocks and lost ratio at the stronger levels.

Block and archive geometry now caps its planning count at physical cores. The
executor can still use the requested logical-thread budget for nested work.

The fixed-32 all-level sweep restored the automatic profile's archive bytes at
all 18 corpus/level pairs:

| Corpus | Levels | Archive-byte delta vs automatic | Round trips |
|---|---:|---:|---:|
| enwik8 | 1–9 | 0 bytes at every level | all passed |
| Silesia tar | 1–9 | 0 bytes at every level | all passed |

Raw, summary, and delta CSVs are in
`D:\tests\axiom-perf\results\scaling-cap-all-levels-2026-08-02`. The original
orientation sweep is in
`D:\tests\axiom-perf\results\scaling-next-phase-2026-08-02`.

## 2026-08-02 — Optimal-DP indexing

The level-9 optimal parser's bounded cost frontier now advances ring slots with
explicit wrap checks, instead of recalculating a runtime modulo for every parser
edge. This is an internal scheduling and indexing change: it preserves the DP
decisions, the AXC bytes, the decoder behaviour, and the archive format.

Profiling 100 MiB of enwik8:

| Phase | Previous pass | Current pass | Delta |
|---|---:|---:|---:|
| Block total | 104.373 s | 101.157 s | −3.08% |
| Optimal parsing | 60.790 s | 57.782 s | −4.95% |

The enwik8 AXC remained exactly 28,477,916 bytes with the same SHA-256. A
Silesia-directory archive remained 54,362,481 bytes; all 12 extracted files
matched the previous archive byte-for-byte, and the new archive passed
`axiomc t`.

## 2026-08-02 — Profiling and matcher tuning

The codec now exposes opt-in phase timing through `axiomc c --profile`; the
workflow and the rules for interpreting it are in
[BENCHMARKING.md](BENCHMARKING.md#profiling-a-single-stream).

Profiling the balanced path showed greedy LZ77 as the dominant phase, while the
maximum path split its time between greedy candidate discovery and optimal
parsing. The retained optimization hoists a repeated cyclic-slot division out of
the level-7 tree matcher's lazy-lookahead descent. It changes no token and no
decoder behaviour.

A controlled Release x64 comparison against `f38f3a6` gave identical archive
bytes at every level, on both enwik8 and the Silesia tar:

| Corpus | Levels | Archive-byte delta | Round trips | Level-7 compression delta (3 repeats) |
|---|---:|---:|---:|---:|
| enwik8 | 1–9 | 0 bytes at every level | all passed | +0.56% |
| Silesia tar | 1–9 | 0 bytes at every level | all passed | +1.05% |

The all-level sweep used one repeat per build, so its timings are directional
only. The level-7 figures come from the three-repeat targeted sweep. Raw,
summary, and delta CSVs are in
`D:\tests\axiom-perf\results\final-all-level-compare-2026-08-02` and
`D:\tests\axiom-perf\results\tree-modulo-compare-2026-08-02`.

## 2026-08-02 — Ratio-neutral optimization baseline

A controlled A/B comparison of the then-current Release x64 build against `HEAD`
(`37dd9906`) on the same host, using the two-repetition harness with round-trip
verification for every row.

| Corpus | Levels | Archive ratio delta | Compression speed | Decompression speed |
|---|---:|---:|---:|---:|
| enwik8 | 1–9 | 0.00% at every level | −3.4% to +12.3% | −14.2% to +12.4% |
| Silesia tar | 1–9 | 0.00% at every level | −0.6% to +22.4% | −8.2% to +7.6% |

Run with `tools\bench_axiom_levels.ps1` against the corpus under
`D:\tests\axiom-perf`. Raw, summary, and delta CSVs are in
`D:\tests\axiom-perf\results\speed-baseline-compare-all-2026-08-02`.
