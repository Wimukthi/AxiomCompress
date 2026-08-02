# Architecture

The project is organized around one rule:

> **The encoder may be expensive. The decoder may not.**
>
> Compression can spend arbitrary CPU searching for a better representation.
> Decompression must stay deterministic, bounded, and cheap to validate.

Everything below follows from that. New encoder work is welcome as long as it
lands as *static, encoder-chosen structure* that the decoder can replay with
table lookups.

## Contents

- [Source map](#source-map)
- [Pipeline](#pipeline)
- [Codec](#codec)
- [Container](#container)
- [Threading](#threading)
- [GUI boundary](#gui-boundary)
- [Benchmarking infrastructure](#benchmarking-infrastructure)
- [Decoder rule](#decoder-rule)

## Source map

| Path | Owns |
|---|---|
| `src/codec/` | Single-block compression and decompression |
| `src/codec/external_codecs.cpp` | Chunk envelope and the Zstandard, LZMA2, and Deflate adapters |
| `src/entropy/` | Huffman and rANS coders |
| `src/archive/container.cpp` | The AXAR engine: directory, solid blocks, encryption, recovery, volumes, signing, SFX |
| `src/archive/container_zip.cpp` | ZIP read/write: miniz wrappers, AES-256 entries, the ZIP provider |
| `src/archive/container_formats.cpp` | Format detection and the provider registry |
| `src/archive/system_provider.cpp` | Read-only provider policy, native ISO reader, Windows `tar.exe` |
| `src/archive/seven_zip_library.cpp` | Direct `7z.dll` adapter |
| `src/archive/zip_split_backend.cpp` | minizip-ng split-set creation and reading |
| `src/core/` | Checksums, crypto, filesystem metadata, Reed-Solomon, task executor |
| `src/cli/` | `axiomc` parsing and workflows |
| `src/gui/` | Native Win32 GUI over the public archive API |
| `src/gui/main_window.cpp` | Window creation, layout, message dispatch, `run_axiom_gui` entry point |
| `src/gui/main_window_*.cpp` | Method groups: browser, address bar, theming, views, search, commands, file ops, quick-add |
| `src/gui/dialog_support.cpp` | Axiom dialog palette and the `Wimukthi.Win32Theme` adapter |
| `src/sfx/` | Read-only self-extractor runtime and the module packager |
| `tests/` | Round-trip, safety, and regression tests |

GUI edits belong in the topical `main_window_*.cpp` translation unit, not back
in `main_window.cpp`. Shared helpers are declared in
`main_window_internal.hpp`.

## Pipeline

```text
encode:  input → classifier → solid block builder → reversible transforms
              → match finder → parser → entropy coder → container writer

decode:  container reader → entropy decoder → token decoder
              → inverse transforms → output
```

The container embeds one `axiom::compress` `.axc` stream per solid block, so
the single-stream codec and the archive share exactly the same encode and
decode path.

Codec selection sits *below* the AXAR service boundary. Encryption, signatures,
recovery records, split volumes, metadata, directory layout, and SFX packaging
all wrap completed AXC bytes, so none of them need codec-specific branches.

## Codec

### Match finders

Three, selected by effort:

| Finder | Levels | Notes |
|---|---|---|
| Fast 2-way row hash (`fast_lz`) | 1 | Fixed-probe, byte-token output |
| Price-aware lazy hash chain | 2–6 | Chain depth and lazy matching rise with level |
| Cyclic-window binary tree | 7–9 | LZMA-style; `--bt` selects it explicitly |

The lazy step is genuinely price-aware: it defers on token-cost comparison and
on repeat-offsets available one position ahead, not merely on "strictly
longer". Level 7 applies the same cost-aware one-byte lookahead through a
non-mutating tree search.

Binary-tree slots are indexed by `position % min(window, input_size)`, a
descent stops when a candidate falls outside the window, and memory stays
proportional to `--window` rather than to the whole input.

Match-length comparison reads eight bytes at a time (SWAR).

### Parsers

Levels 1–7 parse greedily with optional lazy deferral. Levels 8–9 run a bounded
dynamic-programming optimal parser whose candidates come from the binary tree
itself: advancing a position both inserts it and yields each improving
`(length, distance)` pair met during the descent, so one bounded search
surfaces several distinct lengths at their nearest distances.

Two effort shapes exist:

- **Single-pass** (levels 8–9): take the cost model from the greedy parse the
  block encoder already computed, then run the DP once. Level 9 raises tree
  depth, window, block, and maximum-match limits.
- **Two-pass** (explicit `--optimal`): parse with fixed weights, measure the
  output streams, re-parse with measured entropy costs, and keep whichever
  fully encoded result is smaller.

Level 1 is deliberately not an LZMA clone. Its hot path is a fixed-probe row
hash parser over independent blocks plus repeat-offset tokens; it must not
depend on the tree matcher, optimal parser, or a probability model to hit its
speed target.

### Entropy coding

After parsing, LZ77 data is split into separate streams — commands, literal
lengths, match lengths, distances or distance slots plus footer bits, and
literals. Each stream is then coded by:

- byte-level canonical **Huffman**;
- a **4-lane interleaved order-0 rANS**; or
- a **clustered static order-1 rANS** — previous-byte contexts grouped into at
  most 16 transmitted frequency tables plus a context map, decoding with the
  same interleaved table-lookup loop as order-0.

On the speed levels streams go straight to order-0 rANS. On the ratio levels
every coder competes per substream and the smallest wins. Both rANS encoders
use precomputed reciprocals in their hot loops instead of per-symbol division.

A Fenwick-backed adaptive order-1 range coder remains *decodable* for older
archives but is no longer emitted — it decoded roughly 30x slower for a
fraction of a percent. An earlier bit-serial order-0 arithmetic coder was
removed outright once rANS superseded it.

Before entropy-coding a sequence candidate, its stream estimate is compared
with the best exact legacy size; a candidate that cannot plausibly win is
rejected after analysis. Fast-entropy presets share one token-analysis pass
between layouts and skip the legacy bake-off only when the completed payload
already beats a sampled conditional-entropy estimate.

### Reversible transforms

The transform layer supports independently reset:

| ID | Transform |
|---:|---|
| 1 | x86/x64 relative-branch conversion |
| 2 | Byte-delta over a stride |
| 3 | 16-bit numeric: left/2D predictor, signed-residual zigzag, byte-plane shuffle |

PE, x86 ELF, PCM WAV, and uncompressed BMP signatures provide candidate hints.
Direct AXC compression also validates POSIX tar headers, inspects x86 ELF
payloads inside one nested tar, and entropy-screens both raw inputs and
individual tar members for the numeric transform.

A fast trial encode enables the resulting ranges only when they beat the
unfiltered representation. AXAR supplies per-file ranges and disables
block-wide auto-detection when none of its files qualify, which stops a
predictor from crossing solid-block file boundaries. The final AXC CRC always
authenticates the original bytes.

### Block format history

Each block independently picks store, raw LZ77, Huffman-coded LZ77, the level-1
`fast_lz` format, or split-stream LZ77. Every representation below competes in
the same per-block bake-off, and a newer one ships **only when its complete
payload is strictly smaller**.

| AXC | Adds | Still decodable |
|---:|---|---|
| 4 | Canonical fixed 32-byte header | ✓ |
| 5 | Bounded transform-metadata section | ✓ |
| 6 | Sequence-oriented payload: logarithmic length codes, recent-distance slots, previous-byte literal lanes, optional rep0-XOR residual | ✓ |
| 7 | Hybrid: legacy split command/length/distance streams with v6 literal lanes; transform id 3 | ✓ |
| 8 | Slot-context distance-footer coding; static match-byte and full-previous-byte literal modes | ✓ |
| 9 | Parser-checkpoint context-split blocks (current native output) | ✓ |
| 10 | External-codec envelope for Zstandard, LZMA2, and Deflate | ✓ |

Notable details:

- **v8 footers.** Short distance footers are coded completely; longer distances
  keep high bits packed and code the low four alignment bits with four-lane
  rANS tables selected by the distance slot. The decoder derives the context
  sequence from already-decoded slots, so it stays bounded and search-free.
- **v8 literals.** Match-byte mode XORs the first literal after a match with
  its rep0 byte into one of eight lanes chosen by that byte. Full-previous mode
  instead maps all 256 preceding-byte values to at most 16 encoder-chosen
  clusters, one static stream each. Decoding is one map lookup and one bounded
  stream read per literal — nothing learns, nothing searches.
- **v9 checkpoints.** Roughly 2 MiB parser tiles retain the full block match
  window but end at token boundaries, so their optimal DPs are independent. A
  zero-output checkpoint command carries four descriptors that install the
  recent-distance table before the next tile. Decode performs four bounded
  descriptor reads and table assignments.
- **v10 envelope.** Splits input into independently decodable chunks, records
  exact raw and encoded sizes, and permits a stored fallback per chunk. The
  decoder validates geometry and total restored size before invoking a backend,
  allocates only the header-declared bounded output, requires exact input
  consumption, and rejects trailing data. Chunk boundaries are also the
  pause/cancel and progress checkpoints, keeping progress independent of
  backend callback frequency.

Level-9 automatic block planning recognizes validated POSIX ustar members and
uses their boundaries as static match-window and entropy-table reset points.
Large members are split below the normal thread-derived budget and small
adjacent members are coalesced. The parallel block table has always carried
each block's original length, so variable boundaries need no new decoder
representation.

### SIMD

Vectorized where it measurably pays: BLAKE3 hashing (SSE2 through AVX-512,
runtime dispatched by CPUID), PCLMULQDQ-folded CRC-32, and SWAR match
comparison. Integrity hashing and CRC are the vectorized hot spots. The
matchers are scalar by measurement, not by omission.

### Long-term direction

The intended shape of the codec is a hybrid: LZ77-family local matching, long
distance references across solid groups, optional trained dictionaries,
transform tokens for structured formats, optimal parsing driven by estimated
entropy cost, rANS for the practical high-ratio mode, and context-mixed literal
coding reserved for a max-ratio research mode.

Future block headers should add a dictionary identifier, codec parameters,
per-block checksums, and additional codec parameters. The current seek map
lives in the skippable AXAR block-extra area so it does not change the AXAR
header version or the AXC payload bytes.

## Container

The binary layout is specified in [FORMAT.md](FORMAT.md). This section covers
the code structure above it.

### Archive API

The `.axar` API deliberately separates archive storage from file-manager
presentation. Beyond create, list, test, and extract it exposes
destination-aware insertion (`ArchiveInput`), selective extraction,
metadata-only entry moves (`ArchiveMove`), add/update/freshen/sync/delete/
repack, comments and locking, and encryption-mode queries.

Mutating operations honor `OperationControl` and reject locked archives. AXAR
block-encrypted and directory-encrypted archives are editable with the password.
Append-compatible add/update/freshen/sync operations, and signing, retain the
previous complete footer and append a generation containing the replacement
directory; if header flags must change, or the operation is a compaction/rebuild,
the established temporary-archive-plus-rename path is used. Encryption-v2
password-slot changes rewrite only encryption metadata/directory and preserve
existing compressed block bytes; they invalidate signatures and rebuild recovery
data against the new layout.

Archive creation is split between a seekable file writer, which can patch the
final v4/v5 header after metadata capture, and a counted sequential sink. The
`create_archive_to_stream` profile writes AXAR v5 with all known fidelity flags
reserved up front, supports compression and encryption, and deliberately rejects
recovery records because a non-seekable output cannot be repaired or signed in
place. The sink owns no stream lifetime and reports its byte position explicitly.

Selected AXAR extraction is seek-aware when the block carries the optional
subframe map described in `FORMAT.md`. `BlockSource` reads the AXC prefix once,
validates the map against the outer codec, and decodes only the intersecting
parallel block or external-codec chunk. Encrypted, transformed, serial, and
legacy blocks deliberately fall back to the existing bounded whole-block path.
The directory is parsed once, while the path lookup table is built lazily only
when selection or hard-link resolution needs it. Shared reader statistics count
physical archive bytes fetched so the CLI and native progress window can show
the cost of a selected restore.

### Snapshot deduplication

The snapshot profile is isolated behind the AXAR v5 required chunk-table flag.
`build_snapshot_entries` scans each regular file once, feeds bounded
content-defined chunks to the normal block writer, and consults a stable
`(logical_size, BLAKE3)` identity table before writing a new chunk. Encrypted
repositories use a keyed BLAKE3 identity derived from the archive data key by
default, preventing an observer from learning chunk equality from the table.
`EntryRec::chunk_refs` is the only content address used by snapshot entries;
`BlockSource::chunk` validates the stored CRC and identity before exposing the
bytes to extraction, testing, or snapshot restore.

The archive metadata carries both the live directory and bounded historical
manifests. `add_archive_snapshot` appends new chunk blocks and one generation
directory, while list and diff operate only on manifest data and never decode
unchanged content. `restore_archive_snapshot` temporarily selects a historical
entry catalogue while retaining the same chunk table and reader validation.
Ordinary content mutation paths reject a chunk-table archive; this prevents a
legacy update, sync, delete, or move from dropping history. Metadata-only
directory rewrites preserve the chunk table.

`prune_archive_snapshots` appends a manifest-only generation and protects the
current snapshot from deletion. `repack_snapshot_archive` marks every chunk
reachable from the current and retained manifests, copies only the referenced
blocks, remaps chunk and entry addresses, re-seals encrypted blocks with their
new block-index associated data, and rebuilds recovery data. This gives snapshot
repositories an explicit garbage-collection boundary without changing old
archives or pretending that historical chunks are ordinary dead solid ranges.

### Providers

Archive browsing goes through a built-in provider layer:

| Provider | Capability |
|---|---|
| `axar` | Full read/write; adapts the archive API without changing format or behavior |
| `zip` | miniz-backed browse, test, extract, create, add, update, sync, delete, move. New encrypted ZIPs use WinZip AES-256. Existing encrypted ZIPs are read-only. Unchanged plaintext entries are cloned into an atomically rewritten ZIP |
| `system-readonly` | Windows only. Loads `7z.dll` directly for 7z, RAR/RAR5, hybrid ISO/UDF, and CAB; uses `tar.exe` for the TAR family. Never advertises create, update, delete, or move |

The GUI asks a provider for format identity and file-type text, capability
flags (list, extract, test, update, comments, encryption, recovery, snapshots,
signatures, SFX), directory entries, and the test/extract/write operations.
Commands are enabled from those flags rather than failing late.

This layer is **plug-in-shaped but not externally pluggable**. New formats
should land as compiled-in providers first, so the capability model, password
prompts, drag/drop behavior, and command enabling can stabilize before
committing to a public C ABI, DLL loading policy, sandboxing story, and
third-party parser trust model.

Pure ISO9660/Joliet browsing uses Axiom's native directory reader for immediate
display; hybrid media use the authoritative UDF catalog from the DLL. The DLL
adapter consumes structured properties and callbacks rather than launching a
helper process or parsing console text.

ZIP vendors miniz 3.1.2 for a small, build-system-friendly container
reader/writer and Deflate implementation. zlib-ng is a reasonable future
Deflate backend candidate if profiling shows miniz's codec path is the
bottleneck, but it is not a ZIP container layer. A privately namespaced
minizip-ng 4.2.2 core creates and reads standard `.z01`, `.z02`, …, `.zip`
sets, raw-copying completed entries so Deflate data, metadata, CRCs, and AES
ciphertext survive intact.

The per-format roadmap is in
[docs/FORMAT_SUPPORT.md](docs/FORMAT_SUPPORT.md).

### Services

- **Signatures** cover exact stored block bytes and canonical directory
  semantics.
- **SFX** writes a descriptor and an intact AXAR or ZIP at the end of the PE
  image of a dedicated read-only native module, so the payload survives
  Authenticode signing. The module ships as `AxiomSfx.bin`, is never exposed as
  a separate executable, and is read only during SFX creation. The layout and
  both read paths live in `src/archive/sfx_image.cpp`, which the engine and the
  extractor runtime share so they cannot disagree. `AxiomSfxDecodeLib` puts
  AXAR/ZIP listing, testing, signature inspection, and extraction behind the
  read-only `SfxArchiveReader` facade; the Mini stub does not link archive
  writers, mutation providers, or encoder backends. The runtime opens the
  payload where it lies rather than copying it out, and drives all interaction
  through the `SfxUi` interface, so one code path serves both the dialog stub
  and the console-only one. Design and roadmap:
  [docs/SFX_ARCHITECTURE.md](docs/SFX_ARCHITECTURE.md).
- **POSIX metadata** rides in a skippable entry TLV.
- **Recovery records** use the portable Reed-Solomon core and protect the
  archive through the end of the current directory and, for an appended
  generation, its 64-byte generation extension. Repair is atomic.
- **Volumes** are numbered data parts plus optional `.revNNN` parity volumes.
  Joining validates the reconstruction with BLAKE3 before installing it.

Recovery creation for a staged archive is stripe-bounded: it reads 64 KiB
slices across the data shards, encodes independent parity rows on the operation
worker pool, and spools parity until per-shard CRCs are known. This avoids
holding the complete protected archive in memory and avoids copying it to a
second temporary file, while keeping the bytes recovery-service version 1
compatible.

Complete AXAR data-volume sets are exposed through a segmented random-access
source, so list, test, and extract work on the numbered files without creating
a joined archive. Missing data parts fall back to Reed-Solomon reconstruction.

Synchronization is one archive transaction: the source is scanned once and
compared with one loaded catalogue, unchanged compressed blocks are copied
verbatim, changed and new entries are compressed once, and stale entries are
omitted from the final directory. Exact size/CRC/BLAKE3 matches reuse an
existing block range, so content-identical files added under a new path do not
create another compressed copy; repack applies the same coalescing while
rebuilding. When header features remain compatible, the changed blocks and new
directory are appended as one generation and the previous footer remains the
crash fallback. Comment, lock, and signature metadata fold into that directory
before recovery is generated. Header-changing sync and compaction continue to
receive the atomic temporary-file treatment. A no-change sync returns without
touching the archive.

## Threading

### Worker model

`thread_count == 0` means "use the machine": both compression and decode expose
every logical processor to the shared work-stealing executor. Compression block
geometry is a separate decision that targets the *physical* core count, so
adding SMT helpers does not silently halve blocks and weaken ratio. Explicit
thread counts are honored as given.

The codec caps long-running outer block jobs to the block count while spare
workers steal nested parser, candidate-layout, Huffman, split-layout, and
entropy tasks. Tiny single-block inputs stay serial to avoid thread startup
cost. The pool is sized with `size_t` and OS topology discovery rather than a
fixed 32-thread mask, so the model covers AMD SMT, Intel hybrid parts, and
machines with more than 32 processors across multiple processor groups.

### Block sizing

Two layers:

- Archive **solid blocks** group file bytes for cross-file compression and
  selective extraction.
- The single-stream codec can split a solid block into independently compressed
  **sub-blocks**, and AXAR records their optional seek map in the directory.

By default the archive layer raises the target solid-block size to at least
`hardware_threads × 1 MiB` when multiple workers are available, and the codec
layer then shrinks the internal block size as needed, down to a 1 MiB minimum
of useful work. One large solid block can therefore still feed many workers.
An explicit `--block-size` disables this for repeatable tuning runs.

Parallel blocks are independent by design, which sacrifices cross-block
matches. The archive selector still keeps the smallest single-stream result
where ratio beats block-level parallelism; fast levels prefer the parallel
result for large multi-block inputs, because the serial whole-input parse is
otherwise the dominant bottleneck.

Parallel encode and decode compute per-block CRCs on the worker threads and
combine them, avoiding a serial full-buffer CRC pass after the payload is
already available.

### Swarm

The opt-in swarm model segments a greedy parse at fixed, thread-count-
independent boundaries. It builds immutable per-segment indexes, searches
completed earlier segments for full-window reach, emits explicit distances, and
serially restores repeat-offset tokens.

Levels 2–6 use the cooperative hash-chain path directly. Level 1 can trade its
byte-token fast path for that better-ratio parser. Levels 8–9 use a local
binary tree plus prior-segment hash indexes for the preliminary greedy
candidate while keeping the global optimal DP intact. Level 7's path-dependent
lazy tree parse is not segmented.

### Level-9 pipeline

At level 9, global tree discovery and path selection form an exact bounded
pipeline automatically, independent of `--swarm`. The ordered tree publishes
the same candidate sequence as the direct parser in fixed 256 KiB tiles while
the DP consumes the preceding tile. Each tile is a task on the shared executor;
if all helpers are busy, the waiting consumer cooperatively executes its own
next tile. Only the current and next reservoirs are live, and the resulting
tokens are byte-identical across schedules and thread counts.

Level 8 retains the direct parser because its depth-16 tree is too cheap to
amortize tile materialization; custom optimal depths of at least 32 opt in.

The level-9 DP keeps only a `max_match + 1` ring of 64-bit frontier costs, and
its reconstruction state uses 32-bit distances with an explicitly 8-byte
decision record. These are encoder-memory changes only — the same costs,
decisions, tokens, and archive bytes are selected. On enwik9 they reduced peak
commit from 65.66 GiB to 39.55 GiB and improved compression from 130.79 s to
117.85 s on a 16-core/32-thread Ryzen 9 5950X.

The high-level DP remains ordered because its frontier and repeat-offset state
depend on the chosen prior path. A per-segment DP was attempted and rejected —
it lost both ratio and throughput. The AXC v9 checkpoint candidate relaxes that
dependency without hiding it: each fixed tile receives encoder-chosen static
rep state and forbids tokens from crossing its end, making tile DPs independent
and scalable. Because the ordinary global DP is still encoded and compared,
checkpoint framing can never worsen a written block.

### Progress and cancellation

`OperationControl` is the single source of progress truth. Producers publish a
coherent snapshot containing stage bytes, item counts, current path, per-file
bytes, an optional operation-wide phase index and count, a dedicated throughput
counter, archive-output and source-byte counters for live size and ratio, and
the number of source items/bytes represented by reused AXAR ranges.
The phase coordinates let the GUI render one non-resetting overall bar across
scanning, comparison, unchanged-block copying, compression, recovery, and
atomic commit while keeping exact phase-local counters.

Reading, compression, and ordered writing share one monotonic byte epoch:
reports are normalized under the snapshot writer lock, so a delayed producer
cannot move the overall bar backwards. Numeric fields use a sequence-guarded
atomic snapshot; paths are replaced atomically only when they change. Reports
coalesce at 1 MiB unless a stage, item, total, file, or completion boundary
changes, so telemetry never becomes an inner-loop bottleneck.

Progress stays continuous inside multi-second encodes. The parse loops tick a
fractional `encode_progress` hook every 256 KiB of scanned input,
`compress_block` maps each pass into a share of its block's wall time, the
parallel block codec sums per-worker in-flight fractions, and the archive
writer sums per-solid-block contributions across concurrent jobs rather than
using a shared high-water mark. On a level-9 Silesia archive the worst gap
between advances is about 0.6 s with steps under 2 MB, where a whole solid
block previously arrived at once.

Cancellation throws `OperationCancelled` and leaves no partial output — writes
are atomic. The unpaused checkpoint path is an atomic fast path.

## GUI boundary

The Win32 thread owns windows, menus, dialogs, input routing, and presentation
only. Archive identification, provider capability probes, catalog loading,
comments, recovery metadata, signature verification, SFX and split-volume
inspection, and every archive operation run on workers. Results return as
owned, typed messages; closing the main window invalidates the shared lifetime
token and drains queued payloads.

Operation threads never paint, format status text, query HWNDs, inspect a
growing output file, or enqueue progress messages. The GUI samples the
`OperationControl` snapshot at its own cadence, computes rate and ETA from a
rolling phase-local window, and repaints a liveness heartbeat even when an
external backend is between checkpoints. The `7z.dll` adapter publishes
structured progress and per-file write callbacks into the same atomic snapshot.

Editable controls declare their expected type or unit in the visible label and
share a tooltip contract for ranges, examples, and side effects. Integer,
byte-size, and hexadecimal controls share character and paste filters, but
submission validation remains authoritative — it checks full syntax, numeric
ranges, processor limits, required existing paths, and HTTP/HTTPS URLs before
any setting or operation is accepted. Text and path controls have explicit
length limits so native control storage cannot become an unbounded input.

The GUI delegates system light/dark detection, High Contrast behavior, native
title bars, control theme classes, and setting-change invalidation to the
sibling `Wimukthi.Win32Theme` framework, while owning its accent selection,
semantic colors, custom menus, and owner-drawn archive controls.

### Drag and drop

The file list implements `IDataObject`, `IDropSource`, and `IDropTarget`.
Drag-out materializes selected entries only when a shell target actually
requests `CF_HDROP`, avoiding extraction work before Explorer needs the files.

Drag-out has two telemetry phases. The provider first extracts entries into
Axiom's private staging directory. After the drop is accepted, the OLE
`CFSTR_FILECONTENTS` streams are wrapped by a read-only counting `IStream` that
reports bytes actually consumed by the shell, the current relative path, and
completed-file counts, publishing at 1 MiB or file boundaries without
rescanning or recopying. Transfer cancellation is checked on every stream read.
Pause is intentionally unavailable here, because an OLE stream call may be
dispatched on the source STA and blocking it would also block the Resume UI.

## Benchmarking infrastructure

The standing corpora are **enwik8** and the **Silesia corpus**, benchmarked as
a single tar the way zstd and most modern codecs report.

`bench/bench_codecs.py` compares Axiom levels against available LZ4, zstd,
Deflate, bzip2, LZMA2, and WinRAR RAR5 profiles. For folders it builds a
deterministic byte stream of relative paths and file bytes, then feeds that
same stream to every codec, so multi-file container differences stay out of
codec measurements. Every reported row is restored and compared byte-for-byte.

The GUI benchmark follows the same codec-focused rule without a temporary
workspace: generated corpora fill a resident byte vector, a custom file is
preloaded once, and custom folders become a deterministic sorted stream of
relative UTF-8 paths, lengths, and contents. Timed passes call the in-memory
`compress()` and `decompress()` APIs and compare byte-for-byte after each pass.

Usage is documented in [docs/BENCHMARKING.md](docs/BENCHMARKING.md); published
results are in [docs/PERFORMANCE.md](docs/PERFORMANCE.md).

### Compression prognosis

`estimate_compression_curve()` is the shared multi-level estimator behind the
Add-to-archive preview. It scans the selected inputs once, plans one set of
representative regions, reads and transforms each region once, then evaluates
every requested codec level against those identical bytes. Each point carries
projected archive bytes, ratio, saving, confidence interval, sample coverage,
and completion counters. A point is never compared with another point sampled
from a different part of the input.

The estimator is cooperative rather than GUI-aware: it reports immutable curve
snapshots through a callback and checks `OperationControl` between bounded
probes. The dialog owns debounce, cancellation, session caching, and painting.
No estimation worker reads HWND state or paints directly.

## Decoder rule

Any new feature must keep decompression deterministic and bounded:

- **No search** during decompression.
- **No machine-learning inference** during decompression.
- **Clear maximum memory**, derivable from the block header.
- **Reject** malformed distances, sizes, and checksums.

In practice this means new ratio work ships as encoder-chosen static structure
— cluster maps, slot contexts, checkpoint descriptors — that the decoder
replays with table lookups. Every such representation must also win the exact
complete-payload bake-off against the incumbent before it is ever written.
