#!/usr/bin/env python3
"""Regenerate the embedded-HTML C byte arrays from their .html sources.

PlatformIO's src_dir override breaks IDF EMBED_FILES, so we bake each HTML
page into a C byte array instead. Run from firmware/:

    python3 tools/embed_html.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# (html source, generated .c, C symbol base name)
PAGES = [
    ("captive_portal_index.html", "captive_portal_index_html.c", "captive_portal_index_html"),
    ("led_test.html",             "led_test_html.c",             "led_test_html"),
]


def embed(src_name: str, dst_name: str, symbol: str) -> None:
    src = ROOT / "main" / src_name
    dst = ROOT / "main" / dst_name
    data = src.read_bytes()

    lines = [
        f"/* Auto-generated from {src_name} — regenerate via tools/embed_html.py. */",
        "#include <stddef.h>",
        "",
        f"const char {symbol}[] = {{",
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
    lines.append(f"const size_t {symbol}_len = {len(data)};")
    lines.append("")

    dst.write_text("\n".join(lines))
    print(f"wrote {dst.relative_to(ROOT)} ({len(data)} bytes embedded)")


if __name__ == "__main__":
    for src, dst, sym in PAGES:
        embed(src, dst, sym)
