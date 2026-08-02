# Published benchmark results

Measured results for Axiom against reference codecs. To reproduce these on your
own hardware, or to measure a change you are making, see
[BENCHMARKING.md](BENCHMARKING.md).

> **Snapshot: Axiom 0.8.0.0**, measured 2026-07-31. On enwik8 every Axiom
> archive is byte-for-byte the same size as in the 0.4.0.0 snapshot, at all
> nine levels — as expected, since 0.7.0.0 made the extra block methods opt-in
> through `--method` and 0.7.1.0 retuned the *automatic* file-type profiles,
> and neither path changes what an explicit `--level N` emits. The exact rows
> behind every table and chart are versioned in
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

Timings are comparable **within** a snapshot, not across snapshots. Between the
0.4.0.0 and 0.8.0.0 runs, unchanged reference binaries moved by 3–18% on
identical input — machine state, not code. Attribute a speed change to Axiom
only from a controlled A/B run of two builds, which is what
`tools\bench_axiom_levels.ps1` and [BENCHMARKING.md](BENCHMARKING.md) are for.

## Silesia

The twelve Silesia files packed into one 211.9 MB uncompressed tar, so every
codec sees byte-identical input. Axiom's rows include its default validated-tar
member transforms, exactly as a normal `axiomc c --level N` run would.

Sizes here differ from `silesia-0.4.0.0.csv` by up to 3.8 KB — including for
reference codecs that have not changed at all. Both tars are exactly
211,948,032 bytes, but they were written by different `tar` builds, whose
headers differ; see [BENCHMARKING.md](BENCHMARKING.md#corpora). Compare Axiom
against the other codecs *within* a snapshot, not against a previous one.

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

### Reading the table

- Axiom's fast presets trade zstd's throughput for a smaller result; levels 2–7
  cover the range between zstd -3 and WinRAR normal.
- Axiom -9 is 2.8% smaller than zstd -19 at comparable encode time, and 1.7%
  smaller than zstd -22 while encoding 5.2x faster.
- WinRAR best is 3.4% larger and encodes 5.0x faster, but Axiom decodes 1.9x
  faster.
- Axiom -9 is 5.6% larger than LZMA2 -mx9, but encodes 2.2x faster and decodes
  5.1x faster.
- Levels 8 and 9 sit close together here: level 8 saves 5.5% of the encode time
  for a 2.0% larger archive. On enwik8 they converge completely — see below —
  so level 8 earns its place mainly through its smaller 32 MiB window, not
  through speed.

Closing the remaining LZMA2 ratio gap is active work; the measured analysis is
in [GAP_ANALYSIS_LZMA2.md](GAP_ANALYSIS_LZMA2.md).

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
Prefer level 8 here only when its smaller window is needed for memory.

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
here in the same change so the numbers and charts never disagree.

## A note on throughput

Throughput depends on CPU, memory bandwidth, storage, corpus shape, and build
settings. Ratios are far more portable between machines than timings. If you
need numbers for your own hardware, run the harnesses described in
[BENCHMARKING.md](BENCHMARKING.md) rather than scaling these.

## 2026-08-02 ratio-neutral optimization checkpoint

This controlled A/B checkpoint compares the current Release x64 build with
`HEAD` (`37dd9906`) on the same host, using the two-repetition harness and
round-trip verification for every row. It is an engineering measurement, not a
replacement for the published cross-codec snapshot above.

| Corpus | Levels | Archive ratio delta | Compression-speed delta | Decompression-speed delta |
|---|---:|---:|---:|---:|
| enwik8 | 1–9 | 0.00% at every level | −3.4% to +12.3% | −14.2% to +12.4% |
| Silesia tar | 1–9 | 0.00% at every level | −0.6% to +22.4% | −8.2% to +7.6% |

The run used `tools\bench_axiom_levels.ps1` with the corpus under
`D:\tests\axiom-perf`. Raw, summary, and delta CSVs are written to
`D:\tests\axiom-perf\results\speed-baseline-compare-all-2026-08-02`.

## 2026-08-02 profiling and matcher checkpoint

The codec now exposes opt-in phase timing through `axiomc c --profile`; the
workflow and interpretation rules are in [BENCHMARKING.md](BENCHMARKING.md#codec-phase-profiling).
Profiling the balanced path showed greedy LZ77 as the dominant phase, while the
maximum path split its time between greedy candidate discovery and optimal
parsing. The retained optimization hoists a repeated cyclic-slot division out
of the level-7 tree matcher's lazy-lookahead descent. It changes no token or
decoder behavior.

A controlled Release x64 comparison against `f38f3a6` (the previous pass) gave
identical archive bytes at every level on both enwik8 and the Silesia tar:

| Corpus | Levels | Archive-byte delta | Round trips | Level-7 compression delta (3 repeats) |
|---|---:|---:|---:|---:|
| enwik8 | 1–9 | 0 bytes at every level | all passed | +0.56% |
| Silesia tar | 1–9 | 0 bytes at every level | all passed | +1.05% |

The all-level sweep used one repeat per build; its timings are directional only.
The level-7 figures use the three-repeat targeted sweep. Raw, summary, and delta
CSVs are in `D:\tests\axiom-perf\results\final-all-level-compare-2026-08-02`
and `D:\tests\axiom-perf\results\tree-modulo-compare-2026-08-02`.

## 2026-08-02 optimal-DP indexing checkpoint

The level-9 optimal parser's bounded cost frontier now advances ring slots with
explicit wrap checks instead of recalculating a runtime modulo for every parser
edge. This is an internal scheduling/indexing change: it preserves the DP
decisions, AXC bytes, decoder behavior, and archive format.

On the same Release x64 host, profiling 100 MiB enwik8 changed the measured
phases as follows:

| Phase | Previous pass | Current pass | Delta |
|---|---:|---:|---:|
| Block total | 104.373 s | 101.157 s | -3.08% |
| Optimal parsing | 60.790 s | 57.782 s | -4.95% |

The enwik8 AXC remained exactly 28,477,916 bytes with the same SHA-256. A
Silesia-directory archive remained 54,362,481 bytes; all 12 extracted files
matched the previous archive byte-for-byte, and the new archive passed
`axiomc t`. The measurements are single-run directional results; repeat the profiling
workflow before publishing cross-machine throughput claims.

## 2026-08-02 CPU-scaling checkpoint

The first scaling sweep found that explicit SMT geometry was over-splitting
solid input: on the 16-core/32-thread test machine, `--threads 32` created
smaller independent blocks and lost ratio at the stronger levels. Block and
archive geometry now caps its planning count at physical cores; the executor
can still use the requested logical-thread budget for nested work.

The fixed-32 all-level sweep restored the automatic profile's archive bytes at
all 18 corpus/level pairs:

| Corpus | Levels | Archive-byte delta versus automatic | Round trips |
|---|---:|---:|---:|
| enwik8 | 1–9 | 0 bytes at every level | all passed |
| Silesia tar | 1–9 | 0 bytes at every level | all passed |

Raw, summary, and delta CSVs are in
`D:\tests\axiom-perf\results\scaling-cap-all-levels-2026-08-02`. The original
orientation sweep is in `D:\tests\axiom-perf\results\scaling-next-phase-2026-08-02`.

## 2026-08-02 candidate-pipeline tile checkpoint

The tree matcher's producer/consumer candidate pipeline now batches 1 MiB of
positions per tile instead of 256 KiB. Tile production still advances the
match tree in the same order, so this is a scheduling change only: it does not
change match decisions, AXC bytes, or the archive format.

On the same Release x64 host, level 9 with 32 threads and one 64 MiB block for
the 10,192,446-byte Silesia `dickens` file, the reference 256 KiB profile and
the 1 MiB two-run average were:

| Tile size | Greedy LZ77 | Optimal LZ77 | Candidate encoding | Entropy encoding | Archive bytes |
|---:|---:|---:|---:|---:|---:|
| 256 KiB | 3.940 s | 4.301 s | 0.316 s | 0.090 s | 2,888,466 |
| 1 MiB | 3.723 s | 4.383 s | 0.322 s | 0.091 s | 2,888,466 |

The summed profiled phases improved by about 1.5% in this directional
measurement. Both 1 MiB runs produced SHA-256
`5BD317E7B2864CB03DE9E4385123180CF1FC6A6D77030257687E28E52205EE6F`, matching
the 256 KiB output exactly. Repeat the profiling workflow before making
cross-machine throughput claims.

## 2026-08-02 candidate-list initialization checkpoint

`MatchList` keeps a fixed 64-entry backing array, but only its `count` entries
are ever read. The backing `Match` records therefore no longer carry default
initializers, avoiding a 512-byte clear for every position in the pipelined
tree matcher. Every inserted record is still fully assigned before it can be
observed.

On the same Release x64 host and Dickens profile as above, the two-run averages
were:

| Phase | Initialized reference | Current | Delta |
|---|---:|---:|---:|
| Greedy LZ77 | 3.723 s | 3.373 s | -9.4% |
| Optimal LZ77 | 4.383 s | 3.737 s | -14.7% |
| Candidate encoding | 0.322 s | 0.316 s | -1.9% |
| Entropy encoding | 0.091 s | 0.086 s | -5.5% |
| Sum of profiled phases | 8.519 s | 7.512 s | -11.8% |

The current two runs both produced 2,888,466 bytes with SHA-256
`5BD317E7B2864CB03DE9E4385123180CF1FC6A6D77030257687E28E52205EE6F`, identical
to the initialized reference. The timings remain host-specific and
directional.

## 2026-08-02 measured distance-cost checkpoint

Measured DP match transitions previously derived the distance slot and the
distance footer width through two helpers, recalculating the same bit width.
The parser now derives both values from one distance-width calculation. The
unmeasured cost model and the coded token stream are unchanged.

On the same Release x64 host and Dickens profile, the two-run averages were:

| Phase | Candidate-list baseline | Current | Delta |
|---|---:|---:|---:|
| Optimal LZ77 | 3.737 s | 3.636 s | -2.7% |
| Sum of profiled phases | 7.512 s | 7.308 s | -2.7% |

Both current runs remained 2,888,466 bytes with SHA-256
`5BD317E7B2864CB03DE9E4385123180CF1FC6A6D77030257687E28E52205EE6F`. This is
a directional single-host measurement; repeat the profile before making
general throughput claims.
