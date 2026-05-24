# Russian wake word training pipeline

End-to-end recipe for producing `firmware/main/models/wake_word_ru.tflite` —
the INT8 TFLite model that runs on the ESP32-S3 inside `wake_word.c`.

Engine: **[microWakeWord](https://github.com/kahrendt/microWakeWord)** (Apache-2.0,
ESP32-S3-native, used by Home Assistant Voice PE).
Synthetic-voice generator: **[Piper](https://github.com/rhasspy/piper)** —
specifically the `ru_RU-irina-medium` voice already deployed in this project's
backend (`app/bot.py:62`), reused so the device hears training-set audio that
acoustically resembles the model it's also responding to.

## Why we do not train on the device

microWakeWord training needs ~2 GB RAM and ideally a CUDA GPU for the
feature-extraction + Inception-style classifier. The ESP32-S3 only runs
inference. Training is a one-time offline step; the resulting `.tflite` is
~25–35 KB and ships in flash.

## Pipeline overview

```
   wake phrase  ──┐
                  │   generate_corpus.py
   piper voices ──┼─▶ positive_clips/   ──┐
                  │                       │  microWakeWord
   public RU TTS ─┴─▶ negative_clips/   ──┼─▶ train.py ──▶ wake_word_ru.tflite
                                          │
   noise / RIR  ────▶ augmentations    ───┘
```

## One-time setup

```bash
cd firmware/tools/train_wake_word
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt

# Piper will download its ONNX voice file on first synth (~60 MB).
```

## Generate the positive corpus

```bash
python generate_corpus.py \
    --phrase "Эй ассистент" \
    --out-dir corpus/positive \
    --num-samples 2000
```

Defaults:
- 3 voices for diversity (irina / denis / dmitri — all `ru_RU-*-medium`)
- per-clip random length-scale, noise-scale, gain, leading silence
- 16 kHz mono 16-bit WAV (matches XVF3800 post-DSP output exactly)

## Negative corpus

Pull a few thousand seconds of Russian speech that *does not* contain the wake
phrase. Two options:

1. **Synthetic, fast**: re-run `generate_corpus.py` with a phrase file of
   common Russian sentences (`--phrases-file ./not_wake.txt` — script supports
   reading a list of phrases, one per line).
2. **Real speech, higher quality**: download Common Voice Russian
   (https://commonvoice.mozilla.org/ru/datasets — Apache-2.0) and split to
   ~1 s clips. Closer to deployment audio, but a heavier setup.

Both work; mix is fine.

## Train

```bash
# Clone the upstream training repo (we do not vendor — too large + actively
# evolving):
git clone https://github.com/kahrendt/microWakeWord.git ../upstream-microwakeword
python train.py \
    --positive-dir corpus/positive \
    --negative-dir corpus/negative \
    --upstream    ../upstream-microwakeword \
    --output      ../../main/models/wake_word_ru.tflite
```

Training takes ~30–90 minutes on a single consumer GPU, longer on CPU. The
model is automatically quantized INT8 by the upstream pipeline.

## Verify before flashing

```bash
python verify_model.py \
    --model ../../main/models/wake_word_ru.tflite \
    --test-dir corpus/holdout
```

Reports FRR / FAR. Targets for a first usable model: FRR < 15 %, FAR < 1/hr
(see plan M6b verification section).

## Current trained artifact

`firmware/main/models/wake_word_ru.tflite` (62 KB, INT8 streaming MixedNet)
was produced by `train_minimal.py` against 1500 positive + 1500 negative
synthetic clips, 4000 training steps.

**Quality: poor.** The model collapsed during training (recall ≈ 0,
loss diverged from 1.0 → 5.8). Expected for this corpus size + the lack of
real ambient background data — microWakeWord's own README warns "Training a
model that works well is still very difficult." The file ships so that the
M6b integration code (esp-tflite-micro loading, inference loop) has a real
artifact to load and crash-test against. It will **not** reliably detect
"Эй, Фемто!" in the wild.

Path to a usable model (in rough priority order):
 1. Add real ambient negatives: download
    `huggingface.co/datasets/kahrendt/microwakeword/{speech.zip, dinner_party.zip, no_speech.zip}`
    and wire them in as a second `feature_dir` (see notebook cell 9).
    Several GB total but the single biggest model-quality lever.
 2. Add RIR + AudioSet/FMA augmentations so positives sound like they were
    spoken in real rooms with background noise (notebook cell 4).
 3. Train for 10–20 k steps instead of 4 k.
 4. Sweep `positive_class_weight` ∈ {2,5,10} × `negative_class_weight` ∈ {1,3,20}.
 5. Try `--first_conv_filters 16` + smaller `pointwise_filters` if you want
    to shave RAM at the cost of detection quality.

## Notes / known issues

- **Russian-specific Piper voice quality varies by phrase.** Words with stress
  marks (\`ё\`) sometimes synthesize unnaturally. Use a phrase you've
  test-synthesized first — `piper -m ru_RU-irina-medium --output_file /tmp/x.wav "Эй ассистент"`.
- **Custom phrase**: avoid common Russian words ("привет" alone has too many
  false-accept triggers in normal conversation). 3–5 syllables, phonetically
  distinct, is the sweet spot.
- **First model can be deliberately mediocre.** Get the end-to-end pipeline
  working on hardware first; iterate corpus + model afterward.
