#!/usr/bin/env python3
"""Production-grade training run for the "Эй, Фемто!" wake word.

Wires in everything that train_minimal.py skipped:
  - kahrendt/microwakeword pre-built negative spectrograms (speech /
    dinner_party / no_speech) as ambient training+validation+testing data
  - MIT RIRs for room impulse augmentation of positive clips
  - FMA-small as background-noise augmentation source
  - Notebook-style class weights, longer training (10000 steps),
    SpecAugment regularization

Expect this to take 2-3 hours on a fast CPU. No GPU required but it would
help a lot.

Run from firmware/tools/train_wake_word/ inside the venv. Expects:
  - corpus/positive/*.wav  (generate_corpus.py output)
  - ../upstream-microwakeword cloned

Writes:
  - negative_datasets/   — downloaded HF spectrogram features
  - mit_rirs/            — downloaded room impulse responses (.wav)
  - fma_16k/             — downloaded music clips at 16 kHz
  - generated_features/positive/  — augmented spectrograms for our phrase
  - trained_models/wakeword_prod/ — checkpoints + final tflite
  - ../../main/models/wake_word_ru.tflite — final flashed-ready model
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path
from urllib.request import urlretrieve

import yaml

ROOT       = Path(__file__).resolve().parent
POS_DIR    = ROOT / "corpus" / "positive"
NEG_HF_DIR = ROOT / "negative_datasets"
RIR_DIR    = ROOT / "mit_rirs"
FMA_DIR    = ROOT / "fma_16k"
FEAT_ROOT  = ROOT / "generated_features"
MODEL_DIR  = ROOT / "trained_models" / "wakeword_prod"
FINAL_OUT  = ROOT.parents[1] / "main" / "models" / "wake_word_ru.tflite"


# ---------- Step 1: Download datasets -------------------------------------

KAHRENDT_BASE = "https://huggingface.co/datasets/kahrendt/microwakeword/resolve/main/"
KAHRENDT_FILES = ["speech.zip", "dinner_party.zip",
                  "dinner_party_eval.zip", "no_speech.zip"]


def download(url: str, dest: Path) -> None:
    if dest.exists() and dest.stat().st_size > 0:
        print(f"  ✓ {dest.name} already present ({dest.stat().st_size//1024//1024} MB)")
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"  ↓ {url}")
    print(f"    → {dest}")
    urlretrieve(url, dest)
    print(f"  ✓ {dest.stat().st_size//1024//1024} MB")


def fetch_negative_spectrograms() -> None:
    NEG_HF_DIR.mkdir(parents=True, exist_ok=True)
    for fname in KAHRENDT_FILES:
        zip_path = NEG_HF_DIR / fname
        unpacked = NEG_HF_DIR / fname.replace(".zip", "")
        if unpacked.exists():
            print(f"  ✓ {unpacked.name}/ already unpacked")
            continue
        download(KAHRENDT_BASE + fname, zip_path)
        print(f"  ⊁ unpacking {fname}…")
        with zipfile.ZipFile(zip_path) as z:
            z.extractall(NEG_HF_DIR)


def fetch_mit_rirs() -> None:
    if RIR_DIR.exists() and any(RIR_DIR.glob("*.wav")):
        n = len(list(RIR_DIR.glob("*.wav")))
        print(f"  ✓ MIT RIRs already present ({n} files)")
        return
    RIR_DIR.mkdir(parents=True, exist_ok=True)
    print("  ↓ MIT RIRs from HuggingFace streaming dataset…")
    import datasets, scipy.io.wavfile as wf
    import numpy as np
    rir = datasets.load_dataset("davidscripka/MIT_environmental_impulse_responses",
                                split="train", streaming=True)
    for i, row in enumerate(rir):
        name = row['audio']['path'].split('/')[-1]
        wf.write(str(RIR_DIR / name), 16000,
                 (row['audio']['array'] * 32767).astype(np.int16))
        if (i + 1) % 100 == 0:
            print(f"    {i+1} rirs…")
    print(f"  ✓ wrote {len(list(RIR_DIR.glob('*.wav')))} RIRs")


def fetch_fma_small() -> None:
    if FMA_DIR.exists() and any(FMA_DIR.glob("**/*.wav")):
        n = len(list(FMA_DIR.glob("**/*.wav")))
        print(f"  ✓ FMA-small already present ({n} files)")
        return
    FMA_DIR.mkdir(parents=True, exist_ok=True)
    print("  ↓ FMA-small (~7 GB) — this is the slowest download…")
    import datasets, scipy.io.wavfile as wf
    import numpy as np
    fma = datasets.load_dataset("benjamin-paine/free-music-archive-small",
                                split="train", streaming=True)
    fma = fma.cast_column("audio", datasets.Audio(sampling_rate=16000))
    for i, row in enumerate(fma):
        name = f"fma_{i:05d}.wav"
        wf.write(str(FMA_DIR / name), 16000,
                 (row['audio']['array'] * 32767).astype(np.int16))
        if (i + 1) % 100 == 0:
            print(f"    {i+1} fma clips…")
        if i >= 1500:    # cap — we don't need the whole 8 k tracks
            break
    print(f"  ✓ wrote {len(list(FMA_DIR.glob('*.wav')))} FMA clips")


# ---------- Step 2: Spectrogram generation w/ real augmentation ----------

def gen_positive_spectrograms() -> None:
    from microwakeword.audio.augmentation import Augmentation
    from microwakeword.audio.clips import Clips
    from microwakeword.audio.spectrograms import SpectrogramGeneration
    from mmap_ninja.ragged import RaggedMmap

    out_root = FEAT_ROOT / "positive"
    out_root.mkdir(parents=True, exist_ok=True)

    clips = Clips(
        input_directory=str(POS_DIR),
        file_pattern="*.wav",
        max_clip_duration_s=None,
        remove_silence=False,
        random_split_seed=10,
        split_count=0.1,
    )
    augmenter = Augmentation(
        augmentation_duration_s=3.2,
        augmentation_probabilities={
            "SevenBandParametricEQ": 0.25,
            "TanhDistortion":        0.10,
            "PitchShift":            0.15,
            "BandStopFilter":        0.10,
            "AddColorNoise":         0.20,
            "AddBackgroundNoise":    0.75,
            "Gain":                  1.00,
            "RIR":                   0.50,
        },
        impulse_paths=[str(RIR_DIR)],
        background_paths=[str(FMA_DIR)],
        background_min_snr_db=-5,
        background_max_snr_db=10,
        min_jitter_s=0.195,
        max_jitter_s=0.205,
    )
    for split, split_name, repeat, slide in [
        ("training",  "train",      4, 10),    # 4× repetition for more variety
        ("validation","validation", 1, 10),
        ("testing",   "test",       1, 1),
    ]:
        target = out_root / split / "wakeword_mmap"
        if target.exists():
            print(f"  ✓ {target} already present")
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        spec = SpectrogramGeneration(clips=clips, augmenter=augmenter,
                                     slide_frames=slide, step_ms=10)
        print(f"  → {target} (repeat={repeat})")
        RaggedMmap.from_generator(
            out_dir=str(target),
            sample_generator=spec.spectrogram_generator(split=split_name, repeat=repeat),
            batch_size=100,
            verbose=True,
        )


# ---------- Step 3: Config + train ---------------------------------------

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
                "features_dir":        str(NEG_HF_DIR / "speech"),
                "sampling_weight":     10.0,
                "penalty_weight":      1.0,
                "truth":               False,
                "truncation_strategy": "random",
                "type":                "mmap",
            },
            {
                "features_dir":        str(NEG_HF_DIR / "dinner_party"),
                "sampling_weight":     10.0,
                "penalty_weight":      1.0,
                "truth":               False,
                "truncation_strategy": "random",
                "type":                "mmap",
            },
            {
                "features_dir":        str(NEG_HF_DIR / "no_speech"),
                "sampling_weight":     5.0,
                "penalty_weight":      1.0,
                "truth":               False,
                "truncation_strategy": "random",
                "type":                "mmap",
            },
            {   # validation/testing-only ambient set (sampling_weight = 0 omits from training)
                "features_dir":        str(NEG_HF_DIR / "dinner_party_eval"),
                "sampling_weight":     0.0,
                "penalty_weight":      1.0,
                "truth":               False,
                "truncation_strategy": "split",
                "type":                "mmap",
            },
        ],
        "training_steps":         [10000],
        "positive_class_weight":  [1],
        "negative_class_weight":  [20],
        "learning_rates":         [0.001],
        "batch_size":             128,
        "time_mask_max_size":     [5],
        "time_mask_count":        [2],
        "freq_mask_max_size":     [5],
        "freq_mask_count":        [2],
        "eval_step_interval":     500,
        "clip_duration_ms":       1500,
        "target_minimization":    0.2,         # max 0.2 faph on validation_ambient
        "minimization_metric":    "ambient_false_positives_per_hour",
        "maximization_metric":    "average_viable_recall",
    }
    cfg_path = ROOT / "training_parameters_prod.yaml"
    cfg_path.write_text(yaml.dump(config, sort_keys=False))
    print(f"  ✓ wrote {cfg_path}")
    return cfg_path


def run_training(cfg: Path) -> None:
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
    # Use all cores for spectrogram preprocessing batches.
    env.setdefault("TF_NUM_INTEROP_THREADS", "16")
    env.setdefault("TF_NUM_INTRAOP_THREADS", "16")
    subprocess.check_call(cmd, env=env, cwd=str(ROOT))


def main():
    if not POS_DIR.exists() or not any(POS_DIR.glob("*.wav")):
        sys.exit(f"missing positive corpus: {POS_DIR}\nrun generate_corpus.py first.")

    print("== Step 1: download negative spectrogram features (kahrendt/microwakeword)")
    fetch_negative_spectrograms()
    print("\n== Step 2: download MIT RIRs (room impulse responses)")
    fetch_mit_rirs()
    print("\n== Step 3: download FMA-small (background music for augmentation)")
    fetch_fma_small()

    print("\n== Step 4: spectrogram generation for positive clips (augmented)")
    gen_positive_spectrograms()

    print("\n== Step 5: write training config")
    cfg = write_config()

    print("\n== Step 6: run training (10000 steps — this is the slow part)")
    run_training(cfg)

    src = MODEL_DIR / "tflite_stream_state_internal_quant" / "stream_state_internal_quant.tflite"
    if not src.exists():
        sys.exit(f"training finished but tflite not found at {src}")
    FINAL_OUT.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(src, FINAL_OUT)
    print(f"\n✅ flashed-ready model: {FINAL_OUT} ({src.stat().st_size / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
