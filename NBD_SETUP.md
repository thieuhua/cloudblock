# CloudBlock NBD integration on VPS — 2026-10-20

## Implementation
Linux `build/cloudblock-nbd` is an NBD fixed-newstyle protocol server and shares Store with the CLI. Its TCP listener is restricted to `127.0.0.1:10809` (NOT exposed publicly; no TLS). The export name is `cloudblock` or the default empty name.

Supported: NBD_OPT_GO/INFO/LIST/EXPORT_NAME/ABORT, READ, WRITE, FLUSH, DISC, FUA on WRITE. Max 8 MiB per command. Export capacity 64 GiB, default cache 1 GiB, chunk 16 MiB, backing `ROOT/disk/`. One client at a time, serial processing. No TRIM or WRITE_ZEROES, no structured replies, no journal, no safe power-loss guarantee.

Status: compiled and **protocol tests passed using real TCP NBD requests**. **NOT mounted to Linux kernel yet**. This VPS lacked /dev/nbd*, nbd kernel module and privileged capabilities in the tool session. The agent has not used sudo, installed packages, started any service, or altered system configuration.

## Persistent NBD process manager (implemented)

The operator can launch the loopback NBD server as the regular `thieuhua` account, without sudo on the server process:

```bash
# From a root terminal, simply start the process AS thieuhua:
runuser -u thieuhua -- python3 /home/thieuhua/code/cloudblock-local/scripts/nbd_server.py start
runuser -u thieuhua -- python3 /home/thieuhua/code/cloudblock-local/scripts/nbd_server.py status

# Then, only in the root terminal, attach the kernel device:
cd /home/thieuhua/code/cloudblock-local
bash scripts/admin_mount_nbd.sh attach
lsblk /dev/nbd0
```

The manager creates `./nbd-volume` and `.run/`, launches the server detached as `thieuhua`, writes `.run/nbd.pid` and `.run/nbd.log`, and probes 127.0.0.1:10809. It does **not** install an auto-start service and does not touch other projects. To view its logs: `runuser -u thieuhua -- python3 /home/thieuhua/code/cloudblock-local/scripts/nbd_server.py logs`. To stop: unmount and detach the kernel NBD device as root **first**, then invoke the manager `stop` subcommand as `thieuhua`.

2026-10-20: the manager was started as `thieuhua` and verified in two separate shell invocations. Protocol handshake succeeded on loopback, but **the kernel attach has not yet been confirmed**.

## Unprivileged server launch
```bash
cd /home/thieuhua/code/cloudblock-local
bash scripts/develop.sh
./build/cloudblock-nbd ./nbd-volume --port 10809 --cache-mib 1024
```
Use an isolated fresh `./nbd-volume` store, **not** the existing CLI test store. Keep this terminal running. The tool's nonpersistent shell cannot host a persistent service, so a human operator must provide a terminal or explicitly authorize a separate service deployment.

## Privileged operations, operator action only, NOT executed

```bash
# If approved by system administrator and prerequisites are absent:
sudo apt-get install nbd-client e2fsprogs
sudo modprobe nbd max_part=8

# CloudBlock server must already be running in its own non-root terminal
cd /home/thieuhua/code/cloudblock-local
bash scripts/admin_mount_nbd.sh check
sudo bash scripts/admin_mount_nbd.sh attach
sudo lsblk /dev/nbd0

# Only on a confirmed NEW, EMPTY and DISPOSABLE CloudBlock backing store:
# DANGER: this will destroy all content of the virtual disk!
sudo bash scripts/admin_mount_nbd.sh format-new ERASE_NEW_CLOUDBLOCK_VOLUME
sudo bash scripts/admin_mount_nbd.sh mount

findmnt /mnt/cloudblock
echo hello | sudo tee /mnt/cloudblock/hello.txt
sudo cat /mnt/cloudblock/hello.txt

# Stop filesystem use before detach:
sudo bash scripts/admin_mount_nbd.sh detach
```

IMPORTANT: verify that /dev/nbd0 is not used by another service and that `./nbd-volume` is empty before formatting. Never format an existing volume with valuable data. No root command above was performed by ChatGPT.

## Tests without root

```bash
cd /home/thieuhua/code/cloudblock-local
bash scripts/develop.sh
python3 tests/test_nbd.py ./build/cloudblock-nbd
```

The tests create their own temporary NBD volumes, never touch `store/` or `nbd-volume/`, and exercise GO negotiation, LIST, legacy export, READ, WRITE, FUA, FLUSH, invalid ranges, reconnect and cross-process verification.

## Remaining work
1. Operator-approved actual NBD kernel attach + ext4 formatting on a new disposable volume.
2. Stress testing: filesystem format, mount, massive file copy, fsync, reboot, fragmented I/O, forced disconnect.
3. Crash-consistent journal before storing valuable user data.
4. Then Windows driver and remote backend.
