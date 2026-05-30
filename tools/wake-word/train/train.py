#!/usr/bin/env python3
"""Thin wrapper around microWakeWord's training pipeline.

Reads positive + negative WAV corpora produced by `generate_corpus.py` (or
any compatible source: 16 kHz mono int16), runs upstream feature extraction
and training, exports an INT8 .tflite.

The upstream `kahrendt/microWakeWord` repo evolves quickly; rather than
vendoring or pinning a release, we shell out to its CLI entrypoints. You
must clone the repo locally and pass --upstream <path>.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str], cwd: Path | None = None) -> None:
    print(f"$ {' '.join(cmd)}", file=sys.stderr)
    subprocess.check_call(cmd, cwd=cwd)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--positive-dir", type=Path, required=True,
                    help="dir of wake-phrase WAVs (16 kHz mono int16, ~1.5 s)")
    ap.add_argument("--negative-dir", type=Path, required=True,
                    help="dir of not-wake-phrase WAVs (same format)")
    ap.add_argument("--upstream", type=Path, required=True,
                    help="path to a clone of github.com/kahrendt/microWakeWord")
    ap.add_argument("--output", type=Path, required=True,
                    help="where to write the final .tflite (typically "
                         "../../main/models/wake_word_ru.tflite)")
    ap.add_argument("--workdir", type=Path, default=Path("./workdir"),
                    help="scratch dir for features + checkpoints")
    args = ap.parse_args()

    for p in (args.positive_dir, args.negative_dir, args.upstream):
        if not p.exists():
            sys.exit(f"path not found: {p}")
    args.workdir.mkdir(parents=True, exist_ok=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)

    # microWakeWord expects feature parquet files. Generate them from raw audio.
    features_pos = args.workdir / "features_pos.parquet"
    features_neg = args.workdir / "features_neg.parquet"

    # Upstream entrypoint names — these are stable per the v0.x line as of
    # 2026-05. Update if upstream renames them.
    run([sys.executable, "-m", "microwakeword.audio_to_features",
         "--input-dir", str(args.positive_dir),
         "--output",    str(features_pos)],
        cwd=args.upstream)
    run([sys.executable, "-m", "microwakeword.audio_to_features",
         "--input-dir", str(args.negative_dir),
         "--output",    str(features_neg)],
        cwd=args.upstream)

    # Train. The default upstream config produces an Inception-style INT8
    # model around 25 KB; sufficient for the wake-word use case.
    run([sys.executable, "-m", "microwakeword.train",
         "--positive-features", str(features_pos),
         "--negative-features", str(features_neg),
         "--output",            str(args.output),
         "--quantize",          "int8"],
        cwd=args.upstream)

    size_kb = args.output.stat().st_size / 1024
    print(f"\nwrote {args.output} ({size_kb:.1f} KB)", file=sys.stderr)
    print("flash it via: cp <path> firmware/main/models/wake_word_ru.tflite "
          "&& cd firmware && pio run --target upload", file=sys.stderr)


if __name__ == "__main__":
    main()
