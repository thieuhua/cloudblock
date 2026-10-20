# AGENTS.md — CloudBlock project execution contract

Applies to /home/thieuhua/code/cloudblock-local and its descendants.

## Objective and constraints
Work ONLY on CloudBlock local simulator until explicitly authorized to move to WinSpd, networking or Google Drive. Maintain defaults: 64 GiB logical, 16 MiB chunks, **1 GiB cache**, `disk/` backing and `cache/` write-back. The service is not deployed; never claim it is a Windows-mounted disk.

## Work boundaries
- You may edit this project under its own directory and use local unprivileged build/test commands.
- Do not modify `../combat-analysis`, `../codespace1`, systemd/Nginx, VPS MCP service, SSH, firewall, sudoers or system configuration.
- Do not use sudo/root or install system packages.
- Do not delete data in `store/` or external paths; tests must use auto-cleaned temporary directories.
- Do not push to GitHub, change repository visibility, or create an external service without user instruction.
- Avoid broad refactors when a narrow source patch suffices.

## Source map
- `src/main.cpp`: current single-file CLI and block/cache logic; splitting it into a library is future work.
- `tests/test_local.py`: reproducible Python CLI tests.
- `scripts/develop.sh`: repeatable CMake/Ninja build + CTest.
- `docs.md`: architecture, prototype behavior and future milestones.
- `build/`, `store/`: generated files, excluded from git.

## Required behavioral invariants
1. Bounds-checked reads/writes and deterministic LBA-to-chunk mapping.
2. Sparse holes read zero without allocating backing.
3. Dirty chunks must successfully sync before eviction; failed writes must return an error.
4. Never silently mark dirty data clean without verified backing commit.
5. Restart should not silently lose acknowledged writes in normal operation; document unproven power-cut behavior.
6. Cache max counts resident chunk data; report any additional overhead plainly.
7. Exclusive process lock prevents conflicting simultaneous CLI commands.

## Agent workflow
1. Inspect the relevant function and associated test; **do not read or rewrite unrelated repositories**.
2. State the change and any assumption that might alter behavior.
3. Make source changes by a repeatable script or a minimal patch. Preserve user data.
4. Run `bash scripts/develop.sh` and verify tests complete. Add a regression test for any bug fix.
5. For algorithmic changes, test both across a 16 MiB boundary and across CLI restarts. Include tiny cache (32 MiB) to force eviction.
6. Report exact files changed, results, remaining risks. Never assert a capability that was not exercised.

## Data and security
Treat `store/` and any caller-provided root as user data. Never format, reset, or bulk-delete it automatically. Use random isolated temp test directories. The current store format is experimental and not appropriate for important user data.

If instructions conflict, follow the latest explicit user requirement and make the mismatch visible instead of silently altering a defined invariant.

## NBD boundaries

No public NBD listener, sudo, kernel module activation, systemd, firewall edits, mounting, or formatting without explicit separate authorization. `scripts/admin_mount_nbd.sh` is **operator-only**. Protocol tests do not prove successful kernel mount.
