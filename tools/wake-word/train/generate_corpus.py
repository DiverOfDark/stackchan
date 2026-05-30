#!/usr/bin/env python3
"""Generate a positive corpus of wake-phrase clips using Piper TTS.

Outputs 16 kHz mono int16 WAVs to --out-dir, one per requested sample.

Variation comes from three sources, all controllable via flags:
  - voice: cycles through --voices (default: irina, denis, dmitri).
  - synth: per-clip random length_scale + noise_scale + noise_w.
  - audio: per-clip random gain + leading silence + light additive noise.

The output naturally matches what XVF3800 hands to the ESP32 after its
on-chip AEC/AGC/NS pipeline (16 kHz, mono, int16).
"""

from __future__ import annotations

import argparse
import io
import random
import sys
import wave
from pathlib import Path

# Default Russian voices on the Piper Hugging Face mirror. All "medium"
# quality — small enough to download quickly, large enough to sound varied.
DEFAULT_VOICES = ["ru_RU-irina-medium", "ru_RU-denis-medium", "ru_RU-dmitri-medium"]

SAMPLE_RATE = 16_000
TARGET_DURATION_S = 1.5   # microWakeWord wants ~1.5 s clips
CLIP_SAMPLES = int(SAMPLE_RATE * TARGET_DURATION_S)


def _load_voice(name: str, download_dir: Path):
    """Lazy-import Piper so --help works without the dep installed."""
    from piper import PiperVoice
    from piper.download_voices import download_voice

    onnx = download_dir / f"{name}.onnx"
    if not onnx.exists():
        print(f"  downloading {name}…", file=sys.stderr)
        download_dir.mkdir(parents=True, exist_ok=True)
        download_voice(name, download_dir)
    return PiperVoice.load(onnx)


def synthesize(voice, text: str, length_scale: float, noise_scale: float,
               noise_w: float):
    """Run Piper for one phrase and return mono float32 in [-1, 1]."""
    import numpy as np
    import soundfile as sf
    from piper import SynthesisConfig
    # piper.synthesize() yields AudioChunk objects; the simplest portable
    # path is to write through wave and re-read. Avoids depending on Piper's
    # internal types (which have changed across releases).
    syn_cfg = SynthesisConfig(length_scale=length_scale,
                              noise_scale=noise_scale,
                              noise_w_scale=noise_w)
    buf = io.BytesIO()
    sample_rate = None
    with wave.open(buf, "wb") as wav:
        for chunk in voice.synthesize(text, syn_config=syn_cfg):
            if sample_rate is None:
                wav.setnchannels(chunk.sample_channels)
                wav.setsampwidth(chunk.sample_width)
                wav.setframerate(chunk.sample_rate)
                sample_rate = chunk.sample_rate
            wav.writeframes(chunk.audio_int16_bytes)
    buf.seek(0)
    audio, sr = sf.read(buf, dtype="float32")
    if sr != SAMPLE_RATE:
        # Piper voices we use are 22.05 kHz; resample if needed.
        from math import gcd
        from scipy.signal import resample_poly
        g = gcd(int(sr), SAMPLE_RATE)
        audio = resample_poly(audio, SAMPLE_RATE // g, int(sr) // g)
    return audio.astype(np.float32)


def fit_to_window(audio, rng: random.Random):
    """Center the clip in a 1.5 s window, with random leading silence."""
    import numpy as np
    if len(audio) >= CLIP_SAMPLES:
        # Too long: take a random crop containing the clip — typically the
        # phrase fits because we're synthesizing a fixed short utterance.
        start = rng.randint(0, len(audio) - CLIP_SAMPLES)
        return audio[start:start + CLIP_SAMPLES]
    out = np.zeros(CLIP_SAMPLES, dtype=np.float32)
    head_room = CLIP_SAMPLES - len(audio)
    offset = rng.randint(0, head_room)
    out[offset:offset + len(audio)] = audio
    return out


def augment(audio, rng: random.Random):
    """Apply random gain + additive noise. Clips to [-1, 1]."""
    import numpy as np
    peak = float(np.max(np.abs(audio))) or 1.0
    audio = audio / peak * rng.uniform(0.3, 0.95)
    noise_db  = rng.uniform(-50, -30)
    noise_amp = 10 ** (noise_db / 20)
    audio = audio + np.random.normal(0, noise_amp, audio.shape).astype(np.float32)
    return np.clip(audio, -1.0, 1.0)


def to_int16(audio):
    import numpy as np
    return (audio * 32767).astype(np.int16)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--phrase", required=True,
                    help="wake phrase to synthesize (Russian).")
    ap.add_argument("--phrases-file", type=Path,
                    help="optional: file with one phrase per line; "
                         "if given, --phrase is the fallback default and "
                         "samples cycle through the file's phrases (useful "
                         "for negative corpora).")
    ap.add_argument("--out-dir", type=Path, required=True)
    ap.add_argument("--num-samples", type=int, default=1000)
    ap.add_argument("--voices", nargs="+", default=DEFAULT_VOICES)
    ap.add_argument("--piper-dir", type=Path,
                    default=Path.home() / ".cache" / "piper-voices")
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    import numpy as np
    import soundfile as sf

    rng = random.Random(args.seed)
    np.random.seed(args.seed)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    if args.phrases_file:
        phrases = [p.strip() for p in args.phrases_file.read_text().splitlines() if p.strip()]
        if not phrases:
            phrases = [args.phrase]
    else:
        phrases = [args.phrase]

    print(f"loading {len(args.voices)} voice(s)…", file=sys.stderr)
    voices = {name: _load_voice(name, args.piper_dir) for name in args.voices}

    for i in range(args.num_samples):
        text = phrases[i % len(phrases)]
        vname = args.voices[i % len(args.voices)]
        v = voices[vname]
        length_scale = rng.uniform(0.85, 1.20)
        noise_scale  = rng.uniform(0.50, 0.75)
        noise_w      = rng.uniform(0.70, 0.90)
        audio = synthesize(v, text, length_scale, noise_scale, noise_w)
        audio = fit_to_window(audio, rng)
        audio = augment(audio, rng)
        out = args.out_dir / f"clip_{i:05d}_{vname.split('-')[1]}.wav"
        sf.write(out, to_int16(audio), SAMPLE_RATE, subtype="PCM_16")
        if (i + 1) % 100 == 0:
            print(f"  {i + 1}/{args.num_samples}", file=sys.stderr)

    print(f"wrote {args.num_samples} clips to {args.out_dir}", file=sys.stderr)


if __name__ == "__main__":
    main()
