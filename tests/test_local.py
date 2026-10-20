#!/usr/bin/env python3
"""Independent CLI tests. No external modules and no persistent test data."""
import hashlib
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

EXE = str(pathlib.Path(sys.argv[1]).resolve()) if len(sys.argv) > 1 else "./build/cloudblock"
sys.argv = sys.argv[:1]
MIB = 1024 * 1024
CHUNK = 16 * MIB
VOLUME = 64 * 1024 * MIB


class CloudBlockLocalTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="cloudblock-test-")
        self.root = pathlib.Path(self.tmp.name) / "volume"

    def tearDown(self):
        self.tmp.cleanup()

    def run_cli(self, *args, cache=32, ok=True):
        cmd = [EXE, str(self.root), "--cache-mib", str(cache), *map(str, args)]
        p = subprocess.run(cmd, text=True, capture_output=True, timeout=60)
        if ok:
            self.assertEqual(p.returncode, 0, msg=f"{cmd}: {p.stderr}\n{p.stdout}")
        else:
            self.assertNotEqual(p.returncode, 0, msg=f"expected failure: {cmd}")
        return p.stdout.strip()

    def status(self, **kwargs):
        output = self.run_cli("status", **kwargs)
        return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", output)}

    def test_default_cap_and_sparse_zero_read(self):
        proc = subprocess.run([EXE, str(self.root), "status"],
                              check=True, capture_output=True, text=True)
        self.assertIn("cache_limit_bytes=1073741824", proc.stdout)
        self.assertEqual(self.run_cli("read", 0, 4), "00000000")
        self.assertEqual(self.run_cli("read", VOLUME - 4, 4), "00000000")
        self.assertEqual(self.status()["cached_chunks"], 0)
        self.assertEqual(self.status()["disk_chunks"], 0)

    def test_write_across_chunk_and_restart(self):
        self.assertEqual(self.run_cli("write", CHUNK - 2, "deadbeef"),
                         "written=4")
        self.assertEqual(self.run_cli("read", CHUNK - 4, 8),
                         "0000deadbeef0000")
        self.assertEqual(self.status()["cached_chunks"], 2)
        self.assertEqual(self.run_cli("read", CHUNK - 2, 4), "deadbeef")
        self.run_cli("flush")
        self.assertEqual(self.status()["disk_chunks"], 2)
        self.assertEqual(self.run_cli("read", CHUNK - 2, 4), "deadbeef")

    def test_lru_eviction_with_dirty_chunk(self):
        for id_, hexbytes in [(0, "aa"), (1, "bb"), (2, "cc")]:
            self.run_cli("write", id_ * CHUNK + 99, hexbytes)
        s = self.status()
        self.assertLessEqual(s["cached_chunks"], 2)
        self.assertGreaterEqual(s["disk_chunks"], 1)
        for id_, hexbytes in [(0, "aa"), (1, "bb"), (2, "cc")]:
            self.assertEqual(self.run_cli("read", id_ * CHUNK + 99, 1), hexbytes)
        self.run_cli("flush")
        self.assertEqual(self.status()["disk_chunks"], 3)

    def test_34mib_binary_roundtrip(self):
        source = pathlib.Path(self.tmp.name) / "input.bin"
        target = pathlib.Path(self.tmp.name) / "output.bin"
        data = (bytes(range(256)) * (34 * MIB // 256)) + b"END"
        source.write_bytes(data)
        offset = 379
        self.run_cli("put", offset, source)
        self.run_cli("get", offset, len(data), target)
        self.assertEqual(hashlib.sha256(target.read_bytes()).digest(),
                         hashlib.sha256(data).digest())
        self.run_cli("flush")
        target.unlink()
        self.run_cli("get", offset, len(data), target)
        self.assertEqual(hashlib.sha256(target.read_bytes()).digest(),
                         hashlib.sha256(data).digest())

    def test_ranges_and_invalid_hex(self):
        self.run_cli("write", VOLUME, "ff", ok=False)
        self.run_cli("read", VOLUME - 1, 2, ok=False)
        self.run_cli("write", 0, "a", ok=False)
        self.run_cli("write", 0, "zz", ok=False)
        self.run_cli("status", cache=7, ok=False)

    def test_reject_corrupt_chunk(self):
        self.run_cli("write", 0, "ab")
        (self.root / "cache" / "0.bin").write_bytes(b"BAD")
        self.run_cli("read", 0, 1, ok=False)


if __name__ == "__main__":
    unittest.main(verbosity=2)
