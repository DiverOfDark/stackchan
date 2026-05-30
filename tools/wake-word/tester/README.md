# Wake-word model tester

Tiny Flask UI for validating the wake-word `.tflite` against laptop-recorded
audio. Mirrors the firmware inference path exactly (same `pymicro-features`
frontend that microWakeWord training uses, same int8 quantization formula,
same 3-slice rolling input) so any discrepancy between this tester and the
device is *not* the model — it's something in the firmware audio pipeline.

## Run

```bash
cd firmware/tools/wake_word_tester
source ../train_wake_word/.venv/bin/activate   # reuse the existing venv
pip install flask                              # one-time
python app.py
# browse to http://localhost:5000
```

Allow mic access, press record, say "Эй, Фемто!" two or three times, press
stop. The page shows max p(wake) and the per-10ms probability curve over
your recording.

## How to interpret

- **max p ≥ 0.5** with a clear spike on the curve → the model recognizes
  your voice. If the device doesn't fire on the same word, the firmware
  audio path has a residual problem (gain, channel mapping, XVF3800
  preprocessing differs from the laptop mic, …).
- **max p in 0.15–0.5** with a visible bump → the model partially
  recognizes you but is uncertain. Likely close enough that retraining
  with a handful of your real-voice samples would push it over.
- **max p ≤ 0.05, flat curve** → the model genuinely doesn't generalize
  from synthetic TTS to your voice. The training corpus needs real
  recordings.

The first ~300 ms of probabilities are ignored for the "max" calculation
because the streaming model's internal state hasn't filled yet.
