# Beating LZMA2 on Silesia: measured gap analysis and plan

Status: Phase 0 complete; items 1-3, 6, and 7 landed, item 4 rejected, and item
5 measured as neutral on this corpus, 2026-07-16.
This document records the research and each measured implementation step so
work can continue on any machine. Raw data lives in
[`bench/results/gap-2026-07-16/`](../bench/results/gap-2026-07-16/); the tools
that produced it are [`bench/gap_analysis.py`](../bench/gap_analysis.py) and
[`bench/axc_inspect.py`](../bench/axc_inspect.py).

## Goal and targets

Beat LZMA2 on silesia.tar compression ratio while keeping roughly 2x its
compression throughput and the existing ~5x decompression advantage.

Tar-level sizes (bytes, exact published 211,948,032-byte input):

| configuration | bytes | needed to beat |
|---|---:|---:|
| axiom-9 default (this run) | 52,459,618 | — |
| axiom-9 best block size (32M) | 52,371,652 | — |
| 7z LZMA2 mx5 | 49,608,674 | −2.85 MB (−5.4%) |
| 7z LZMA2 mx9 | 48,685,411 | −3.77 MB (−7.2%) |

## Measurement environment (important caveat)

Phase 0 ran on a **10-physical-core, 16 GB laptop**, not the published
benchmark workstation (Ryzen 9 5950X, 16 cores). Consequences:

- Sizes are trustworthy; **timings are not comparable** to the published CSV.
- Default auto block size here was ~21.2 MB (input/10) vs 13.2 MB published,
  which is why the default tar encode measured 52,459,618 vs the published
  52,541,522.
- Level 9 holds **~3.5 GB of matcher/parser state per in-flight 64 MB block**
  (~55 bytes/byte). `--block-size 64M --threads 0` OOM-killed the encoder on
  16 GB; `gap_analysis.py` now caps threads on explicit-block runs (the ratio
  depends only on block boundaries, never on worker count). Re-baseline
  timings and the `--threads 0` sweep on the workstation.

### Workstation re-baseline

The continuation run used the published Ryzen 9 5950X workstation (16 physical
cores / 32 logical processors, 64 GB RAM). The canonical twelve members total
211,938,580 bytes. Rebuilding the tar produced exactly 211,948,032 bytes with
SHA-256 `CE7C944571B354A181EE0111B58A82B06C4A3CCF11D415D92C710CC4947688C4`.
Because the machine has more than 32 GB, the 64 MiB row used `--threads 0`.

| configuration | Phase-0 laptop | workstation baseline | workstation time |
|---|---:|---:|---:|
| axiom-9 default | 52,459,618 | 52,541,485 | 25.570 s |
| axiom-9 64M | 52,459,308 | 52,467,254 | 82.779 s |
| axiom-9 32M | 52,371,652 | **52,371,674** | 73.671 s |
| axiom-9 16M | 52,485,610 | 52,485,632 | 45.961 s |

The explicit-block results match the recorded run within 22-42 bytes; the
small default difference is expected from automatic block geometry using 16
instead of 10 physical cores. Per-member sizes match the Phase-0 table exactly.
Raw workstation results are in `workstation-baseline-members.csv` and
`workstation-baseline-tar.csv` in the results directory above.

## Implementation progress

### Item 1 landed: slot-context distance footer coding

AXC v8 adds two block candidates: contextual distance slots with flat literals,
and the same footer representation with context-split literals. For each
non-direct distance, footer widths up to four bits are coded completely; longer
footers retain their high bits packed and code the low alignment nibble. A
static four-lane rANS model is selected by the distance slot. Only used slot
tables are transmitted, and decode is a bounded table lookup driven by the
already-decoded slot stream. The complete v8 payload competes against the v7
raw-footer payload and is accepted only when strictly smaller.

| configuration | workstation baseline | item 1 | saved | item-1 time |
|---|---:|---:|---:|---:|
| axiom-9 default | 52,541,485 | **52,178,206** | 363,279 | 19.987 s |
| axiom-9 64M | 52,467,254 | **52,226,668** | 240,586 | 76.835 s |
| axiom-9 32M | 52,371,674 | **52,107,109** | 264,565 | 70.652 s |
| axiom-9 16M | 52,485,632 | **52,164,405** | 321,227 | 41.689 s |

The default archive selected contextual footers in 9 of 16 blocks and retained
the raw representation in 7. Across the selected blocks, 4,937,240 alignment
symbols compressed to 1,970,020 bytes (**3.192 bits/symbol**); packed high bits
remain 5,685,250 bytes. The single-member total improved by 699,746 bytes, led
by mozilla (-305,018), x-ray (-133,078), sao (-125,370), and mr (-104,502).

Verification: the full Release test/safety/fuzz suite passes; the tar and all
twelve members round-trip; the current decoder reads a retained v7 archive;
and an alternating same-binary decode test measured 298.2 MB/s for v7 versus
290.6 MB/s for v8 (-2.61%, including file output). Sharing the encoded slot
stream between both bake-off candidates left archive bytes identical and cut a
subsequent default encode spot-check to 17.807 s, **2.26x faster** than the
recorded 40.27 s LZMA2 mx9 run. Raw post-item results are in
`item1-distance-footer-members.csv` and `item1-distance-footer-tar.csv`.

Remaining gap from the best 32 MiB row: 2,498,435 bytes to mx5 and 3,421,698
bytes to mx9.

### Item 2a landed: clustered order-1 competes per literal lane

The context-split literal lanes no longer force the fast order-0 rANS path.
Each lane runs the existing full stream bake-off, including clustered order-1,
and retains its previous representation unless the complete encoded lane is
strictly smaller. On the default tar only 19 of 128 lane instances select
order-1; the other lanes keep Huffman, rANS, or stored representations.

| configuration | item 1 | item 2a | saved | item-2a time |
|---|---:|---:|---:|---:|
| axiom-9 default | 52,178,206 | **52,112,522** | 65,684 | 22.575 s |
| axiom-9 64M | 52,226,668 | **52,040,068** | 186,600 | 75.492 s |
| axiom-9 32M | 52,107,109 | **51,993,512** | 113,597 | 70.142 s |
| axiom-9 16M | 52,164,405 | **52,092,526** | 71,879 | 43.943 s |

The full Release suite passes and both the default and 32 MiB archives
round-trip. The best row is now 2,384,838 bytes above mx5 and 3,308,101 bytes
above mx9. The default encode remains about 1.78x the recorded 40.27 s mx9
throughput in this full sweep; a less noisy standalone encode measured 18.284 s
(2.20x). Raw results are in `item2-lane-order1-members.csv` and
`item2-lane-order1-tar.csv`.

### Item 2b landed: static match-byte literal context

AXC v8 literal mode 2 separates the first literal after a match from ordinary
literals. Its symbol is XORed with the byte at rep0 and assigned to one of eight
static lanes by the high bits of that matched byte; all other literals retain
the previous-byte lanes. This gives the matched-literal distribution its own
tables without any decoder adaptation or search. The complete sixteen-lane
suffix competes against the previous eight-lane raw/rep0-XOR winner and is
emitted only when strictly smaller.

| configuration | item 2a | item 2b | saved | item-2b sweep time |
|---|---:|---:|---:|---:|
| axiom-9 default | 52,112,522 | **52,040,815** | 71,707 | 21.719 s |
| axiom-9 64M | 52,040,068 | **51,996,126** | 43,942 | 79.536 s |
| axiom-9 32M | 51,993,512 | **51,932,052** | 61,460 | 65.366 s |
| axiom-9 16M | 52,092,526 | **52,020,106** | 72,420 | 43.309 s |

Eight of sixteen default blocks select match-byte mode; five keep raw literals
and three keep rep0-XOR. A conservative order-0 prefilter skips only candidates
whose estimated match-byte suffix already loses to the prior mode. It preserves
the default and 32 MiB archives byte-for-byte while reducing standalone default
encode time to **18.792 s (2.14x LZMA2 mx9)**. Alternating decode tests measured
the same 0.3459 s median for item 2a and item 2b, including file output. Current
decode also round-trips the retained v7 baseline archive. Raw full-sweep results
are in `item2-match-byte-members.csv` and `item2-match-byte-tar.csv`.

Item 2 as a whole saves 175,057 bytes on the best 32 MiB row. The remaining gap
is 2,323,378 bytes to mx5 and 3,246,641 bytes to mx9.

### Item 3 landed: tar-aware adaptive block geometry

Level 9 automatic geometry now validates POSIX ustar member boundaries and uses
them as static block/table-reset points. Large members are split into chunks at
75% of the normal thread-derived block budget; small adjacent members are
coalesced to at least 4 MiB. This keeps at least as much parallel work as the
old uniform plan while preventing entropy tables and match windows from
straddling unrelated content. Explicit `--block-size` remains uniform and
unchanged. The existing block table already stores every original block length,
so the decoder format and hot path require no change.

| configuration | item 2 | item 3 | saved | item-3 sweep time |
|---|---:|---:|---:|---:|
| axiom-9 automatic | 52,040,815 | **51,511,761** | 529,054 | 17.407 s |
| axiom-9 64M (explicit) | 51,996,126 | 51,996,126 | 0 | 62.412 s |
| axiom-9 32M (explicit) | 51,932,052 | 51,932,052 | 0 | 54.667 s |
| axiom-9 16M (explicit) | 52,020,106 | 52,020,106 | 0 | 32.713 s |

The adaptive tar uses 29 independently decodable blocks and improves 420,291
bytes over the previous best explicit row. It round-trips to the canonical tar
hash, the full Release test/safety/fuzz suite passes, and the measured automatic
encode is **2.31x faster** than the recorded 40.27 s LZMA2 mx9 run. Alternating
same-binary decode measurements improved from 0.2631 s to 0.2477 s median
(including file output) because the smaller blocks expose more parallel decode
work. Raw results are in `item3-adaptive-blocks-members.csv` and
`item3-adaptive-blocks-tar.csv`.

The remaining gap is 1,903,087 bytes to mx5 and 2,826,350 bytes to mx9.

### Item 4 measured and rejected: record/stride transform

The detector correctly identified sao's 28-byte record stride. Two static,
reversible candidates were measured: struct-of-arrays shuffle plus per-column
delta, and shuffle alone. Neither survives the required complete-archive
bake-off:

| candidate | silesia.tar bytes | change vs item 3 | time |
|---|---:|---:|---:|
| item 3 (no record transform) | **51,511,761** | - | 17.407 s |
| stride-28 shuffle + delta | 51,867,289 | +355,528 | 23.269 s |
| stride-28 shuffle only | 51,583,644 | +71,883 | 23.554 s |

The experiment also exposed that the legacy transform trial sums all selected
ranges: a losing sao transform could hitchhike on the large independent word16
wins for mr/x-ray. A single-member sao tar rejected both candidates, but the
combined tar accepted them. Since neither representation wins the complete
archive, the record transform and format identifier were removed; no decoder
surface or archive-format change ships from this item.

### Item 5 corrected: ELF recognition landed, Silesia result is neutral

The premise in the original plan was wrong for the canonical corpus file:
`D:\Silesia\mozilla` contains no ELF magic. Its 134 native-image members use
little-endian `0x0183`, the Alpha ECOFF machine magic, so applying x86 branch
conversion to them would be incorrect. The encoder now recognizes 32/64-bit
x86 ELF headers and can descend through exactly one validated
nested tar to find them. This reuses the existing x86 representation and its
trial gate; it adds no decoder format or hot-path work.

The canonical tar is byte-identical to item 3 at **51,511,761 bytes** because it
contains no nested x86 ELF payload. An exploratory Alpha fixed-width branch
filter was rejected: direct mozilla correctly rejected it and stayed at
15,819,070 bytes, while the legacy aggregate transform gate let the losing
candidate hitchhike on mr/x-ray and inflated the full tar to 53,909,939 bytes
(+2,398,178). The Alpha identifier and decoder code were removed.

### Item 6 landed: word16 detection for raw inputs

Non-tar AXC inputs now run the same conservative word16 entropy screen that was
previously reachable only through tar members. The existing transform trial is
still authoritative, so ordinary raw files remain unchanged when the predictor
does not win.

| raw member | item 3 | item 6 | saved | 7z mx9 | lead over mx9 |
|---|---:|---:|---:|---:|---:|
| mr | 3,016,417 | **2,225,159** | 791,258 | 2,748,257 | 523,098 (19.0%) |
| x-ray | 5,119,568 | **3,774,530** | 1,345,038 | 4,479,871 | 705,341 (15.7%) |

Both archives round-trip byte-for-byte and carry one bounded word16 range. The
canonical tar remains exactly **51,511,761 bytes**, as its member-scoped word16
ranges were already present. AXAR now explicitly disables core block-wide
auto-detection when its per-file range list is empty; this prevents the newly
reachable raw detector from crossing unrelated solid-block file boundaries and
keeps compression-estimator behavior consistent.

The full gap sweep confirms that only those two auto-thread member rows change:
their summed Axiom size falls by 2,136,296 bytes. In the single-thread modeling
row, the two gains total 2,406,790 bytes and the twelve-member gap to summed mx9
falls to 1,863,702 bytes. Raw results are in `item6-word16-gap-members.csv`,
`item6-word16-gap-tar.csv`, and `item6-word16-raw-members.csv`.

### Item 7 landed: clustered full-previous-byte literal contexts

AXC v8 literal mode 3 replaces the eight high-bit lanes with a 256-entry map
from the actual previous output byte to at most 16 encoder-chosen clusters. Each
cluster has one static entropy-coded literal stream. Decode performs one map
lookup and one bounded stream read per literal; no model adapts and no search is
introduced. A conditional-entropy screen avoids building clearly losing
candidates, and the complete map, tables, streams, and framing must strictly
beat the existing raw, rep0-XOR, and match-byte suffixes.

| configuration | item 3 | item 7 | saved | item-7 time |
|---|---:|---:|---:|---:|
| axiom-9 automatic | 51,511,761 | **51,386,561** | 125,200 | 17.148 s |
| axiom-9 64M | 51,996,126 | 51,996,126 | 0 | 68.158 s |
| axiom-9 32M | 51,932,052 | **51,929,793** | 2,259 | 64.019 s |
| axiom-9 16M | 52,020,106 | **51,972,846** | 47,260 | 41.292 s |

The automatic archive selects the mode in 16 of 29 blocks and pays only 4,112
bytes for all context maps and cluster-count framing. The twelve independently
compressed members save 133,337 bytes in total, led by webster (-79,549),
dickens (-15,222), and ooffice (-14,663). This is not a Silesia-only heuristic:
a 270,883,328-byte Arduino/toolchain tar selected the mode in 21 of 50 blocks
and saved 80,919 bytes, an Axiom source tar saved 419 bytes, and an Autoruns
binary tar rejected it exactly and stayed byte-identical.

Controlled encode A/B measurements were +0.40% on Silesia and +0.44% on the
Arduino holdout. A paired in-memory decode benchmark alternated both archive
forms in one process: the new form was 0.64% faster on Silesia over 101 pairs
and within 0.08% on Arduino over 61 pairs. The Release test/safety/fuzz suite
passes, a deterministic first-order-source test forces mode 3 and verifies its
round trip, and corrupt cluster counts/maps are rejected. Raw results are in
`item7-full-previous-members.csv`, `item7-full-previous-tar.csv`, and
`item7-full-previous-holdouts.csv`.

The remaining tar gap is 1,777,887 bytes to mx5 and 2,701,150 bytes to mx9.

### Swarm parse prototype validated: full-window ratio without serial encode

An encoder-only experiment (behind `--swarm`, hash-chain levels, presets never
enable it) tests decoupling parallelism from block geometry. One logical block
is parsed as concurrent segments against the full block window:

- **Phase 1** builds immutable per-segment hash-chain indexes in parallel.
  Insertion depends only on input bytes, and the stored links equal exactly
  what sequential insertion would produce.
- **Phase 2** parses every segment concurrently. A candidate walk visits the
  segment's own incrementally revealed chain, then earlier segments' completed
  chains — the same candidates, in the same newest-to-oldest order, under the
  same depth budget as the serial matcher. Matches stop at segment boundaries
  and the recent-distance list restarts per segment, so only explicit
  distances are emitted.
- **Phase 3** serially rewrites distance==rep matches into rep tokens under
  the true decoder-visible state and merges adjacent literal runs.

The payload format and decoder are untouched, and output is deterministic for
a given input regardless of worker timing. Controlled level-6 measurement on
the two members that measurably want a full window (10-core machine,
best-of-two, swarm output round-trip verified):

| member | serial 1-thread (full window) | parallel default (split) | swarm (full window, all cores) |
|---|---:|---:|---:|
| webster | 9,796,783 in 71.5 s | 10,392,825 in 5.2 s | **9,796,807 in 20.7 s** |
| nci | 1,965,352 in 9.9 s | 2,237,680 in 0.8 s | **1,965,390 in 3.6 s** |

The seam cost is **24 and 38 bytes** — effectively zero — while recovering the
full window ratio (−5.7% and −12.2% versus split blocks) at 2.7–3.5x the
serial speed. The full Release test/safety/fuzz suite passes with the flag
compiled in; default behavior is byte-identical since presets never set it.

Two follow-up changes landed after that first measurement. `TaskExecutor`
gained a `current()` accessor and the swarm joins the ambient executor when
one exists, so block-path workers fan segment tasks into the shared budget
instead of nesting pools. And the serial whole-input analysis now scopes one
executor over its candidates (plain `compress()` calls previously ran the
entire entropy bake-off on one core; explicit `--threads 1` and dual-candidate
runs keep their old behavior). The full suite passes after both.

Tar-level level-6 comparison on the same 10-core machine (single runs,
swarm outputs round-trip verified):

| configuration | bytes | seconds |
|---|---:|---:|
| default auto split (~21 MiB blocks) | 56,240,198 | 8.3 |
| 64 MiB blocks (4 busy workers) | 55,966,155 | 94.6 |
| **64 MiB blocks + swarm inside** | **55,966,074** | **47.4** |
| one solid 256 MiB-window block + swarm | 57,433,908 | 80.8 |
| one solid block, serial (ceiling) | 57,433,728 | 241.4 |

Three conclusions:

1. **At a fixed block geometry the swarm strictly dominates.** The 64 MiB
   configuration keeps its exact archive (81 bytes smaller, in fact) while the
   segment tasks raise worker occupancy from 4 to 10 — 2.0x faster. This is
   the concrete form of "swarm beats split blocks": same ratio point, idle
   workers converted to speed.
2. **The serial-equivalence holds at scale.** One solid block parsed by the
   swarm lands within 180 bytes of the serial ceiling (0.0003%) at 3.0x its
   speed, consistent with the 24–38-byte member results.
3. **A uniform giant window is confirmed anti-productive — for the third
   independent time.** The solid 256 MiB-window run is 1.47 MB *worse* than
   4x64 MiB blocks: one entropy-table set straddling mozilla, webster, and
   x-ray loses far more than the extra window reach gains (cross-member
   matching was already measured near-worthless). The shipping shape is
   therefore **content-adaptive blocks (item 3) with the swarm inside each**
   — window and tables sized to content, cores kept busy regardless of block
   count, and ratio independent of the machine's core count.

The opt-in has now been extended to levels 1, 8, and 9. Level 1 bypasses its
byte-token fast path when swarm is explicitly requested and uses the cooperative
hash parser. Levels 8-9 use a per-segment binary tree locally plus immutable
prior-segment chain heads for full-window reach in the greedy candidate. Their
global optimal DP remains unchanged: an attempted segmented DP was rejected
because it was both larger and slower (level 8: 53,401,600 bytes in 40.8 s,
versus 52,495,767 bytes in 32.3 s for split).

Controlled Silesia tar results on the same 10-core machine (211,948,032-byte
input, explicit preset block/window sizes, single runs except for the final
level-9 two-run validation, all swarm outputs SHA-256 round-trip verified):

| level | split | swarm | interpretation |
|---|---:|---:|---|
| 1 | 66,248,720 bytes / 1.15 s | **62,308,127 bytes** / 3.72 s | 5.95% smaller; ratio override, not a speed win |
| 8 | 52,495,767 bytes / 32.27 s | **52,490,464 bytes** / **27.86 s** | 5,303 bytes smaller; 13.7% faster in this run |
| 9 | 51,996,245 bytes / 68.88 s | **51,991,188 bytes** / **46.26 s** | 5,057 bytes smaller; 32.8% faster (slower of two final runs) |

Raw rows, including the rejected segmented-DP trials, are in
`swarm-extended-levels.csv`.

The high-level result is intentionally conservative: swarm adds a parallel
greedy representation, while the existing block bake-off and the unsegmented
optimal candidate protect ratio. Supplemental prior-segment chain walks are
bounded by the preset's optimal search depth (16 at level 8, 32 at level 9),
rather than repeating the local tree's much larger 128/512 depth. That bound
turned the initial 4-7% slowdown into the measured 14% throughput gain above.
Level 9 then separates exact global-tree discovery from the path-dependent DP:
the tree publishes fixed 256 KiB candidate tiles one task ahead, with only two
tiles live, and the consumer cooperatively executes its next tile if other block
workers occupy the executor. The final two runs were 44.29 and 46.26 s, another
21-25% faster than the 58.79 s pre-pipeline swarm and byte-identical to it. The
shallower level-8 tree retains the direct DP because three trials showed the tile
framing cost about one second there. Levels 8-9 remain opt-in until
broader-corpus measurements confirm the result.
The next ratio-oriented swarm work is to re-tune the adaptive-geometry split
threshold; geometry no longer needs to over-split large members merely to feed
workers.

It is exposed as an opt-in now. The CLI takes `--swarm` (documented in
`CLI_GUIDE.md`), and the GUI Add-to-archive and Settings > Compression pages
have a "Threading model" selector (Split blocks / Swarm) with an inline
level-specific tradeoff note. It is enabled at levels 1–6 and 8–9; level 7 is
disabled because its lazy tree parse is path-dependent. The
choice persists via the `ThreadModel` setting and flows through
`compression_options()` to `swarm_parse`. Presets still never set it, so
default output is unchanged.

Corpus layout used: the twelve Silesia members in `D:\Silesia`, and the tar
built with `tar --force-local --format=ustar -b 1 -cf silesia.tar dickens
mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml` (from the
member directory, alphabetical order, blocking factor 1 → exactly
211,948,032 bytes, matching the published input).

## Finding 1: the gap is binary-data modeling, not text

Per-member, axiom-9 single block (`--threads 1`, full-member window, optimal
parse) vs 7z LZMA2: see `silesia-gap-members.csv`. Deltas vs mx9:

| member | axiom-9 (t1) | 7z mx9 | delta | % |
|---|---:|---:|---:|---:|
| mozilla | 15,710,839 | 13,344,686 | **+2,366,153** | +17.7% |
| x-ray | 5,322,310 | 4,479,871 | +842,439 | +18.8% |
| sao | 5,018,349 | 4,413,926 | +604,423 | +13.7% |
| mr | 3,120,919 | 2,748,257 | +372,662 | +13.6% |
| samba | 4,088,518 | 3,759,770 | +328,748 | +8.7% |
| webster | 8,580,650 | 8,388,839 | +191,811 | +2.3% |
| osdb | 2,980,775 | 2,851,796 | +128,979 | +4.5% |
| dickens | 2,903,688 | 2,831,111 | +72,577 | +2.6% |
| reymont | 1,387,313 | 1,318,394 | +68,919 | +5.2% |
| xml | 478,443 | 455,003 | +23,440 | +5.2% |
| nci | 1,544,070 | 1,741,410 | **−197,340** | −11.3% |
| ooffice | 2,383,869 | 2,425,568 | **−41,699** | −1.7% |

All four text members combined are ~360 KB of gap; the binary/numeric members
carry ~4.6 MB. Axiom already beats mx9 outright on nci and ooffice.

## Finding 2: mozilla is a pure modeling loss — no filter on either side

- `7z -m0=LZMA2 -mx=9 -mf=off` produces **byte-identical** output to the
  default on mozilla (13,344,686). LZMA2's 2.37 MB advantage there owes
  nothing to BCJ; it is adaptive-context modeling of executable bytes.
- Axiom's x86 transform **cannot fire on mozilla**: despite executable-looking
  member names, the native images are Alpha ECOFF (`0x0183`), not Linux ELF or
  x86 PE. Corpus inspection found zero ELF headers. One-level nested-tar x86
  ELF discovery is now implemented, but correctly finds no candidate here.

## Finding 3: word16 already beats LZMA2 where it fires — it just rarely fires

Wrapping the medical members in a single-member tar (so the tar-member entropy
screen runs) and re-encoding:

| member | raw file | as tar member | change | vs 7z mx9 |
|---|---:|---:|---:|---:|
| mr | 3,120,919 | **2,226,730** | −28.6% | **beats by 19.0%** |
| x-ray | 5,302,338 | **3,790,586** | −28.5% | **beats by 15.4%** |
| sao | 5,018,349 | 5,018,455 | none | still +13.7% |

At Phase 0 the word16 predictor was attempted only for tar members
(`transform.cpp`, `detect_word16_predictor` call in the tar walk); raw file
inputs could not reach it. Item 6 removes that asymmetry. At tar level it fires
for mr and x-ray (the current header carries 34 bytes of transform metadata,
including those two ranges), and those wins were already inside the baseline.

## Finding 4: where the compressed bytes live (stream accounting)

`axc_inspect.py` on the tar-level archive (52.46 MB total, 10 blocks:
6 context-split + 4 split-slots):

| stream | raw | coded | bits/raw byte | share | coder |
|---|---:|---:|---:|---:|---|
| distance_footer | 16.59 MB | 16.50 MB | **7.96 (raw)** | **31.5%** | stored/rans |
| literal lanes (8) | 18.41 MB | 17.07 MB | 7.0–7.8 | 32.6% | **order-0 rANS only** |
| literals (flat, 4 blocks) | 3.52 MB | 2.90 MB | 6.58 | 5.5% | clustered order-1 |
| match_lengths | 11.70 MB | 6.21 MB | 4.25 | 11.8% | clustered order-1 |
| distance_slots | 9.70 MB | 6.19 MB | 5.10 | 11.8% | clustered order-1 |
| commands | 16.21 MB | 2.28 MB | 1.13 | 4.4% | clustered order-1 |
| literal_lengths | 4.62 MB | 1.30 MB | 2.25 | 2.5% | mixed |

Two structural weaknesses stand out:

1. **Distance footer bits are stored raw** (`encode_lz77_split_streams_slots`
   writes `extra.finish()` with no order-1 and near-8 bpb). LZMA codes the low
   4 bits of long distances with an adaptive ALIGN bit-tree and fully models
   short-distance bits. A third of the archive sits in this stream.
2. **The v7 literal context lanes never see the strong coder**:
   `write_context_literal_streams` (`src/codec/lz77_split.cpp`) calls
   `write_stream(..., try_order1=false, fast=true, prefer_rans=true)`, so each
   lane is order-0 rANS at best. On mozilla the eight lanes are ~50% of the
   payload at 7.2–7.7 bpb; on sao they are 68% at ~8.0 bpb (28-byte records —
   prev-byte context is the wrong model entirely; position-mod-stride is
   right).

Per-member inspections (mozilla, mr-as-tar, sao) are in
`stream-inspection.txt` alongside the CSVs.

## Finding 5: uniform block size is a dead end; adaptive geometry is not

Tar-level block-size sweep (`silesia-gap-tar.csv`):

| config | bytes |
|---|---:|
| default (auto ≈ 21.2M on 10 cores) | 52,459,618 |
| 16M | 52,485,642 |
| 32M | **52,371,652** |
| 64M | 52,467,293 |

The whole curve spans 114 KB — but it nets out large opposing per-member
effects. From the member sweep (default multi-block vs single full-window
block): webster **−873 KB** with a full window, nci −264 KB, mozilla −374 KB,
samba −140 KB; while x-ray (+20 KB) and sao (+5 KB) actually prefer small
blocks (per-block table adaptation beats window on noisy numeric data).
Cross-member matching is worth ~nothing (sum of per-member mx9 sizes is within
73 KB of tar-level mx9). Conclusion: don't chase a global bigger window;
choose block geometry (and entropy-table segmentation) by content.

## Re-ranked plan

Ordered by measured headroom per effort; estimates are non-overlapping.
Everything is encoder-side static structure — decode stays table-lookup,
bounded, search-free, preserving the ~5x decode advantage over LZMA2.

1. **Landed: entropy-code distance footer bits** (0.26 MB tar-level; 0.70 MB
   summed single-member gain). LZMA-ALIGN-style: code
   the low 4 footer bits through the clustered coder contexted by slot (and
   the full footer for short distances); keep raw storage as a per-stream
   candidate so it only ships when smaller. Format: new stream coder or AXC v8
   block candidate in `encode_lz77_split_streams_slots` /
   `encode_lz77_context_split_streams`.
2. **Landed: literal modeling upgrade** (0.18 MB best-row gain). Clustered
   order-1 competes inside the eight previous-byte lanes, and a separate static
   match-byte representation competes for first literals after a match.
   sao/osdb want position-mod-stride
   contexts instead of prev-byte (see item 4).
3. **Landed: content-adaptive block geometry + segmented entropy tables**
   (0.42 MB versus the previous best row; 0.53 MB default-to-default). Validated
   tar member boundaries choose static table/window resets, and large members
   are split below the normal auto budget to retain throughput. This replaces
   the earlier "global bigger window" idea, which measurement killed.
4. **Measured and rejected: record/stride transform.** The detector found sao's
   28-byte stride, but shuffle+delta regressed 355,528 bytes and shuffle-only
   regressed 71,883 bytes. No representation ships.
5. **Corrected/neutral: ELF x86 filter + nested-tar recursion.** ELF machine
   detection and one-level nested-tar discovery landed using the existing x86
   representation and trial gate. Canonical mozilla is Alpha ECOFF, not ELF,
   so the tar result is byte-identical; the unsafe Alpha prototype was removed.
6. **Landed: expose word16 to raw file inputs** (no tar-level gain,
   user-facing). Auto-thread mr/x-ray improve by 0.79/1.35 MB and now beat mx9.
7. **Landed: clustered full-previous-byte literals** (0.13 MB automatic-tar
   gain). The 256 static contexts share at most 16 streams and exact-compete
   with all prior literal modes. Independent Arduino/toolchain and source-tree
   holdouts confirm the representation generalizes; binary data can reject it.

After measuring all seven items, the best tar is item 7 at 51,386,561 bytes:
1,777,887 bytes behind mx5 and 2,701,150 behind mx9. The original
item-5 forecast was based on the incorrect ELF assumption, while item 4 lost
its bake-off. The next ratio phase therefore has to attack the remaining
mozilla literal-modeling gap directly rather than count those estimates again.

Encoder memory (secondary): the July 18 DP compaction reduced the measured
enwik9 peak from 65.66 GiB to 39.55 GiB. The tree and full-path rep history are
still the dominant per-block state and remain candidates for a cache-local
redesign.

## Reproducing / continuing on the workstation

```powershell
# 1. Corpus (once): members in D:\Silesia, then build the exact tar
cd D:\Silesia
tar --force-local --format=ustar -b 1 -cf D:\tests\axiom-perf\silesia.tar `
  dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml

# 2. Re-baseline everything on the workstation (timings + sizes)
py bench\gap_analysis.py --axiom out\Release\axiomc.exe `
  --members-dir D:\Silesia --tar D:\tests\axiom-perf\silesia.tar `
  --output-dir D:\tests\axiom-perf\results

# 3. Stream accounting on any archive of interest
py bench\axc_inspect.py path\to\archive.axc
```

Next ratio phase:

- Productionize the validated swarm parse: parallelize the serial-path entropy
  bake-off, then measure a level 5/6 "solid" mode (full-window ratio at
  mid throughput) and design the level 8–9 hybrid candidate source
  (per-segment tree + prior-segment chains into the DP). This also equalizes
  ratio across machine core counts.
- First test position-mod-stride literal contexts for sao/osdb without physically
  shuffling records. Stride and clustered tables are encoder-chosen static
  metadata, so the decoder remains bounded; the complete representation still
  has to win the existing block bake-off.
- Before adding another transform family, replace the legacy aggregate filter
  gate with independent candidate selection so a losing range cannot hitchhike
  on unrelated word16 wins (the item-4 and Alpha experiments both exposed this).
- The full-window optimal-parse ceiling remains a diagnostic only: a 212 MB
  single block currently exceeds `optimal_parse_limit` (64 MiB), and raising it
  is unlikely to satisfy the no-throughput-regression constraint.

## Level-9 DP checkpoints (2026-07-18)

The remaining serial DP dependency was tested as a versioned, decoder-static
representation rather than by weakening match discovery. Token-aligned parser
tiles are approximately 2 MiB; each keeps the full block window, receives four
greedy-selected recent-distance seeds, and runs an independent optimal DP on the
shared executor. The concatenated tokens retain global context-split entropy
streams. A checkpoint seed first references one of the decoder's current four
distances when possible and transmits a slotted explicit distance only as a
fallback. Decode is four bounded descriptor reads plus table assignments.

The absolute-distance prototype lost every Silesia block by tens of bytes. Rep
reuse removed enough framing that one 5,346,816-byte block won the exact complete
payload bake-off (476,299 versus 476,477 bytes). On the current working tree and
the exact 211,948,032-byte tar:

| configuration | archive bytes | ratio | compression | result |
|---|---:|---:|---:|---|
| same run, all checkpoint candidates rejected | 51,405,635 | 4.12305x | 29.732 s | incumbent |
| AXC v9 2 MiB checkpoints, strict per-block bake-off | **51,405,457** | **4.12306x** | 30.317 s | 178 bytes smaller |

SHA-256 round-trip passed (0.308 s decode), the full
codec/archive/safety/fuzz suite passed, and
the checkpoint token stream was byte-identical with three and four executor
workers. `axc_inspect.py` now recognizes block codec 10. This is deliberately a
small first ratio win: the architectural result is that independent DPs can use
all available executor workers without accepting the large ratio loss measured
for hard segmented DP, while the ordinary global parse remains the fallback.

## enwik9 thread scaling and bottleneck trace (2026-07-18)

Machine: AMD Ryzen 9 5950X, 16 physical cores / 32 logical processors, 64 GiB
RAM. Input: canonical 1,000,000,000-byte `D:\enwik9`. All rows use level 9;
the complete raw matrix is
`bench/results/enwik9-thread-scaling-2026-07-18.csv`.

| configuration | time | active logical CPUs | peak commit | bytes | result |
|---|---:|---:|---:|---:|---|
| fixed 64 MiB, 16 workers | 141.243 s | 13.31 | 65.55 GiB | 226,778,869 | ratio reference |
| fixed 64 MiB, 32 workers | 139.635 s | 13.69 | 65.57 GiB | 226,778,869 | SMT did not scale |
| 32 automatic blocks | 89.201 s | 24.69 | 65.69 GiB | 233,427,563 | rejected: +6.65 MB |
| exact tree/DP pipeline, pre-compaction | 130.789 s | 15.08 | 65.66 GiB | 227,343,036 | traced baseline |
| exact pipeline + compact DP | **117.850 s** | **15.64** | **39.55 GiB** | **227,343,036** | landed |

The isolated 64 MiB trace was decisive: direct parsing used one CPU for
63.069 s; the exact two-stage tree/DP pipeline used 1.29 CPUs and took 58.133 s
with identical 17,387,614-byte output. DP therefore supplies only about 29% of
a second thread while ordered binary-tree discovery remains the long pole. With
16 blocks already walking trees on all 16 physical cores, SMT siblings compete
for cache and memory bandwidth; Task Manager showing about 49% does not mean the
executor is capped at 16. It means only about one hardware thread per core is
useful during the dominant phase.

Automatic execution now exposes all logical processors, discovered across all
Windows processor groups, while automatic block geometry remains based on
physical cores. This preserves the incumbent ratio and lets nested work use SMT
on AMD, Intel, and larger machines without a fixed 32-thread ceiling. The exact
level-9 pipeline is automatic; the checkpoint representation remains an opt-in
swarm candidate and still ships only when its complete payload is smaller.

Three representation-neutral DP changes removed the measured memory bottleneck:

- recent-distance state uses four 32-bit values rather than four `size_t`s;
- `ParseDecision` is explicitly 8 bytes instead of 12 bytes with padding; and
- only the live `max_match + 1` cost frontier is retained in a ring.

Together they reduced peak commit by 26.11 GiB (39.8%) and improved wall time by
12.939 s (9.9%), with identical archive bytes. Full Release tests and SHA-256
round-trip passed. A GUI-equivalent AXAR integration run completed in 72.518 s,
averaged 16.34 active logical CPUs, peaked at 36 process threads and 9.96 GiB
commit, and passed `axiomc test`; its separate solid-block geometry is recorded
in the CSV and is not ratio-comparable to the AXC row. The next throughput target
is not more blind worker creation:
it is a cache-local, full-history tree candidate source (segment-local tree plus
immutable prior-segment indexes) that can feed independent checkpoint DPs. It
must still win the existing complete-payload bake-off before becoming writable.
