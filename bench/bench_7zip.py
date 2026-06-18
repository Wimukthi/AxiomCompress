#!/usr/bin/env python3
import argparse
import csv
import ctypes
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
COMMON_7ZIP_PATHS = (
    Path(r"C:\Program Files\7-Zip\7z.exe"),
    Path(r"C:\Program Files (x86)\7-Zip\7z.exe"),
)


def find_7zip():
    configured = os.environ.get("SEVENZIP")
    if configured:
        return configured

    on_path = shutil.which("7z")
    if on_path:
        return on_path

    for candidate in COMMON_7ZIP_PATHS:
        if candidate.exists():
            return str(candidate)

    return None


def file_size(path):
    return Path(path).stat().st_size


def folder_files(root):
    return sorted(path for path in root.rglob("*") if path.is_file())


def folder_content_size(root):
    return sum(path.stat().st_size for path in folder_files(root))


def files_equal(left, right):
    with Path(left).open("rb") as left_file, Path(right).open("rb") as right_file:
        while True:
            left_chunk = left_file.read(CHUNK_SIZE)
            right_chunk = right_file.read(CHUNK_SIZE)
            if left_chunk != right_chunk:
                return False
            if not left_chunk:
                return True


def build_directory_bundle(source, destination):
    files = folder_files(source)

    with destination.open("wb") as output:
        output.write(BUNDLE_MAGIC)
        output.write(struct.pack("<Q", len(files)))

        for path in files:
            rel = path.relative_to(source).as_posix().encode("utf-8")
            output.write(struct.pack("<Q", len(rel)))
            output.write(rel)
            output.write(struct.pack("<Q", path.stat().st_size))

            with path.open("rb") as input_file:
                shutil.copyfileobj(input_file, output, CHUNK_SIZE)

    return {
        "input_kind": "directory",
        "file_count": len(files),
        "content_bytes": folder_content_size(source),
        "corpus_path": destination,
        "corpus_bytes": file_size(destination),
    }


def prepare_corpus(source, temp):
    if source.is_file():
        return {
            "input_kind": "file",
            "file_count": 1,
            "content_bytes": file_size(source),
            "corpus_path": source,
            "corpus_bytes": file_size(source),
        }

    if source.is_dir():
        return build_directory_bundle(source, temp / "corpus.bundle")

    raise SystemExit(f"input does not exist: {source}")


def peak_working_set_bytes(pid):
    if os.name != "nt":
        return 0

    class ProcessMemoryCounters(ctypes.Structure):
        _fields_ = [
            ("cb", ctypes.c_ulong),
            ("PageFaultCount", ctypes.c_ulong),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
        ]

    process_query_information = 0x0400
    process_vm_read = 0x0010

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)

    handle = kernel32.OpenProcess(process_query_information | process_vm_read, False, pid)
    if not handle:
        return 0

    try:
        counters = ProcessMemoryCounters()
        counters.cb = ctypes.sizeof(counters)
        ok = psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb)
        return int(counters.PeakWorkingSetSize) if ok else 0
    finally:
        kernel32.CloseHandle(handle)


def run(command, cwd=None):
    started = time.perf_counter()
    process = subprocess.Popen(
        command,
        cwd=cwd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    peak_bytes = 0
    while process.poll() is None:
        peak_bytes = max(peak_bytes, peak_working_set_bytes(process.pid))
        time.sleep(0.01)

    peak_bytes = max(peak_bytes, peak_working_set_bytes(process.pid))
    elapsed = time.perf_counter() - started

    if process.returncode != 0:
        raise subprocess.CalledProcessError(process.returncode, command)

    return elapsed, peak_bytes


def sevenzip_compress(sevenzip, source, archive):
    # Use the same byte stream Axiom sees. Native multi-file 7-Zip comparison
    # belongs after Axiom has a real multi-file solid archive container.
    return run(
        [
            str(sevenzip),
            "a",
            "-t7z",
            "-m0=LZMA2",
            "-mx=9",
            "-ms=on",
            str(archive),
            source.name,
        ],
        cwd=source.parent,
    )


def sevenzip_decompress(sevenzip, archive, output_dir):
    return run([str(sevenzip), "x", "-y", f"-o{output_dir}", str(archive)])


def axiom_compress_command(axiom, source, archive, threads, block_size, force_parallel,
                           optimal, optimal_depth, optimal_candidates):
    command = [str(axiom), "c"]

    if threads is not None:
        command.extend(["--threads", str(threads)])
    if block_size is not None:
        command.extend(["--block-size", str(block_size)])
    if force_parallel:
        command.append("--parallel")
    if optimal:
        command.append("--optimal")
    if optimal_depth is not None:
        command.extend(["--optimal-depth", str(optimal_depth)])
    if optimal_candidates is not None:
        command.extend(["--optimal-candidates", str(optimal_candidates)])

    command.extend([str(source), str(archive)])
    return command


def main():
    parser = argparse.ArgumentParser(description="Compare AxiomCompress against 7-Zip.")
    parser.add_argument("--axiom", required=True, help="Path to axiomc executable.")
    parser.add_argument("--input", required=True, help="Input file or directory to benchmark.")
    parser.add_argument("--sevenzip", default=find_7zip())
    parser.add_argument("--axiom-threads", type=int, default=0,
                        help="Axiom worker threads; 0 means hardware default.")
    parser.add_argument("--axiom-block-size", default="4M",
                        help="Axiom block size passed to --block-size.")
    parser.add_argument("--axiom-parallel", action="store_true",
                        help="Force Axiom's independent-block threaded archive format.")
    parser.add_argument("--axiom-optimal", action="store_true",
                        help="Enable Axiom's bounded optimal parser.")
    parser.add_argument("--axiom-optimal-depth", type=int, default=None,
                        help="Axiom optimal parser hash-chain depth; implies --optimal.")
    parser.add_argument("--axiom-optimal-candidates", type=int, default=None,
                        help="Axiom optimal parser match candidates per position; implies --optimal.")
    args = parser.parse_args()

    if not args.sevenzip:
        raise SystemExit("7z was not found. Set SEVENZIP or put 7z on PATH.")

    axiom = Path(args.axiom).resolve()
    sevenzip = Path(args.sevenzip).resolve()
    source = Path(args.input).resolve()

    if not axiom.exists():
        raise SystemExit(f"axiom executable not found: {axiom}")
    if not sevenzip.exists():
        raise SystemExit(f"7z executable not found: {sevenzip}")

    with tempfile.TemporaryDirectory() as temp_dir:
        temp = Path(temp_dir)
        corpus = prepare_corpus(source, temp)
        corpus_path = corpus["corpus_path"]

        axiom_archive = temp / "test.axc"
        axiom_restored = temp / "test.restored"
        seven_archive = temp / "test.7z"
        seven_extract_dir = temp / "seven"

        axiom_command = axiom_compress_command(
            axiom,
            corpus_path,
            axiom_archive,
            args.axiom_threads,
            args.axiom_block_size,
            args.axiom_parallel,
            args.axiom_optimal,
            args.axiom_optimal_depth,
            args.axiom_optimal_candidates,
        )

        axiom_c_time, axiom_c_peak = run(axiom_command)
        axiom_d_time, axiom_d_peak = run([str(axiom), "d", str(axiom_archive), str(axiom_restored)])

        if not files_equal(corpus_path, axiom_restored):
            raise SystemExit("Axiom round-trip failed")

        seven_c_time, seven_c_peak = sevenzip_compress(sevenzip, corpus_path, seven_archive)
        seven_d_time, seven_d_peak = sevenzip_decompress(sevenzip, seven_archive, seven_extract_dir)

        seven_restored = seven_extract_dir / corpus_path.name
        if not files_equal(corpus_path, seven_restored):
            raise SystemExit("7-Zip round-trip failed")

        row = {
            "input": str(source),
            "input_kind": corpus["input_kind"],
            "file_count": corpus["file_count"],
            "content_bytes": corpus["content_bytes"],
            "corpus_bytes": corpus["corpus_bytes"],
            "axiom_bytes": file_size(axiom_archive),
            "sevenzip_bytes": file_size(seven_archive),
            "axiom_ratio": f"{file_size(axiom_archive) / corpus['corpus_bytes']:.6f}",
            "sevenzip_ratio": f"{file_size(seven_archive) / corpus['corpus_bytes']:.6f}",
            "axiom_c_sec": f"{axiom_c_time:.6f}",
            "axiom_d_sec": f"{axiom_d_time:.6f}",
            "sevenzip_c_sec": f"{seven_c_time:.6f}",
            "sevenzip_d_sec": f"{seven_d_time:.6f}",
            "axiom_c_peak_bytes": axiom_c_peak,
            "axiom_d_peak_bytes": axiom_d_peak,
            "sevenzip_c_peak_bytes": seven_c_peak,
            "sevenzip_d_peak_bytes": seven_d_peak,
            "axiom_threads": args.axiom_threads,
            "axiom_block_size": args.axiom_block_size,
            "axiom_parallel": args.axiom_parallel,
            "axiom_optimal": args.axiom_optimal or args.axiom_optimal_depth is not None or args.axiom_optimal_candidates is not None,
            "axiom_optimal_depth": args.axiom_optimal_depth,
            "axiom_optimal_candidates": args.axiom_optimal_candidates,
        }

        writer = csv.DictWriter(sys.stdout, fieldnames=list(row.keys()), lineterminator="\n")
        writer.writeheader()
        writer.writerow(row)


if __name__ == "__main__":
    main()
