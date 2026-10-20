#!/usr/bin/env python3
"""Real TCP NBD negotiation and block I/O tests (no /dev/nbd, no root required)."""
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

EXE = str(pathlib.Path(sys.argv[1]).resolve()) if len(sys.argv) > 1 else "./build/cloudblock-nbd"
CLI = str(pathlib.Path(EXE).with_name("cloudblock"))
sys.argv = sys.argv[:1]
OPT_MAGIC = 0x49484156454F5054
OPT_REPLY_MAGIC = 0x0003E889045565A9
REQUEST_MAGIC = 0x25609513
REPLY_MAGIC = 0x67446698
VOLUME = 64 * 1024 * 1024 * 1024
CHUNK = 16 * 1024 * 1024

def recvall(sock, size):
    result = bytearray()
    while len(result) < size:
        data = sock.recv(size - len(result))
        if not data:
            raise EOFError(f"peer disconnected after {len(result)}/{size} bytes")
        result.extend(data)
    return bytes(result)


def option(sock, number, payload=b""):
    sock.sendall(struct.pack("!QII", OPT_MAGIC, number, len(payload)) + payload)


def opt_reply(sock):
    magic, opt, kind, length = struct.unpack("!QIII", recvall(sock, 20))
    if magic != OPT_REPLY_MAGIC:
        raise AssertionError(f"invalid opt reply magic {magic:x}")
    return opt, kind, recvall(sock, length)


class Client:
    def __init__(self, host, port, legacy=False):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.settimeout(10)
        magic, optmagic, flags = struct.unpack("!QQH", recvall(self.sock, 18))
        assert magic == 0x4E42444D41474943
        assert optmagic == OPT_MAGIC and flags & 1
        self.sock.sendall(struct.pack("!I", 1 if legacy else 3))
        self.legacy = legacy
        self.seq = 0

    def negotiation_go(self, name="", infos=(0, 1, 3)):
        name = name.encode()
        payload = struct.pack("!I", len(name)) + name + struct.pack("!H", len(infos))
        payload += b"".join(struct.pack("!H", i) for i in infos)
        option(self.sock, 7, payload)
        found = {}
        while True:
            opt, kind, data = opt_reply(self.sock)
            assert opt == 7
            if kind == 1:
                assert 0 in found
                size, flags = struct.unpack("!QH", found[0])
                assert size == VOLUME and flags & 1 and flags & 4 and flags & 8
                return found
            assert kind == 3, (kind, data)
            info_type = struct.unpack("!H", data[:2])[0]
            found[info_type] = data[2:]

    def legacy_export(self):
        option(self.sock, 1)
        size, flags = struct.unpack("!QH", recvall(self.sock, 10))
        assert size == VOLUME and flags & 4
        assert recvall(self.sock, 124) == bytes(124)

    def request(self, cmd, off=0, size=0, data=b"", flags=0):
        self.seq += 1
        cookie = self.seq.to_bytes(8, "big")
        body = struct.pack("!IHH8sQI", REQUEST_MAGIC, flags, cmd, cookie, off, size)
        self.sock.sendall(body + data)
        if cmd == 2:
            return None
        magic, error, got_cookie = struct.unpack("!II8s", recvall(self.sock, 16))
        assert magic == REPLY_MAGIC and cookie == got_cookie
        if error != 0:
            return error
        if cmd == 0:
            return recvall(self.sock, size)
        return 0

    def close(self):
        try:
            self.request(2)
        except OSError:
            pass
        self.sock.close()


class NBDProtocolTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="cb-nbd-test-")
        self.root = pathlib.Path(self.tmp.name) / "storage"
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.bind(("127.0.0.1", 0))
            self.port = sock.getsockname()[1]
        self.proc = subprocess.Popen(
            [EXE, str(self.root), "--port", str(self.port), "--cache-mib", "32"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        for _ in range(100):
            if self.proc.poll() is not None:
                self.fail(f"NBD server exited: {self.proc.stderr.read()}")
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.1):
                    break
            except ConnectionRefusedError:
                time.sleep(0.02)
        else:
            self.fail("NBD server did not bind loopback socket")
        self.client = None

    def tearDown(self):
        if self.client:
            self.client.close()
        self.proc.terminate()
        try:
            self.proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.communicate(timeout=5)
        self.tmp.cleanup()

    def connect(self, legacy=False):
        self.client = Client("127.0.0.1", self.port, legacy=legacy)
        if legacy:
            self.client.legacy_export()
        else:
            self.client.negotiation_go("cloudblock")
        return self.client

    def test_fixed_newstyle_read_write_flush_restart(self):
        c = self.connect()
        self.assertEqual(c.request(0, CHUNK - 3, 6), bytes(6))
        self.assertEqual(c.request(1, CHUNK - 3, 6, b"ABCDEF"), 0)
        self.assertEqual(c.request(0, CHUNK - 3, 6), b"ABCDEF")
        self.assertEqual(c.request(3), 0)
        self.assertEqual(c.request(0, CHUNK - 3, 6), b"ABCDEF")
        self.assertTrue((self.root / "disk" / "0.bin").exists())
        self.assertTrue((self.root / "disk" / "1.bin").exists())
        c.close()
        self.client = None
        # Clean disconnect, close process, then test persistent bytes via the independent CLI.
        self.proc.terminate()
        self.proc.communicate(timeout=5)
        p = subprocess.run([CLI, str(self.root), "read", str(CHUNK - 3), "6"],
                           capture_output=True, text=True, check=True, timeout=15)
        self.assertEqual(p.stdout.strip(), "414243444546")

    def test_fua_legacy_export(self):
        c = self.connect(legacy=True)
        self.assertEqual(c.request(1, 1024, 4, b"TEST", flags=1), 0)
        self.assertTrue((self.root / "disk" / "0.bin").exists())
        self.assertEqual(c.request(0, 1024, 4), b"TEST")

    def test_negotiation_options_and_bad_bounds(self):
        c = Client("127.0.0.1", self.port)
        self.client = c
        option(c.sock, 999)
        opt, kind, payload = opt_reply(c.sock)
        self.assertEqual((opt, kind, payload), (999, 0x80000001, b""))
        option(c.sock, 3)
        opt, kind, payload = opt_reply(c.sock)
        self.assertEqual((opt, kind), (3, 2))
        self.assertIn(b"cloudblock", payload)
        self.assertEqual(opt_reply(c.sock), (3, 1, b""))
        c.negotiation_go()
        self.assertEqual(c.request(0, VOLUME - 2, 4), 22)
        self.assertEqual(c.request(1, VOLUME - 1, 4, b"WXYZ"), 22)
        self.assertEqual(c.request(0, VOLUME - 4, 4), bytes(4))
        self.assertEqual(c.request(0, 0, 0), b"")

    def test_multi_session_persistence(self):
        c = self.connect()
        self.assertEqual(c.request(1, 4096, 4, b"NBD!"), 0)
        c.close()
        self.client = None
        c = self.connect()
        self.assertEqual(c.request(0, 4096, 4), b"NBD!")


if __name__ == "__main__":
    unittest.main(verbosity=2)
