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

`src/nbd_server.cpp` shares `src/block_store.hpp` with CLI and handles NBD fixed-newstyle READ/WRITE/FLUSH/FUA on loopback. Tested over TCP. No kernel attach/ext4 test yet. See `NBD_SETUP.md`.
