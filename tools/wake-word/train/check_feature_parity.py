#!/usr/bin/env python3
"""Guard: the device's feature frontend must match microWakeWord's training one.

A wake-word model only works if the log-mel spectrogram it sees at inference is
the same one it was trained on. Training features come from pymicro_features
(`MicroFrontend`), which is a thin binding over the TFLM microfrontend C lib;
the device recomputes the same features in wake_word.cc. If any frontend knob
drifts between the two, the model silently degrades — no crash, just a bad
detection rate that looks like a training problem.

This script parses the frontend config out of wake_word.cc and compares it to
the pymicro_features reference values. It needs no ML dependencies, so it can
run in CI as a cheap regression guard.

Reference values are transcribed from pymicro_features at:
  rhasspy/pymicro-features  src/micro_features.cpp  (init_cfg + FLOAT32_SCALE)
Re-check them if you bump the pymicro_features version used for training.

Exit code 0 = parity OK, 1 = mismatch, 2 = could not parse.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

WAKE_WORD_CC = (Path(__file__).resolve().parents[2]
                / "components" / "wake_word" / "wake_word.cc")

# field -> expected value (from pymicro_features src/micro_features.cpp)
REFERENCE = {
    "window.size_ms":                        30.0,
    "window.step_size_ms":                   10.0,
    "filterbank.num_channels":               40.0,
    "filterbank.lower_band_limit":           125.0,
    "filterbank.upper_band_limit":           7500.0,
    "noise_reduction.smoothing_bits":        10.0,
    "noise_reduction.even_smoothing":        0.025,
    "noise_reduction.odd_smoothing":         0.06,
    "noise_reduction.min_signal_remaining":  0.05,
    "pcan_gain_control.enable_pcan":         1.0,
    "pcan_gain_control.strength":            0.95,
    "pcan_gain_control.offset":              80.0,
    "pcan_gain_control.gain_bits":           21.0,
    "log_scale.enable_log":                  1.0,
    "log_scale.scale_shift":                 6.0,
    # The 0.0390625 prescale pymicro_features applies inside process_samples;
    # wake_word.cc applies it explicitly because it reads the raw frontend
    # output. (FLOAT32_SCALE in micro_features.cpp.)
    "prescale":                              0.0390625,
}


def parse_wake_word_cc(text: str) -> dict[str, float]:
    # #define MACRO value  — for the window/step/channels macros.
    macros = {m: float(v) for m, v in
              re.findall(r"#define\s+(\w+)\s+(\d+(?:\.\d+)?)", text)}

    def resolve(token: str) -> float:
        token = token.strip().rstrip("f")
        if token in macros:
            return macros[token]
        return float(token)

    found: dict[str, float] = {}
    # s_frontend_cfg.<path> = <value>;
    for path, val in re.findall(
            r"s_frontend_cfg\.([\w.]+)\s*=\s*([^;]+);", text):
        try:
            found[path] = resolve(val)
        except ValueError:
            pass  # non-numeric RHS (none expected) — skip

    # The prescale lives in: s_quant_mult = 0.0390625f / s_ww_input->params.scale;
    m = re.search(r"s_quant_mult\s*=\s*([\d.]+)f?\s*/", text)
    if m:
        found["prescale"] = float(m.group(1))
    return found


def main() -> int:
    if not WAKE_WORD_CC.exists():
        print(f"error: {WAKE_WORD_CC} not found", file=sys.stderr)
        return 2

    found = parse_wake_word_cc(WAKE_WORD_CC.read_text())

    ok = True
    print(f"{'field':<38} {'wake_word.cc':>14} {'pymicro ref':>14}  status")
    for field, ref in REFERENCE.items():
        dev = found.get(field)
        if dev is None:
            print(f"{field:<38} {'MISSING':>14} {ref:>14.5g}  ✗ not parsed")
            ok = False
            continue
        match = abs(dev - ref) <= 1e-6 * max(1.0, abs(ref))
        print(f"{field:<38} {dev:>14.5g} {ref:>14.5g}  {'✓' if match else '✗ MISMATCH'}")
        ok = ok and match

    print()
    if ok:
        print("PARITY OK — device frontend matches microWakeWord training.")
        return 0
    print("PARITY MISMATCH — retrain features and device WILL disagree. Fix "
          "wake_word.cc (or update REFERENCE if pymicro_features changed).")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
