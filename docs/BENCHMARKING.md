# How to benchmark Axiom

This is the guide for measuring Axiom when you change compression speed,
decompression speed, memory use, or a preset. The benchmark tooling never
touches the archive format — it runs codecs, verifies round-trips, and writes
CSV.

Published results live in [PERFORMANCE.md](PERFORMANCE.md).

## Four rules

1. **Release builds only.** Debug distorts both throughput and memory badly
   enough to make a comparison meaningless.
2. **Verify every row.** Every harness here round-trips and compares before it
   records a result. Never publish an unverified number.
3. **More than one corpus.** A single file can easily make a change look far
   better than it is.
4. **Medians over repeats.** Use at least three repeats for any comparison you
   intend to act on.

```powershell
.\tools\build_msvc.ps1 -Configuration Release -AutoIncrementVersion:$false
.\tools\test_msvc.ps1  -Configuration Release -AutoIncrementVersion:$false
```

The CLI lands at `out\Release\axiomc.exe`.

## Corpora

Two standing corpora are used for published comparisons:

| Corpus | Size | What it exercises |
|---|---|---|
| [enwik8](https://mattmahoney.net/dc/textdata.html) | 100 MB | Text ratio; the de-facto LZMA-class benchmark |
| [Silesia](https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia) | ~212 MB | Mixed text, binary, medical, and database data |

Silesia is benchmarked as **one uncompressed tar**, which is what zstd and most
modern codecs report against. Feed that same tar to every codec, so container
overhead and file grouping can't skew the comparison:

```powershell
cd D:\Silesia
tar --format=ustar -b 1 -cf D:\tests\axiom-perf\silesia.tar `
  dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml
```

Alphabetical order with blocking factor 1 produces exactly 211,948,032 bytes,
matching the published input.

**Check the byte count, not a hash.** Tar implementations write different
uid/gid/uname/gname header fields, so two perfectly correct Silesia tars can
differ by a few hundred bytes after compression while both being exactly
211,948,032 bytes long. The byte count is the only portable check.

The `tar` on `PATH` in Windows PowerShell is bsdtar, which is what the command
above assumes. GNU tar additionally needs `--force-local`, or it reads the `D:`
in the output path as a remote host name — and bsdtar rejects that flag
outright.

For local engineering work, keep a directory covering four shapes:

```text
D:\tests\axiom-perf\
  corpora\
    text-or-source-file             dictionary and parser quality
    mixed-folder-or-file            metadata and file boundaries
    already-compressed-file         the compressor should fail cheaply
    long-distance-repetition-file   large windows and solid blocks
  results\
```

## Comparing against other compressors

`bench/bench_codecs.py` is the codec-neutral harness behind the published
tables. It runs Axiom levels 1–9 plus whichever LZ4, zstd, Deflate, bzip2,
LZMA2, and WinRAR RAR5 profiles it can find, against the same byte stream.

```powershell
python .\bench\bench_codecs.py `
  --axiom .\out\Release\axiomc.exe `
  --input D:\tests\axiom-perf\silesia.tar `
  --winrar "C:\Program Files\WinRAR\Rar.exe" `
  --output D:\tests\axiom-perf\results\silesia-codecs.csv
```

- Reference tools are auto-detected on `PATH` and in the usual Windows install
  locations. Override with `--lz4`, `--zstd`, `--sevenzip`, `--winrar`.
- Missing reference tools are reported and skipped. Axiom is always required.
- The default protocol is best-of-two compression and best-of-three
  decompression, with every restore compared byte-for-byte.
- `--quick` selects a short smoke-test profile.
- WinRAR profiles are RAR5 normal (`-m3`) and best with a fixed 128 MiB
  dictionary (`-m5 -md128m`).
- The CSV is rewritten after every verified row, so a reference tool failing
  late doesn't throw away the measurements already completed.

For folders, the harness builds a deterministic byte stream of relative paths
and file bytes and feeds that identical stream to every codec.

### The enwik8 sweep

`tools\bench_enwik8.ps1` downloads enwik8 on first run, sweeps Axiom's match
finders and window sizes from 1 MiB up through the full input, and round-trips
every row before reporting a ratio.

```powershell
.\tools\bench_enwik8.ps1
.\tools\bench_enwik8.ps1 -Quick
.\tools\bench_enwik8.ps1 -Axiomc out\Release\axiomc.exe
```

Leave `-Scratch` at its default unless you know the replacement is equally
fast. This sweep writes the full decoded corpus once per row and times it, so
pointing it at a hard disk halves the reported decompression throughput while
leaving every ratio untouched. To skip the download, drop a known-good `enwik8`
into `%LOCALAPPDATA%\axiom-bench\corpus\` rather than relocating the scratch
directory.

## Comparing two builds

This is the measurement that actually tells you whether your change helped.
Keep a known-good `axiomc.exe` outside the build output, then:

```powershell
.\tools\bench_axiom_levels.ps1 `
  -BaselineAxiomc D:\baselines\axiomc.exe `
  -CurrentAxiomc .\out\Release\axiomc.exe `
  -CorpusDir D:\tests\axiom-perf\corpora `
  -OutputDir D:\tests\axiom-perf\results\level-sweep `
  -Levels 1,2,3,4,5,6,7,8,9 `
  -Repeats 3
```

Omit `-BaselineAxiomc` to run the current build only, which is useful for a
quick sweep before committing to a full baseline comparison. Add
`-GenerateSampleCorpora -SampleSizeMiB 8` to have the script create
deterministic sample files for a smoke test.

### Custom profiles

Use `-Profiles` to test non-default arguments or candidate presets, written as
`name=arguments`:

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

### What it writes

| File | Contents |
|---|---|
| `axiom-levels-raw.csv` | One row per run, corpus, profile, tool, and repeat |
| `axiom-levels-summary.csv` | Median archive size, ratio, compress speed, decompress speed |
| `axiom-levels-delta.csv` | Current-versus-baseline deltas, when a baseline was given |

**Positive deltas always mean the current build is better.** A positive
`RatioDeltaPct` means a smaller archive; positive `CompressDeltaPct` and
`DecompressDeltaPct` mean faster. Negative values are regressions.

Decompressed output is verified with SHA-256 before any result is recorded.

## Profiling a single stream

`--profile` reports coarse per-phase timings for one Axiom stream:

```powershell
.\out\Release\axiomc.exe c --level 5 --profile `
  D:\tests\axiom-perf\enwik8 `
  D:\tests\axiom-perf\results\profile-enwik8-l5.axc
```

The report covers block scheduling, greedy LZ77, optimal parsing, candidate
encoding, and entropy encoding. When the tiled optimal parser is active, it also
breaks optimal parsing into `lz77-optimal-dp`, `lz77-optimal-candidates`, and
`lz77-optimal-reconstruction`. The candidate row is the aggregate CPU time of
producer tiles and can overlap the DP row; it is omitted when the parser uses
the serial tree path.

This is diagnostic only. The callback is opt-in, it does not change the AXC wire
format, and it is currently accepted only by the single-stream `c` command with
`--method axiom`. The public C++ equivalent is
`CompressionOptions::compression_telemetry`.

**Read the timings carefully.** `block-total`, `lz77-greedy`, `lz77-optimal`,
`lz77-optimal-candidates`, and `candidate-encoding` are sums of worker events,
so they represent aggregate CPU work and can overlap each other.
`parallel-blocks` and `lz77-optimal-dp` are enclosing wall-time-like phases for
their respective scopes.

Use the profile to *choose* a hot path, then use the two-build harness above to
decide whether your change is a real throughput win. Keep archive bytes, ratio,
and round-trip hashes as hard gates: a faster parse that changes the selected
token stream or loses ratio is not an acceptable preset change.

## CPU scaling

Test throughput changes with the default automatic thread count **and** at least
one fixed high thread count. That combination catches the two regressions that
keep recurring: too few blocks to feed the CPU, and serial work — whole-buffer
CRC, input I/O — dominating the threaded codec.

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

**Do not pass `--block-size` for the default scaling check.** An explicit block
size disables automatic block sizing, which hides whether the normal CLI and GUI
path is feeding enough work to all available cores — the exact thing you were
trying to measure.

Low CPU utilization is not automatically a regression. Level 1 can become
limited by memory bandwidth and archive I/O on easy corpora, and ordered match
discovery at levels 8–9 means one busy thread per physical core is sometimes the
fastest schedule available. Treat low utilization as a regression only when
throughput *also* fails to scale on larger or harder corpora.

Automatic block geometry follows physical cores even when `--threads` asks for
more logical SMT workers. That keeps independent blocks large enough to retain
match history, while the requested thread budget stays available for nested
codec work and independent archive operations. Validate oversubscribed runs
against the automatic or physical-core profile for both archive bytes and
throughput.

## Changing a preset

Level 9 currently uses a 64 MiB block and window maximum. Larger 96 MiB and
128 MiB configurations help pathological long-distance corpora but cost more
memory and time, which is why they are not the default:

```powershell
"level9_64m=--level 9 --block-size 64M --window 64M"
"level9_96m=--level 9 --block-size 96M --window 96M"
"level9_128m=--level 9 --block-size 128M --window 128M"
```

Two rules for preset work:

- Only promote a profile when it improves the **overall corpus set**, not one
  synthetic case.
- When comparing explicit block-size profiles, keep a matching no-`--block-size`
  profile in the same run. The no-override row is the user-facing default, and
  the only one that exercises automatic CPU-aware block sizing.

## Where the compressed bytes went

To see how an archive's compressed bytes are distributed across streams:

```powershell
py bench\axc_inspect.py path\to\archive.axc
```

It reports per-stream raw and coded sizes, bits per raw byte, share of the
payload, and which coder was selected. `bench/gap_analysis.py` drives the full
per-member sweep used for ratio research. The measured analysis of the LZMA2
ratio gap is in [GAP_ANALYSIS_LZMA2.md](GAP_ANALYSIS_LZMA2.md).

## The in-app benchmark

**Tools > Benchmark…** measures Axiom's own method at a selected level,
entirely in memory. It is the right tool for a quick throughput check on a
specific machine.

It is not a substitute for the harnesses above, which store raw data and support
baseline builds. See
[GUI_GUIDE.md](GUI_GUIDE.md#measuring-speed-on-your-machine).

## Publishing a new snapshot

1. Run the cross-codec harness on both standing corpora.
2. Commit the verified CSVs under `bench/results/`, with the version in the
   filename.
3. Regenerate the charts: `python tools\generate_readme_charts.py`.
4. Update the tables in [PERFORMANCE.md](PERFORMANCE.md) and the headline table
   in the README from those same CSVs.

Keep all four in sync. The chart generator reads the CSVs directly and rejects
missing, unknown, or unverified rows, so the charts and the raw data cannot
drift apart — but the prose tables can, and only discipline prevents it.
