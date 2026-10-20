#!/usr/bin/env python3
"""Manage the Linux CloudBlock NBD server as unprivileged account 'thieuhua'.

Does not perform sudo, mount, modprobe, systemd or filesystem formatting.
Use: python3 scripts/nbd_server.py start|status|stop|logs|foreground
"""
from __future__ import annotations

import argparse
import fcntl
import os
from pathlib import Path
import pwd
import signal
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "build" / "cloudblock-nbd"
VOLUME = ROOT / "nbd-volume"
STATE = ROOT / ".run"
PID_FILE = STATE / "nbd.pid"
LOG_FILE = STATE / "nbd.log"
LOCK_FILE = STATE / "manager.lock"
PORT = 10809


def check_account():
    actual = pwd.getpwuid(os.geteuid()).pw_name
    if actual != "thieuhua":
        raise RuntimeError(
            "Must run as thieuhua (not root). From a root shell: "
            "runuser -u thieuhua -- python3 "
            + str(ROOT / "scripts" / "nbd_server.py") + " start"
        )


def can_connect() -> bool:
    try:
        with socket.create_connection(("127.0.0.1", PORT), timeout=0.25):
            return True
    except OSError:
        return False


def process_is_ours(pid: int) -> bool:
    if pid <= 1:
        return False
    try:
        proc = Path("/proc") / str(pid)
        if os.readlink(proc / "exe") != str(BIN.resolve()):
            return False
        command = (proc / "cmdline").read_bytes().split(b"\0")
        if os.fsencode(VOLUME) not in command:
            return False
        status = (proc / "stat").read_text()
        # A zombie still has a /proc/PID/exe on some kernels.
        if status.rsplit(") ", 1)[-1].split()[0] == "Z":
            return False
        return True
    except (FileNotFoundError, PermissionError, OSError, IndexError):
        return False


def get_managed_pid() -> int | None:
    try:
        pid = int(PID_FILE.read_text().strip())
    except (FileNotFoundError, OSError, ValueError):
        return None
    return pid if process_is_ours(pid) else None


def kernel_attached() -> bool:
    # Check active kernel attachment, even if this shell cannot see host /sys.
    # An established connection to the export is a conservative stop blocker.
    for net_table in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            for row in Path(net_table).read_text().splitlines()[1:]:
                fields = row.split()
                local_port = int(fields[1].split(":")[-1], 16)
                if local_port == PORT and fields[3] == "01":
                    return True
        except (OSError, IndexError, ValueError):
            return True
    # Kernel NBD connection may survive even when mount has been removed.
    pid_path = Path("/sys/block/nbd0/pid")
    if pid_path.exists():
        return True
    try:
        p = subprocess.run(
            ["findmnt", "-rn", "-S", "/dev/nbd0"],
            capture_output=True, check=False,
            timeout=3,
        )
        return p.returncode == 0
    except (FileNotFoundError, subprocess.TimeoutExpired):
        # Unknown state: conservative refusal.
        return True


def ensure_dirs():
    STATE.mkdir(exist_ok=True, mode=0o700)
    VOLUME.mkdir(exist_ok=True, mode=0o700)
    os.chmod(STATE, 0o700)
    os.chmod(VOLUME, 0o700)


def log_tail() -> str:
    if not LOG_FILE.exists():
        return "(no server log)"
    return "\n".join(LOG_FILE.read_text(errors="replace").splitlines()[-40:])


def start():
    ensure_dirs()
    if not BIN.is_file() or not os.access(BIN, os.X_OK):
        raise RuntimeError("Binary missing; first run: bash scripts/develop.sh")
    with LOCK_FILE.open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        current = get_managed_pid()
        if current is not None:
            if can_connect():
                print(f"RUNNING: pid={current}, 127.0.0.1:{PORT}")
                return
            raise RuntimeError(
                f"Server PID {current} exists but socket is unavailable. Inspect logs."
            )
        if can_connect():
            raise RuntimeError(
                f"TCP 127.0.0.1:{PORT} already occupied by an unmanaged process."
            )
        args = [str(BIN), str(VOLUME), "--port", str(PORT), "--cache-mib", "1024"]
        with LOG_FILE.open("ab", buffering=0) as log:
            child = subprocess.Popen(
                args, cwd=ROOT, stdin=subprocess.DEVNULL,
                stdout=log, stderr=log,
                close_fds=True, start_new_session=True,
            )
        PID_FILE.write_text(str(child.pid) + "\n")
        for _ in range(40):
            if not process_is_ours(child.pid):
                raise RuntimeError("Server exited at startup.\n" + log_tail())
            if can_connect():
                print(f"READY: pid={child.pid}, 127.0.0.1:{PORT}")
                print(f"Backing chunks: {VOLUME / 'disk'}")
                print("Cache: 1 GiB")
                print("Root terminal next: bash scripts/admin_mount_nbd.sh attach")
                return
            time.sleep(0.1)
        raise RuntimeError("Server startup timed out.\n" + log_tail())


def status():
    current = get_managed_pid()
    if current is None:
        print("STOPPED: no managed CloudBlock NBD server")
        return 1
    if not can_connect():
        print(f"ERROR: pid={current} running, but port {PORT} is unavailable")
        return 1
    print(f"RUNNING: pid={current}, 127.0.0.1:{PORT}")
    return 0


def stop():
    ensure_dirs()
    with LOCK_FILE.open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if kernel_attached():
            raise RuntimeError(
                "/dev/nbd0 appears attached. Unmount and detach as root BEFORE stopping."
            )
        current = get_managed_pid()
        if current is not None:
            os.kill(current, signal.SIGTERM)
            for _ in range(40):
                if not process_is_ours(current):
                    break
                time.sleep(0.1)
            if process_is_ours(current):
                raise RuntimeError("Server did not stop; refusing SIGKILL")
            print(f"STOPPED: pid={current}")
        else:
            print("Already stopped")
        PID_FILE.unlink(missing_ok=True)


def foreground():
    ensure_dirs()
    if not BIN.is_file():
        raise RuntimeError("Binary missing; run bash scripts/develop.sh")
    os.execv(str(BIN), [str(BIN), str(VOLUME), "--port", str(PORT),
                           "--cache-mib", "1024"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "command", choices=["start", "status", "stop", "logs", "foreground"]
    )
    args = parser.parse_args()
    check_account()
    if args.command == "start":
        start()
    elif args.command == "status":
        return status()
    elif args.command == "stop":
        stop()
    elif args.command == "logs":
        print(log_tail())
    elif args.command == "foreground":
        foreground()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
