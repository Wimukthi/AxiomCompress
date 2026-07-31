# Published benchmark results

Measured results for Axiom against reference codecs. To reproduce these on your
own hardware, or to measure a change you are making, see
[BENCHMARKING.md](BENCHMARKING.md).

> **Snapshot: Axiom 0.4.0.0.** These runs have not been repeated since. Later
> releases retuned the built-in profiles (0.7.1.0) and added selectable
> Zstandard/LZMA2/Deflate block methods (0.7.0.0), so current builds may differ.
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
member transforms, exactly as a normal `axiomc c --level N` run would.

![Silesia compression ratio by codec](images/silesia-compression-ratio.svg)

![Silesia compression ratio versus compression throughput](images/silesia-speed-ratio.svg)

| Codec / level | Compressed | Ratio | Compress | Decompress |
|---|---:|---:|---:|---:|
| LZ4 -1 | 100.9 MB | 2.10x | 0.10 s | 0.10 s |
| LZ4 -9 (HC) | 78.0 MB | 2.72x | 0.42 s | 0.10 s |
| zstd -1 | 73.3 MB | 2.89x | 0.08 s | 0.15 s |
| zstd -3 | 66.2 MB | 3.20x | 0.13 s | 0.17 s |
| **Axiom -1** | 64.9 MB | 3.27x | 0.81 s | 0.23 s |
| gzip Deflate -9 | 64.7 MB | 3.28x | 91.35 s | 0.81 s |
| zstd -9 | 59.2 MB | 3.58x | 0.57 s | 0.16 s |
| **Axiom -2** | 59.1 MB | 3.59x | 1.80 s | 0.24 s |
| **Axiom -3** | 58.4 MB | 3.63x | 2.06 s | 0.24 s |
| **Axiom -4** | 56.8 MB | 3.73x | 2.73 s | 0.24 s |
| **Axiom -5** (default) | 56.5 MB | 3.75x | 3.17 s | 0.24 s |
| **Axiom -6** | 56.2 MB | 3.77x | 4.24 s | 0.24 s |
| **Axiom -7** | 54.9 MB | 3.86x | 6.31 s | 0.24 s |
| WinRAR -m3 | 54.2 MB | 3.91x | 2.20 s | 0.51 s |
| bzip2 -9 | 54.2 MB | 3.91x | 7.50 s | 2.17 s |
| WinRAR -m5 128M | 53.2 MB | 3.99x | 3.54 s | 0.50 s |
| zstd -19 | 52.8 MB | 4.01x | 20.38 s | 0.18 s |
| **Axiom -8** | 52.4 MB | 4.04x | 14.38 s | 0.26 s |
| zstd -22 --ultra | 52.3 MB | 4.05x | 98.93 s | 0.19 s |
| **Axiom -9** | 51.4 MB | 4.12x | 21.31 s | 0.24 s |
| LZMA2 -mx5 | 49.6 MB | 4.27x | 19.84 s | 0.81 s |
| LZMA2 -mx9 | 48.7 MB | 4.35x | 40.03 s | 1.26 s |

Raw data: [`silesia-0.4.0.0.csv`](../bench/results/silesia-0.4.0.0.csv).

### Reading the table

- Axiom's fast presets trade zstd's throughput for a smaller result; levels 2–7
  cover the range between zstd -3 and WinRAR normal.
- Axiom -9 is 2.8% smaller than zstd -19 at similar compression time, and 1.7%
  smaller than zstd -22 while encoding 4.6x faster.
- WinRAR best is 3.3% larger and encodes 6.0x faster, but Axiom decodes 2.1x
  faster.
- LZMA2 -mx9 is 5.5% smaller than Axiom -9, but Axiom encodes 1.9x faster and
  decodes 5.3x faster.

Closing the remaining LZMA2 ratio gap is active work; the measured analysis is
in [GAP_ANALYSIS_LZMA2.md](GAP_ANALYSIS_LZMA2.md).

## enwik8

The same 22-profile protocol on enwik8 — 100 MB of English Wikipedia text, the
de-facto LZMA-class ratio benchmark — with default `--threads 0`.

![enwik8 compression ratio by codec](images/enwik8-compression-ratio.svg)

![enwik8 compression ratio versus compression throughput](images/enwik8-speed-ratio.svg)

| Axiom level | Compressed | Ratio | Compress | Decompress |
|---:|---:|---:|---:|---:|
| 1 | 37.3 MB | 2.68x | 0.35 s | 0.11 s |
| 2 | 32.8 MB | 3.05x | 0.91 s | 0.10 s |
| 3 | 32.4 MB | 3.09x | 1.18 s | 0.10 s |
| 4 | 32.1 MB | 3.12x | 2.04 s | 0.10 s |
| 5 (default) | 31.8 MB | 3.14x | 2.54 s | 0.10 s |
| 6 | 31.7 MB | 3.15x | 3.42 s | 0.10 s |
| 7 | 30.7 MB | 3.25x | 2.96 s | 0.11 s |
| 8 | 28.7 MB | 3.48x | 6.82 s | 0.10 s |
| 9 | 28.5 MB | 3.51x | 7.05 s | 0.10 s |

On enwik8, Axiom -9 encodes 5.8x faster and decodes 6.5x faster than
LZMA2 -mx9, while LZMA2 remains 12.9% smaller.

Raw data: [`enwik8-0.4.0.0.csv`](../bench/results/enwik8-0.4.0.0.csv).

### Full-window diagnostic

A separate full-window sweep reaches 3.57x on enwik8, but only at 1.9 MB/s
against the level-9 preset's 13.5 MB/s. It is kept as a diagnostic rather than
promoted to a preset. The complete level/window sweep is in
[`enwik8-level-window-0.4.0.0.csv`](../bench/results/enwik8-level-window-0.4.0.0.csv).

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
