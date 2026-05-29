#!/usr/bin/env python3
"""Offline validator for the wake-word .tflite — mirrors wake_word.cc exactly.

The whole point of this script is *parity with the device*. It feeds test
WAVs through:

  1. the SAME feature frontend the firmware uses — pymicro_features, which is
     a binding around the exact TFLM microfrontend that wake_word.cc compiles
     in. (Verified parameter-for-parameter; see check_feature_parity.py.)
  2. the SAME input quantization — int8 = round(feature / input_scale + zp).
     pymicro_features already applies the 0.0390625 prescale internally, so we
     must NOT apply it again here; wake_word.cc applies it because it reads the
     raw TFLM frontend output. Net formula is identical.
  3. the SAME detector — fire when >= WAKE_MIN_HITS of the last WAKE_WINDOW_LEN
     model invokes cross WAKE_THRESHOLD (the burst detector added in the
     "fix mic leveling + wake-word burst detector" commit).

so the FRR / FAR it reports predict on-device behavior — to the extent the
test audio resembles what the device actually hears.

LIMITATION — read before trusting the numbers:
  With SYNTHETIC (Piper) test clips this validates feature/quant parity, proves
  the model fires on clean positives, and lets you pick an operating point. It
  does NOT predict real-world FRR/FAR: the device hears your voice through the
  XVF3800 beamformer/AEC/AGC + the +18 dB soft-limiter, which is a different
  acoustic distribution. For field-accurate numbers, point --positive-dir /
  --negative-dir at audio captured through the device.

Usage:
    python verify_model.py \
        --model ../../main/models/wake_word_ru.tflite \
        --positive-dir corpus/holdout/positive \
        --negative-dir corpus/holdout/negative

    # Sweep the operating point (threshold x min-hits) and print an ROC table:
    python verify_model.py --model ... --positive-dir ... --negative-dir ... --roc
"""

from __future__ import annotations

import argparse
import sys
import wave
from pathlib import Path

import numpy as np

# ---- Detector constants — MUST stay in sync with wake_word.cc ------------
# (firmware/components/wake_word/wake_word.cc). check_feature_parity.py guards
# the frontend half of this contract; keep these three in step by hand.
WAKE_WINDOW_LEN = 5
WAKE_MIN_HITS = 2
WAKE_THRESHOLD = 0.95
WAKE_COOLDOWN_MS = 2000

FRONTEND_STRIDE_MS = 10          # one feature slice per 10 ms of audio
SAMPLE_RATE = 16000
STRIDE_SAMPLES = SAMPLE_RATE * FRONTEND_STRIDE_MS // 1000   # 160
FEATURE_SIZE = 40


# ---- Audio + frontend ---------------------------------------------------

def read_wav_16k_mono(path: Path) -> np.ndarray:
    """Load a 16 kHz mono 16-bit PCM WAV as int16. Errors on any other format
    rather than silently resampling — training/holdout clips are produced at
    16 kHz mono (generate_corpus.py), and a quiet resample would break parity."""
    with wave.open(str(path), "rb") as w:
        if w.getframerate() != SAMPLE_RATE or w.getnchannels() != 1 or w.getsampwidth() != 2:
            raise ValueError(
                f"{path.name}: need 16 kHz mono 16-bit, got "
                f"{w.getframerate()} Hz / {w.getnchannels()} ch / {w.getsampwidth()*8} bit")
        return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)


def extract_features(pcm: np.ndarray):
    """Run the TFLM microfrontend over `pcm`, 10 ms at a time, exactly as the
    device does. Returns a list of 40-dim float feature slices (already
    0.0390625-prescaled by pymicro_features)."""
    from pymicro_features import MicroFrontend

    fe = MicroFrontend()
    feats = []
    raw = pcm.astype("<i2").tobytes()
    # Feed STRIDE_SAMPLES (160) at a time; the frontend buffers its 30 ms
    # window internally and emits one slice per 10 ms step.
    for off in range(0, len(pcm) - STRIDE_SAMPLES + 1, STRIDE_SAMPLES):
        chunk = raw[off * 2 : (off + STRIDE_SAMPLES) * 2]
        # process_samples -> MicroFrontendOutput(features: list[float],
        # samples_read: int). `features` is empty until the 30 ms window
        # fills, then carries one 40-dim slice per 10 ms step.
        values = fe.process_samples(chunk).features
        if values:
            feats.append(np.asarray(values, dtype=np.float32))
    return feats


# ---- Model ---------------------------------------------------------------

class StreamingModel:
    """Wraps the streaming quantized tflite, exposing one prob per invoke and
    mirroring wake_word.cc's slice-accumulation + dequant."""

    def __init__(self, model_path: Path):
        try:
            from tflite_runtime.interpreter import Interpreter
        except ImportError:
            try:
                from ai_edge_litert.interpreter import Interpreter
            except ImportError:
                from tensorflow.lite import Interpreter  # type: ignore

        self.interp = Interpreter(model_path=str(model_path))
        self.interp.allocate_tensors()
        self.in_d = self.interp.get_input_details()[0]
        self.out_d = self.interp.get_output_details()[0]

        in_bytes = int(np.prod(self.in_d["shape"]))   # int8 → 1 byte each
        if in_bytes % FEATURE_SIZE != 0:
            raise ValueError(f"input {in_bytes} bytes not a multiple of {FEATURE_SIZE}")
        self.slices = in_bytes // FEATURE_SIZE         # == s_ww_input_slices
        self.in_scale, self.in_zp = self.in_d["quantization"]
        self.out_scale, self.out_zp = self.out_d["quantization"]

    def quantize(self, feat: np.ndarray) -> np.ndarray:
        # int8 = round(value / scale + zp); value already carries 0.0390625.
        q = np.round(feat / self.in_scale + self.in_zp)
        return np.clip(q, -128, 127).astype(np.int8)

    def probs_for_clip(self, feats) -> list[float]:
        """Accumulate `slices` fresh feature slices per invoke (non-overlapping,
        like run_wake_word) and return one dequantized prob per invoke."""
        probs: list[float] = []
        pending = []
        for f in feats:
            pending.append(self.quantize(f))
            if len(pending) < self.slices:
                continue
            block = np.concatenate(pending).reshape(self.in_d["shape"])
            self.interp.set_tensor(self.in_d["index"], block)
            self.interp.invoke()
            raw = self.interp.get_tensor(self.out_d["index"]).flatten()[0]
            prob = float(self.out_scale * (int(raw) - self.out_zp))
            probs.append(min(1.0, max(0.0, prob)))
            pending = []
        return probs


# ---- Detector (mirror of update_window in wake_word.cc) ------------------

def count_fires(probs, threshold, min_hits, invoke_ms) -> int:
    """Number of distinct wake fires over a clip's per-invoke probs, applying
    the window/min-hits rule and the refractory cooldown."""
    fires = 0
    cooldown_invokes = int(round(WAKE_COOLDOWN_MS / invoke_ms)) if invoke_ms else 0
    last_fire = -10**9
    for i in range(len(probs)):
        if i + 1 < WAKE_WINDOW_LEN:
            continue
        window = probs[i + 1 - WAKE_WINDOW_LEN : i + 1]
        hits = sum(1 for p in window if p >= threshold)
        if hits >= min_hits and (i - last_fire) > cooldown_invokes:
            fires += 1
            last_fire = i
    return fires


# ---- Evaluation ---------------------------------------------------------

def load_probs(model: StreamingModel, wav_dir: Path):
    """Return [(name, probs, duration_s), ...] for every WAV in wav_dir."""
    out = []
    for wav in sorted(wav_dir.glob("*.wav")):
        pcm = read_wav_16k_mono(wav)
        feats = extract_features(pcm)
        probs = model.probs_for_clip(feats)
        out.append((wav.name, probs, len(pcm) / SAMPLE_RATE))
    return out


def evaluate(pos, neg, threshold, min_hits, invoke_ms):
    pos_detect = sum(1 for _, p, _ in pos if count_fires(p, threshold, min_hits, invoke_ms) > 0)
    frr = 1.0 - (pos_detect / len(pos)) if pos else float("nan")

    neg_fires = sum(count_fires(p, threshold, min_hits, invoke_ms) for _, p, _ in neg)
    neg_hours = sum(d for _, _, d in neg) / 3600.0
    faph = (neg_fires / neg_hours) if neg_hours > 0 else float("nan")
    return frr, faph, pos_detect, neg_fires


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", required=True, type=Path)
    ap.add_argument("--positive-dir", required=True, type=Path)
    ap.add_argument("--negative-dir", required=True, type=Path)
    ap.add_argument("--roc", action="store_true",
                    help="sweep threshold x min-hits and print an operating-point table")
    args = ap.parse_args()

    for d in (args.positive_dir, args.negative_dir):
        if not d.exists() or not any(d.glob("*.wav")):
            return _fail(f"no WAVs in {d}")

    model = StreamingModel(args.model)
    invoke_ms = model.slices * FRONTEND_STRIDE_MS
    print(f"model: {args.model.name}  slices/invoke={model.slices} "
          f"({invoke_ms} ms/invoke)  in_scale={model.in_scale:.5f} "
          f"out_scale={model.out_scale:.5f}")

    pos = load_probs(model, args.positive_dir)
    neg = load_probs(model, args.negative_dir)
    pos_pmax = max((max(p) if p else 0.0) for _, p, _ in pos)
    print(f"positives: {len(pos)} clips, p_max over all = {pos_pmax:.3f}")
    print(f"negatives: {len(neg)} clips, "
          f"{sum(d for _, _, d in neg)/60:.1f} min total\n")

    if pos_pmax < WAKE_THRESHOLD:
        print(f"WARNING: model never exceeds the configured threshold "
              f"{WAKE_THRESHOLD} on ANY positive clip — it cannot fire. "
              f"The model needs retraining, not threshold tuning.\n")

    if args.roc:
        thresholds = [0.02, 0.05, 0.08, 0.10, 0.15, 0.20, 0.30, 0.50]
        print("ROC sweep (FRR = missed wakes; FAPh = false fires per hour):")
        for mh in (1, 2, 3):
            print(f"\n  min_hits = {mh}")
            print(f"  {'thresh':>7} {'FRR':>7} {'FAPh':>8}  {'detect':>7} {'falses':>7}")
            for th in thresholds:
                frr, faph, pd, nf = evaluate(pos, neg, th, mh, invoke_ms)
                print(f"  {th:>7.2f} {frr*100:>6.1f}% {faph:>8.2f}  "
                      f"{pd:>4}/{len(pos):<3} {nf:>7}")
        print("\nPick the row with FRR < 15% and FAPh < 1.0, then set "
              "WAKE_THRESHOLD / WAKE_MIN_HITS in wake_word.cc to match.")
    else:
        frr, faph, pd, nf = evaluate(pos, neg, WAKE_THRESHOLD, WAKE_MIN_HITS, invoke_ms)
        print(f"At wake_word.cc operating point "
              f"(threshold={WAKE_THRESHOLD}, min_hits={WAKE_MIN_HITS}):")
        print(f"  FRR  = {frr*100:.1f}%   ({pd}/{len(pos)} positives detected)")
        print(f"  FAPh = {faph:.2f}      ({nf} false fires)")
        print(f"  Targets: FRR < 15%, FAPh < 1.0   "
              f"({'PASS' if frr < 0.15 and faph < 1.0 else 'FAIL'})")
    return 0


def _fail(msg: str) -> int:
    print(f"error: {msg}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
