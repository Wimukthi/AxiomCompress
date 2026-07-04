# Architecture

The project is structured so the encoder can become much more complex without making the decoder expensive or unpredictable.

## Quick map

Use this document when you need to know where a feature belongs:

| Area | Owns |
|---|---|
| `src/codec` | Single-block compression and decompression |
| `src/archive` | `.axar` container, metadata, encryption, recovery, volumes, signing, SFX |
| `src/core` | Shared utilities: checksums, crypto, filesystem metadata, Reed-Solomon |
| `src/cli` | `axiomc` command parsing and CLI workflows |
| `src/gui` | Native Win32 GUI over the public archive APIs |
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
  (`fast_lz`, level 1), a **lazy hash-chain matcher** (levels 2–6), and an
  LZMA-style **cyclic-window binary-tree matcher** (`--bt`, levels 7–9), plus an
  optional bounded **optimal (DP) parser**.
- Repeat-offset (rep0–rep3) matches and split LZ77 streams, with an optional
  position-slot distance representation.
- Entropy via byte-level canonical Huffman, a **4-lane interleaved order-0 rANS**,
  and a Fenwick-backed adaptive **order-1** range coder. (An earlier bit-serial
  order-0 *arithmetic* coder was removed once rANS superseded it at the same size
  and far higher decode speed.)
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
  preserved by cloning them into an atomically rewritten ZIP.

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
- plain TAR is the next realistic full-support provider because it can support
  create/extract/update/delete/move through atomic rewrites without codec or
  licensing complications;
- compressed TAR variants should start with browse/extract/test/create, then add
  update/delete/move only after the full-stream rewrite UX is clear;
- 7z/RAR/ISO/CAB-style providers should start as view/extract/test providers
  unless their container semantics and licensing justify more.

For ZIP specifically, Axiom currently vendors miniz 3.1.1 because it provides a
small, build-system-friendly ZIP container reader/writer and Deflate/Inflate
implementation. zlib-ng remains a reasonable future Deflate/Inflate backend
candidate if profiling shows that miniz's codec path is the bottleneck, but it
is not a ZIP container layer. ZIP support owns central-directory rewrites in the
provider layer, including the WinZip AES extra-field and payload rewrite used
for AES-256 encrypted ZIP creation. Future work is mainly richer metadata,
comments, editable encrypted ZIPs, and possibly swapping the Deflate backend if
profiling justifies it.

The detailed format support roadmap lives in
[`docs/FORMAT_SUPPORT.md`](docs/FORMAT_SUPPORT.md).

### Archive services

Archive-level services include:

- signatures over exact stored block bytes and canonical directory semantics;
- SFX output by appending an intact archive plus a fixed trailer to the native GUI
  stub;
- POSIX mode/uid/gid metadata through a skippable entry TLV;
- recovery records backed by the portable Reed-Solomon core;
- numbered data volumes and optional `.revNNN` recovery volumes.

Recovery protects the archive through the end of the central directory and can
repair damaged shards atomically. Volume joining validates the reconstructed
archive with BLAKE3 before installing it. Long recovery and volume operations
honor `OperationControl`.

### GUI drag/drop boundary

The Win32 file list implements `IDataObject`, `IDropSource`, and `IDropTarget`.
Drag-out materializes selected archive entries only when a shell target requests
`CF_HDROP`, which avoids doing extraction work before Explorer actually needs the
files.

## Single-stream container

The archive header stores:

- Magic and version.
- Codec identifier.
- Original size.
- Payload size.
- CRC-32 of the uncompressed data.

Future block headers should add:

- Transform chain.
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
matching and the full entropy bake-off as the level rises; levels 7–9 switch to
the binary-tree matcher with growing windows. Level 9 keeps the deepest default
tree search but uses bounded solid blocks so mixed random/text archives do not
spend maximum effort on data that will be stored anyway.
Individual flags (`--chain-depth`, `--nice`, `--lazy`/`--no-lazy`,
`--fast-entropy`, `--bt`, `--window`, `--optimal…`) override the preset, so a level
is just a starting point. The decoder is identical at every level.

### Normal hash-chain parsing

Normal mode is greedy with optional **lazy matching**:

1. Find a match at position `p`.
2. Peek at `p + 1`.
3. If `p + 1` has a strictly longer match, emit one literal and take that better
   match next.

This gives shallow chains much of the ratio of deeper chains without making the
fast levels too slow. Match-length comparison reads eight bytes at a time.

### Entropy selection

After parsing, Axiom splits the LZ77 data into separate streams. The entropy stage
then either:

- trial-encodes every available coder and keeps the smallest result on higher
  levels, or
- uses a one-pass order-0 estimate on levels 1-3.

Fast levels code literals with rANS instead of the bit-serial order-1 coder.
Order-1 can improve text ratio, but it decodes much more slowly and would dominate
decode time.

### Binary-tree mode

`--bt` swaps the hash chain for an LZMA-style binary-tree match finder over a
cyclic window.

- Tree slots are indexed by `position % min(window, input_size)`.
- A descent stops when a candidate falls outside the configured window.
- Memory stays proportional to `--window`, not to the whole input.
- Set `--window` as large as the block to search the full block.

### Optimal parser

`--optimal` enables bounded dynamic-programming parsing over the same match
finder. It scores literals, useful match lengths, and repeat-offset matches, then
reconstructs the lowest-cost token sequence.

The parser runs twice:

1. First pass: use fixed weights.
2. Measure the output streams.
3. Second pass: use measured entropy costs.
4. Keep whichever fully encoded result is smaller.

The default optimal effort uses a shallower chain than greedy parsing so high
levels stay practical. Use `--optimal-depth` and `--optimal-candidates` only when
benchmarking or deliberately trading runtime for ratio.

## Benchmarking

The standing corpus is **enwik8** (100 MB of English Wikipedia text), the de-facto
LZMA-class ratio benchmark. `tools\bench_enwik8.ps1` downloads it on first run,
sweeps the match finders and window sizes, prints a 7-Zip reference, and verifies
every row by round-trip before reporting a ratio.

For ad-hoc inputs, the Python harness (`bench/bench_7zip.py`) can compare files
directly; for folders it builds a deterministic byte stream of relative paths and
file bytes and feeds that same stream to Axiom and 7-Zip, which keeps multi-file
measurements fair until the container exposes native solid blocks to the harness.

## Decoder Rule

The encoder is allowed to be expensive. The decoder is not.

Any new feature must keep decompression deterministic and bounded:

- No search during decompression.
- No machine-learning inference during decompression.
- Clear maximum memory from the block header.
- Reject malformed distances, sizes, and checksums.

## Threading

`thread_count == 0` means all hardware threads. The codec caps the actual worker
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
substreams can use store, Huffman, **order-0 rANS**, or the adaptive **order-1**
range coder. The order-1 coder transmits no table; both endpoints evolve identical
per-context (previous-byte) models. On the speed levels it is skipped in favor of
rANS even on literals (it decodes bit-serially and would dominate decode time); on
the ratio levels it is selected per substream only when it is the smallest.

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
