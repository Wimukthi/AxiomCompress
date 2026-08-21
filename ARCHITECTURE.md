# Architecture

Everything in this project follows from one rule:

> **The encoder may be expensive. The decoder may not.**
>
> Compression can spend as much CPU as it likes searching for a better way to
> represent the data. Decompression has to stay predictable, bounded, and cheap
> to verify.

That asymmetry is the whole design. It is why an archive written at level 9
opens as fast as one written at level 1, and why the decoder can state its
maximum memory use from the block header before it allocates anything.

New encoder work is welcome, on one condition: it has to land as *static,
encoder-chosen structure* that the decoder can replay with table lookups. If a
clever idea requires the decoder to search, adapt, or infer, it doesn't belong
here.

This document explains how the code is arranged and why. For the byte layout,
see [FORMAT.md](FORMAT.md). For the terminology, see
[docs/GLOSSARY.md](docs/GLOSSARY.md).

## Contents

- [Source map](#source-map)
- [The pipeline](#the-pipeline)
- [Codec](#codec)
- [Container](#container)
- [Threading](#threading)
- [The GUI boundary](#the-gui-boundary)
- [Benchmarking infrastructure](#benchmarking-infrastructure)
- [The decoder rule](#the-decoder-rule)

## Source map

| Path | Owns |
|---|---|
| `src/codec/` | Compressing and decompressing a single block |
| `src/codec/external_codecs.cpp` | The chunk envelope and the Zstandard, LZMA2, and Deflate adapters |
| `src/entropy/` | Huffman and rANS coders |
| `src/archive/container.cpp` | The AXAR engine: directory, solid blocks, encryption, recovery, volumes, signing, SFX |
| `src/archive/container_zip.cpp` | ZIP read and write: miniz wrappers, AES-256 entries, the ZIP provider |
| `src/archive/container_formats.cpp` | Format detection and the provider registry |
| `src/archive/system_provider.cpp` | Read-only provider policy, the native ISO reader, Windows `tar.exe` |
| `src/archive/seven_zip_library.cpp` | The direct `7z.dll` adapter |
| `src/archive/zip_split_backend.cpp` | minizip-ng split-set creation and reading |
| `src/core/` | Checksums, crypto, filesystem metadata, Reed-Solomon, the task executor |
| `src/cli/` | `axiomc` parsing and workflows |
| `src/gui/` | The native Win32 app, built on the public archive API |
| `src/gui/main_window.cpp` | Window creation, layout, message dispatch, the `run_axiom_gui` entry point |
| `src/gui/main_window_*.cpp` | Method groups: browser, address bar, theming, views, search, commands, file ops, quick-add |
| `src/gui/dialog_support.cpp` | The Axiom dialog palette and the `Wimukthi.Win32Theme` adapter |
| `src/sfx/` | The read-only self-extractor runtime and the module packager |
| `tests/` | Round-trip, safety, and regression tests |

GUI edits belong in the topical `main_window_*.cpp` file, not back in
`main_window.cpp`. Shared helpers are declared in `main_window_internal.hpp`.

## The pipeline

```text
encode:  input → classifier → solid block builder → reversible transforms
              → match finder → parser → entropy coder → container writer

decode:  container reader → entropy decoder → token decoder
              → inverse transforms → output
```

Notice how much shorter the decode path is. That isn't an accident of the
diagram; it's the point.

The container embeds one AXC payload per solid block. Axiom's own blocks go
through `axiom::compress`. Zstandard, LZMA2, and Deflate go through the bounded
AXC v10 external-codec envelope. The archive layer and the single-stream layer
therefore share the same codec adapters, while external backends keep their own
chunk geometry.

Codec selection sits *below* the AXAR service boundary. Encryption, signatures,
recovery records, split volumes, metadata, directory layout, and SFX packaging
all wrap finished AXC bytes, so choosing a method doesn't require
codec-specific branches through the services.

The large-solid LZMA2 profile is the deliberate exception. Its disk-streamed
writer needs its own AXAR flag, and it does not currently combine with
encryption or recovery records.

## Codec

### Match finders

Three of them, chosen by how much effort the level asks for:

| Finder | Levels | Notes |
|---|---|---|
| Fast 2-way row hash (`fast_lz`) | 1 | Fixed number of probes, byte-token output |
| Cost-aware lazy hash chain | 2–6 | Chain depth and lazy matching rise with the level |
| Cyclic-window binary tree | 7–9 | LZMA-style; `--bt` selects it explicitly |

The lazy step is genuinely cost-aware. It defers a match by comparing token
costs, and by checking whether a repeat offset will be available one position
ahead — not merely by asking whether the next match is longer. Level 7 applies
the same one-byte lookahead through a tree search that doesn't mutate the tree.

Binary-tree slots are indexed by `position % min(window, input_size)`. A
descent stops as soon as a candidate falls outside the window, so memory stays
proportional to `--window` rather than to the whole input.

Match-length comparison reads eight bytes at a time (SWAR).

### Parsers

Levels 1 to 7 parse greedily, with optional lazy deferral.

Levels 8 and 9 run a bounded dynamic-programming parser that finds the cheapest
path through the block. Its candidates come from the binary tree itself:
advancing a position both inserts it and yields every improving
`(length, distance)` pair met on the way down, so one bounded search surfaces
several distinct lengths at their nearest distances.

There are two effort shapes:

- **Single-pass** (levels 8–9). Take the cost model from the greedy parse the
  block encoder has already computed, then run the DP once. Level 9 raises the
  tree depth, window, block, and maximum-match limits.
- **Two-pass** (explicit `--optimal`). Parse with fixed weights, measure the
  resulting streams, re-parse using those measured entropy costs, and keep
  whichever fully encoded result is smaller.

Level 1 is deliberately not a cut-down LZMA. Its hot path is a fixed-probe row
hash parser over independent blocks plus repeat-offset tokens, and it must not
grow a dependency on the tree matcher, the optimal parser, or a probability
model — those would cost it the speed that justifies its existence.

### Entropy coding

After parsing, the LZ77 data is split into separate streams: commands, literal
lengths, match lengths, distances (or distance slots plus footer bits), and
literals. Each stream is then coded by one of:

- byte-level canonical **Huffman**;
- a **4-lane interleaved order-0 rANS**; or
- a **clustered static order-1 rANS** — previous-byte contexts grouped into at
  most 16 transmitted frequency tables plus a context map, decoded by the same
  interleaved table-lookup loop as order-0.

On the speed levels, streams go straight to order-0 rANS. On the ratio levels
every coder competes per substream and the smallest wins. Both rANS encoders
use precomputed reciprocals in their hot loops rather than dividing per symbol.

Before entropy-coding a sequence candidate, its estimated stream size is
compared against the best exact legacy size, and a candidate that can't
plausibly win is dropped after analysis. The fast-entropy presets share one
token-analysis pass between layouts, and skip the legacy bake-off only when the
finished payload already beats a sampled conditional-entropy estimate.

One coder is kept for reading only. A Fenwick-backed adaptive order-1 range
coder remains *decodable* for older archives but is no longer emitted: it
decoded roughly 30 times slower for a fraction of a percent. An earlier
bit-serial order-0 arithmetic coder was removed outright once rANS superseded
it.

### Reversible transforms

The transform layer supports three transforms, each independently resettable:

| ID | Transform |
|---:|---|
| 1 | x86/x64 relative-branch conversion |
| 2 | Byte delta over a stride |
| 3 | 16-bit numeric: left/2D predictor, signed-residual zigzag, byte-plane shuffle |

PE, x86 ELF, PCM WAV, and uncompressed BMP signatures supply candidate hints.
Direct AXC compression also validates POSIX tar headers, inspects x86 ELF
payloads inside one nested tar, and entropy-screens both raw inputs and
individual tar members for the numeric transform.

A fast trial encode enables the resulting ranges only when they beat the
unfiltered representation. AXAR supplies per-file ranges and switches off
block-wide auto-detection when none of its files qualify, which stops a
predictor from running across a solid-block file boundary. The final AXC CRC
always authenticates the original bytes, whatever happened in between.

### Block format history

Each block independently picks one of: store, raw LZ77, Huffman-coded LZ77, the
level-1 `fast_lz` format, or split-stream LZ77.

Every representation below competes in the same per-block bake-off, and a newer
one ships **only when its complete payload is strictly smaller**. That rule is
what makes it safe to keep adding representations.

| AXC | Adds | Still decodable |
|---:|---|---|
| 4 | Canonical fixed 32-byte header | ✓ |
| 5 | Bounded transform-metadata section | ✓ |
| 6 | Sequence-oriented payload: logarithmic length codes, recent-distance slots, previous-byte literal lanes, optional rep0-XOR residual | ✓ |
| 7 | Hybrid: legacy split command/length/distance streams with v6 literal lanes; transform id 3 | ✓ |
| 8 | Slot-context distance-footer coding; static match-byte and full-previous-byte literal modes | ✓ |
| 9 | Parser-checkpoint context-split blocks (the current native output) | ✓ |
| 10 | External-codec envelope for Zstandard, LZMA2, and Deflate | ✓ |

Four of those deserve more detail.

**v8 footers.** Short distance footers are coded completely. Longer distances
keep their high bits packed and code the low four alignment bits with four-lane
rANS tables selected by the distance slot. The decoder derives the context
sequence from slots it has already decoded, so it stays bounded and
search-free.

**v8 literals.** Match-byte mode XORs the first literal after a match with its
rep0 byte, into one of eight lanes chosen by that byte. Full-previous mode maps
all 256 preceding-byte values to at most 16 encoder-chosen clusters, one static
stream each. Decoding either is one map lookup and one bounded stream read per
literal. Nothing learns; nothing searches.

**v9 checkpoints.** Roughly 2 MiB parser tiles keep the full block match window
but end at token boundaries, which makes their optimal DPs independent. A
zero-output checkpoint command carries four descriptors that install the
recent-distance table before the next tile begins. Decoding is four bounded
descriptor reads and four table assignments.

**v10 envelope.** Splits the input into independently decodable chunks, records
exact raw and encoded sizes, and permits a stored fallback per chunk. The
decoder validates the geometry and the total restored size *before* invoking a
backend, allocates only the bounded output the header declared, requires the
backend to consume its input exactly, and rejects trailing data.

Chunk boundaries double as the pause, cancel, and progress checkpoints, which
keeps progress reporting independent of how often a backend feels like calling
back. LZMA2 uses a 32-bit dictionary and chunk ceiling; the public 4 GiB
setting maps to 4 GiB−1 on the wire. The encoder keeps one stable LZMA2
property across a whole payload, including a short final chunk, so the decoder
never has to infer changing dictionary geometry.

Level-9 automatic block planning recognises validated POSIX ustar members and
uses their boundaries as static match-window and entropy-table reset points.
Large members are split below the normal thread-derived budget, and small
adjacent members are coalesced. The parallel block table has always carried
each block's original length, so variable boundaries need no new decoder
representation.

### SIMD

Vectorized where it measurably pays: BLAKE3 hashing (SSE2 through AVX-512,
dispatched at runtime by CPUID), PCLMULQDQ-folded CRC-32, and SWAR match
comparison.

Integrity hashing and CRC are the vectorized hot spots. The matchers are scalar
by measurement, not by omission.

### Where the codec is heading

The intended long-term shape is a hybrid: LZ77-family local matching, long
distance references across solid groups, optional trained dictionaries,
transform tokens for structured formats, optimal parsing driven by estimated
entropy cost, rANS for the practical high-ratio mode, and context-mixed literal
coding reserved for a max-ratio research mode.

Future block headers should add a dictionary identifier, codec parameters, and
per-block checksums. The current seek map lives in the skippable AXAR
block-extra area precisely so it doesn't change the AXAR header version or the
AXC payload bytes.

## Container

The byte layout is specified in [FORMAT.md](FORMAT.md). This section is about
the code above it.

### The archive API

The `.axar` API deliberately separates archive *storage* from file-manager
*presentation*. Beyond create, list, test, and extract, it exposes
destination-aware insertion (`ArchiveInput`), selective extraction,
metadata-only entry moves (`ArchiveMove`), add/update/freshen/sync/delete/
repack, comments and locking, and encryption-mode queries.

Mutating operations honour `OperationControl` and reject locked archives. AXAR
block-encrypted and directory-encrypted archives remain editable with the
password.

Append-compatible add, update, freshen, sync, and signing keep the previous
complete footer and append a generation containing the replacement directory.
If header flags have to change, or the operation compacts or rebuilds, the
established temporary-archive-plus-rename path takes over instead.
Encryption-v2 password-slot changes rewrite only the encryption metadata and
directory, preserving existing compressed block bytes; they invalidate
signatures and rebuild recovery data against the new layout.

Archive creation splits into two writers: a seekable file writer that can patch
the final v4/v5 header after metadata capture, and a counted sequential sink.
The `create_archive_to_stream` profile writes AXAR v5 with all known fidelity
flags reserved up front, supports compression and encryption, and deliberately
rejects recovery records — a non-seekable output cannot be repaired or signed in
place, so pretending otherwise would be a lie. The sink owns no stream lifetime
and reports its byte position explicitly.

Selected AXAR extraction is seek-aware whenever the block carries the optional
subframe map described in `FORMAT.md`. `BlockSource` reads the AXC prefix once,
validates the map against the outer codec, and decodes only the intersecting
parallel block or external-codec chunk. Encrypted, transformed, serial, and
legacy blocks deliberately fall back to the existing bounded whole-block path.
The directory is parsed once; the path lookup table is built lazily, only when
selection or hard-link resolution needs it. Shared reader statistics count the
physical archive bytes fetched, so the CLI and the progress window can show
what a selected restore actually cost.

### Content-addressed deduplication

The shared chunk engine has two isolated AXAR v5 profiles: snapshot history
behind required flag `0x0020`, and a mutable live archive behind `0x0080`.
The flags are mutually exclusive, so old readers fail closed and ordinary AXAR
archives are never silently converted.

`build_chunked_entries` scans each regular file once, feeds bounded
content-defined chunks to the normal block writer, and consults a stable
hash table keyed by `(logical_size, BLAKE3)` before writing a new chunk. Encrypted
repositories use a keyed BLAKE3 identity derived from the archive data key by
default, so an observer can't learn chunk equality from the table.
`EntryRec::chunk_refs` is the only content address a chunk-addressed entry uses,
and `BlockSource::chunk` validates both the stored CRC and the identity before
exposing bytes to extraction, testing, or restore.

Snapshot metadata carries both the live directory and bounded historical
manifests. `add_archive_snapshot` appends new chunk blocks and one generation
directory, while list and diff operate purely on manifest data and never decode
unchanged content. `restore_archive_snapshot` temporarily selects a historical
entry catalogue while keeping the same chunk table and reader validation.

Snapshot repositories reject ordinary mutation so update, sync, delete, or move
cannot silently drop history. Live-dedup archives route those same operations
through the chunk catalogue: additions and replacements append only new
identities, directory-only changes retain existing references, and deletions
leave unreachable chunks for garbage collection. The archive-level profile
persists chunker version and geometry so later writes cannot drift.

`prune_archive_snapshots` appends a manifest-only generation and protects the
current snapshot from deletion. `repack_snapshot_archive` marks every chunk
reachable from the current and retained manifests, copies only the referenced
blocks, remaps chunk and entry addresses, re-seals encrypted blocks with their
new block-index associated data, and rebuilds recovery data. Snapshot
repositories therefore get an explicit garbage-collection boundary, without
changing old archives and without pretending historical chunks are ordinary
dead solid ranges. The same mark/copy/remap engine compacts a live-dedup archive
from its current catalogue alone.

### Providers

Archive browsing goes through a built-in provider layer:

| Provider | What it can do |
|---|---|
| `axar` | Full read and write. Adapts the archive API without changing format or behaviour |
| `zip` | miniz-backed browse, test, extract, create, add, update, sync, delete, move. New encrypted ZIPs use WinZip AES-256; existing encrypted ZIPs are read-only. Unchanged plaintext entries are cloned into an atomically rewritten ZIP |
| `system-readonly` | Windows only. Loads `7z.dll` directly for 7z, RAR/RAR5, hybrid ISO/UDF, and CAB; uses `tar.exe` for the TAR family. Never advertises create, update, delete, or move |

The GUI asks a provider for its format identity and file-type text, its
capability flags (list, extract, test, update, comments, encryption, recovery,
snapshots, signatures, SFX), its directory entries, and the test, extract, and
write operations. Commands are enabled from those flags, rather than being
offered and then failing late.

This layer is **plug-in-shaped but not externally pluggable**, on purpose. New
formats should land as compiled-in providers first, so the capability model,
password prompts, drag-and-drop behaviour, and command enabling can stabilize
before anyone commits to a public C ABI, a DLL loading policy, a sandboxing
story, and a trust model for third-party parsers.

Pure ISO9660/Joliet browsing uses Axiom's native directory reader for immediate
display; hybrid media use the authoritative UDF catalog from the DLL. The DLL
adapter consumes structured properties and callbacks rather than launching a
helper process or parsing console text.

ZIP vendors miniz 3.1.2 for a small, build-system-friendly container
reader/writer and Deflate implementation. zlib-ng would be a reasonable future
Deflate backend if profiling ever shows miniz's codec path is the bottleneck,
but it is not a ZIP container layer and shouldn't be mistaken for one. A
privately namespaced minizip-ng 4.2.2 core creates and reads standard `.z01`,
`.z02`, …, `.zip` sets, raw-copying completed entries so Deflate data,
metadata, CRCs, and AES ciphertext survive intact.

The per-format roadmap is in [docs/FORMAT_SUPPORT.md](docs/FORMAT_SUPPORT.md).

### Services

- **Signatures** cover the exact stored block bytes and the canonical directory
  semantics.
- **SFX** writes a descriptor and an intact AXAR or ZIP at the end of the PE
  *image* of a dedicated read-only module — not at the end of the file — so the
  payload survives Authenticode signing. The module ships as `AxiomSfx.bin`, is
  never exposed as a separate executable, and is read only during SFX creation.
  The layout and both read paths live in `src/archive/sfx_image.cpp`, which the
  engine and the extractor runtime share so they cannot disagree with each
  other. `AxiomSfxDecodeLib` puts AXAR/ZIP listing, testing, signature
  inspection, and extraction behind the read-only `SfxArchiveReader` facade; the
  Mini stub links no archive writers, mutation providers, or encoder backends.
  The runtime opens the payload where it lies rather than copying it out, and
  drives all interaction through the `SfxUi` interface, so one code path serves
  both the dialog stub and the console-only one. Design and roadmap:
  [docs/SFX_ARCHITECTURE.md](docs/SFX_ARCHITECTURE.md).
- **POSIX metadata** rides in a skippable entry TLV.
- **Recovery records** use the portable Reed-Solomon core and protect the
  archive through the end of the current directory and, for an appended
  generation, its 64-byte generation extension. Repair is atomic.
- **Volumes** are numbered data parts plus optional `.revNNN` parity volumes.
  Joining validates the reconstruction with BLAKE3 before installing it.

Recovery creation for a staged archive is stripe-bounded: it reads 64 KiB
slices across the data shards, encodes independent parity rows on the operation
worker pool, and spools parity until per-shard CRCs are known. That avoids
holding the complete protected archive in memory, and avoids copying it to a
second temporary file, while keeping the bytes compatible with recovery-service
version 1.

Complete AXAR data-volume sets are exposed through a segmented random-access
source, so list, test, and extract work directly on the numbered files without
creating a joined archive. Missing data parts fall back to Reed-Solomon
reconstruction.

Synchronization is one archive transaction. The source is scanned once and
compared against one loaded catalogue; unchanged compressed blocks are copied
verbatim; changed and new entries are compressed once; stale entries are simply
omitted from the final directory. Exact size, CRC, and BLAKE3 matches reuse an
existing block range, so content-identical files added under a new path do not
create another compressed copy — and repack applies the same coalescing while
it rebuilds.

When header features stay compatible, the changed blocks and new directory are
appended as one generation and the previous footer remains the crash fallback.
Comment, lock, and signature metadata fold into that directory before recovery
is generated. Header-changing sync and compaction still get the atomic
temporary-file treatment. A sync that finds nothing to change returns without
touching the archive at all.

## Threading

### The worker model

`thread_count == 0` means "use the machine": both compression and decode expose
every logical processor to the shared work-stealing executor.

Compression block *geometry* is a separate decision, and it targets the
**physical** core count. This matters more than it sounds. If block planning
followed logical processors, adding SMT would silently halve the block size and
weaken the ratio for no gain. Explicit thread counts are honoured as given.

The codec caps long-running outer block jobs at the block count, while spare
workers steal nested parser, candidate-layout, Huffman, split-layout, and
entropy tasks. Tiny single-block inputs stay serial, because starting threads
would cost more than it saves.

The pool is sized with `size_t` and real OS topology discovery, not a fixed
32-thread mask, so the model covers AMD SMT, Intel hybrid parts, and machines
with more than 32 processors spread across multiple processor groups.

### Block sizing

There are two layers:

- Archive **solid blocks** group file bytes for cross-file compression and
  selective extraction.
- The single-stream codec can split a solid block into independently compressed
  **sub-blocks**, and AXAR records their optional seek map in the directory.

By default the archive layer raises the target solid-block size to at least
`hardware_threads × 1 MiB` when multiple workers are available, and the codec
layer then shrinks the internal block size as needed, down to a 1 MiB minimum
of useful work. One large solid block can therefore still feed many workers. An
explicit `--block-size` disables all of this, which is what you want for
repeatable tuning runs and nothing else.

AXAR LZMA2 can deliberately select an 8–64 GiB outer solid block. That
large-solid profile stages raw bytes in a temporary file and streams bounded
AXEC chunks instead of materializing the complete block. It defaults to 512 MiB
codec chunks and can raise them to the selected dictionary, with the outer block
and the codec chunk remaining separate limits. The profile carries a required
AXAR flag, uses no file-aware transforms, and currently excludes encryption and
recovery records; old readers reject it before ordinary block decoding begins.

Parallel blocks are independent by design, which sacrifices matches across
their boundaries. The archive selector still keeps the smallest single-stream
result where ratio beats block-level parallelism. Fast levels prefer the
parallel result on large multi-block inputs, because there the serial
whole-input parse is the dominant bottleneck.

Parallel encode and decode compute per-block CRCs on the worker threads and
combine them, avoiding a serial full-buffer CRC pass after the payload is
already available.

### Swarm

The opt-in swarm model segments a greedy parse at fixed boundaries that do not
depend on the thread count. It builds immutable per-segment indexes, searches
completed earlier segments for full-window reach, emits explicit distances, and
serially restores repeat-offset tokens.

Levels 2–6 use the cooperative hash-chain path directly. Level 1 can trade its
byte-token fast path for that better-ratio parser. Levels 8–9 use a local
binary tree plus prior-segment hash indexes for the preliminary greedy
candidate while keeping the global optimal DP intact. Level 7's path-dependent
lazy tree parse is not segmented, and shouldn't be.

### The level-9 pipeline

At level 9, global tree discovery and path selection form an exact bounded
pipeline automatically, independent of `--swarm`.

The ordered tree publishes the same candidate sequence as the direct parser, in
fixed 256 KiB tiles, while the DP consumes the preceding tile. Each tile is a
task on the shared executor; if every helper is busy, the waiting consumer
cooperatively executes its own next tile rather than blocking. Only the current
and next reservoirs are live, and the resulting tokens are byte-identical
across schedules and thread counts.

Level 8 keeps the direct parser, because its depth-16 tree is too cheap to
amortize tile materialization. Custom optimal depths of 32 or more opt in.

The level-9 DP keeps only a `max_match + 1` ring of 64-bit frontier costs, and
its reconstruction state uses 32-bit distances with an explicitly 8-byte
decision record. These are encoder-memory changes only — the same costs,
decisions, tokens, and archive bytes come out. On enwik9 they cut peak commit
from 65.66 GiB to 39.55 GiB and improved compression time from 130.79 s to
117.85 s on a 16-core / 32-thread Ryzen 9 5950X.

The high-level DP stays ordered, because its frontier and repeat-offset state
depend on the path already chosen. A per-segment DP was attempted and rejected:
it lost both ratio and throughput. The AXC v9 checkpoint candidate relaxes that
dependency without hiding it — each fixed tile receives encoder-chosen static
rep state and forbids tokens from crossing its end, which makes tile DPs
independent and scalable. Because the ordinary global DP is still encoded and
compared, checkpoint framing can never make a written block worse.

### Progress and cancellation

`OperationControl` is the single source of truth for progress. Producers
publish a coherent snapshot: stage bytes, item counts, current path, per-file
bytes, an optional operation-wide phase index and count, a dedicated throughput
counter, archive-output and source-byte counters for live size and ratio, and
the number of source items and bytes represented by reused AXAR ranges. The
phase coordinates let the GUI render one non-resetting overall bar across
scanning, comparison, unchanged-block copying, compression, recovery, and
atomic commit, while keeping exact phase-local counters.

Reading, compression, and ordered writing share one monotonic byte epoch.
Reports are normalized under the snapshot writer lock, so a delayed producer
can never move the overall bar backwards. Numeric fields use a sequence-guarded
atomic snapshot; paths are replaced atomically, and only when they change.
Reports coalesce at 1 MiB unless a stage, item, total, file, or completion
boundary changes, so telemetry never becomes an inner-loop bottleneck.

Progress stays continuous inside multi-second encodes. The parse loops tick a
fractional `encode_progress` hook every 256 KiB of scanned input,
`compress_block` maps each pass into a share of its block's wall time, the
parallel block codec sums per-worker in-flight fractions, and the archive
writer sums per-solid-block contributions across concurrent jobs rather than
using a shared high-water mark. On a level-9 Silesia archive the worst gap
between advances is about 0.6 s, with steps under 2 MB — where a whole solid
block used to arrive at once.

Cancellation throws `OperationCancelled` and leaves no partial output, because
writes are atomic. The unpaused checkpoint path is an atomic fast path.

### Presenting progress

The snapshot is deliberately wide — 21 fields — because producers should report
what they know and let each frontend decide what is worth showing. Frontends
are expected to curate, not to mirror. The Windows progress window renders six
lines plus two bars, and keeps elapsed time, time since the last update, and
physical archive bytes read behind a **Details** disclosure; the CLI renders one
line. Both drop fields that would not change a reader's decision.

Two rules exist because breaking them produced wrong numbers rather than merely
noisy ones:

- **Scope must be stated wherever two scopes coexist.** With phases active, the
  bar and its percentage span the whole operation via `overall_progress()`,
  while the byte and item counters describe only the current phase. Time
  remaining is derived from the phase counters, so it is a phase estimate. The
  window says "overall", "in this stage", and "left in this stage" rather than
  letting one row imply a single measurement.
- **Throughput has one definition, in `core/progress_rate.hpp`.** It samples
  `throughput_bytes` — falling back to `completed_bytes` — over a trailing
  four-second window. A cumulative average lags a speed change permanently, and
  reading `completed_bytes` during the first solid block reports 0 B/s while
  the reader is visibly busy. The CLI and the GUI share the tracker so they
  cannot drift apart again.

## The GUI boundary

The Win32 thread owns windows, menus, dialogs, input routing, and presentation.
That's all it owns.

Archive identification, provider capability probes, catalog loading, comments,
recovery metadata, signature verification, SFX and split-volume inspection, and
every archive operation run on workers. Results come back as owned, typed
messages. Closing the main window invalidates the shared lifetime token and
drains any queued payloads.

Operation threads never paint, never format status text, never query an HWND,
never inspect a growing output file, and never enqueue progress messages. The
GUI samples the `OperationControl` snapshot at its own cadence, computes rate
and ETA from a rolling phase-local window, and repaints a liveness heartbeat
even when an external backend is between checkpoints. The `7z.dll` adapter
publishes structured progress and per-file write callbacks into that same
atomic snapshot.

Editable controls declare their expected type or unit in the visible label, and
share a tooltip contract for ranges, examples, and side effects. Integer,
byte-size, and hexadecimal controls share character and paste filters — but
submission validation remains authoritative. It checks full syntax, numeric
ranges, processor limits, required existing paths, and HTTP/HTTPS URLs before
any setting or operation is accepted. Text and path controls have explicit
length limits, so native control storage can never become an unbounded input.

The GUI delegates system light/dark detection, High Contrast behaviour, native
title bars, control theme classes, and setting-change invalidation to the
sibling `Wimukthi.Win32Theme` framework, while owning its own accent selection,
semantic colors, custom menus, and owner-drawn archive controls.

### Drag and drop

The file list implements `IDataObject`, `IDropSource`, and `IDropTarget`.

Dragging a **filesystem** selection out is trivial: the data object offers
`CF_HDROP` with the real paths and the shell does the rest.

Dragging an **archive** selection out cannot work that way, because the files
do not exist yet. That path offers virtual files instead —
`CFSTR_FILEDESCRIPTORW` describes the entries, and `CFSTR_FILECONTENTS`
supplies each one as an `IStream`. Extraction into Axiom's private staging
directory is deferred until the shell asks for the first stream, so hovering
over Explorer never starts any work.

Drag-out therefore has two phases, and they behave very differently.

**Extraction** runs on a worker thread while the GUI thread waits in a nested
message loop, so its progress window repaints normally.

**Transfer** does not. The shell reads the staged files by calling back into
our `IStream`, and those calls are marshalled onto the source STA — the GUI
thread. While they run, that thread dispatches no `WM_TIMER` and no input.
`CFSTR_FILECONTENTS` streams are wrapped by a read-only counting `IStream` that
reports bytes actually consumed by the shell, the current relative path, and
completed-file counts, publishing at 1 MiB or file boundaries without
rescanning or recopying; cancellation is checked on every read. All of that
telemetry reaches `OperationControl` correctly — but a progress window owned by
the GUI thread would never paint it, and its Cancel button would never receive
a click.

So the transfer progress window is hosted on its own UI thread
(`ThreadedOperationProgressWindow`). It polls the same `OperationControl`
snapshot every 33 ms, which is safe from any thread, and it takes no owner
window: owning a window across threads attaches the two input queues and would
reintroduce exactly the stall the separate thread exists to remove. For the
same reason it declines `WM_SETTINGCHANGE` theme tracking, since that reads and
writes process-wide theme state that only the GUI thread may touch.

Pause remains deliberately unavailable. Blocking inside an OLE stream read
would stall the shell's copy with nothing able to resume it — an unpausable
operation is better than a deadlocked one. Cancel is available and now
actually clickable.

## Benchmarking infrastructure

The standing corpora are **enwik8** and the **Silesia corpus**, benchmarked as a
single tar, the way zstd and most modern codecs report.

`bench/bench_codecs.py` compares Axiom levels against whichever of LZ4, zstd,
Deflate, bzip2, LZMA2, and WinRAR RAR5 are available. For folders it builds a
deterministic byte stream of relative paths and file bytes, then feeds that same
stream to every codec, so multi-file container differences stay out of the codec
measurement. Every reported row is restored and compared byte-for-byte.

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
and completion counters.

A point is never compared with another point sampled from a different part of
the input. That constraint is the entire reason the curve is trustworthy.

The estimator is cooperative rather than GUI-aware: it reports immutable curve
snapshots through a callback and checks `OperationControl` between bounded
probes. The dialog owns debounce, cancellation, session caching, and painting.
No estimation worker reads HWND state or paints anything.

## The decoder rule

Any new feature has to keep decompression deterministic and bounded:

- **No search** during decompression.
- **No machine-learning inference** during decompression.
- **A clear maximum memory**, derivable from the block header.
- **Reject** malformed distances, sizes, and checksums.

In practice that means new ratio work ships as encoder-chosen static structure —
cluster maps, slot contexts, checkpoint descriptors — which the decoder replays
with table lookups. And every such representation must win the exact
complete-payload bake-off against the incumbent before it is ever written to
disk.
