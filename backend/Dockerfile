# Self-hosted Russian voice assistant — Pipecat + SmallWebRTC.
FROM python:3.12-slim

# Runtime libraries:
#  - ffmpeg / libgomp1: ctranslate2 (faster-whisper) and onnxruntime (Piper, Silero)
#  - ca-certificates: HTTPS to Hugging Face / the Piper voice repo on first run
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates ffmpeg libgomp1 \
    && rm -rf /var/lib/apt/lists/*

# uv — the Python package/dependency manager.
COPY --from=ghcr.io/astral-sh/uv:latest /uv /uvx /bin/

ENV UV_LINK_MODE=copy \
    UV_COMPILE_BYTECODE=1 \
    HF_HOME=/models/hf \
    PIPER_DOWNLOAD_DIR=/models/piper \
    HTTP_PORT=7860

WORKDIR /app

# Install dependencies first so this layer is cached across code changes.
# uv.lock is copied if present (committed lockfile -> reproducible builds).
COPY pyproject.toml uv.lock* ./
RUN uv sync --no-dev --no-install-project

# Application code. Glob *.py so adding new modules doesn't silently break
# the image (tests/ is a subdir and is not matched by *.py at the top level).
COPY *.py *.html ./

EXPOSE 7860

# Default: run the voice assistant server.
# The initContainer overrides this to: uv run --no-dev python download_models.py
CMD ["uv", "run", "--no-dev", "python", "bot.py"]
