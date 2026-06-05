#!/usr/bin/env python3
"""Pull device-captured wake samples from the backend and sort them into the
training corpus.

The device uploads the audio that triggered EVERY wake fire (real *and* false)
to the backend's ``/wake-sample`` endpoint. This script downloads them, lets you
label each as a real wake or a false fire, and stages them into the training
corpus that ``train_production.py`` consumes:

  * false fires  → ``corpus/hard_negatives/``   (high-weight hard negatives)
  * real wakes   → ``corpus/positive_real/``    (more real positive data)

The strong false positives are indistinguishable from real wakes at the
metric level (peak/avg/hits all in the real-wake range), so the only way to
make the model stop firing on them is to train on the actual audio. This is
that collection loop.

Typical use:

    # 1. Download new samples into captured_wakes/inbox/
    python collect_hard_negatives.py fetch \\
        --backend https://voice-assistant.kirillorlov.pro

    # 2. Label them — plays each clip (if a player is available), shows its
    #    metrics, and asks real / false / skip:
    python collect_hard_negatives.py label
    #    (or just move files by hand: inbox/*.wav -> false/ or real/)

    # 3. Stage the labelled clips into the training corpus, then retrain:
    python collect_hard_negatives.py stage
    python train_production.py

Only the standard library is used (urllib/json/wave) so it runs in the training
venv without extra deps.
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CAP_DIR = ROOT / "captured_wakes"
INBOX = CAP_DIR / "inbox"
FALSE_DIR = CAP_DIR / "false"
REAL_DIR = CAP_DIR / "real"
CORPUS_HARD_NEG = ROOT / "corpus" / "hard_negatives"
CORPUS_POS_REAL = ROOT / "corpus" / "positive_real"


def _get(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=30) as r:
        return r.read()


def cmd_fetch(args: argparse.Namespace) -> None:
    base = args.backend.rstrip("/")
    INBOX.mkdir(parents=True, exist_ok=True)
    listing = json.loads(_get(f"{base}/wake-samples"))
    samples = listing.get("samples", [])
    print(f"backend reports {len(samples)} sample(s) in {listing.get('dir')}")
    new = 0
    for s in samples:
        name = s["wav"]
        dest = INBOX / name
        # Already sorted? Skip (don't re-download things we've labelled).
        if dest.exists() or (FALSE_DIR / name).exists() or (REAL_DIR / name).exists():
            continue
        dest.write_bytes(_get(f"{base}/wake-samples/{name}"))
        meta = s.get("meta", {})
        dest.with_suffix(".json").write_text(json.dumps(meta, indent=2))
        new += 1
        print(f"  ↓ {name}  peak={meta.get('peak')} avg={meta.get('avg')} hits={meta.get('hits')}")
    print(f"downloaded {new} new sample(s) into {INBOX}")


def _play(wav: Path) -> None:
    for player in (["aplay", "-q"], ["ffplay", "-autoexit", "-nodisp", "-loglevel", "quiet"],
                   ["paplay"], ["afplay"]):
        if shutil.which(player[0]):
            try:
                subprocess.run(player + [str(wav)], check=False)
            except Exception:
                pass
            return
    print("  (no audio player found — open the WAV manually)")


def cmd_label(args: argparse.Namespace) -> None:
    FALSE_DIR.mkdir(parents=True, exist_ok=True)
    REAL_DIR.mkdir(parents=True, exist_ok=True)
    wavs = sorted(INBOX.glob("*.wav"))
    if not wavs:
        print(f"nothing to label in {INBOX} — run `fetch` first.")
        return
    print(f"{len(wavs)} clip(s) to label.  [f]alse fire / [r]eal wake / [s]kip / [q]uit\n")
    for wav in wavs:
        meta = {}
        j = wav.with_suffix(".json")
        if j.exists():
            try:
                meta = json.loads(j.read_text())
            except ValueError:
                pass
        print(f"{wav.name}  peak={meta.get('peak')} avg={meta.get('avg')} "
              f"hits={meta.get('hits')} win={meta.get('win')}")
        if not args.no_play:
            _play(wav)
        while True:
            choice = input("  [f]alse / [r]eal / [s]kip / [p]lay again / [q]uit > ").strip().lower()
            if choice == "p":
                _play(wav)
                continue
            if choice in ("f", "r", "s", "q"):
                break
        if choice == "q":
            break
        if choice == "s":
            continue
        target = FALSE_DIR if choice == "f" else REAL_DIR
        wav.rename(target / wav.name)
        if j.exists():
            j.rename(target / j.name)
        print(f"  → {target.name}/")


def _stage(src: Path, dst: Path) -> int:
    dst.mkdir(parents=True, exist_ok=True)
    n = 0
    for wav in src.glob("*.wav"):
        shutil.copy2(wav, dst / wav.name)
        n += 1
    return n


def cmd_stage(args: argparse.Namespace) -> None:
    nf = _stage(FALSE_DIR, CORPUS_HARD_NEG)
    nr = _stage(REAL_DIR, CORPUS_POS_REAL)
    print(f"staged {nf} false fire(s) → {CORPUS_HARD_NEG}")
    print(f"staged {nr} real wake(s)  → {CORPUS_POS_REAL}")
    print("\nnow run:  python train_production.py")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    pf = sub.add_parser("fetch", help="download new wake samples from the backend")
    pf.add_argument("--backend", required=True,
                    help="backend base URL, e.g. https://voice-assistant.kirillorlov.pro")
    pf.set_defaults(func=cmd_fetch)

    pl = sub.add_parser("label", help="interactively sort inbox clips into false/real")
    pl.add_argument("--no-play", action="store_true", help="don't try to play clips")
    pl.set_defaults(func=cmd_label)

    ps = sub.add_parser("stage", help="copy labelled clips into the training corpus")
    ps.set_defaults(func=cmd_stage)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
