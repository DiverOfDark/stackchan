#
# Pre-download the Whisper model and the Piper voice onto the models PVC.
#
# Runs as an initContainer so the main container starts fast and a pod restart
# never re-downloads (models persist on the PVC: HF_HOME + PIPER_DOWNLOAD_DIR).
# Both steps are idempotent — they skip work if the files already exist.
#
import os
from pathlib import Path

WHISPER_MODEL = os.getenv("WHISPER_MODEL", "medium")
WHISPER_COMPUTE_TYPE = os.getenv("WHISPER_COMPUTE_TYPE", "int8")
PIPER_VOICE = os.getenv("PIPER_VOICE", "ru_RU-irina-medium")
PIPER_DOWNLOAD_DIR = Path(os.getenv("PIPER_DOWNLOAD_DIR", "/models/piper"))


def ensure_whisper() -> None:
    # Constructing WhisperModel downloads the weights into the HF cache
    # (HF_HOME) if they are not already there.
    print(f"[init] Ensuring Whisper '{WHISPER_MODEL}' ({WHISPER_COMPUTE_TYPE}) ...", flush=True)
    from faster_whisper import WhisperModel

    WhisperModel(WHISPER_MODEL, device="cpu", compute_type=WHISPER_COMPUTE_TYPE)
    print("[init] Whisper ready.", flush=True)


def ensure_piper() -> None:
    PIPER_DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)
    onnx = PIPER_DOWNLOAD_DIR / f"{PIPER_VOICE}.onnx"
    if onnx.exists():
        print(f"[init] Piper voice '{PIPER_VOICE}' already present.", flush=True)
        return
    print(f"[init] Downloading Piper voice '{PIPER_VOICE}' ...", flush=True)
    from piper.download_voices import download_voice

    download_voice(PIPER_VOICE, PIPER_DOWNLOAD_DIR)
    print("[init] Piper voice ready.", flush=True)


if __name__ == "__main__":
    ensure_whisper()
    ensure_piper()
    print("[init] All models present.", flush=True)
