#!/usr/bin/env python3
"""Structural inspector for .axc archives: per-block and per-stream accounting.

Walks the self-describing AXC container without decoding entropy payloads and
reports, for every block, the chosen block codec and the size of each entropy
stream unit (raw symbol bytes vs coded bytes vs chosen stream coder). The
aggregate table answers "where do the compressed bytes actually live" so
modeling work can be aimed at the streams that dominate.

Understands AXC versions 4 through 8 as written by current encoders:
  header:      magic "AXIOMC1\\0", u16 version, u8 codec, u8 flags,
               u64 original, u64 payload, u32 crc, then (v5+ when flags&1)
               u32 transform metadata size + metadata bytes.
  codec ids:   0 store, 1 raw LZ77, 2 LZ77+Huffman, 3 parallel blocks,
               4 split, 5 split+slots, 6 sequences, 7 contextual slots.
  block ids:   0 store, 1 raw LZ77, 2 Huffman, 3 split, 4 split+slots,
               5 fast_lz, 6 sequences, 7 context split,
               8 contextual slots, 9 contextual slots + context split.
  stream unit: varuint raw_size, u8 coder, varuint payload_size, payload.
  coders:      0 store, 1 huffman, 2 legacy order1, 3 rans, 4 rans_order1.
"""

import argparse
import sys
from collections import defaultdict
from pathlib import Path

MAGIC = b"AXIOMC1\0"

CODEC_NAMES = {
    0: "store", 1: "raw-lz77", 2: "lz77-huffman", 3: "parallel-blocks",
    4: "split", 5: "split-slots", 6: "sequences",
    7: "contextual-slots",
}
TRANSFORM_NAMES = {1: "x86", 2: "delta", 3: "word16"}
BLOCK_NAMES = {
    0: "store", 1: "raw-lz77", 2: "huffman", 3: "split", 4: "split-slots",
    5: "fast-lz", 6: "sequences", 7: "context-split",
    8: "contextual-slots", 9: "contextual-slots-context-split",
}
STREAM_CODER_NAMES = {
    0: "store", 1: "huffman", 2: "order1-legacy", 3: "rans", 4: "rans-o1",
}

SPLIT_STREAMS = ["commands", "literal_lengths", "match_lengths", "distances",
                 "literals"]
SLOT_STREAMS = ["commands", "literal_lengths", "match_lengths",
                "distance_slots", "distance_footer", "literals"]
SEQUENCE_CODE_STREAMS = ["literal_length_codes", "match_length_codes",
                         "offset_codes"]
SEQUENCE_BLOBS = ["literal_length_extra", "match_length_extra", "offset_extra"]


class Cursor:
    def __init__(self, data, offset=0):
        self.data = data
        self.offset = offset

    def u8(self):
        value = self.data[self.offset]
        self.offset += 1
        return value

    def bytes(self, count):
        chunk = self.data[self.offset:self.offset + count]
        if len(chunk) != count:
            raise ValueError("truncated archive")
        self.offset += count
        return chunk

    def uint(self, width):
        return int.from_bytes(self.bytes(width), "little")

    def varuint(self):
        result = 0
        shift = 0
        while True:
            byte = self.u8()
            result |= (byte & 0x7F) << shift
            if byte < 0x80:
                return result
            shift += 7


def read_stream_unit(cursor, label, sink):
    raw_size = cursor.varuint()
    coder = cursor.u8()
    payload_size = cursor.varuint()
    cursor.bytes(payload_size)
    sink.append({
        "stream": label,
        "raw_bytes": raw_size,
        "coded_bytes": payload_size,
        "coder": STREAM_CODER_NAMES.get(coder, f"unknown-{coder}"),
    })


def read_raw_blob(cursor, label, sink):
    size = cursor.varuint()
    cursor.bytes(size)
    sink.append({"stream": label, "raw_bytes": size, "coded_bytes": size,
                 "coder": "raw-bits"})


def read_contextual_rans_blob(cursor, label, sink):
    size = cursor.varuint()
    payload = cursor.bytes(size)
    model = Cursor(payload)
    decoded_size = model.varuint()
    sink.append({"stream": label, "raw_bytes": decoded_size,
                 "coded_bytes": size, "coder": "slot-context-rans"})


def walk_split(cursor, names, sink):
    for name in names:
        read_stream_unit(cursor, name, sink)


def walk_literal_lanes(cursor, sink):
    mode = cursor.u8()
    mode_names = {0: "raw", 1: "rep0-xor", 2: "match-byte",
                  3: "full-prev-clustered"}
    sink.append({"stream": "literal_mode", "raw_bytes": 0, "coded_bytes": 1,
                 "coder": mode_names.get(mode, f"unknown-{mode}")})
    if mode == 3:
        cluster_count = cursor.u8()
        if not 1 <= cluster_count <= 16:
            raise ValueError(f"invalid literal cluster count {cluster_count}")
        context_map = cursor.bytes(256)
        if any(cluster >= cluster_count for cluster in context_map):
            raise ValueError("literal context maps to missing cluster")
        sink.append({"stream": "literal_context_map", "raw_bytes": 256,
                     "coded_bytes": 257, "coder": f"{cluster_count}-cluster-map"})
        for cluster in range(cluster_count):
            read_stream_unit(cursor, f"literal_cluster_{cluster}", sink)
        return
    for lane in range(8):
        read_stream_unit(cursor, f"literal_lane_{lane}", sink)
    if mode == 2:
        for lane in range(8):
            read_stream_unit(cursor, f"match_literal_lane_{lane}", sink)


def walk_sequences(cursor, sink):
    cursor.varuint()  # sequence count
    cursor.varuint()  # trailing literals
    for name in SEQUENCE_CODE_STREAMS:
        mode = cursor.u8()
        if mode != 0:
            raise ValueError(f"unknown sequence code mode {mode}")
        read_stream_unit(cursor, name, sink)
    for name in SEQUENCE_BLOBS:
        read_raw_blob(cursor, name, sink)
    walk_literal_lanes(cursor, sink)


def walk_block_payload(block_codec, payload, sink):
    cursor = Cursor(payload)
    if block_codec == 3:
        walk_split(cursor, SPLIT_STREAMS, sink)
    elif block_codec == 4:
        walk_split(cursor, SLOT_STREAMS, sink)
    elif block_codec == 6:
        walk_sequences(cursor, sink)
    elif block_codec == 7:
        walk_split(cursor, SLOT_STREAMS[:-1], sink)
        walk_literal_lanes(cursor, sink)
    elif block_codec in (8, 9):
        for name in ["commands", "literal_lengths", "match_lengths",
                     "distance_slots", "distance_footer_high"]:
            read_stream_unit(cursor, name, sink)
        read_contextual_rans_blob(cursor, "distance_footer_align", sink)
        if block_codec == 8:
            read_stream_unit(cursor, "literals", sink)
        else:
            walk_literal_lanes(cursor, sink)
    else:
        sink.append({"stream": BLOCK_NAMES.get(block_codec, "?"),
                     "raw_bytes": len(payload), "coded_bytes": len(payload),
                     "coder": "opaque"})
        return
    if cursor.offset != len(payload):
        raise ValueError(
            f"block payload has {len(payload) - cursor.offset} unparsed bytes")


def inspect(path):
    data = Path(path).read_bytes()
    cursor = Cursor(data)
    if cursor.bytes(8) != MAGIC:
        raise ValueError("not an AXC archive")
    version = cursor.uint(2)
    codec = cursor.u8()
    flags = cursor.u8()
    original = cursor.uint(8)
    payload_size = cursor.uint(8)
    cursor.uint(4)  # crc32
    transform_bytes = 0
    transforms = []
    if version >= 5:
        transform_bytes = cursor.uint(4)
        metadata = Cursor(cursor.bytes(transform_bytes))
        if transform_bytes:
            for _ in range(metadata.varuint()):
                transform = metadata.u8()
                parameter = metadata.u8()
                offset = metadata.varuint()
                size = metadata.varuint()
                metadata.varuint()  # logical source offset
                transforms.append((transform, parameter, offset, size))

    payload = cursor.bytes(payload_size)
    header_bytes = len(data) - payload_size

    print(f"{path}")
    print(f"  version {version}, codec {CODEC_NAMES.get(codec, codec)}, "
          f"original {original}, payload {payload_size}, "
          f"header+transform {header_bytes} "
          f"(transform metadata {transform_bytes})")
    if transforms:
        print("  transforms " + ", ".join(
            f"{TRANSFORM_NAMES.get(kind, kind)}(p={parameter}, off={offset}, size={size})"
            for kind, parameter, offset, size in transforms))

    blocks = []
    if codec == 3:
        inner = Cursor(payload)
        block_count = inner.varuint()
        for index in range(block_count):
            block_original = inner.varuint()
            block_codec = inner.u8()
            block_payload_size = inner.varuint()
            block_payload = inner.bytes(block_payload_size)
            blocks.append((index, block_original, block_codec, block_payload))
        if inner.offset != len(payload):
            raise ValueError("trailing bytes after block payloads")
    else:
        remap = {0: 0, 1: 1, 2: 2, 4: 3, 5: 4, 6: 6, 7: 8}
        blocks.append((0, original, remap.get(codec, codec), payload))

    per_stream = defaultdict(lambda: [0, 0, defaultdict(int)])
    block_codec_counts = defaultdict(lambda: [0, 0, 0])
    for index, block_original, block_codec, block_payload in blocks:
        sink = []
        walk_block_payload(block_codec, block_payload, sink)
        name = BLOCK_NAMES.get(block_codec, f"unknown-{block_codec}")
        counts = block_codec_counts[name]
        counts[0] += 1
        counts[1] += block_original
        counts[2] += len(block_payload)
        for item in sink:
            entry = per_stream[item["stream"]]
            entry[0] += item["raw_bytes"]
            entry[1] += item["coded_bytes"]
            entry[2][item["coder"]] += 1

    print(f"\n  {len(blocks)} block(s):")
    for name, (count, original_sum, payload_sum) in block_codec_counts.items():
        print(f"    {name:<14} x{count:<3} original {original_sum:>12} "
              f"-> payload {payload_sum:>12}")

    total_coded = sum(entry[1] for entry in per_stream.values())
    print(f"\n  {'stream':<22} {'raw':>12} {'coded':>12} {'bits/raw':>9} "
          f"{'share':>7}  coders")
    ordered = sorted(per_stream.items(), key=lambda kv: kv[1][1], reverse=True)
    for name, (raw, coded, coders) in ordered:
        bits = (coded * 8.0 / raw) if raw else 0.0
        share = 100.0 * coded / total_coded if total_coded else 0.0
        coder_text = ",".join(f"{coder}x{count}"
                              for coder, count in sorted(coders.items()))
        print(f"  {name:<22} {raw:>12} {coded:>12} {bits:>9.3f} "
              f"{share:>6.2f}%  {coder_text}")
    print(f"  {'TOTAL':<22} {'':>12} {total_coded:>12}")
    framing = payload_size - total_coded
    print(f"  block/stream framing overhead: {framing} bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("archives", nargs="+", help=".axc files to inspect")
    args = parser.parse_args()
    for path in args.archives:
        try:
            inspect(path)
        except ValueError as error:
            print(f"{path}: {error}", file=sys.stderr)
            return 1
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
