#!/usr/bin/env python3
"""Run the wake-word .tflite against laptop-recorded audio.

Mirror of the on-device inference: same feature extractor (pymicro-features
== the C microfrontend lib vendored under components/wake_word/microfrontend/),
same int8 quantization formula (ESPHome's `(u*256+333)//666 + INT8_MIN`),
same 3-slice rolling input. The point is to see the model's behaviour on
real human voice without the device or firmware in the loop.

Run from firmware/tools/wake_word_tester/ inside the training venv:

    source ../train_wake_word/.venv/bin/activate
    pip install flask              # one-time
    python app.py
    # then open http://localhost:5000
"""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import tensorflow as tf
from flask import Flask, jsonify, request, send_from_directory
from pymicro_features import MicroFrontend

ROOT       = Path(__file__).resolve().parent
MODEL_PATH = ROOT.parents[1] / "main" / "models" / "wake_word_ru.tflite"

SAMPLE_RATE      = 16000
STRIDE_SAMPLES   = 160      # 10 ms @ 16 kHz
FEATURE_SIZE     = 40
INPUT_SLICES     = 3        # matches the model's 1×3×40 input
WARMUP_SLICES    = 30       # ignore first ~300 ms (model state hasn't filled)

app = Flask(__name__, static_folder=str(ROOT / "static"))

# Load model once.
_interpreter = tf.lite.Interpreter(model_path=str(MODEL_PATH))
_interpreter.allocate_tensors()
_in  = _interpreter.get_input_details()[0]
_out = _interpreter.get_output_details()[0]
print(f"loaded {MODEL_PATH.name}: in={_in['shape']} ({_in['dtype'].__name__}) "
      f"→ out={_out['shape']} ({_out['dtype'].__name__})", flush=True)


def features_for_audio(samples: np.ndarray) -> list[np.ndarray]:
    """Run pymicro-features over int16 mono samples → list of int8[40] slices."""
    mf = MicroFrontend()
    mf.reset()
    slices: list[np.ndarray] = []
    pos = 0
    while pos + STRIDE_SAMPLES <= len(samples):
        chunk = samples[pos:pos + STRIDE_SAMPLES]
        out = mf.process_samples(chunk.tobytes())
        if len(out.features) >= FEATURE_SIZE:
            # Convert uint16 spectrogram bins → int8 the same way the firmware
            # and microWakeWord training do.
            int8 = np.empty(FEATURE_SIZE, dtype=np.int8)
            for i in range(FEATURE_SIZE):
                v = (int(out.features[i]) * 256 + 333) // 666
                v -= 128
                int8[i] = max(-128, min(127, v))
            slices.append(int8)
        pos += STRIDE_SAMPLES
    return slices


def probabilities_for_features(slices: list[np.ndarray]) -> list[float]:
    """Slide a 3-frame window through `slices`, invoke the model, return per-frame p(wake)."""
    if not slices:
        return []
    # Reset streaming state so each request starts clean.
    _interpreter.reset_all_variables()
    scale, zp = _out["quantization"]
    win = np.zeros((1, INPUT_SLICES, FEATURE_SIZE), dtype=np.int8)
    probs: list[float] = []
    for i, slc in enumerate(slices):
        # Shift left, append newest at the right (matches device code).
        win[0, :INPUT_SLICES - 1] = win[0, 1:INPUT_SLICES]
        win[0, INPUT_SLICES - 1]  = slc
        _interpreter.set_tensor(_in["index"], win)
        _interpreter.invoke()
        raw = int(_interpreter.get_tensor(_out["index"]).flatten()[0])
        p = max(0.0, float(scale * (raw - zp)))
        probs.append(p)
    return probs


@app.route("/")
def index():
    return send_from_directory(ROOT, "index.html")


@app.route("/predict", methods=["POST"])
def predict():
    raw = request.get_data()
    if not raw:
        return jsonify({"error": "no audio"}), 400
    samples = np.frombuffer(raw, dtype=np.int16)
    if len(samples) < 480:
        return jsonify({"error": f"too short ({len(samples)} samples)"}), 400

    audio_rms  = float(np.sqrt(np.mean(samples.astype(np.float64) ** 2)))
    audio_peak = int(np.max(np.abs(samples)))

    slices = features_for_audio(samples)
    probs  = probabilities_for_features(slices)

    # Skip the warm-up at the beginning when computing the peak.
    usable = probs[WARMUP_SLICES:] if len(probs) > WARMUP_SLICES else probs
    if usable:
        max_p = float(max(usable))
        peak_i = usable.index(max_p) + WARMUP_SLICES
    else:
        max_p, peak_i = 0.0, 0

    return jsonify({
        "max_prob":      max_p,
        "peak_time_ms":  peak_i * 10,
        "n_slices":      len(slices),
        "n_samples":     int(len(samples)),
        "audio_rms":     audio_rms,
        "audio_peak":    audio_peak,
        "probs":         probs,
    })


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=int(os.getenv("PORT", "5000")), debug=False)
