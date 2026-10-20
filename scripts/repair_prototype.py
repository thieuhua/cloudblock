#!/usr/bin/env python3
"""Idempotent narrow fixup for the CloudBlock v0.1 -> v0.2 local workspace.

Usage: python3 scripts/repair_prototype.py
Never writes outside this source checkout or touches runtime store/disk data.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
main = ROOT / "src" / "main.cpp"
old = "        if (in) {\n            in.read(buf.data(),"
new = "        if (in.is_open()) {\n            in.read(buf.data(),"

source = main.read_text(encoding="utf-8")
if old in source:
    assert source.count(old) == 1, "ambiguous copy_chunk match"
    main.write_text(source.replace(old, new), encoding="utf-8")
    print("FIXED copy_chunk unopened stream guard")
elif new in source:
    print("OK copy_chunk guard already fixed")
else:
    raise SystemExit("Unexpected source layout; refusing to patch")

for rel in ("docs.md", "README.md", "AGENTS.md"):
    path = ROOT / rel
    content = path.read_text(encoding="utf-8")
    # The original generated Markdown used backslash-escaped code fences.
    corrected = content.replace("\\`", "`")
    if corrected != content:
        path.write_text(corrected, encoding="utf-8")
        print("FIXED Markdown quoting:", rel)
    else:
        print("OK Markdown:", rel)
