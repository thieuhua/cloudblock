# CloudBlock — Development direction (local-first)

Updated: 2026-10-20. This file is the engineering contract for the **local simulator**, not a claim that the full virtual drive is complete.

## Intent
Build a user-controlled virtual block store: a Windows filesystem will eventually see contiguous logical block addresses although the actual content lives in separately managed chunks, an SSD cache, and eventually Google Drive.

**Current target: prove local correctness before drivers, networking, or cloud APIs.**
The CloudBlock CLI is a *simulator*: it is NOT a mounted disk, and it does not create G: or offer a filesystem.

## Active prototype v0.2 on VPS
- Repository/worktree location: /home/thieuhua/code/cloudblock-local
- Language: C++23, built with CMake and Ninja, tested with Python stdlib.
- Logical device capacity: 64 GiB (sparse, zero-filled holes).
- Chunk size: 16 MiB; id = byte_offset / 16777216.
- Cache limit: **1 GiB** by default, counted in 16 MiB chunk files (max 64 resident chunks). Override using --cache-mib (multiple of 16, min 16).
- ROOT/cache/N.bin: local write-back copy for chunk N.
- ROOT/disk/N.bin: local backing copy for chunk N; substitutes for a remote cloud chunk file.
- The root is chosen by the caller. ROOT/.lock excludes simultaneous processes on one store.
- CLI: status, read, write, put, get, flush.

Commands:
```sh
bash scripts/develop.sh
./build/cloudblock ./store status
./build/cloudblock ./store write 16777214 deadbeef
./build/cloudblock ./store read 16777214 4
./build/cloudblock ./store flush
```
Full tests: `ctest --test-dir build --output-on-failure`. Tests create and destroy **their own isolated temporary roots**. They do not use `./store`.

## Current behavior
- An unallocated chunk reads as zeros without fetching/allocating a chunk file.
- First modification to a chunk creates a 16 MiB cache chunk, seeded from backing if present, otherwise zero.
- Reads and writes cross chunk boundaries and validate the 64 GiB logical range.
- Cache file writes are flushed to OS and filesystem via file sync before success is reported on a normal return.
- `flush` synchronously copies each dirty cached chunk to `disk/` via a temporary file + rename.
- LRU-ish cache eviction picks the least recently accessed entry; dirty victims sync before eviction.
- After restarting the CLI, every cache chunk that exists is treated as dirty (conservative; there is no persisted clean/dirty journal).
- Reads after a graceful restart use the cached version as authoritative.
- A startup scan removes stale `.tmp` files and restores Windows backup renames (`.bak`). No user data is intentionally removed during normal operations beyond internal temp files and evicted clean chunk copies.
- CLI access is sequential. Separate invocations on one volume are excluded by an exclusive lock, not scheduled concurrently.

## Explicit limitations / do not overclaim
1. This is a CLI simulation, **not** an OS virtual disk, block driver, NTFS, or Google Drive mount.
2. No write-in-progress rollback or crash-atomic multi-chunk transaction; an interrupted in-place cache write can leave partially changed content.
3. No durable journal/dirty-range bitmap, checksums, cryptographic integrity, generations, transaction snapshots or automatic backup recovery beyond conservative resync.
4. The 1 GiB cap applies to resident chunk data, **not** total disk use. Backing `disk/` continues to grow as logical chunks are populated.
5. Flush waits for the local backing directory, not Google Drive.
6. Windows directory rename durability is not established; power-loss guarantees are NOT proven on Windows.
7. No timeout, background queue, networking, path bonding, encryption, remote backend, user interface, installer or service.

## Invariants for future patches
- Logical offsets map deterministically to chunks, without reliance on host-file order.
- Reads of untouched sectors yield zero, regardless of chunk boundary.
- Never evict a dirty chunk without completing a successful durable copy to its backing file.
- Cache data wins after recovery, unless an explicit verified journal protocol changes this rule.
- A failed copy must not destroy an existing valid backing chunk.
- New work must not access or modify other projects, deployed services, MCP configuration, or existing user data.

## Implementation roadmap
P0 — **local correctness (current)**: unit + CLI round trips, cross-boundary writes, restart, errors, LRU and 34 MiB SHA-256 round-trip. Require tests pass on both Linux and Windows before calling portability proven.

P1 — **crash recovery**: append-only journal/write-ahead data, checksums, replay, power-cut injection, generation numbers, and clear durability semantics. Avoid expanding features before proving the safety model.

P2 — **cache engine**: split library/API from CLI, asynchronous local-backend uploads with throttling, dirty-sector map, backpressure, read-ahead, fault injection, observability.

P3 — **Windows integration**: evaluate WinSpd/WNBD compatibility against current Windows 11. Add a minimal virtual block device, then format/mount NTFS. Test failure to unmount/stop safely.

P4 — **remote**: Google Drive API backend with resumable upload, quota pacing and upload reconciliation; multipath routing through real independent adapters only after throughput experiments.

Do not equate prototype passing tests with production readiness. For each milestone record exact commands, environment, test results, and limitations.

## NBD userspace milestone — 2026-10-20

`src/nbd_server.cpp` shares `src/block_store.hpp` with CLI and handles NBD fixed-newstyle READ/WRITE/FLUSH/FUA on loopback. Tested over TCP. Kernel `/dev/nbd0` attach, ext4 mount, read/write and clean restart have subsequently passed on VPS. See `NBD_SETUP.md`.

## Mounted NBD eviction test — 2026-10-20 (PASS)

Actual VPS mount `/dev/nbd0` → `/mnt/cloudblock`, ext4 RW; NBD server owned by `thieuhua`. Ran `python3 -u scripts/safe_disk_test.py pressure --confirm-pressure` after reducing pressure backing-growth cap to 1600 MiB while retaining >=8 GiB host-free reserve and >=96 MiB available-RAM requirement. Results: 70 sparse 4 KiB writes at 16 MiB logical-file spacing, each fsynced, all 70 locations read back exactly. Observed 98 unique cached chunk IDs, 34 observed evicted IDs with valid backing files, resident cache 64/64 chunks at finish. `disk/` backing grew from 2272 MiB (143 chunks) to 2816 MiB (176 chunks), +544 MiB; free physical filesystem went from 10570 MiB to 10026 MiB. Python peak RSS ~17.3 MiB. File test cleanup PASS and server still running with ext4 mounted. **Note:** No TRIM, so the +544 MiB physical backing growth remains after deleting the temporary ext4 test file. This proves local eviction + data readback under ext4/NBD, NOT power-loss recovery or dirty-generation correctness under concurrent mutation. Do not overclaim.

## Windows port branch (experimental)
`feat/windows-local-wnbd`: optional Windows-native WNBD callback adapter using the existing chunk Store; pinned Cloudbase WNBD public SDK headers; CMake and PowerShell scripts to prepare runtime `E:\CloudBlock/{cache,disk,metadata,logs}` while preserving source `E:\thieuhua\cloudblock`. Tested only Linux regression builds, **not tested on Windows and no driver installed**. Initial executable only creates raw virtual disk with a fixed instance name (`CloudBlockLocal`); it never formats NTFS or mounts a letter. Current Store lacks journaling and TRIM. Do not store important data.

## Windows local verification (2026-10-20)
On SOTERAN Windows: core CLI compiled (GCC UCRT64 16.1.0) and CTest PASS; local write/read/flush across chunk boundary PASS. Optional direct WNBD callback adapter compiled and linked PASS after adding -municode for MinGW's wmain. No WNBD driver or DLL present, so kernel block device, GPT/NTFS and Explorer mount remain UNTESTED. E:\CloudBlock layout created and metadata persisted. No OS disk formatting performed.
