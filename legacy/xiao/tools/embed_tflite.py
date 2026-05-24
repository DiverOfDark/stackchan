#!/usr/bin/env python3
"""Bake firmware/main/models/wake_word_ru.tflite into a C byte array.

Mirrors tools/embed_html.py — PlatformIO's src_dir override breaks the IDF
EMBED_FILES path resolution, so we generate a .c file the build links in
directly. Run from firmware/:

    python3 tools/embed_tflite.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC  = ROOT / "main" / "models" / "wake_word_ru.tflite"
DST  = ROOT / "components" / "wake_word" / "wake_word_model_data.c"

if not SRC.exists():
    raise SystemExit(f"missing: {SRC} — train the model first "
                     "(see tools/train_wake_word/README.md)")

data = SRC.read_bytes()

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
