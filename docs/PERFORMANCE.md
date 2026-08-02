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
