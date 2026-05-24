#!/usr/bin/env python3
"""Minimal training run for the "Эй, Фемто!" wake word.

Strips out the upstream notebook's huge background-dataset downloads (MIT
RIRs / AudioSet / FMA / kahrendt/microwakeword negative spectrograms — many
GB) and uses just our synthetic positive + negative corpora plus light
augmentation. The resulting model is preliminary: it will likely have
higher FAR than a model trained with real ambient data, but it's a real
.tflite you can flash and start iterating on.

Run from firmware/tools/train_wake_word/ inside the venv. Expects:
  - corpus/positive/*.wav  (generate_corpus.py output)
  - corpus/negative/*.wav  (generate_corpus.py --phrases-file output)
  - ../upstream-microwakeword cloned

Writes:
  - generated_features/{positive,negative}/  — spectrogram mmaps
  - trained_models/wakeword/  — checkpoints + final tflite
  - ../../main/models/wake_word_ru.tflite  — copy of the final model
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import yaml

ROOT       = Path(__file__).resolve().parent
POS_DIR    = ROOT / "corpus" / "positive"
NEG_DIR    = ROOT / "corpus" / "negative"
FEAT_ROOT  = ROOT / "generated_features"
MODEL_DIR  = ROOT / "trained_models" / "wakeword"
FINAL_OUT  = ROOT.parents[1] / "main" / "models" / "wake_word_ru.tflite"


def gen_spectrograms(positive: bool, wav_dir: Path, out_dir: Path) -> None:
    """Generate mmap spectrogram features for a split."""
    from microwakeword.audio.augmentation import Augmentation
    from microwakeword.audio.clips import Clips
    from microwakeword.audio.spectrograms import SpectrogramGeneration
    from mmap_ninja.ragged import RaggedMmap

    out_dir.mkdir(parents=True, exist_ok=True)

    clips = Clips(
        input_directory=str(wav_dir),
        file_pattern="*.wav",
        max_clip_duration_s=None,
        remove_silence=False,
        random_split_seed=10,
        split_count=0.1,
    )
    # Minimal augmentation set — no RIR / background mixing because those
    # datasets aren't downloaded. Only level (Gain) + light EQ.
    augmenter = Augmentation(
        augmentation_duration_s=3.2,
        augmentation_probabilities={
            "SevenBandParametricEQ": 0.3,
            "TanhDistortion":        0.1,
            "PitchShift":            0.1,
            "BandStopFilter":        0.1,
            "AddColorNoise":         0.3,
            "AddBackgroundNoise":    0.0,   # disabled — no bg dataset
            "Gain":                  1.0,
            "RIR":                   0.0,   # disabled — no RIR dataset
        },
        impulse_paths=[],
        background_paths=[],
        background_min_snr_db=-5,
        background_max_snr_db=10,
        min_jitter_s=0.195,
        max_jitter_s=0.205,
    )

    for split, split_name, repeat in [
        ("training",  "train",      2),
        ("validation","validation", 1),
        ("testing",   "test",       1),
    ]:
        target = out_dir / split
        target.mkdir(parents=True, exist_ok=True)
        if (target / "wakeword_mmap").exists():
            print(f"  skip {target} (exists)")
            continue
        slide = 10 if split != "testing" else 1
        spec = SpectrogramGeneration(
            clips=clips, augmenter=augmenter,
            slide_frames=slide, step_ms=10,
        )
        print(f"  → {target}")
        RaggedMmap.from_generator(
            out_dir=str(target / "wakeword_mmap"),
            sample_generator=spec.spectrogram_generator(split=split_name, repeat=repeat),
            batch_size=100,
            verbose=True,
        )


def write_config() -> Path:
    config = {
        "window_step_ms":      10,
        "train_dir":           str(MODEL_DIR),
        "features": [
            {
                "features_dir":        str(FEAT_ROOT / "positive"),
                "sampling_weight":     2.0,
                "penalty_weight":      1.0,
                "truth":               True,
                "truncation_strategy": "truncate_start",
                "type":                "mmap",
            },
            {
                "features_dir":        str(FEAT_ROOT / "negative"),
                "sampling_weight":     10.0,
                "penalty_weight":      1.0,
                "truth":               False,
                "truncation_strategy": "random",
                "type":                "mmap",
            },
        ],
        # Shorter than the notebook default (10000) — preliminary model.
        "training_steps":         [4000],
        # Notebook defaults (1 vs 20) collapse to all-negative without ambient
        # data + much longer training. Rebalanced to actually learn positives.
        "positive_class_weight":  [5],
        "negative_class_weight":  [3],
        "learning_rates":         [0.001],
        "batch_size":             64,
        "time_mask_max_size":     [0],
        "time_mask_count":        [0],
        "freq_mask_max_size":     [0],
        "freq_mask_count":        [0],
        "eval_step_interval":     200,
        "clip_duration_ms":       1500,
        "target_minimization":    0.9,
        "minimization_metric":    None,
        "maximization_metric":    "average_viable_recall",
    }
    cfg_path = ROOT / "training_parameters.yaml"
    cfg_path.write_text(yaml.dump(config, sort_keys=False))
    print(f"wrote {cfg_path}")
    return cfg_path


def main():
    for p in (POS_DIR, NEG_DIR):
        if not p.exists() or not any(p.glob("*.wav")):
            sys.exit(f"missing or empty: {p}\nrun generate_corpus.py first.")

    print(f"generating spectrograms (positive: {len(list(POS_DIR.glob('*.wav')))} clips)…")
    gen_spectrograms(True,  POS_DIR, FEAT_ROOT / "positive")
    print(f"generating spectrograms (negative: {len(list(NEG_DIR.glob('*.wav')))} clips)…")
    gen_spectrograms(False, NEG_DIR, FEAT_ROOT / "negative")

    cfg = write_config()

    print("running microwakeword.model_train_eval — this is the slow part…")
    cmd = [
        sys.executable, "-m", "microwakeword.model_train_eval",
        f"--training_config={cfg}",
        "--train", "1",
        "--restore_checkpoint", "1",
        "--test_tflite_streaming_quantized", "1",
        "--use_weights", "best_weights",
        "mixednet",
        "--pointwise_filters", "64,64,64,64",
        "--repeat_in_block",   "1,1,1,1",
        "--mixconv_kernel_sizes", "[5],[7,11],[9,15],[23]",
        "--residual_connection", "0,0,0,0",
        "--first_conv_filters", "32",
        "--first_conv_kernel_size", "5",
        "--stride", "3",
    ]
    env = os.environ.copy()
    env["PYTHONPATH"] = str(ROOT / ".." / "upstream-microwakeword")
    subprocess.check_call(cmd, env=env, cwd=str(ROOT))

    src = MODEL_DIR / "tflite_stream_state_internal_quant" / "stream_state_internal_quant.tflite"
    if not src.exists():
        sys.exit(f"training finished but tflite not found at {src}")
    FINAL_OUT.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(src, FINAL_OUT)
    print(f"\nflashed-ready model: {FINAL_OUT} ({src.stat().st_size / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
