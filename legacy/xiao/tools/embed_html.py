#!/usr/bin/env python3
"""Regenerate main/captive_portal_index_html.c from the .html source.

PlatformIO's src_dir override breaks IDF EMBED_FILES, so we bake the HTML
into a C byte array instead. Run from firmware/:

    python3 tools/embed_html.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "main" / "captive_portal_index.html"
DST = ROOT / "main" / "captive_portal_index_html.c"

data = SRC.read_bytes()

lines = [
    "/* Auto-generated from captive_portal_index.html — regenerate via tools/embed_html.py. */",
    "#include <stddef.h>",
    "",
    "const char captive_portal_index_html[] = {",
]
chunk = []
for i, b in enumerate(data, 1):
    chunk.append(f"0x{b:02x},")
    if i % 16 == 0:
        lines.append("    " + " ".join(chunk))
        chunk = []
if chunk:
    lines.append("    " + " ".join(chunk))
lines.append("};")
lines.append(f"const size_t captive_portal_index_html_len = {len(data)};")
lines.append("")

DST.write_text("\n".join(lines))
print(f"wrote {DST.relative_to(ROOT)} ({len(data)} bytes embedded)")
