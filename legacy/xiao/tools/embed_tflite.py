#!/usr/bin/env python3
"""Bake firmware/main/models/wake_word_ru.tflite into a C byte array.

Mirrors tools/embed_html.py — PlatformIO's src_dir override breaks the IDF
EMBED_FILES path resolution, so we generate a .c file the build links in
directly. Run from firmware/:

    python3 tools/embed_tflite.py

Pass --check to verify the committed .c is in sync with the .tflite without
rewriting it (exit 1 if stale) — wire this into CI so a retrained model can't
ship without re-embedding. (This exact drift once shipped a synthetic-overfit
model to the device while a better .tflite sat un-embedded in the repo.)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC  = ROOT / "main" / "models" / "wake_word_ru.tflite"
DST  = ROOT / "components" / "wake_word" / "wake_word_model_data.c"

if not SRC.exists():
    raise SystemExit(f"missing: {SRC} — train the model first "
                     "(see tools/train_wake_word/README.md)")

data = SRC.read_bytes()

if "--check" in sys.argv[1:]:
    if not DST.exists():
        raise SystemExit(f"{DST.name} missing — run: python3 tools/embed_tflite.py")
    embedded = bytes(int(x, 16) for x in re.findall(
        r"0x([0-9a-fA-F]{2})", DST.read_text()))
    if embedded == data:
        print(f"OK — {DST.name} matches {SRC.name} ({len(data)} bytes)")
        raise SystemExit(0)
    raise SystemExit(
        f"STALE — {DST.name} ({len(embedded)} bytes) does not match "
        f"{SRC.name} ({len(data)} bytes). The device would run an old model. "
        f"Run: python3 tools/embed_tflite.py")

lines = [
    "/* Auto-generated from main/models/wake_word_ru.tflite — regenerate via tools/embed_tflite.py. */",
    "#include <stddef.h>",
    "",
    "/* TFLite Micro requires the model buffer to be 16-byte-aligned. */",
    "__attribute__((aligned(16)))",
    "const unsigned char wake_word_model_data[] = {",
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
lines.append(f"const size_t wake_word_model_data_len = {len(data)};")
lines.append("")

DST.write_text("\n".join(lines))
print(f"wrote {DST.relative_to(ROOT)} ({len(data)} bytes embedded)")
