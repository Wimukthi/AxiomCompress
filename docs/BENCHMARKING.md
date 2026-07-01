# Benchmarking Axiom

Use this guide when changing compression speed, decompression speed, memory use,
or compression-ratio presets. The benchmark scripts do not change the archive
format; they only run `axiomc.exe`, verify round-trips, and write CSV results.

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

## GUI benchmark

The GUI benchmark is available from:

```text
Tools > Benchmark...
```

Use it for quick local feedback and user-facing throughput checks. Use
`tools\bench_axiom_levels.ps1` for repeatable engineering comparisons, because
it stores raw data and supports baseline comparisons.
