Fonts, all under the SIL Open Font License 1.1.

- Barlow Condensed — Jeremy Tribby. Latin only (Google's Barlow has no
  Cyrillic).
- Fira Sans Condensed — Mozilla Foundation / Telefonica (`FIRA-OFL.txt`).
  The four weights matching the Barlow faces, subset to Cyrillic with
  `pyftsubset --unicodes="U+0400-045F,U+0490-0491,U+00AB,U+00BB,U+2013,U+2014,U+2026,U+2116,U+201C,U+201E"`.
  The renderer falls back to these for glyphs Barlow lacks (Russian captions).
- Share Tech Mono — Carrois Apostrophe (Latin only; Cyrillic falls back to
  Fira Sans Condensed Medium).
- Noto Sans JP (subset: フェムト警告監視・·) — Google
- Terminus 4.49.1 (bitmap, 6×12 / 8×14 / 8×16, Latin + Cyrillic) — Dimitar
  Zhekov (`TERMINUS-OFL.txt`). Converted with `tools/bdf2bin.py` to `*.fbf`.
