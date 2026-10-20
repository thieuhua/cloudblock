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

`build/cloudblock-nbd` serves fixed-newstyle NBD on `127.0.0.1:10809`, backed by the same 1 GiB cache/chunk engine. NBD protocol tests pass; **not mounted to kernel yet**, as this VPS has no /dev/nbd* or loaded nbd module and agent lacks admin rights. See [NBD_SETUP.md](NBD_SETUP.md).

Start/manage the userspace NBD server as account `thieuhua` with `python3 scripts/nbd_server.py start|status|stop|logs`; if currently root, use `runuser -u thieuhua -- python3 /home/thieuhua/code/cloudblock-local/scripts/nbd_server.py start`. See [NBD_SETUP.md](NBD_SETUP.md) before attaching /dev/nbd0.
