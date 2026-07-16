#!/usr/bin/env python3
"""Generate README benchmark SVGs from versioned, verified CSV snapshots."""

from __future__ import annotations

import csv
import html
import math
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUTPUT_DIR = ROOT / "docs" / "images"
BENCHMARKS = (
    ("silesia", "Silesia",
     ROOT / "bench" / "results" / "silesia-0.4.0.0.csv",
     "211.9 MB mixed-data tar"),
    ("enwik8", "enwik8",
     ROOT / "bench" / "results" / "enwik8-0.4.0.0.csv",
     "100 MB English Wikipedia text"),
)

LABELS = {
    "axiom-1": "Axiom -1", "axiom-2": "Axiom -2",
    "axiom-3": "Axiom -3", "axiom-4": "Axiom -4",
    "axiom-5": "Axiom -5", "axiom-6": "Axiom -6",
    "axiom-7": "Axiom -7", "axiom-8": "Axiom -8",
    "axiom-9": "Axiom -9", "lz4-1": "LZ4 -1",
    "lz4-9-hc": "LZ4 -9 (HC)", "zstd-1": "zstd -1",
    "zstd-3": "zstd -3", "zstd-9": "zstd -9",
    "zstd-19": "zstd -19", "zstd-22-ultra": "zstd -22 --ultra",
    "7z-lzma2-mx5": "LZMA2 -mx5", "7z-lzma2-mx9": "LZMA2 -mx9",
    "7z-bzip2-mx9": "bzip2 -9", "7z-gzip-mx9": "gzip Deflate -9",
    "winrar-m3": "WinRAR -m3", "winrar-m5-128m": "WinRAR -m5 128M",
}


@dataclass(frozen=True)
class Result:
    label: str
    input_mb: float
    size_mb: float
    ratio: float
    compress_seconds: float
    decompress_seconds: float

    @property
    def family(self) -> str:
        if self.label.startswith("Axiom"):
            return "axiom"
        if self.label.startswith("zstd"):
            return "zstd"
        if self.label.startswith("LZ4"):
            return "lz4"
        if self.label.startswith("LZMA2"):
            return "lzma2"
        if self.label.startswith("WinRAR"):
            return "winrar"
        return "other"

    @property
    def compression_mbps(self) -> float:
        return self.input_mb / self.compress_seconds


def read_results(path: Path) -> list[Result]:
    results = []
    with path.open(newline="", encoding="utf-8") as file:
        for row in csv.DictReader(file):
            if row["codec"] not in LABELS or row["verified"] != "True":
                raise RuntimeError(f"unexpected or unverified benchmark row: {row}")
            results.append(Result(
                label=LABELS[row["codec"]],
                input_mb=int(row["input_bytes"]) / 1_000_000,
                size_mb=int(row["archive_bytes"]) / 1_000_000,
                ratio=float(row["ratio"]),
                compress_seconds=float(row["compress_seconds"]),
                decompress_seconds=float(row["decompress_seconds"]),
            ))
    if len(results) != len(LABELS):
        raise RuntimeError(f"{path} has {len(results)} rows; expected {len(LABELS)}")
    return results


def svg_start(width: int, height: int, title: str, description: str) -> list[str]:
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">',
        f'  <title id="title">{html.escape(title)}</title>',
        f'  <desc id="desc">{html.escape(description)}</desc>',
        "  <style>",
        "    :root{--bg:#fff;--fg:#172033;--muted:#667085;--grid:#d9dee8;"
        "--axiom:#e26d2f;--zstd:#238b74;--lz4:#3973c6;--lzma2:#a54d9d;"
        "--winrar:#c43b4d;--other:#7b8798}",
        "    @media(prefers-color-scheme:dark){:root{--bg:#0d1117;--fg:#e6edf3;"
        "--muted:#9ba7b4;--grid:#30363d;--axiom:#ff934f;--zstd:#4fc3a1;"
        "--lz4:#73a7ef;--lzma2:#d88ad0;--winrar:#ff667a;--other:#a6b0bd}}",
        "    .bg{fill:var(--bg)}.fg{fill:var(--fg)}.muted{fill:var(--muted)}"
        ".grid{stroke:var(--grid);stroke-width:1}.axis{stroke:var(--muted);stroke-width:1.2}",
        "    text{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;font-size:13px}"
        ".title{font-size:20px;font-weight:500}.subtitle{font-size:12px}.value{font-weight:500}",
        "    .axiom{fill:var(--axiom)}.zstd{fill:var(--zstd)}.lz4{fill:var(--lz4)}"
        ".lzma2{fill:var(--lzma2)}.winrar{fill:var(--winrar)}.other{fill:var(--other)}"
        ".best{stroke:var(--fg);stroke-width:2}",
        "  </style>",
        f'  <rect class="bg" width="{width}" height="{height}"/>',
    ]


def write_ratio_chart(results: list[Result], slug: str, title: str,
                      corpus_note: str) -> None:
    ordered = sorted(results, key=lambda item: item.ratio, reverse=True)
    width, row_height, top, bottom, left, right = 1000, 29, 82, 48, 180, 84
    height = top + len(ordered) * row_height + bottom
    plot_width = width - left - right
    max_ratio = 4.5
    lines = svg_start(
        width, height, f"{title} compression ratio by codec",
        "Horizontal bars compare compression ratios. Higher values are better; "
        "Axiom results are orange.")
    lines += [
        f'  <text class="title fg" x="24" y="32">{html.escape(title)} compression ratio</text>',
        f'  <text class="subtitle muted" x="24" y="54">{html.escape(corpus_note)} &#183; higher is better</text>',
    ]
    for tick in range(5):
        x = left + plot_width * tick / max_ratio
        lines.append(f'  <line class="grid" x1="{x:.1f}" y1="68" x2="{x:.1f}" y2="{height-bottom}"/>')
        lines.append(f'  <text class="muted" x="{x:.1f}" y="{height-18}" text-anchor="middle">{tick}&#215;</text>')
    for index, result in enumerate(ordered):
        y = top + index * row_height
        bar_width = plot_width * result.ratio / max_ratio
        highlighted = result.label == "Axiom -9"
        weight = ' font-weight="500"' if highlighted else ""
        lines.append(f'  <text class="fg" x="{left-12}" y="{y+16}" text-anchor="end"{weight}>{html.escape(result.label)}</text>')
        lines.append(
            f'  <rect class="{result.family}{" best" if highlighted else ""}" '
            f'x="{left}" y="{y+3}" width="{bar_width:.1f}" height="19" rx="3">'
            f'<title>{html.escape(result.label)}: {result.ratio:.2f}x, '
            f'{result.size_mb:.1f} MB</title></rect>')
        lines.append(f'  <text class="value fg" x="{left+bar_width+8:.1f}" y="{y+17}">{result.ratio:.2f}&#215;</text>')
    lines.append("</svg>")
    (OUTPUT_DIR / f"{slug}-compression-ratio.svg").write_text(
        "\n".join(lines) + "\n", encoding="utf-8")


def marker(result: Result, x: float, y: float, highlighted: bool) -> str:
    css = f'{result.family}{" best" if highlighted else ""}'
    size = 7 if highlighted else 5
    tooltip = (f"{html.escape(result.label)}: {result.ratio:.2f}x, "
               f"{result.compression_mbps:.1f} MB/s ({result.compress_seconds:.2f} s)")
    if result.family in {"zstd", "winrar"}:
        return f'<rect class="{css}" x="{x-size:.1f}" y="{y-size:.1f}" width="{size*2}" height="{size*2}"><title>{tooltip}</title></rect>'
    if result.family == "lz4":
        return f'<path class="{css}" d="M{x:.1f},{y-size:.1f} L{x+size:.1f},{y:.1f} L{x:.1f},{y+size:.1f} L{x-size:.1f},{y:.1f} Z"><title>{tooltip}</title></path>'
    if result.family == "lzma2":
        return f'<path class="{css}" d="M{x:.1f},{y-size-1:.1f} L{x+size+1:.1f},{y+size:.1f} L{x-size-1:.1f},{y+size:.1f} Z"><title>{tooltip}</title></path>'
    return f'<circle class="{css}" cx="{x:.1f}" cy="{y:.1f}" r="{size}"><title>{tooltip}</title></circle>'


def write_tradeoff_chart(results: list[Result], slug: str, title: str,
                         corpus_note: str) -> None:
    width, height, left, right, top, bottom = 1000, 610, 82, 34, 76, 70
    plot_width, plot_height = width - left - right, height - top - bottom
    min_speed, max_speed, min_ratio, max_ratio = 1.0, 4000.0, 1.5, 4.5

    def x_pos(speed: float) -> float:
        return left + plot_width * (math.log10(speed) - math.log10(min_speed)) / (
            math.log10(max_speed) - math.log10(min_speed))

    def y_pos(ratio: float) -> float:
        return top + plot_height * (max_ratio - ratio) / (max_ratio - min_ratio)

    lines = svg_start(
        width, height, f"{title} compression ratio versus compression throughput",
        "Scatter plot with logarithmic compression throughput horizontally and "
        "ratio vertically. Better results move toward the upper right.")
    lines += [
        '  <text class="title fg" x="24" y="32">Ratio versus compression throughput</text>',
        f'  <text class="subtitle muted" x="24" y="54">{html.escape(corpus_note)} &#183; better results move toward the upper right</text>',
    ]
    for tick in [1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 4000]:
        x = x_pos(tick)
        lines.append(f'  <line class="grid" x1="{x:.1f}" y1="{top}" x2="{x:.1f}" y2="{height-bottom}"/>')
        lines.append(f'  <text class="muted" x="{x:.1f}" y="{height-bottom+24}" text-anchor="middle">{tick:g}</text>')
    for tick in [1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5]:
        y = y_pos(tick)
        lines.append(f'  <line class="grid" x1="{left}" y1="{y:.1f}" x2="{width-right}" y2="{y:.1f}"/>')
        lines.append(f'  <text class="muted" x="{left-12}" y="{y+4:.1f}" text-anchor="end">{tick:.1f}&#215;</text>')
    lines += [
        f'  <line class="axis" x1="{left}" y1="{height-bottom}" x2="{width-right}" y2="{height-bottom}"/>',
        f'  <line class="axis" x1="{left}" y1="{top}" x2="{left}" y2="{height-bottom}"/>',
        f'  <text class="fg" x="{left+plot_width/2:.1f}" y="{height-18}" text-anchor="middle">Compression throughput (MB/s, log scale)</text>',
        f'  <text class="fg" transform="translate(20 {top+plot_height/2:.1f}) rotate(-90)" text-anchor="middle">Compression ratio</text>',
    ]
    for result in results:
        lines.append("  " + marker(result, x_pos(result.compression_mbps),
                                    y_pos(result.ratio), result.label == "Axiom -9"))
    label_offsets = {
        "Axiom -9": (10, -12), "zstd -19": (10, 18),
        "zstd -22 --ultra": (-10, -12), "LZMA2 -mx9": (10, -18),
        "LZMA2 -mx5": (10, 18), "WinRAR -m5 128M": (10, 18),
        "Axiom -1": (10, -10), "zstd -1": (-10, 18),
    }
    by_label = {result.label: result for result in results}
    for label, (dx, dy) in label_offsets.items():
        result = by_label[label]
        x, y = x_pos(result.compression_mbps), y_pos(result.ratio)
        anchor = "end" if dx < 0 else "start"
        weight = ' font-weight="500"' if label == "Axiom -9" else ""
        lines.append(f'  <text class="fg" x="{x+dx:.1f}" y="{y+dy:.1f}" text-anchor="{anchor}"{weight}>{html.escape(label)}</text>')
    legend = [("axiom", "Axiom", "circle"), ("zstd", "zstd", "square"),
              ("lz4", "LZ4", "diamond"), ("lzma2", "LZMA2", "triangle"),
              ("winrar", "WinRAR", "square"),
              ("other", "gzip/bzip2", "circle")]
    for index, (family, label, shape) in enumerate(legend):
        x = 492 + index * 76
        if shape == "square":
            lines.append(f'  <rect class="{family}" x="{x}" y="45" width="9" height="9"/>')
        elif shape == "diamond":
            lines.append(f'  <path class="{family}" d="M{x+4.5},44 L{x+9},49 L{x+4.5},54 L{x},49 Z"/>')
        elif shape == "triangle":
            lines.append(f'  <path class="{family}" d="M{x+4.5},44 L{x+9},54 L{x},54 Z"/>')
        else:
            lines.append(f'  <circle class="{family}" cx="{x+4.5}" cy="49" r="4.5"/>')
        lines.append(f'  <text class="muted" x="{x+14}" y="53">{html.escape(label)}</text>')
    lines.append("</svg>")
    (OUTPUT_DIR / f"{slug}-speed-ratio.svg").write_text(
        "\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    row_count = 0
    for slug, title, csv_path, corpus_note in BENCHMARKS:
        results = read_results(csv_path)
        write_ratio_chart(results, slug, title, corpus_note)
        write_tradeoff_chart(results, slug, title, corpus_note)
        row_count += len(results)
    print(f"wrote four charts from {row_count} verified rows to {OUTPUT_DIR}")


if __name__ == "__main__":
    main()
