#!/usr/bin/env python3
"""One-time safe extraction of CloudBlock Store for CLI + NBD adapter.

Keeps all current logic unchanged and preserves the existing public CLI.
Only edits src/main.cpp and creates src/block_store.hpp.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
main = root / "src" / "main.cpp"
header = root / "src" / "block_store.hpp"
source = main.read_text(encoding="utf-8")
if source.startswith('#include "block_store.hpp"'):
    assert header.is_file(), "already refactored but header missing"
    print("Already split Store; no changes")
else:
    assert not header.exists(), "unexpected block_store.hpp: refusing overwrite"
    marker = "static void help() {\n"
    assert source.count(marker) == 1, "CLI split boundary not unique"
    before, cli = source.split(marker, 1)
    header.write_text("#pragma once\n" + before, encoding="utf-8")
    main.write_text('#include "block_store.hpp"\n\n' + marker + cli, encoding="utf-8")
    print("Extracted block_store.hpp, CLI uses same Store implementation")
