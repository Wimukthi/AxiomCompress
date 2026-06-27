# Architecture

The project is structured so the encoder can become much more complex without making the decoder expensive or unpredictable.

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
  blocks with a central directory (see [FORMAT.md](FORMAT.md)).
- Two front-ends — a CLI (`axiomc`) and a Windows GUI (`Axiom.exe`) — over the
  same library. Long operations report progress and honor cooperative
  pause/cancel through an `OperationControl` passed in the option structs;
  cancellation throws `OperationCancelled` and leaves no partial output (writes
  are atomic).

The container embeds one `axiom::compress` `.axc` stream per solid block, so the
single-stream codec and the archive share exactly the same encode/decode path.
The decoder is deliberately simple and bounded (see "Decoder Rule"); all the
complexity lives in the encoder.

The `.axar` public API deliberately separates archive storage from file-manager
presentation. Besides create/list/test/extract, it provides destination-aware
insertion (`ArchiveInput`), selective extraction, metadata-only entry moves
(`ArchiveMove`), add/update/fresh/sync/delete/repack, comments, locking, and an
`ArchiveEncryptionMode` query. All mutating operations use a temporary archive and
replacement rename, honor `OperationControl`, and reject locked archives. Data-only
encrypted archives are editable with the password; encrypted-directory archives
currently remain read-only.

Phase-5 services are archive-level APIs: signatures cover exact stored block bytes
and canonical directory semantics, SFX appends an intact archive plus a fixed trailer
to the native GUI stub, and POSIX mode/uid/gid uses a skippable entry TLV. The Win32
file list implements `IDataObject`, `IDropSource`, and `IDropTarget`; drag-out only
materializes selected entries when a shell target requests `CF_HDROP`.

Phase-4 services use the tested portable Reed–Solomon core. An optional
self-locating recovery service protects the archive through the end of its central
directory and can atomically repair damaged shards. Multi-volume orchestration
wraps the exact completed archive bytes in checked `partNNN.axar` data shards and
optional `.revNNN` parity shards; joining validates the complete archive with
BLAKE3 before installing it. Both long operations honor `OperationControl`.

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

## Parser Modes

A single `--level 1..9` knob selects the speed/ratio operating point (default 5).
Levels 1–6 drive the hash-chain matcher, raising chain depth and turning on lazy
matching and the full entropy bake-off as the level rises; levels 7–9 switch to
the binary-tree matcher with growing windows, and level 9 adds the optimal parser.
Individual flags (`--chain-depth`, `--nice`, `--lazy`/`--no-lazy`,
`--fast-entropy`, `--bt`, `--window`, `--optimal…`) override the preset, so a level
is just a starting point. The decoder is identical at every level.

Normal mode uses greedy hash-chain parsing with optional **lazy matching** (before
committing a match at `p`, peek at `p+1`; if a strictly longer match starts there,
emit a literal so the better match is taken next — this lets a shallow chain reach
close to a deep chain's ratio), then lets split streams and entropy coding recover
as much ratio as possible cheaply. Match-length comparison reads eight bytes at a
time. The split-stream entropy stage either trial-encodes every coder and keeps the
smallest (high levels) or, in fast mode (levels 1–3), picks a coder from a one-pass
order-0 entropy estimate and codes literals with **rANS rather than the bit-serial
order-1 coder** — order-1 wins a little ratio on text but decodes ~15× slower and
dominates decode time, so the speed levels take the far faster rANS literal stream.
`--bt` swaps the hash chain for
an LZMA-style binary-tree (suffix-BST) match finder over a *cyclic window*: the
tree is indexed by `position % min(window, n)`, and a descent stops as soon as a
candidate falls outside the window. That window bound is the node-deletion
mechanism — it keeps the tree's footprint proportional to `--window` rather than
the input, and keeps the descent from chasing evicted positions. Set `--window`
as large as the block to recover full-window matches. `--optimal` enables bounded
dynamic-programming parsing over the same match finder. That parser scores
literals, a limited set of useful match lengths, and repeat-offset matches, then
reconstructs the lowest-cost path into the LZ77 token format. The recent-distance
list is path dependent, so each position carries the rep state of the lowest-cost
path that reaches it; because a forward position's cost is final once the loop
reaches it, that rep state is settled deterministically from the recorded
decision and the emitted token sequence decodes to exactly the same state.

The parser runs twice. The first pass uses fixed weights; its output is measured
(order-0 entropy of the literal, command, length, and distance-slot streams, plus
the exact slot footer bits for each distance) to build a cost model that reflects
the real entropy-coded size, and the second pass re-parses with those costs. The
encoder keeps whichever pass is smaller after entropy coding. The default optimal effort
uses a shallower chain than greedy parsing so ultra mode stays practical; use
`--optimal-depth` and `--optimal-candidates` to trade runtime for ratio during
benchmark runs.

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

Parallel blocks are independent by design: each block picks store, raw LZ77,
Huffman-coded LZ77, the level-1 `fast_lz` format, or split-stream LZ77 whose
substreams can use store, Huffman, **order-0 rANS**, or the adaptive **order-1**
range coder. The order-1 coder transmits no table; both endpoints evolve identical
per-context (previous-byte) models. On the speed levels it is skipped in favor of
rANS even on literals (it decodes bit-serially and would dominate decode time); on
the ratio levels it is selected per substream only when it is the smallest. The
container-level block payload records enough sizes for deterministic
reconstruction. This sacrifices cross-block matches when the parallel codec is
selected, so the archive selector still keeps the smallest single-stream result
for cases where ratio wins over block-level parallelism.
