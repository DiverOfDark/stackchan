#!/usr/bin/env python3
"""Blow up a small set of real "Эй, Фемто!" recordings into a much larger
augmented training corpus via pitch / time / EQ / noise variations.

Each input clip in `corpus/positive_real/` becomes N augmented clips in
`corpus/positive_real_aug/`. With 10-15 real recordings × 15 variants you
get ~150-225 effective samples — enough to push microWakeWord past
"sounds nothing like a Piper voice" into actually recognizing your voice.

Run from firmware/tools/wake_word_tester/ inside the training venv:

    source ../train_wake_word/.venv/bin/activate
    python augment_samples.py [--variants 15]

The variants per source are deliberately diverse: pitch ±3 semitones,
speed ±15%, light EQ tilts, and per-clip noise + gain randomization.
Source clips are 1.5 s mono int16 @ 16 kHz (auto-trimmed by app.py /save).
"""

from __future__ import annotations

import argparse
import random
import shutil
from pathlib import Path

import numpy as np
import soundfile as sf

ROOT       = Path(__file__).resolve().parent
SRC_DIR    = ROOT.parent / "train_wake_word" / "corpus" / "positive_real"
DST_DIR    = ROOT.parent / "train_wake_word" / "corpus" / "positive_real_aug"

SAMPLE_RATE = 16000
CLIP_LEN    = SAMPLE_RATE * 3 // 2   # 1.5 s

# Variation grid — picked to overlap human delivery variance without being so
# extreme that the result no longer sounds like the same phrase.
PITCH_SEMITONES = [-3, -1.5, 0, 1.5, 3]
TIME_RATES      = [0.88, 1.0, 1.13]


def pitch_shift(samples: np.ndarray, n_steps: float) -> np.ndarray:
    import librosa
    f = librosa.effects.pitch_shift(samples.astype(np.float32) / 32768.0,
                                     sr=SAMPLE_RATE, n_steps=n_steps)
    return (np.clip(f, -1.0, 1.0) * 32767).astype(np.int16)


def time_stretch(samples: np.ndarray, rate: float) -> np.ndarray:
    import librosa
    f = librosa.effects.time_stretch(samples.astype(np.float32) / 32768.0,
                                      rate=rate)
    return (np.clip(f, -1.0, 1.0) * 32767).astype(np.int16)


def random_eq(samples: np.ndarray, rng: random.Random) -> np.ndarray:
    """Three-band shelf via successive biquads. Subtle ±3 dB tilt."""
    from scipy.signal import butter, sosfilt
    f = samples.astype(np.float32)
    # Random low-shelf gain (boost/cut bass)
    low_gain  = 10 ** (rng.uniform(-3, 3) / 20)
    high_gain = 10 ** (rng.uniform(-3, 3) / 20)
    sos_lp = butter(2, 300 / (SAMPLE_RATE / 2), btype='low',  output='sos')
    sos_hp = butter(2, 3000 / (SAMPLE_RATE / 2), btype='high', output='sos')
    low  = sosfilt(sos_lp, f) * (low_gain  - 1)
    high = sosfilt(sos_hp, f) * (high_gain - 1)
    out  = f + low + high
    return np.clip(out, -32768, 32767).astype(np.int16)


def random_noise(samples: np.ndarray, rng: random.Random) -> np.ndarray:
    """Add gaussian noise at -40 to -25 dB below peak."""
    snr_db = rng.uniform(25, 40)
    sig_peak = float(np.max(np.abs(samples))) or 1.0
    noise_amp = sig_peak / (10 ** (snr_db / 20))
    noise = np.random.normal(0, noise_amp, samples.shape).astype(np.float32)
    out = samples.astype(np.float32) + noise
    return np.clip(out, -32768, 32767).astype(np.int16)


def random_gain(samples: np.ndarray, rng: random.Random) -> np.ndarray:
    """Peak-normalize then attenuate to a random level in [-9, -3] dBFS."""
    peak = float(np.max(np.abs(samples))) or 1.0
    target_db = rng.uniform(-9, -3)
    target_peak = 32767 * (10 ** (target_db / 20))
    out = samples.astype(np.float32) * (target_peak / peak)
    return np.clip(out, -32768, 32767).astype(np.int16)


def pad_or_trim(samples: np.ndarray) -> np.ndarray:
    if len(samples) >= CLIP_LEN:
        return samples[:CLIP_LEN]
    out = np.zeros(CLIP_LEN, dtype=np.int16)
    out[:len(samples)] = samples
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--variants", type=int, default=15,
                    help="how many augmented clips to emit per source recording")
    ap.add_argument("--clean", action="store_true",
                    help="wipe the destination dir first")
    ap.add_argument("--seed", type=int, default=1337)
    args = ap.parse_args()

    sources = sorted(SRC_DIR.glob("real_*.wav"))
    if not sources:
        raise SystemExit(
            f"no recordings in {SRC_DIR}/. Use the wake_word_tester UI "
            "(app.py) to record some first.")
    if args.clean and DST_DIR.exists():
        shutil.rmtree(DST_DIR)
    DST_DIR.mkdir(parents=True, exist_ok=True)

    rng = random.Random(args.seed)
    np.random.seed(args.seed)
    print(f"sources: {len(sources)}  → emitting {args.variants}/source = {len(sources)*args.variants} clips")

    next_idx = 0
    for src in sources:
        base, sr = sf.read(src, dtype="int16")
        if sr != SAMPLE_RATE:
            print(f"  warn: {src.name} is {sr} Hz, expected {SAMPLE_RATE}; skipping")
            continue
        for _ in range(args.variants):
            pitch = rng.choice(PITCH_SEMITONES)
            rate  = rng.choice(TIME_RATES)
            x = base
            if pitch != 0: x = pitch_shift(x, pitch)
            if rate  != 1: x = time_stretch(x, rate)
            x = random_eq(x, rng)
            x = random_gain(x, rng)
            x = random_noise(x, rng)
            x = pad_or_trim(x)
            out = DST_DIR / f"aug_{next_idx:05d}.wav"
            sf.write(out, x, SAMPLE_RATE, subtype="PCM_16")
            next_idx += 1
        print(f"  ✓ {src.name} → {args.variants} variants")

    print(f"\nwrote {next_idx} clips to {DST_DIR.relative_to(ROOT.parent.parent)}")
    print("now run: python ../train_wake_word/train_production.py")


if __name__ == "__main__":
    main()
