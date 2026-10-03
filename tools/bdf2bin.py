#!/usr/bin/env python3
"""Convert a monospace BDF bitmap font (Terminus) to Femto's compact format.

Format (little-endian): b"FBF1", u8 cell_w, u8 cell_h, u8 ascent, u16 count,
then per glyph: u32 codepoint + cell_h rows of ceil(cell_w/8) bytes (MSB left).
Only the characters Femto draws are kept (ASCII, Cyrillic, a few symbols).

Usage: tools/bdf2bin.py ter-u12n.bdf assets/fonts/terminus-12n.fbf
"""
import struct
import sys

KEEP = set(range(0x20, 0x7F)) | set(range(0x410, 0x450)) | {0x401, 0x451, 0xB7, 0x2014, 0x2013, 0x2026, 0xD7, 0xAB, 0xBB, 0x2116, 0xB0, 0x25B6, 0x2192}


def main(src, dst):
    lines = open(src, encoding="latin-1").read().splitlines()
    w = h = asc = None
    glyphs = {}
    i = 0
    while i < len(lines):
        l = lines[i]
        if l.startswith("FONTBOUNDINGBOX"):
            w, h, xo, yo = map(int, l.split()[1:])
        elif l.startswith("FONT_ASCENT"):
            asc = int(l.split()[1])
        elif l.startswith("ENCODING"):
            cp = int(l.split()[1])
        elif l.startswith("BBX"):
            bw, bh, bx, by = map(int, l.split()[1:])
        elif l == "BITMAP":
            rows = []
            i += 1
            while lines[i] != "ENDCHAR":
                rows.append(int(lines[i], 16))
                i += 1
            if cp in KEEP:
                # Place the glyph box into the full cell (top = ascent).
                bpr = (w + 7) // 8
                cell = [0] * h
                top = asc - (by + bh)
                src_bpr = (bw + 7) // 8
                for r, v in enumerate(rows):
                    y = top + r
                    if 0 <= y < h:
                        # left-align source bits to cell width, shift by bx
                        v <<= (bpr - src_bpr) * 8
                        v >>= max(bx, 0)
                        cell[y] = v
                glyphs[cp] = (bpr, cell)
        i += 1
    out = bytearray(b"FBF1" + struct.pack("<BBBH", w, h, asc, len(glyphs)))
    for cp in sorted(glyphs):
        bpr, cell = glyphs[cp]
        out += struct.pack("<I", cp)
        for v in cell:
            out += v.to_bytes(bpr, "big")
    open(dst, "wb").write(out)
    print(f"{dst}: {w}x{h} ascent {asc}, {len(glyphs)} glyphs, {len(out)} bytes")


if __name__ == "__main__":
    main(*sys.argv[1:3])
