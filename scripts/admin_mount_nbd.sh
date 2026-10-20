#!/usr/bin/env bash
# Operator-run only. Do not invoke from the unprivileged agent.
set -euo pipefail
device=/dev/nbd0
port=10809
mountpoint=/mnt/cloudblock
action=check
if [[ $# -gt 0 ]]; then action="$1"; fi
check() {
  printf 'CloudBlock NBD operator preflight:\n'
  printf '  user: %s\n' "$(id -un)"
  if [[ -e /sys/module/nbd ]]; then echo '  kernel NBD module: present'
  else echo '  kernel NBD module: absent (requires approved root: modprobe nbd max_part=8)'; fi
  if [[ -b "$device" ]]; then echo "  $device: available"; else echo "  $device: absent"; fi
  for tool in nbd-client mkfs.ext4 mount umount lsblk; do
    if command -v "$tool" >/dev/null; then echo "  $tool: available"
    else echo "  $tool: absent"; fi
  done
  echo '  TCP endpoint: 127.0.0.1:10809; only local'
}
root_only() {
  if [[ "$(id -u)" != '0' ]]; then
    echo 'This action needs operator root approval; this script will never sudo automatically.' >&2
    exit 1
  fi
}
device_only() { [[ -b "$device" ]] || { echo "$device unavailable" >&2; exit 1; }; }
case "$action" in
  check) check ;;
  attach)
    root_only; device_only
    command -v nbd-client >/dev/null || { echo 'nbd-client absent' >&2; exit 1; }
    [[ ! -e /sys/block/nbd0/pid ]] || { echo 'nbd0 may already be attached; refusing' >&2; exit 1; }
    if ! python3 - "$port" <<'PYCHECK'
import socket
import sys
try:
    with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=1):
        pass
except OSError as exc:
    print("ERROR: CloudBlock NBD server is not listening:", exc, file=sys.stderr)
    print("As root run: runuser -u thieuhua -- python3 /home/thieuhua/code/cloudblock-local/scripts/nbd_server.py start", file=sys.stderr)
    sys.exit(1)
PYCHECK
    then
      exit 1
    fi
    nbd-client 127.0.0.1 "$port" "$device" -N cloudblock
    lsblk "$device"
    ;;
  format-new)
    root_only; device_only
    token=
    if [[ $# -gt 1 ]]; then token="$2"; fi
    [[ "$token" == 'ERASE_NEW_CLOUDBLOCK_VOLUME' ]] || {
      echo 'Refusing format without ERASE_NEW_CLOUDBLOCK_VOLUME token' >&2; exit 1;
    }
    command -v mkfs.ext4 >/dev/null || { echo 'mkfs.ext4 missing' >&2; exit 1; }
    if findmnt -n -S "$device" >/dev/null; then
      echo 'Device mounted! Refusing format' >&2; exit 1
    fi
    if command -v blkid >/dev/null && blkid -p "$device" >/dev/null 2>&1; then
      echo 'Disk signature detected. Refusing format' >&2; exit 1
    fi
    echo 'WARNING: formatting destroys content of the CloudBlock export.'
    mkfs.ext4 -m 0 "$device"
    ;;
  mount)
    root_only; device_only
    mkdir -p "$mountpoint"
    mount -t ext4 "$device" "$mountpoint"
    findmnt "$mountpoint"
    ;;
  detach)
    root_only; device_only
    if findmnt -n "$mountpoint" >/dev/null; then umount "$mountpoint"; fi
    nbd-client -d "$device"
    ;;
  *)
    echo 'Usage: admin_mount_nbd.sh check|attach|format-new ERASE_NEW_CLOUDBLOCK_VOLUME|mount|detach' >&2
    exit 2
    ;;
esac
