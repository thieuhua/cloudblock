# CloudBlock Local Simulator (C++23)

Local prototype: logical sparse 64 GiB block address space; 16 MiB chunk files; **1 GiB write-back cache** in `ROOT/cache/`; local backing in `ROOT/disk/`. No Windows drive letter, no driver, no cloud or network.

On Debian 12 / Linux:
```bash
bash scripts/develop.sh
./build/cloudblock ./store status
./build/cloudblock ./store write 16777214 deadbeef
./build/cloudblock ./store read 16777214 4
./build/cloudblock ./store flush
```

Windows 11 CMake + MSYS2/Ninja (portability not yet tested on Windows):
```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\cloudblock.exe .\store status
```

Use `--cache-mib 32` before the command to force small-cache eviction tests. Binary transfer: `put OFFSET INPUT` / `get OFFSET LENGTH OUTPUT`. CLI commands are individually locked, not concurrently scheduled. All tests use temporary roots.

See [docs.md](docs.md) for architecture, proven behavior, limitations, and roadmap; see [AGENTS.md](AGENTS.md) for agent requirements.

## Linux NBD adapter

`build/cloudblock-nbd` serves fixed-newstyle NBD on `127.0.0.1:10809`, backed by the same 1 GiB cache/chunk engine. NBD protocol tests pass; subsequently mounted by the operator on `/dev/nbd0` (64 GiB ext4) and verified read/write. See [NBD_SETUP.md](NBD_SETUP.md).

Start/manage the userspace NBD server as account `thieuhua` with `python3 scripts/nbd_server.py start|status|stop|logs`; if currently root, use `runuser -u thieuhua -- python3 /home/thieuhua/code/cloudblock-local/scripts/nbd_server.py start`. See [NBD_SETUP.md](NBD_SETUP.md) before attaching /dev/nbd0.

## Safe mounted-disk benchmark (non-destructive)

Run as the unprivileged `thieuhua` user while `/mnt/cloudblock` is mounted read/write:

```bash
python3 scripts/safe_disk_test.py smoke                # default 16 MiB
python3 scripts/safe_disk_test.py smoke --size-mib 1    # quick check
python3 scripts/safe_disk_test.py prepare              # retain 16 MiB for manual remount
python3 scripts/safe_disk_test.py verify               # verify after a manual remount
python3 scripts/safe_disk_test.py cleanup              # remove retained test file
```

Safety budget: small 128 KiB streaming buffers, 96 MiB minimum available RAM, 8 GiB reserved host free space, 512 MiB maximum backing allocation growth per default run. Uses its own uniquely named directory and does not touch existing files. **Deleting files from ext4 does not shrink CloudBlock `disk/`** because TRIM is not supported yet.

Optional pressure test, **only with explicit consent**: `python3 scripts/safe_disk_test.py pressure --confirm-pressure` writes 70 sparse 4 KiB extents across >1 GiB of logical address space, with a 3 GiB backing-growth limit. This consumes real VPS disk even after filesystem test-file deletion, so do not run casually. The script never formats, mounts, unmounts, disconnects, or kills NBD.

Mounted NBD cache-pressure check passed on 2026-10-20: `python3 scripts/safe_disk_test.py pressure --confirm-pressure`; 70 sparse fsynced writes/readbacks, 34 cached chunk IDs observed evicted and backed. Permanent physical backing growth was ~544 MiB because TRIM is unimplemented. The script now limits new backing growth to 1600 MiB with a hard 8 GiB host-free reserve. See `docs.md` for exact findings.

## Native Windows status (2026-10-20)

Direct execution on SOTERAN (Windows 11 / MSYS2 UCRT64 GCC 16.1.0 / Ninja):
- Source: E:\thieuhua\cloudblock (branch feat/windows-local-wnbd).
- Runtime: E:\CloudBlock\cache, disk, metadata, logs.
- CMake native CLI compilation PASS; CTest 1/1 PASS.
- 4-byte write across 16 MiB boundary, read and flush PASS; 2 x 16 MiB chunk files materialized under E:\CloudBlock\disk.
- Native WNBD adapter compilation and linking PASS with GCC MinGW and -municode.
- WNBD kernel driver and libwnbd.dll are NOT installed yet; no virtual disk created and no format or X: mapping performed.
- Do not trust data without an implemented crash journal; no TRIM.

Windows build:
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\windows\build.ps1 -WNBD

After separately installing/validating a signed WNBD driver and DLL, the guarded run script is:
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\windows\run-wnbd.ps1 -Dll 'C:\absolute\path\libwnbd.dll'

The run script refuses if WNBD service is absent. Never initialize or format arbitrary raw disks.
