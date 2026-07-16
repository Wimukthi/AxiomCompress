#!/usr/bin/env python3
"""Phase-0 gap analysis: locate where axiom-9 loses bytes to LZMA2 on Silesia.

Two measurement sets:

1. Per member: each Silesia file is compressed alone with axiom-9 (default
   auto-split threads and --threads 1 single-block) and with 7-Zip LZMA2
   (mx5, mx9). The --threads 1 row is the modeling/parse comparison: every
   member fits one 64 MiB block, so the auto block-splitting tax is absent
   and both codecs see the whole file as their window.

2. Tar level: the published silesia.tar is compressed with axiom-9 at the
   default auto block sizing and at explicit 64/32/16 MiB block sizes. The
   curve quantifies how much ratio the per-core block splitting costs on the
   published benchmark configuration.

Axiom outputs are round-trip verified. Reference-codec outputs are not
re-verified here; bench_codecs.py already validates those profiles.
"""

import argparse
import csv
import subprocess
import sys
import tempfile
import time
from pathlib import Path

MEMBERS = [
    "dickens", "mozilla", "mr", "nci", "ooffice", "osdb",
    "reymont", "samba", "sao", "webster", "x-ray", "xml",
]

CHUNK_SIZE = 1024 * 1024

MEMBER_AXIOM_CONFIGS = [
    ("axiom9", ["--level", "9", "--threads", "0"]),
    ("axiom9-t1", ["--level", "9", "--threads", "1"]),
]

MEMBER_SEVENZIP_CONFIGS = [
    ("7z-mx5", ["-t7z", "-m0=LZMA2", "-mx=5", "-mmt=on", "-ms=on"]),
    ("7z-mx9", ["-t7z", "-m0=LZMA2", "-mx=9", "-mmt=on", "-ms=on"]),
]

# Explicit-block ratios depend only on the block boundaries, not the worker
# count, so the thread counts below just bound peak encoder memory: level 9
# holds roughly 3.5 GiB of matcher/parser state per in-flight 64 MiB block,
# which OOMs a 16 GiB machine at --threads 0.
TAR_AXIOM_CONFIGS = [
    ("tar-axiom9-default", ["--level", "9", "--threads", "0"]),
    ("tar-axiom9-64m", ["--level", "9", "--block-size", "64M",
                        "--window", "64M", "--threads", "0"]),
    ("tar-axiom9-32m", ["--level", "9", "--block-size", "32M",
                        "--window", "32M", "--threads", "4"]),
    ("tar-axiom9-16m", ["--level", "9", "--block-size", "16M",
                        "--window", "16M", "--threads", "6"]),
]


def run(command, cwd=None):
    started = time.perf_counter()
    process = subprocess.run(
        [str(item) for item in command],
        cwd=cwd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    elapsed = time.perf_counter() - started
    if process.returncode != 0:
        rendered = " ".join(str(item) for item in command)
        raise SystemExit(f"command failed ({process.returncode}): {rendered}")
    return elapsed


def files_equal(left, right):
    if left.stat().st_size != right.stat().st_size:
        return False
    with left.open("rb") as left_file, right.open("rb") as right_file:
        while True:
            left_chunk = left_file.read(CHUNK_SIZE)
            right_chunk = right_file.read(CHUNK_SIZE)
            if left_chunk != right_chunk:
                return False
            if not left_chunk:
                return True


def axiom_row(axiom, source, label, arguments, temp, verify=True):
    archive = temp / f"{source.name}-{label}.axc"
    seconds = run([axiom, "c", *arguments, source, archive])
    if verify:
        restored = temp / f"{source.name}-{label}.restored"
        run([axiom, "d", archive, restored])
        if not files_equal(source, restored):
            raise SystemExit(f"{label} round-trip failed for {source}")
        restored.unlink()
    size = archive.stat().st_size
    archive.unlink()
    return size, seconds


def sevenzip_row(sevenzip, source, label, arguments, temp):
    archive = temp / f"{source.name}-{label}.7z"
    seconds = run(
        [sevenzip, "a", "-bd", "-bso0", "-bsp0", "-y", *arguments,
         archive, source.name],
        cwd=source.parent,
    )
    size = archive.stat().st_size
    archive.unlink()
    return size, seconds


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--axiom", required=True, help="Path to axiomc executable.")
    parser.add_argument("--sevenzip", default=r"C:\Program Files\7-Zip\7z.exe")
    parser.add_argument("--members-dir", required=True,
                        help="Directory holding the twelve Silesia files.")
    parser.add_argument("--tar", required=True, help="Path to silesia.tar.")
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    axiom = Path(args.axiom).resolve()
    sevenzip = Path(args.sevenzip)
    members_dir = Path(args.members_dir)
    tar_path = Path(args.tar)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    for path, what in ((axiom, "axiomc"), (sevenzip, "7z"),
                       (members_dir, "members dir"), (tar_path, "tar")):
        if not path.exists():
            raise SystemExit(f"{what} not found: {path}")

    rows = []
    member_csv = output_dir / "silesia-gap-members.csv"
    with tempfile.TemporaryDirectory(prefix="axiom-gap-") as directory:
        temp = Path(directory)
        for member in MEMBERS:
            source = members_dir / member
            row = {"member": member, "input_bytes": source.stat().st_size}
            for label, arguments in MEMBER_AXIOM_CONFIGS:
                print(f"{member}: {label}...", file=sys.stderr, flush=True)
                size, seconds = axiom_row(axiom, source, label, arguments, temp)
                row[f"{label}_bytes"] = size
                row[f"{label}_seconds"] = f"{seconds:.3f}"
            for label, arguments in MEMBER_SEVENZIP_CONFIGS:
                print(f"{member}: {label}...", file=sys.stderr, flush=True)
                size, seconds = sevenzip_row(sevenzip, source, label, arguments, temp)
                row[f"{label}_bytes"] = size
                row[f"{label}_seconds"] = f"{seconds:.3f}"
            rows.append(row)
            with member_csv.open("w", newline="", encoding="utf-8") as file:
                writer = csv.DictWriter(file, fieldnames=list(rows[0].keys()))
                writer.writeheader()
                writer.writerows(rows)

        tar_rows = []
        tar_csv = output_dir / "silesia-gap-tar.csv"
        for label, arguments in TAR_AXIOM_CONFIGS:
            print(f"tar: {label}...", file=sys.stderr, flush=True)
            size, seconds = axiom_row(axiom, tar_path, label, arguments, temp)
            tar_rows.append({
                "config": label,
                "input_bytes": tar_path.stat().st_size,
                "archive_bytes": size,
                "ratio": f"{tar_path.stat().st_size / size:.6f}",
                "seconds": f"{seconds:.3f}",
            })
            with tar_csv.open("w", newline="", encoding="utf-8") as file:
                writer = csv.DictWriter(file, fieldnames=list(tar_rows[0].keys()))
                writer.writeheader()
                writer.writerows(tar_rows)

    total_input = sum(row["input_bytes"] for row in rows)
    print(f"\nPer-member results ({total_input} input bytes total):")
    header = (f"{'member':<10} {'input':>10} {'axiom9':>10} {'axiom9-t1':>10} "
              f"{'7z-mx5':>10} {'7z-mx9':>10} {'t1-mx9':>9} {'pct':>7}")
    print(header)
    totals = {key: 0 for key in
              ("axiom9_bytes", "axiom9-t1_bytes", "7z-mx5_bytes", "7z-mx9_bytes")}
    for row in sorted(rows, key=lambda r: r["axiom9-t1_bytes"] - r["7z-mx9_bytes"],
                      reverse=True):
        delta = row["axiom9-t1_bytes"] - row["7z-mx9_bytes"]
        pct = 100.0 * delta / row["7z-mx9_bytes"]
        print(f"{row['member']:<10} {row['input_bytes']:>10} "
              f"{row['axiom9_bytes']:>10} {row['axiom9-t1_bytes']:>10} "
              f"{row['7z-mx5_bytes']:>10} {row['7z-mx9_bytes']:>10} "
              f"{delta:>9} {pct:>6.2f}%")
        for key in totals:
            totals[key] += row[key]
    delta = totals["axiom9-t1_bytes"] - totals["7z-mx9_bytes"]
    pct = 100.0 * delta / totals["7z-mx9_bytes"]
    print(f"{'TOTAL':<10} {total_input:>10} {totals['axiom9_bytes']:>10} "
          f"{totals['axiom9-t1_bytes']:>10} {totals['7z-mx5_bytes']:>10} "
          f"{totals['7z-mx9_bytes']:>10} {delta:>9} {pct:>6.2f}%")

    print("\nTar block-size experiments:")
    for row in tar_rows:
        print(f"{row['config']:<22} {row['archive_bytes']:>10} bytes "
              f"ratio {row['ratio']} in {row['seconds']}s")


if __name__ == "__main__":
    main()
