# Architecture

The project is structured so the encoder can become much more complex without making the decoder expensive or unpredictable.

## Quick map

Use this document when you need to know where a feature belongs:

| Area | Owns |
|---|---|
| `src/codec` | Single-block compression and decompression |
| `src/archive` | `.axar` container, metadata, encryption, recovery, volumes, signing, SFX |
| `src/archive/container.cpp` | The AXAR engine: directory parsing, solid blocks, encryption, recovery, volumes, signing, SFX |
| `src/archive/container_zip.cpp` | ZIP read/write: miniz wrappers, ZipCrypto/AES-256 entries, the ZIP provider |
| `src/archive/container_formats.cpp` | Format detection (magic/extension sniffing) and the provider registry |
| `src/archive/system_provider.cpp` | Read-only providers backed by bundled 7-Zip and Windows `tar.exe` |
| `src/archive/container_internal.hpp` | Internal helpers shared between the archive translation units |
| `src/core` | Shared utilities: checksums, crypto, filesystem metadata, Reed-Solomon |
| `src/cli` | `axiomc` command parsing and CLI workflows |
| `src/gui` | Native Win32 GUI over the public archive APIs |
| `src/gui/main_window.cpp` | Main window creation, layout, message dispatch, and the `run_axiom_gui` entry point |
| `src/gui/main_window_*.cpp` | The other main-window method groups: browser/tree wiring, address bar, theming, dark-drawn views, find dialog, commands, file operations, helpers, quick-add/SFX startup |
| `src/gui/main_window_internal.hpp` | Declarations shared between the main-window translation units |
| `tests` | Round-trip, safety, and regression tests |

The important design rule is simple: compression may spend more CPU to find a
better representation, but decompression must stay deterministic, bounded, and
easy to validate.

```text
archive input
  -> classifier
  -> solid block builder
  -> reversible transforms
  -> match finder
  -> parser
  -> entropy coder
  -> container writer

container reader
  -> entropy decoder
  -> token decoder
  -> inverse transforms
  -> restored output
```

## Implementation

The codec currently implements:

- Three match finders selected by effort: a **fast 2-way row-hash matcher**
  (`fast_lz`, level 1), a **price-aware lazy hash-chain matcher** (levels 2–6:
  the lazy step defers on token-cost comparison and on repeat-offsets available
  one position ahead, not just on "strictly longer"), and an LZMA-style
  **cyclic-window binary-tree matcher** (`--bt`, levels 7–9). Level 7 applies
  the same cost-aware one-byte lookahead through a non-mutating tree search;
  levels 8–9 use the optimal parser instead.
- A bounded **optimal (DP) parser** whose candidates come from the binary tree
  (LZMA-style `GetMatches`: the descent yields several distinct lengths, each
  at its nearest distance). Level 9 runs it two-pass (re-parse with measured
  entropy costs); level 8 runs it single-pass with the cost model measured from
  the greedy parse, for most of the ratio at roughly half the time.
- Repeat-offset (rep0–rep3) matches and split LZ77 streams, with an optional
  position-slot distance representation.
- Entropy via byte-level canonical Huffman, a **4-lane interleaved order-0
  rANS**, and a **clustered static order-1 rANS** (previous-byte contexts
  grouped into at most 16 transmitted frequency tables; decodes at table-lookup
  speed). At the thorough levels the order-1 coder competes for the literal,
  command, length, and distance-slot streams and is kept only when strictly
  smaller. (A Fenwick-backed adaptive order-1 range coder remains decodable for
  older archives but is no longer emitted — it decoded ~30x slower for a
  fraction of a percent; an earlier bit-serial order-0 arithmetic coder was
  removed entirely once rANS superseded it.)
- SIMD where it measurably pays: BLAKE3 hashing (SSE2→AVX-512, runtime
  dispatched), PCLMULQDQ-folded CRC-32, and SWAR match comparison. Integrity
  hashing and CRC are the vectorized hot spots; the matchers are scalar by
  measurement, not omission.
- A threaded independent-block codec, and a multi-file `.axar` container of solid
  blocks with a central directory (see [FORMAT.md](FORMAT.md)). The default
  writer now sizes both archive solid blocks and internal codec blocks from the
  selected hardware-thread count so `--threads 0` feeds all detected workers.
- Two front-ends — a CLI (`axiomc`) and a Windows GUI (`Axiom.exe`) — over the
  same library. Long operations report progress and honor cooperative
  pause/cancel through an `OperationControl` passed in the option structs;
  cancellation throws `OperationCancelled` and leaves no partial output (writes
  are atomic).

The container embeds one `axiom::compress` `.axc` stream per solid block, so the
single-stream codec and the archive share exactly the same encode/decode path.
The decoder is deliberately simple and bounded (see "Decoder Rule"); all the
complexity lives in the encoder.

### Archive API shape

The `.axar` public API deliberately separates archive storage from file-manager
presentation. Besides create/list/test/extract, it exposes:

- destination-aware insertion through `ArchiveInput`;
- selective extraction;
- metadata-only entry moves through `ArchiveMove`;
- add, update, freshen, sync, delete, and repack;
- comments and locking;
- archive encryption mode queries.

All mutating operations use a temporary archive and replacement rename. They
honor `OperationControl` and reject locked archives. Data-only encrypted archives
are editable with the password; encrypted-directory archives currently remain
read-only.

### Archive providers

Archive browsing now goes through a built-in provider layer. The registered
providers are:

- `axar`: the native full read/write provider, adapting the existing archive API
  without changing the format or behavior;
- `zip`: a miniz-backed provider for browsing, testing, extracting, creating,
  adding, updating, synchronizing, deleting, and moving entries in normal ZIP
  archives. New encrypted ZIPs use WinZip AES-256 file-data encryption.
  Existing encrypted ZIPs can be listed, tested, and extracted with a password,
  but are not edited in place yet. Existing unchanged plaintext entries are
  preserved by cloning them into an atomically rewritten ZIP. AXAR and ZIP can
  both be packaged behind the native self-extractor stub.
- `system-readonly`: a Windows-only read-only provider. It uses Axiom's bundled
  7-Zip console backend for 7z, RAR/RAR5, ISO, and CAB, and Windows `tar.exe`
  for TAR-family archives. It never advertises create, update, delete, or move.

The GUI asks the provider for:

- format identity and file type text;
- capability flags such as list, extract, test, update, comments, encryption,
  recovery, signatures, and SFX;
- directory entries for the browser;
- test, extraction, and write operations.

This is intentionally **plug-in-shaped but not externally pluggable** yet. New
formats should land as compiled-in providers first so the capability model,
password prompts, drag/drop behavior, and command enabling can stabilize without
committing to a public C ABI, DLL loading policy, sandboxing story, or third-party
parser trust model.

The intended support split is:

- full native support remains `.axar`;
- ZIP has practical read/write support for plaintext archives, exact
  central-directory packed sizes, and AES-256 file-data encryption for new ZIPs.
  Existing encrypted ZIPs are read/test/extract only; comments, encrypted names,
  rich attributes, and AXAR-specific services remain unsupported;
- AXAR exposes per-file Packed values as estimates because files share solid
  blocks; archive-level size and ratio remain exact in the information dialog;
- AXAR and ZIP are the only creation targets in the GUI. The Add-to-archive
  dialog filters out read-only providers even though the Open dialog can browse
  them;
- 7z/RAR/CAB support is view/extract/test through the bundled 7-Zip backend.
  ISO browsing uses a native ISO9660/Joliet directory reader for immediate
  display; ISO extraction/test still use the bundled 7-Zip backend.
  TAR-family support remains view/extract/test through Windows `tar.exe`. A
  future direct libarchive or 7-Zip SDK backend can replace either
  implementation behind the same provider interface if better progress reporting
  or sandboxing is needed.

For ZIP specifically, Axiom currently vendors miniz 3.1.1 because it provides a
small, build-system-friendly ZIP container reader/writer and Deflate/Inflate
implementation. zlib-ng remains a reasonable future Deflate/Inflate backend
candidate if profiling shows that miniz's codec path is the bottleneck, but it
is not a ZIP container layer. ZIP support owns central-directory rewrites in the
provider layer, including the WinZip AES extra-field and payload rewrite used
for AES-256 encrypted ZIP creation. Future work is mainly richer metadata,
comments, editable encrypted ZIPs, and possibly swapping the Deflate backend if
profiling justifies it.

A privately namespaced minizip-ng 4.2.2 container/split-stream core creates and
directly reads standard `.z01`, `.z02`, ..., `.zip` sets. Axiom raw-copies completed
entries, preserving Deflate data, metadata, CRCs, and WinZip AES ciphertext. Its
vendored split writer carries a documented local-header boundary fix verified
against bundled 7-Zip. Split sets are browse/test/extract only after creation;
editing requires recreating the set.

The detailed format support roadmap lives in
[`docs/FORMAT_SUPPORT.md`](docs/FORMAT_SUPPORT.md).

### Archive services

Archive-level services include:

- signatures over exact stored block bytes and canonical directory semantics;
- SFX output by appending an intact AXAR or ZIP archive plus a fixed trailer to the native GUI
  stub;
- POSIX mode/uid/gid metadata through a skippable entry TLV;
- recovery records backed by the portable Reed-Solomon core;
- numbered data volumes and optional `.revNNN` recovery volumes.

Recovery protects the archive through the end of the central directory and can
repair damaged shards atomically. Volume joining validates the reconstructed
archive with BLAKE3 before installing it. Long recovery and volume operations
honor `OperationControl`.

Complete AXAR data-volume sets are exposed through a segmented random-access
source, so list/test/extract operate on the numbered files without creating a
joined archive. The provider marks this logical archive read-only. Missing data
parts retain the existing Reed-Solomon join/reconstruction path.

### GUI drag/drop boundary

The Win32 file list implements `IDataObject`, `IDropSource`, and `IDropTarget`.
Drag-out materializes selected archive entries only when a shell target requests
`CF_HDROP`, which avoids doing extraction work before Explorer actually needs the
files.

## Single-stream container

The archive header stores:

- Magic and version.
- Codec identifier.
- Version-5 transform-present flags and a bounded transform-range section.
- Original size.
- Payload size.
- CRC-32 of the uncompressed data.

The current transform layer supports independently reset x86/x64 relative-branch
conversion and byte-delta ranges. PE, PCM WAV, and uncompressed BMP signatures
provide candidate hints; a fast trial encode enables the filter only when it is
expected to beat the unfiltered representation. AXAR passes one range per file or
file fragment so mixed solid blocks remain reversible, while the final AXC CRC
continues to authenticate the original bytes.

Future block headers should add:

- Dictionary identifier.
- Codec parameters.
- Per-block checksums.
- Optional seek index.

## Codec Direction

The long-term codec should be a hybrid:

- LZ77-family local matching for fast repeated substrings.
- Long-distance references across solid groups.
- Optional trained dictionaries.
- Transform tokens for structured formats.
- Optimal parsing driven by estimated entropy cost.
- rANS or range coding for the practical high-ratio mode.
- Context-mixed literal coding for max-ratio research mode.

## Fast Profile Constraint

Level 1 is intentionally not an LZMA clone. Its hot path is a fixed-probe row
hash parser over independent blocks, plus repeat-offset sequence tokens and
Axiom split streams. It must not depend on the LZMA-style binary-tree matcher,
optimal parser, or probability/range model to hit its speed target. Those remain
higher-effort ratio tools for levels that explicitly trade speed away.

## Parser modes

A single `--level 1..9` knob selects the speed/ratio operating point (default 5).
Levels 1–6 drive the hash-chain matcher, raising chain depth and turning on lazy
matching and the full entropy bake-off as the level rises; level 7 switches to
a cost-aware lazy binary tree, and levels 8–9 use growing tree windows plus the
optimal parser (single-pass at 8, two-pass at 9). Individual flags (`--chain-depth`,
`--nice`, `--lazy`/`--no-lazy`, `--fast-entropy`, `--bt`, `--window`,
`--optimal…`) override the preset, so a level is just a starting point. The
decoder is identical at every level.

### Normal hash-chain parsing

Normal mode is greedy with optional **price-aware lazy matching**:

1. Find a match at position `p`.
2. Peek at `p + 1`.
3. Defer (emit one literal) when the deferred path is cheaper per byte under
   the token cost model — strictly longer matches still defer, and so do
   similar-length matches that land a much nearer distance — or when any
   repeat-offset at `p + 1` reaches the current match's length (reps code no
   distance at all).

This gives shallow chains much of the ratio of deeper chains without making the
fast levels too slow. Match-length comparison reads eight bytes at a time.

### Entropy selection

After parsing, Axiom splits the LZ77 data into separate streams (commands,
literal lengths, match lengths, distances or distance slots + footer bits, and
literals). The entropy stage then either:

- trial-encodes every available coder per stream and keeps the smallest result
  on higher levels — including the clustered order-1 rANS for the literal and
  sequence streams, which carry strong previous-symbol structure — or
- uses a one-pass order-0 estimate on levels 1-3.

All selected coders decode at table-lookup speed; decode time is flat across
levels by design.

### Binary-tree mode

`--bt` swaps the hash chain for an LZMA-style binary-tree match finder over a
cyclic window.

- Tree slots are indexed by `position % min(window, input_size)`.
- A descent stops when a candidate falls outside the configured window.
- Memory stays proportional to `--window`, not to the whole input.
- Set `--window` as large as the block to search the full block.
- Level 7 searches `p + 1` without inserting it and defers the current match
  when the next path is cheaper per byte or exposes an equal-length rep match.

### Optimal parser

`--optimal` (and levels 8–9 by preset) enables bounded dynamic-programming
parsing. It scores literals, useful match lengths, and repeat-offset matches,
then reconstructs the lowest-cost token sequence. On the tree levels the DP's
candidates come from the cyclic binary tree itself: advancing a position both
inserts it and yields each improving (length, distance) pair met during the
descent, so a bounded search surfaces several distinct lengths at their nearest
distances — substantially better parses than hash-chain candidates for the
same work.

Two effort shapes exist:

- **Two-pass** (level 9, `--optimal`): parse with fixed weights, measure the
  output streams, re-parse with measured entropy costs, keep whichever fully
  encoded result is smaller.
- **Single-pass** (level 8): measure the cost model from the greedy parse the
  block encoder already computed, then run the DP once — most of the two-pass
  ratio at roughly half the time.

Use `--optimal-depth` and `--optimal-candidates` only when benchmarking or
deliberately trading runtime for ratio (deeper descents keep helping slightly).

## Benchmarking

The standing corpora are **enwik8** (100 MB of English Wikipedia text, the
de-facto LZMA-class ratio benchmark) and the **Silesia corpus** (~212 MB of
mixed text/binary/medical/database data, benchmarked as a single tar, which is
what zstd and most modern codecs report against). `tools\bench_enwik8.ps1`
downloads enwik8 on first run, sweeps Axiom's match finders and window sizes,
and verifies every row by round-trip before reporting a ratio.

The cross-codec harness (`bench/bench_codecs.py`) compares Axiom levels against
available LZ4, zstd, Deflate, bzip2, and LZMA2 profiles. For folders it builds a
deterministic byte stream of relative paths and file bytes, then feeds that same
stream to every codec. Each reported row is restored and compared byte-for-byte,
keeping multi-file container differences out of codec measurements.

## Decoder Rule

The encoder is allowed to be expensive. The decoder is not.

Any new feature must keep decompression deterministic and bounded:

- No search during decompression.
- No machine-learning inference during decompression.
- Clear maximum memory from the block header.
- Reject malformed distances, sizes, and checksums.

## Threading

### GUI responsiveness and progress telemetry

The Win32 thread owns windows, menus, dialogs, input routing, and presentation
only. Archive identification, provider capability probes, catalog loading,
comments, recovery metadata, signature verification, SFX/split-volume
inspection, and all archive operations run on workers. Results return as owned,
typed messages; closing the main window invalidates the shared lifetime token and
drains any already-queued payloads.

`OperationControl` is also the single source of progress truth. Producers publish
a coherent snapshot containing stage bytes, item counts, current path, and
per-file bytes. Numeric fields use a sequence-guarded atomic snapshot and paths
are replaced atomically only when they change. Reports are coalesced at 1 MiB
unless a stage, item, total, file, or completion boundary changes, so telemetry
cannot become an inner-loop throughput bottleneck.

Progress stays continuous even inside multi-second encodes: the parse loops
(greedy chains, tree matcher, and both optimal-parser passes) tick a fractional
`encode_progress` hook every 256 KiB of scanned input, `compress_block` maps
each pass into a rough share of its block's wall time, the parallel block codec
sums per-worker in-flight fractions into the byte-progress channel, and the
archive writer sums per-solid-block contributions across its concurrent jobs
(never a shared high-water mark, which would collapse concurrent blocks into
block-sized jumps). Measured on a level-9 Silesia archive: the worst gap
between progress advances is ~0.6 s and steps stay under 2 MB, where a whole
solid block previously arrived at once.

GUI progress windows and the interactive CLI poll this snapshot; operation
threads never paint, format status text, query HWNDs, inspect a growing output
file, or enqueue progress messages. The GUI samples at its own cadence, computes
rate and ETA from a rolling phase-local window, and repaints a liveness heartbeat
even when an external backend is between measurable checkpoints. Bundled 7-Zip
operations request and parse its native percentage stream for accurate progress.
Pause and cancellation retain their cooperative `OperationControl` checkpoints;
the unpaused checkpoint path is an atomic fast path.

Archive drag-out has two explicit telemetry phases. The provider first extracts
selected entries into Axiom's private staging directory. After the drop is
accepted, the OLE `CFSTR_FILECONTENTS` streams are wrapped by a read-only
counting `IStream`; this reports bytes actually consumed by the shell, the
current relative path, and completed-file counts while Explorer writes the drop
destination. The wrapper does not rescan or recopy data and publishes at 1 MiB
or file boundaries. Transfer cancellation is checked on every stream read;
pause is intentionally unavailable for this phase because an OLE stream call
may be dispatched on the source STA and blocking it would also block Resume UI.

`thread_count == 0` means "use the machine": compression workers default to the
**physical core count** (hyperthread siblings measured flat-to-negative on the
codec's memory-bound hot loops), while decode uses all logical processors.
Explicit thread counts are honored as given. The codec caps the actual worker
count to the number of useful work items so small inputs do not create idle
threads, while large inputs split enough independent blocks to keep the requested
workers busy.

There are two block-sizing layers:

- Archive solid blocks group file bytes for cross-file compression and selective
  extraction.
- The single-stream codec can split a solid block into independently compressed
  sub-blocks.

By default, the archive layer raises the target solid-block size to at least
`hardware_threads * 1 MiB` when multiple workers are available. The codec layer
then shrinks the internal block size as needed, down to 1 MiB minimum useful
work, so one large solid block can still feed many compression workers. Supplying
an explicit `--block-size` disables this automatic sizing for repeatable tuning
runs.

Parallel blocks are independent by design: each block picks store, raw LZ77,
Huffman-coded LZ77, the level-1 `fast_lz` format, or split-stream LZ77 whose
substreams can use store, Huffman, **order-0 rANS**, or the **clustered static
order-1 rANS** (previous-byte contexts grouped into at most 16 transmitted
tables plus a context map; decodes with the same interleaved table-lookup loop
as order-0). On the speed levels streams go straight to order-0 rANS; on the
ratio levels every coder competes per substream and the smallest wins. The
legacy adaptive order-1 range coder remains decodable but is never emitted.

The container-level block payload records enough sizes for deterministic
reconstruction. Parallel-block encode and decode also compute per-block CRCs on
the worker threads and combine them, avoiding a serial full-buffer CRC pass after
the selected payload is already available. This keeps the format identical while
removing a CPU-scaling bottleneck.

This sacrifices cross-block matches when the parallel codec is selected, so the
archive selector still keeps the smallest single-stream result for cases where
ratio wins over block-level parallelism. Fast non-thorough levels prefer the
parallel result for large multi-block inputs because the serial whole-input parse
is otherwise the dominant bottleneck.
