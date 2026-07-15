#!/usr/bin/env python3
"""Round-trip-verified cross-codec benchmark for AxiomCompress."""

import argparse
import csv
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


BUNDLE_MAGIC = b"AXIOM_BENCH_DIR_1\0"
CHUNK_SIZE = 1024 * 1024


def find_executable(explicit, env_name, command, common_paths):
    if explicit:
        candidate = Path(explicit)
        if not candidate.is_file():
            raise SystemExit(f"{command} executable not found: {candidate}")
        return candidate.resolve()
    configured = os.environ.get(env_name)
    if configured and Path(configured).is_file():
        return Path(configured).resolve()
    found = shutil.which(command)
    if found:
        return Path(found).resolve()
    for candidate in common_paths:
        if candidate.is_file():
            return candidate.resolve()
    return None


def files_equal(left, right):
    if not left.is_file() or not right.is_file() or left.stat().st_size != right.stat().st_size:
        return False
    with left.open("rb") as left_file, right.open("rb") as right_file:
        while True:
            left_chunk = left_file.read(CHUNK_SIZE)
            right_chunk = right_file.read(CHUNK_SIZE)
            if left_chunk != right_chunk:
                return False
            if not left_chunk:
                return True


def build_directory_bundle(source, destination):
    files = sorted(path for path in source.rglob("*") if path.is_file())
    with destination.open("wb") as output:
        output.write(BUNDLE_MAGIC)
        output.write(struct.pack("<Q", len(files)))
        for path in files:
            relative = path.relative_to(source).as_posix().encode("utf-8")
            output.write(struct.pack("<Q", len(relative)))
            output.write(relative)
            output.write(struct.pack("<Q", path.stat().st_size))
            with path.open("rb") as input_file:
                shutil.copyfileobj(input_file, output, CHUNK_SIZE)
    return destination


def prepare_corpus(source, temp):
    if source.is_file():
        return source
    if source.is_dir():
        return build_directory_bundle(source, temp / "corpus.bundle")
    raise SystemExit(f"input does not exist: {source}")


def remove_output(path):
    if path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


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
        rendered = subprocess.list2cmdline([str(item) for item in command])
        raise SystemExit(f"command failed ({process.returncode}): {rendered}")
    return elapsed


def axiom_profiles(executable, levels, threads):
    profiles = []
    for level in levels:
        profiles.append({
            "label": f"axiom-{level}",
            "tool": "Axiom",
            "extension": ".axc",
            "compress": lambda source, archive, level=level: (
                [executable, "c", "--level", str(level), "--threads", str(threads), source, archive],
                None,
            ),
            "decompress": lambda archive, output: (
                [executable, "d", archive, output], None, output
            ),
        })
    return profiles


def zstd_profiles(executable, quick):
    if not executable:
        return []
    levels = [("zstd-1", ["-1", "-T0"]), ("zstd-3", ["-3", "-T0"])]
    if not quick:
        levels += [
            ("zstd-9", ["-9", "-T0"]),
            ("zstd-19", ["-19", "-T0"]),
            ("zstd-22-ultra", ["--ultra", "-22", "-T0"]),
        ]
    result = []
    for label, arguments in levels:
        result.append({
            "label": label,
            "tool": "zstd",
            "extension": ".zst",
            "compress": lambda source, archive, arguments=arguments: (
                [executable, "-q", "-f", *arguments, source, "-o", archive], None
            ),
            "decompress": lambda archive, output: (
                [executable, "-q", "-f", "-d", archive, "-o", output], None, output
            ),
        })
    return result


def lz4_profiles(executable, quick):
    if not executable:
        return []
    levels = [("lz4-1", "-1")]
    if not quick:
        levels.append(("lz4-9-hc", "-9"))
    result = []
    for label, argument in levels:
        result.append({
            "label": label,
            "tool": "LZ4",
            "extension": ".lz4",
            "compress": lambda source, archive, argument=argument: (
                [executable, "-q", "-f", argument, source, archive], None
            ),
            "decompress": lambda archive, output: (
                [executable, "-q", "-f", "-d", archive, output], None, output
            ),
        })
    return result


def sevenzip_profiles(executable, quick):
    if not executable:
        return []
    levels = [("7z-lzma2-mx5", ".7z", ["-t7z", "-m0=LZMA2", "-mx=5", "-mmt=on", "-ms=on"])]
    if not quick:
        levels += [
            ("7z-lzma2-mx9", ".7z", ["-t7z", "-m0=LZMA2", "-mx=9", "-mmt=on", "-ms=on"]),
            ("7z-bzip2-mx9", ".bz2", ["-tbzip2", "-mx=9", "-mmt=on"]),
            ("7z-gzip-mx9", ".gz", ["-tgzip", "-mx=9", "-mmt=on"]),
        ]
    result = []
    for label, extension, arguments in levels:
        def compress(source, archive, arguments=arguments):
            return ([executable, "a", "-bd", "-bso0", "-bsp0", "-y",
                     *arguments, archive, source.name], source.parent)

        def decompress(archive, output, source_name=None, extension=extension):
            output.mkdir()
            # Raw bzip2 streams do not preserve the input filename. 7-Zip
            # restores them using the archive stem, unlike gzip and 7z.
            restored_name = archive.stem if extension == ".bz2" else source_name
            return ([executable, "e", "-bd", "-bso0", "-bsp0", "-y",
                     f"-o{output}", archive], None, output / restored_name)

        result.append({
            "label": label,
            "tool": "7-Zip",
            "extension": extension,
            "compress": compress,
            "decompress": decompress,
            "sevenzip": True,
        })
    return result


def winrar_profiles(executable, quick):
    if not executable:
        return []
    levels = [("winrar-m3", ["-ma5", "-m3"])]
    if not quick:
        levels.append(("winrar-m5-128m", ["-ma5", "-m5", "-md128m"]))
    result = []
    for label, arguments in levels:
        def compress(source, archive, arguments=arguments):
            return ([executable, "a", "-idq", "-o+", *arguments,
                     archive, source.name], source.parent)

        def decompress(archive, output, source_name=None):
            output.mkdir()
            return ([executable, "x", "-idq", "-o+", archive,
                     str(output) + os.sep], None, output / source_name)

        result.append({
            "label": label,
            "tool": "WinRAR",
            "extension": ".rar",
            "compress": compress,
            "decompress": decompress,
            "directory_extract": True,
        })
    return result


def benchmark_profile(profile, corpus, temp, compress_repeats, decompress_repeats):
    archive = temp / f"{profile['label']}{profile['extension']}"
    compress_times = []
    archive_sizes = []
    for _ in range(compress_repeats):
        remove_output(archive)
        command, cwd = profile["compress"](corpus, archive)
        compress_times.append(run(command, cwd))
        archive_sizes.append(archive.stat().st_size)
    if len(set(archive_sizes)) != 1:
        raise SystemExit(f"{profile['label']} produced inconsistent archive sizes")

    decompress_times = []
    for repeat in range(decompress_repeats):
        output = temp / f"{profile['label']}-restore-{repeat}"
        remove_output(output)
        if profile.get("sevenzip") or profile.get("directory_extract"):
            command, cwd, restored = profile["decompress"](
                archive, output, source_name=corpus.name
            )
        else:
            command, cwd, restored = profile["decompress"](archive, output)
        decompress_times.append(run(command, cwd))
        if not files_equal(corpus, restored):
            raise SystemExit(f"{profile['label']} round-trip failed")
        remove_output(output)

    input_bytes = corpus.stat().st_size
    archive_bytes = archive_sizes[0]
    best_compress = min(compress_times)
    best_decompress = min(decompress_times)
    mib = input_bytes / (1024 * 1024)
    return {
        "codec": profile["label"],
        "tool": profile["tool"],
        "input_bytes": input_bytes,
        "archive_bytes": archive_bytes,
        "ratio": f"{input_bytes / archive_bytes:.6f}",
        "compress_seconds": f"{best_compress:.6f}",
        "decompress_seconds": f"{best_decompress:.6f}",
        "compress_mib_s": f"{mib / best_compress:.4f}",
        "decompress_mib_s": f"{mib / best_decompress:.4f}",
        "compress_repeats": compress_repeats,
        "decompress_repeats": decompress_repeats,
        "verified": True,
    }


def write_csv(path, rows):
    fields = list(rows[0].keys())
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(
        description=("Benchmark Axiom against available LZ4, zstd, 7-Zip, "
                     "and WinRAR codecs.")
    )
    parser.add_argument("--axiom", required=True, help="Path to axiomc executable.")
    parser.add_argument("--input", required=True, help="Input file or directory.")
    parser.add_argument("--output", help="Optional output CSV path.")
    parser.add_argument("--levels", default="1,2,3,4,5,6,7,8,9")
    parser.add_argument("--axiom-threads", type=int, default=0)
    parser.add_argument("--compress-repeats", type=int, default=2)
    parser.add_argument("--decompress-repeats", type=int, default=3)
    parser.add_argument("--zstd", help="Path to zstd; otherwise auto-detected.")
    parser.add_argument("--lz4", help="Path to lz4; otherwise auto-detected.")
    parser.add_argument("--sevenzip", help="Path to 7z; otherwise auto-detected.")
    parser.add_argument("--winrar", help="Path to Rar.exe; otherwise auto-detected.")
    parser.add_argument("--quick", action="store_true", help="Run a reduced profile set.")
    args = parser.parse_args()

    if args.compress_repeats < 1 or args.decompress_repeats < 1:
        raise SystemExit("repeat counts must be positive")
    levels = [int(value) for value in args.levels.split(",") if value]
    if args.quick and args.levels == "1,2,3,4,5,6,7,8,9":
        levels = [1, 5, 9]
    if not levels or any(level < 1 or level > 9 for level in levels):
        raise SystemExit("--levels must contain values from 1 through 9")

    axiom = find_executable(args.axiom, "AXIOMC", "axiomc", [])
    zstd = find_executable(args.zstd, "ZSTD", "zstd", [
        Path(r"C:\Program Files\PeaZip\res\bin\zstd\zstd.exe")
    ])
    lz4 = find_executable(args.lz4, "LZ4", "lz4", [])
    sevenzip = find_executable(args.sevenzip, "SEVENZIP", "7z", [
        Path(r"C:\Program Files\7-Zip\7z.exe"),
        Path(r"C:\Program Files (x86)\7-Zip\7z.exe"),
    ])
    winrar = find_executable(args.winrar, "WINRAR", "rar", [
        Path(r"C:\Program Files\WinRAR\Rar.exe"),
        Path(r"C:\Program Files (x86)\WinRAR\Rar.exe"),
    ])

    for name, executable in (("LZ4", lz4), ("zstd", zstd),
                             ("7-Zip", sevenzip), ("WinRAR", winrar)):
        if not executable:
            print(f"Skipping {name}: executable not found", file=sys.stderr)

    source = Path(args.input).resolve()
    with tempfile.TemporaryDirectory(prefix="axiom-codec-bench-") as directory:
        temp = Path(directory)
        corpus = prepare_corpus(source, temp)
        profiles = axiom_profiles(axiom, levels, args.axiom_threads)
        profiles += lz4_profiles(lz4, args.quick)
        profiles += zstd_profiles(zstd, args.quick)
        profiles += sevenzip_profiles(sevenzip, args.quick)
        profiles += winrar_profiles(winrar, args.quick)
        rows = []
        for profile in profiles:
            print(f"Benchmarking {profile['label']}...", file=sys.stderr)
            rows.append(benchmark_profile(
                profile, corpus, temp, args.compress_repeats, args.decompress_repeats
            ))
            if args.output:
                # Preserve every verified row so a late external-tool failure
                # does not discard several minutes of valid benchmark data.
                write_csv(Path(args.output), rows)

    fields = list(rows[0].keys())
    writer = csv.DictWriter(sys.stdout, fieldnames=fields, lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)


if __name__ == "__main__":
    main()
