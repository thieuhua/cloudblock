#!/usr/bin/env python3
"""Bounded, low-memory filesystem test for the mounted CloudBlock NBD volume.

It never formats, mounts, detaches or kills the NBD process.
Default smoke mode writes only 16 MiB and cleans its named test file.
Persistent prepare/verify/cleanup supports a controlled remount test.
Optional pressure mode exercises 70 sparse ext4 file offsets, and is explicitly
opt-in due to permanent backing chunk allocation without TRIM.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import resource
import secrets
import subprocess
import sys
import time

MIB = 1024 * 1024
BLOCK = 128 * 1024
PATCH = 4096
CHUNK = 16 * MIB
MOUNT = Path("/mnt/cloudblock")
ROOT = Path(__file__).resolve().parents[1]
BACKING = ROOT / "nbd-volume"
HOST_STATE = ROOT / ".run" / "safe_disk_test.json"
TEST_PREFIX = ".cloudblock-safe-test-"
SEED = b"CloudBlock-safe-probe-v1"
RESERVE = 8 * 1024 * MIB    # never knowingly cross this host-free threshold
MIN_AVAILABLE_RAM = 96 * MIB
DEFAULT_MIB = 16
MAX_SMOKE_MIB = 64
SMOKE_GROWTH_LIMIT = 512 * MIB
PRESSURE_GROWTH_LIMIT = 1600 * MIB  # tight budget for 70 physical 16 MiB chunks
PRESSURE_OFFSETS = 70


class SafeAbort(Exception):
    pass


def fail(reason: str):
    raise SafeAbort(reason)


def mem_available() -> int:
    try:
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemAvailable:"):
                return int(line.split()[1]) * 1024
    except (OSError, IndexError, ValueError):
        pass
    fail("Cannot read Linux MemAvailable; fail closed")


def free_host() -> int:
    vfs = os.statvfs(BACKING)
    return vfs.f_bavail * vfs.f_frsize


def allocated_bytes(path: Path) -> int:
    total = 0
    for folder, _, filenames in os.walk(path, followlinks=False):
        for name in filenames:
            entry = Path(folder) / name
            if entry.is_symlink():
                fail(f"Symlink encountered in backing: {entry}")
            total += entry.stat().st_blocks * 512
    return total


def probe_mount():
    if not BACKING.is_dir():
        fail(f"Missing backing directory: {BACKING}")
    if MOUNT.is_symlink() or not MOUNT.is_dir():
        fail(f"Mountpoint missing or symlink: {MOUNT}")
    p = subprocess.run(
        ["findmnt", "-n", "-o", "TARGET,SOURCE,FSTYPE,OPTIONS", "--target", str(MOUNT)],
        text=True, capture_output=True, check=False, timeout=4
    )
    parts = p.stdout.split()
    if p.returncode or len(parts) < 4 or parts[:3] != [str(MOUNT), "/dev/nbd0", "ext4"]:
        fail("Expected direct rw ext4 mount on /dev/nbd0, found: " + p.stdout.strip())
    if "rw" not in parts[3].split(","):
        fail("CloudBlock mounted read-only")
    if os.stat(MOUNT).st_dev == os.stat(BACKING).st_dev:
        fail("Host backing and target mount are on the same filesystem!")
    if not os.access(MOUNT, os.W_OK | os.X_OK):
        fail(f"No write permission on {MOUNT} for {os.geteuid()}")


def report(prefix: str):
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    disk = allocated_bytes(BACKING / "disk")
    cache = allocated_bytes(BACKING / "cache")
    print(
        f"{prefix}: host_free={free_host()/MIB:.0f}MiB "
        f"available_RAM={mem_available()/MIB:.0f}MiB "
        f"python_peak_RSS={rss/1024:.1f}MiB "
        f"backing={disk/MIB:.0f}MiB cache={cache/MIB:.0f}MiB",
        flush=True,
    )


class Budget:
    def __init__(self, max_growth: int):
        self.max_growth = max_growth
        self.initial_free = free_host()
        self.initial_alloc = allocated_bytes(BACKING)
        self.last_report = 0.0
        if mem_available() < MIN_AVAILABLE_RAM:
            fail(f"Available RAM below {MIN_AVAILABLE_RAM // MIB} MiB")
        if self.initial_free < RESERVE + max_growth:
            fail(
                f"Insufficient host space for budget: free {self.initial_free//MIB}MiB; "
                f"require reserve {RESERVE//MIB}MiB + budget {max_growth//MIB}MiB"
            )
        report("PRECHECK")

    def check(self):
        available = mem_available()
        free = free_host()
        alloc = allocated_bytes(BACKING)
        growth = max(0, alloc - self.initial_alloc)
        if available < MIN_AVAILABLE_RAM:
            fail(f"RAM safeguard: MemAvailable {available//MIB}MiB < {MIN_AVAILABLE_RAM//MIB}MiB")
        if free < RESERVE:
            fail(f"Disk safeguard: host free {free//MIB}MiB < {RESERVE//MIB}MiB")
        if growth > self.max_growth:
            fail(f"Disk safeguard: backing growth {growth//MIB}MiB > "
                 f"{self.max_growth//MIB}MiB budget")
        if time.monotonic() - self.last_report >= 15:
            report("CHECK")
            self.last_report = time.monotonic()


def pattern(index: int) -> bytes:
    return hashlib.shake_256(SEED + index.to_bytes(8, "big")).digest(BLOCK)


def patches(size: int) -> dict[int, bytes]:
    rng = random.Random(20261020)
    n = min(32, size // PATCH)
    offsets = rng.sample(range(size // PATCH), n)
    result = {}
    for i, block in enumerate(offsets):
        result[block * PATCH] = hashlib.shake_256(
            SEED + b"patch" + i.to_bytes(4, "big")
        ).digest(PATCH)
    return result


def want_block(index: int, size: int, edits: dict[int, bytes]) -> bytes:
    offset = index * BLOCK
    n = min(BLOCK, size - offset)
    buf = bytearray(pattern(index)[:n])
    for patch_off, data in edits.items():
        if offset <= patch_off < offset + n:
            pos = patch_off - offset
            buf[pos:pos + PATCH] = data
    return bytes(buf)


def advise_drop(fd: int):
    # Advisory: reclaim the benchmark file's page-cache pages after fsync.
    if hasattr(os, "posix_fadvise") and hasattr(os, "POSIX_FADV_DONTNEED"):
        try:
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        except OSError:
            pass


def write_dataset(path: Path, size: int, budget: Budget):
    print(f"Writing {size//MIB}MiB using only {BLOCK//1024}KiB buffers", flush=True)
    edits = patches(size)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    fd = os.open(path, flags, 0o600)
    try:
        for idx in range((size + BLOCK - 1) // BLOCK):
            data = pattern(idx)[:min(BLOCK, size - idx * BLOCK)]
            offset = 0
            while offset < len(data):
                n = os.write(fd, data[offset:])
                if n <= 0:
                    fail("Short/zero file write")
                offset += n
            if (idx + 1) % 8 == 0:  # every MiB; does not flush unrelated files
                os.fsync(fd)
                advise_drop(fd)
                budget.check()
        os.fsync(fd)
        for off, data in sorted(edits.items()):
            pos = os.lseek(fd, off, os.SEEK_SET)
            if pos != off:
                fail("Seek failed")
            n = os.write(fd, data)
            if n != len(data):
                fail("Short patch write")
        os.fsync(fd)
        advise_drop(fd)
        budget.check()
    finally:
        os.close(fd)
    print(f"WRITE PASS: {size//MIB}MiB; {len(edits)} random 4KiB overwrites", flush=True)


def verify_dataset(path: Path, size: int, budget: Budget | None):
    edits = patches(size)
    actual = hashlib.sha256()
    expected = hashlib.sha256()
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    try:
        if os.fstat(fd).st_size != size:
            fail(f"Wrong test file length: {os.fstat(fd).st_size} vs {size}")
        for index in range((size + BLOCK - 1) // BLOCK):
            n = min(BLOCK, size - index * BLOCK)
            data = bytearray()
            while len(data) < n:
                chunk = os.read(fd, n - len(data))
                if not chunk:
                    fail(f"Unexpected EOF at block {index}")
                data.extend(chunk)
            reference = want_block(index, size, edits)
            if data != reference:
                fail(f"DATA MISMATCH at logical file offset {index * BLOCK}")
            actual.update(data)
            expected.update(reference)
            if index % 8 == 7:
                advise_drop(fd)
                if budget:
                    budget.check()
    finally:
        os.close(fd)
    if actual.digest() != expected.digest():
        fail("SHA-256 mismatch")
    print(f"VERIFY PASS: sha256={actual.hexdigest()} ({size//MIB}MiB)", flush=True)


def ensure_no_checkpoint():
    if HOST_STATE.exists():
        fail(f"Existing retained test state: {HOST_STATE}. Use verify/cleanup first.")


def make_probe(kind: str, size: int) -> tuple[Path, str]:
    name = TEST_PREFIX + secrets.token_hex(8)
    folder = MOUNT / name
    folder.mkdir(mode=0o700)
    token = secrets.token_hex(16)
    metadata = {"version": 1, "token": token, "directory": name,
                "kind": kind, "size": size}
    marker = folder / "manifest.json"
    with marker.open("x") as fp:
        json.dump(metadata, fp)
        fp.flush()
        os.fsync(fp.fileno())
    return folder, token


def load_checkpoint() -> tuple[Path, dict]:
    try:
        state = json.loads(HOST_STATE.read_text())
    except (OSError, ValueError) as exc:
        fail(f"No usable checkpoint file: {exc}")
    name = state.get("directory", "")
    if not isinstance(name, str) or not name.startswith(TEST_PREFIX):
        fail("Unsafe test directory in state")
    if "/" in name or ".." in name:
        fail("Invalid test path")
    folder = MOUNT / name
    if folder.is_symlink() or not folder.is_dir():
        fail(f"Retained test directory missing: {folder}")
    marker = folder / "manifest.json"
    if marker.is_symlink():
        fail("Refusing symlink marker")
    try:
        actual = json.loads(marker.read_text())
    except (OSError, ValueError) as exc:
        fail(f"Missing/invalid marker: {exc}")
    if state != actual or state.get("version") != 1:
        fail("Host/volume checkpoint token mismatch: refusing to edit")
    return folder, state


def save_checkpoint(folder: Path):
    ensure_no_checkpoint()
    metadata = json.loads((folder / "manifest.json").read_text())
    HOST_STATE.parent.mkdir(exist_ok=True)
    with HOST_STATE.open("x") as fp:
        json.dump(metadata, fp)
        fp.flush()
        os.fsync(fp.fileno())


def safe_cleanup(folder: Path, token: str, persisted: bool):
    if folder.is_symlink() or not folder.is_dir() or folder.parent != MOUNT:
        fail("Refusing to clean unsafe test directory")
    marker = folder / "manifest.json"
    if marker.is_symlink():
        fail("Marker is symlink")
    meta = json.loads(marker.read_text())
    if meta.get("token") != token or meta.get("directory") != folder.name:
        fail("Probe marker mismatch; refusing cleanup")
    allowed = {"manifest.json", "data.bin", "pressure.bin"}
    names = {x.name for x in folder.iterdir()}
    if not names.issubset(allowed):
        fail(f"Unknown files present: {names - allowed}; refusing cleanup")
    for name in ["data.bin", "pressure.bin", "manifest.json"]:
        p = folder / name
        if p.is_symlink():
            fail("Refusing to remove symlink " + name)
        if p.is_file():
            p.unlink()
    folder.rmdir()
    if persisted:
        HOST_STATE.unlink(missing_ok=True)
    print("CLEANUP PASS (filesystem files deleted; allocated CloudBlock chunks are NOT reclaimed without TRIM)", flush=True)


def cache_ids() -> set[int]:
    return {int(p.stem) for p in (BACKING / "cache").glob("*.bin")
            if p.stem.isdecimal()}


def pressure_test(folder: Path, budget: Budget):
    path = folder / "pressure.bin"
    initial = cache_ids()
    observed = set(initial)
    evicted = set()
    baseline_disk_files = len(list((BACKING / "disk").glob("*.bin")))
    print(f"PRESSURE: {PRESSURE_OFFSETS} sparse 4KiB writes spaced 16MiB apart; "
          f"initial_cache={len(initial)}/64 backing_chunks={baseline_disk_files}", flush=True)
    with path.open("xb", buffering=0) as f:
        for i in range(PRESSURE_OFFSETS):
            off = i * CHUNK
            f.seek(off)
            data = hashlib.shake_256(SEED + b"pressure" + i.to_bytes(4, "big")).digest(PATCH)
            if f.write(data) != PATCH:
                fail("Short pressure write")
            f.flush()
            os.fsync(f.fileno())
            advise_drop(f.fileno())
            budget.check()
            current = cache_ids()
            if len(current) > 64:
                fail(f"Cache occupancy exceeds 64 chunks: {len(current)}")
            evicted.update(observed - current)
            observed.update(current)
            if i % 10 == 9:
                print(f"  {i+1}/{PRESSURE_OFFSETS} offsets written; "
                      f"cache={len(current)}/64, observed_evictions={len(evicted)}", flush=True)
    with path.open("rb", buffering=0) as f:
        for i in range(PRESSURE_OFFSETS):
            f.seek(i * CHUNK)
            want = hashlib.shake_256(SEED + b"pressure" + i.to_bytes(4, "big")).digest(PATCH)
            got = f.read(PATCH)
            if got != want:
                fail(f"Pressure offset {i} mismatch")
            budget.check()
    post_cache = cache_ids()
    disk_ids = {int(p.stem) for p in (BACKING / "disk").glob("*.bin")
                if p.stem.isdecimal()}
    missing_backing = sorted(evicted - disk_ids)
    if missing_backing:
        fail(f"Evicted chunks missing backing copies: {missing_backing[:10]}")
    print(f"EVICTION EVIDENCE: observed_unique_cached={len(observed)}, "
          f"evicted_ids={len(evicted)}, final_cache={len(post_cache)}/64, "
          f"backing_chunks={len(disk_ids)} (+{len(disk_ids) - baseline_disk_files})", flush=True)
    if not evicted:
        fail("Sparse ext4 writes did not prove eviction; no cached chunk disappeared")
    print("PRESSURE PASS: evictions observed; all sparse offsets read back correctly", flush=True)
    report("PRESSURE")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("command", nargs="?", default="smoke",
                    choices=("smoke", "prepare", "verify", "cleanup", "pressure"))
    ap.add_argument("--size-mib", type=int, default=DEFAULT_MIB,
                    help="smoke/prepare actual file size, maximum 64 MiB, default 16")
    ap.add_argument("--confirm-pressure", action="store_true",
                    help="required for pressure mode; backing allocation persists after deletion")
    args = ap.parse_args()
    if args.size_mib < 1 or args.size_mib > MAX_SMOKE_MIB:
        fail("size-mib must be 1..64; this is a bounded test")
    probe_mount()
    if args.command in ("verify", "cleanup"):
        folder, state = load_checkpoint()
        if args.command == "verify":
            print(f"VERIFY retained {state['directory']}", flush=True)
            verify_dataset(folder / "data.bin", state["size"], None)
            report("AFTER VERIFY")
        else:
            safe_cleanup(folder, state["token"], persisted=True)
            report("AFTER CLEANUP")
        return

    ensure_no_checkpoint()
    if args.command == "pressure" and not args.confirm_pressure:
        fail("Pressure test is opt-in; pass --confirm-pressure after reading warning.")
    max_growth = PRESSURE_GROWTH_LIMIT if args.command == "pressure" else SMOKE_GROWTH_LIMIT
    budget = Budget(max_growth)
    folder, token = make_probe(args.command, args.size_mib * MIB)
    retain = args.command == "prepare"
    try:
        if args.command == "pressure":
            pressure_test(folder, budget)
        else:
            data = folder / "data.bin"
            write_dataset(data, args.size_mib * MIB, budget)
            verify_dataset(data, args.size_mib * MIB, budget)
        if retain:
            save_checkpoint(folder)
            print(f"CHECKPOINT PREPARED: {folder}; after remount run 'verify', then 'cleanup'")
        else:
            safe_cleanup(folder, token, persisted=False)
        report("FINAL")
    except BaseException:
        if retain:
            # Retain even on interruption: create a local checkpoint for recovery.
            if not HOST_STATE.exists():
                try:
                    save_checkpoint(folder)
                except Exception as exc:
                    print("WARNING: cannot save checkpoint:", exc, file=sys.stderr)
            print("Retained partial test data for controlled cleanup.", file=sys.stderr)
        else:
            try:
                safe_cleanup(folder, token, persisted=False)
            except Exception as exc:
                print("WARNING: cleanup failed:", exc, file=sys.stderr)
        raise


if __name__ == "__main__":
    try:
        main()
    except (SafeAbort, OSError, ValueError, subprocess.TimeoutExpired) as exc:
        print(f"ABORT: {exc}", file=sys.stderr, flush=True)
        sys.exit(1)
