#!/usr/bin/env python3
"""Generate the benchmark SVGs embedded in README.md.

The README table remains the source of truth so chart values cannot silently
drift away from the documented benchmark snapshot.
"""

from __future__ import annotations

import html
import math
import re
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
README = ROOT / "README.md"
OUTPUT_DIR = ROOT / "docs" / "images"
INPUT_MB = 211.948032


@dataclass(frozen=True)
class Result:
    label: str
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
        return INPUT_MB / self.compress_seconds


ROW_PATTERN = re.compile(
    r"^\|\s*(.+?)\s*\|\s*([0-9.]+) MB\s*\|\s*([0-9.]+)x\s*\|"
    r"\s*([0-9.]+) s\s*\|\s*([0-9.]+) s\s*\|$"
)


def read_results() -> list[Result]:
    results: list[Result] = []
    for line in README.read_text(encoding="utf-8").splitlines():
        match = ROW_PATTERN.match(line)
        if not match:
            continue
        label = match.group(1).replace("**", "").replace(" (default)", "")
        results.append(
            Result(
                label=label,
                size_mb=float(match.group(2)),
                ratio=float(match.group(3)),
                compress_seconds=float(match.group(4)),
                decompress_seconds=float(match.group(5)),
            )
        )
    if len(results) < 10:
        raise RuntimeError("README benchmark table was not found or is incomplete")
    return results


def svg_start(width: int, height: int, title: str, description: str) -> list[str]:
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">',
        f"  <title id=\"title\">{html.escape(title)}</title>",
        f"  <desc id=\"desc\">{html.escape(description)}</desc>",
        "  <style>",
        "    :root { --bg:#ffffff; --fg:#172033; --muted:#667085; --grid:#d9dee8;",
        "      --axiom:#e26d2f; --zstd:#238b74; --lz4:#3973c6; --lzma2:#a54d9d; --winrar:#c43b4d; --other:#7b8798; }",
        "    @media (prefers-color-scheme: dark) { :root { --bg:#0d1117; --fg:#e6edf3;",
        "      --muted:#9ba7b4; --grid:#30363d; --axiom:#ff934f; --zstd:#4fc3a1;",
        "      --lz4:#73a7ef; --lzma2:#d88ad0; --winrar:#ff667a; --other:#a6b0bd; } }",
        "    .bg{fill:var(--bg)} .fg{fill:var(--fg)} .muted{fill:var(--muted)}",
        "    .grid{stroke:var(--grid);stroke-width:1} .axis{stroke:var(--muted);stroke-width:1.2}",
        "    text{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;font-size:13px}",
        "    .title{font-size:20px;font-weight:500} .subtitle{font-size:12px}",
        "    .value{font-weight:500} .axiom{fill:var(--axiom)} .zstd{fill:var(--zstd)}",
        "    .lz4{fill:var(--lz4)} .lzma2{fill:var(--lzma2)} .winrar{fill:var(--winrar)} .other{fill:var(--other)}",
        "    .best{stroke:var(--fg);stroke-width:2}",
        "  </style>",
        f'  <rect class="bg" width="{width}" height="{height}"/>',
    ]


def write_ratio_chart(results: list[Result]) -> None:
    ordered = sorted(results, key=lambda item: item.ratio, reverse=True)
    width = 1000
    row_height = 29
    top = 82
    bottom = 48
    left = 180
    right = 84
    height = top + len(ordered) * row_height + bottom
    plot_width = width - left - right
    max_ratio = 4.5

    lines = svg_start(
        width,
        height,
        "Silesia compression ratio by codec",
        "Horizontal bars compare compression ratios. Higher values are better; Axiom results are orange.",
    )
    lines += [
        '  <text class="title fg" x="24" y="32">Silesia compression ratio</text>',
        '  <text class="subtitle muted" x="24" y="54">211.9 MB tar · higher is better</text>',
    ]

    for tick in [0, 1, 2, 3, 4]:
        x = left + plot_width * tick / max_ratio
        lines.append(f'  <line class="grid" x1="{x:.1f}" y1="68" x2="{x:.1f}" y2="{height-bottom}"/>')
        lines.append(f'  <text class="muted" x="{x:.1f}" y="{height-18}" text-anchor="middle">{tick}×</text>')

    for index, result in enumerate(ordered):
        y = top + index * row_height
        bar_width = plot_width * result.ratio / max_ratio
        special = result.label == "Axiom -9"
        label_weight = ' font-weight="500"' if special else ""
        lines.append(
            f'  <text class="fg" x="{left-12}" y="{y+16}" text-anchor="end"{label_weight}>'
            f'{html.escape(result.label)}</text>'
        )
        lines.append(
            f'  <rect class="{result.family}{" best" if special else ""}" x="{left}" y="{y+3}" '
            f'width="{bar_width:.1f}" height="19" rx="3"><title>{html.escape(result.label)}: '
            f'{result.ratio:.2f}×, {result.size_mb:.1f} MB</title></rect>'
        )
        lines.append(
            f'  <text class="value fg" x="{left+bar_width+8:.1f}" y="{y+17}">{result.ratio:.2f}×</text>'
        )

    lines.append("</svg>")
    (OUTPUT_DIR / "silesia-compression-ratio.svg").write_text("\n".join(lines) + "\n", encoding="utf-8")


def marker(result: Result, x: float, y: float, highlighted: bool) -> str:
    css = f'{result.family}{" best" if highlighted else ""}'
    size = 7 if highlighted else 5
    title = (
        f"{html.escape(result.label)}: {result.ratio:.2f}×, "
        f"{result.compression_mbps:.1f} MB/s ({result.compress_seconds:.2f} s)"
    )
    if result.family in {"zstd", "winrar"}:
        return f'<rect class="{css}" x="{x-size}" y="{y-size}" width="{size*2}" height="{size*2}"><title>{title}</title></rect>'
    if result.family == "lz4":
        return f'<path class="{css}" d="M{x:.1f},{y-size} L{x+size:.1f},{y:.1f} L{x:.1f},{y+size} L{x-size:.1f},{y:.1f} Z"><title>{title}</title></path>'
    if result.family == "lzma2":
        return f'<path class="{css}" d="M{x:.1f},{y-size-1} L{x+size+1:.1f},{y+size} L{x-size-1:.1f},{y+size} Z"><title>{title}</title></path>'
    return f'<circle class="{css}" cx="{x:.1f}" cy="{y:.1f}" r="{size}"><title>{title}</title></circle>'


def write_tradeoff_chart(results: list[Result]) -> None:
    width = 1000
    height = 610
    left = 82
    right = 34
    top = 76
    bottom = 70
    plot_width = width - left - right
    plot_height = height - top - bottom
    min_speed = 2.0
    max_speed = 4000.0
    min_ratio = 2.0
    max_ratio = 4.5

    def x_pos(speed: float) -> float:
        return left + plot_width * (math.log10(speed) - math.log10(min_speed)) / (
            math.log10(max_speed) - math.log10(min_speed)
        )

    def y_pos(ratio: float) -> float:
        return top + plot_height * (max_ratio - ratio) / (max_ratio - min_ratio)

    lines = svg_start(
        width,
        height,
        "Silesia compression ratio versus compression throughput",
        "Scatter plot with compression throughput on a logarithmic horizontal axis and ratio vertically. Better results move toward the upper right.",
    )
    lines += [
        '  <text class="title fg" x="24" y="32">Ratio versus compression throughput</text>',
        '  <text class="subtitle muted" x="24" y="54">211.9 MB Silesia tar · better results move toward the upper right</text>',
    ]

    for tick in [2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 4000]:
        x = x_pos(tick)
        lines.append(f'  <line class="grid" x1="{x:.1f}" y1="{top}" x2="{x:.1f}" y2="{height-bottom}"/>')
        lines.append(f'  <text class="muted" x="{x:.1f}" y="{height-bottom+24}" text-anchor="middle">{tick:g}</text>')
    for tick in [2.0, 2.5, 3.0, 3.5, 4.0, 4.5]:
        y = y_pos(tick)
        lines.append(f'  <line class="grid" x1="{left}" y1="{y:.1f}" x2="{width-right}" y2="{y:.1f}"/>')
        lines.append(f'  <text class="muted" x="{left-12}" y="{y+4:.1f}" text-anchor="end">{tick:.1f}×</text>')
    lines += [
        f'  <line class="axis" x1="{left}" y1="{height-bottom}" x2="{width-right}" y2="{height-bottom}"/>',
        f'  <line class="axis" x1="{left}" y1="{top}" x2="{left}" y2="{height-bottom}"/>',
        f'  <text class="fg" x="{left+plot_width/2:.1f}" y="{height-18}" text-anchor="middle">Compression throughput (MB/s, log scale)</text>',
        f'  <text class="fg" transform="translate(20 {top+plot_height/2:.1f}) rotate(-90)" text-anchor="middle">Compression ratio</text>',
    ]

    for result in results:
        x = x_pos(result.compression_mbps)
        y = y_pos(result.ratio)
        highlighted = result.label == "Axiom -9"
        lines.append("  " + marker(result, x, y, highlighted))

    label_offsets = {
        "Axiom -9": (10, -12),
        "zstd -19": (10, 18),
        "zstd -22 --ultra": (10, -12),
        "LZMA2 -mx9": (-10, 19),
        "LZMA2 -mx5": (10, -10),
        "WinRAR -m5 128M": (10, 18),
        "Axiom -1": (10, -10),
        "zstd -1": (-10, 18),
    }
    by_label = {result.label: result for result in results}
    for label, (dx, dy) in label_offsets.items():
        result = by_label[label]
        x = x_pos(result.compression_mbps)
        y = y_pos(result.ratio)
        anchor = "end" if dx < 0 else "start"
        weight = ' font-weight="500"' if label == "Axiom -9" else ""
        lines.append(
            f'  <text class="fg" x="{x+dx:.1f}" y="{y+dy:.1f}" text-anchor="{anchor}"{weight}>'
            f'{html.escape(label)}</text>'
        )

    legend = [("axiom", "Axiom", "circle"), ("zstd", "zstd", "square"),
              ("lz4", "LZ4", "diamond"), ("lzma2", "LZMA2", "triangle"),
              ("winrar", "WinRAR", "square"),
              ("other", "gzip/bzip2", "circle")]
    legend_x = 492
    for index, (family, label, shape) in enumerate(legend):
        x = legend_x + index * 76
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
    (OUTPUT_DIR / "silesia-speed-ratio.svg").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    results = read_results()
    write_ratio_chart(results)
    write_tradeoff_chart(results)
    print(f"wrote {len(results)} benchmark rows to {OUTPUT_DIR}")


if __name__ == "__main__":
    main()
