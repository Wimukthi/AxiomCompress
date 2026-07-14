# Benchmarking Axiom

Use this guide when changing compression speed, decompression speed, memory use,
or compression-ratio presets. The benchmark scripts do not change the archive
format; they run codecs, verify round-trips, and write CSV results.

## Build first

Benchmark only Release builds. Debug builds distort both throughput and memory
behavior.

```powershell
.\tools\build_msvc.ps1 -Configuration Release -AutoIncrementVersion:$false
.\tools\test_msvc.ps1 -Configuration Release -AutoIncrementVersion:$false
```

The CLI executable should be:

```text
out\Release\axiomc.exe
```

## Corpus selection

Use more than one corpus. A single file can make a change look better than it
really is.

Recommended local layout:

```text
D:\tests\axiom-perf\
  corpora\
    text-or-source-file
    mixed-folder-or-file
    already-compressed-file
    long-distance-repetition-file
  results\
```

Include these categories:

- Text or source code, where dictionary and parser quality matter.
- Mixed real-world files, where metadata and file boundaries matter.
- Already-compressed or random data, where the compressor should fail cheaply.
- Long-distance repetition, where large windows and solid blocks are useful.

For cross-codec comparisons, the standing corpus is the **Silesia corpus**
(https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia, ~212 MB of mixed
text/binary data): pack the twelve files into one uncompressed tar and feed
that same tar to every codec (`axiomc c`, `zstd`, `7z`, ...) so container
overhead and file grouping do not skew the comparison. Verify every row by
round-trip. The README's performance section is measured this way.

## Cross-codec suite

Use the codec-neutral harness for the published comparison. It runs Axiom levels
1–9 and any available LZ4, zstd, Deflate, bzip2, and LZMA2 profiles against the
same byte stream:

```powershell
python .\bench\bench_codecs.py `
  --axiom .\out\Release\axiomc.exe `
  --input D:\tests\axiom-perf\silesia.tar `
  --output D:\tests\axiom-perf\results\silesia-codecs.csv
```

The harness auto-detects tools on `PATH` and common Windows install locations.
Use `--lz4`, `--zstd`, or `--sevenzip` for explicit executable paths. Missing
reference tools are reported and skipped; Axiom is always required. The default
protocol is best-of-two compression and best-of-three decompression, with every
restore compared byte-for-byte. `--quick` selects a short smoke-test profile.

If you only need a smoke test, let the script create deterministic sample files:

```powershell
.\tools\bench_axiom_levels.ps1 `
  -CurrentAxiomc .\out\Release\axiomc.exe `
  -CorpusDir D:\tests\axiom-perf\sample-corpora `
  -OutputDir D:\tests\axiom-perf\sample-results `
  -GenerateSampleCorpora `
  -SampleSizeMiB 8 `
  -Repeats 1
```

## Compare two builds across levels

Keep a known-good `axiomc.exe` somewhere outside the build output, then compare
it with the current Release build:

```powershell
.\tools\bench_axiom_levels.ps1 `
  -BaselineAxiomc D:\baselines\axiomc.exe `
  -CurrentAxiomc .\out\Release\axiomc.exe `
  -CorpusDir D:\tests\axiom-perf\corpora `
  -OutputDir D:\tests\axiom-perf\results\level-sweep `
  -Levels 1,2,3,4,5,6,7,8,9 `
  -Repeats 3
```

Use at least three repeats for real comparisons. The summary uses medians, which
reduces noise from background CPU and disk activity.

## Compare custom tuning profiles

Use `-Profiles` when testing non-default arguments or candidate presets. Each
profile is written as `name=arguments`.

```powershell
.\tools\bench_axiom_levels.ps1 `
  -BaselineAxiomc D:\baselines\axiomc.exe `
  -CurrentAxiomc .\out\Release\axiomc.exe `
  -CorpusDir D:\tests\axiom-perf\corpora `
  -OutputDir D:\tests\axiom-perf\results\profile-sweep `
  -Profiles @(
    "level8=--level 8",
    "level9=--level 9",
    "level9_64m=--level 9 --block-size 64M --window 64M",
    "level9_deeper=--level 9 --block-size 64M --window 64M --chain-depth 768"
  ) `
  -Repeats 3
```

If `-BaselineAxiomc` is omitted, the script runs the current build only. That is
useful for a quick profile sweep before doing a full baseline comparison.

```powershell
.\tools\bench_axiom_levels.ps1 `
  -CurrentAxiomc .\out\Release\axiomc.exe `
  -CorpusDir D:\tests\axiom-perf\corpora `
  -OutputDir D:\tests\axiom-perf\results\current-only `
  -Profiles @(
    "level9=--level 9",
    "level9_96m=--level 9 --block-size 96M --window 96M"
  ) `
  -Repeats 3
```

## Output files

The level comparator writes three CSV files:

| File | Purpose |
|---|---|
| `axiom-levels-raw.csv` | One row per run, corpus, profile, tool, and repeat |
| `axiom-levels-summary.csv` | Median archive size, ratio, compress speed, and decompress speed |
| `axiom-levels-delta.csv` | Current-vs-baseline deltas when a baseline is provided |

Delta interpretation:

- Positive `RatioDeltaPct` means the current build produced a smaller archive.
- Positive `CompressDeltaPct` means the current build compressed faster.
- Positive `DecompressDeltaPct` means the current build decompressed faster.
- Negative values are regressions for that metric.

The script verifies decompressed output with SHA-256 before recording a result.

## Check CPU scaling explicitly

Throughput changes must be tested with the default automatic thread count and
with at least one fixed high thread count. This catches two common regressions:
too few blocks to feed the CPU, and serial work such as whole-buffer CRC or input
I/O dominating the threaded codec.

Recommended checks:

```powershell
.\tools\bench_axiom_levels.ps1 `
  -BaselineAxiomc D:\baselines\axiomc.exe `
  -CurrentAxiomc .\out\Release\axiomc.exe `
  -CorpusDir D:\tests\axiom-perf\corpora `
  -OutputDir D:\tests\axiom-perf\results\cpu-scaling `
  -Profiles @(
    "l1_auto=--level 1 --threads 0",
    "l1_fixed=--level 1 --threads 32",
    "l8_auto=--level 8 --threads 0",
    "l8_fixed=--level 8 --threads 32"
  ) `
  -Repeats 3
```

Do not pass `--block-size` for the default scaling check. An explicit block size
turns off automatic block sizing and can hide whether the normal CLI/GUI path is
feeding enough work to all available cores.

The 0.1.1.0 release candidate used the D:\tests tuning corpus and showed these
median improvements with archive size unchanged:

| Corpus / profile | Compress before | Compress after | Decode-to-NUL after | Compression CPU cores after |
|---|---:|---:|---:|---:|
| mixed-64m, level 1 auto | 211.9 MiB/s | 907.2 MiB/s | 2204.0 MiB/s | 1.1 |
| mixed-64m, level 8 auto | 126.2 MiB/s | 255.0 MiB/s | 2059.3 MiB/s | 21.7 |
| long-distance-112m, level 1 auto | 127.4 MiB/s | 217.3 MiB/s | 1048.8 MiB/s | 1.6 |
| long-distance-112m, level 8 auto | 84.3 MiB/s | 127.9 MiB/s | 894.4 MiB/s | 1.8 |
| mixed-512m, level 1 auto | 229.8 MiB/s | 1098.1 MiB/s | 3853.7 MiB/s | 3.2 |
| mixed-512m, level 8 auto | 127.3 MiB/s | 246.3 MiB/s | 2729.8 MiB/s | 21.6 |

Level 1 still reports fewer compression CPU cores on easy corpora because it can
become limited by memory bandwidth and archive I/O after the block-splitting
fix. Treat low CPU utilization as a regression only when throughput also fails
to scale on larger or harder corpora.

## Current preset notes

Level 9 currently uses a 64 MiB block/window maximum. Larger 96 MiB and 128 MiB
tests can help pathological long-distance corpora, but they cost more memory and
time and were not kept as the default maximum preset. Keep that tradeoff visible
when testing new candidates:

```powershell
"level9_64m=--level 9 --block-size 64M --window 64M"
"level9_96m=--level 9 --block-size 96M --window 96M"
"level9_128m=--level 9 --block-size 128M --window 128M"
```

Only promote a profile to a default preset when it improves the overall corpus
set, not just one synthetic case.

When comparing explicit block-size profiles, keep a matching no-`--block-size`
profile in the same run. The no-override row is the user-facing default and is
the only one that exercises automatic CPU-aware block sizing.

## GUI benchmark

The GUI benchmark is available from:

```text
Tools > Benchmark...
```

Use it for quick local feedback and user-facing throughput checks. Use
`tools\bench_axiom_levels.ps1` for repeatable engineering comparisons, because
it stores raw data and supports baseline comparisons.
