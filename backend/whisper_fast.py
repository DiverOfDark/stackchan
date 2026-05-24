"""WhisperSTTService variant with beam_size / best_of overridden for speed.

faster-whisper's default decoding parameters are ``beam_size=5`` and
``best_of=5``. Dropping both to 1 typically cuts STT wall-clock by 30–50%
on CPU at a small accuracy cost — well worth it in a voice loop where
sub-second STT matters more than rare hallucinations on hard audio.

Pipecat's stock ``WhisperSTTService`` doesn't expose these knobs. Rather
than copy its entire ``run_stt`` body (which would silently drift away
from upstream), we just wrap the underlying ``WhisperModel.transcribe``
method to inject our defaults on every call. The parent class keeps
driving the rest of the pipeline.
"""

from __future__ import annotations

from pipecat.services.whisper.stt import WhisperSTTService


class FastWhisperSTTService(WhisperSTTService):
    """``WhisperSTTService`` with ``beam_size`` / ``best_of`` overrides.

    Args:
        beam_size: Beam search width passed to faster-whisper. Default 1
            (greedy decoding) — fastest. Bump up to trade speed for accuracy.
        best_of: Number of candidates to sample with temperature > 0. Default 1.
        **kwargs: Forwarded to ``WhisperSTTService.__init__`` unchanged
            (model, device, compute_type, settings, etc.).
    """

    # Process-wide preloaded WhisperModel handle. Set once at app startup
    # (see _prewarm_whisper in bot.py); every FastWhisperSTTService()
    # constructed after that point reuses this model instead of allocating
    # a fresh one. Required to avoid OOMKill under reconnect loops: a
    # large-v3 turbo model is ~1.5 GB resident; loading one per session
    # exhausts the pod's memory limit in 2-3 reconnects. faster-whisper's
    # WhisperModel.transcribe() is safe to call concurrently against the
    # same model (CTranslate2 manages its own batch scheduler).
    _shared_model = None

    @classmethod
    def set_shared_model(cls, model) -> None:
        """Install a process-wide preloaded WhisperModel."""
        cls._shared_model = model

    def _load(self):
        # Override the parent's per-instance model load — reuse the
        # shared one if available, fall back to parent if not (e.g. in
        # tests or during prewarm itself).
        if self.__class__._shared_model is not None:
            self._model = self.__class__._shared_model
            return
        super()._load()

    def __init__(
        self,
        *,
        beam_size: int = 1,
        best_of: int = 1,
        **kwargs,
    ):
        super().__init__(**kwargs)
        self._beam_size = beam_size
        self._best_of = best_of

        # Wrap the underlying faster-whisper model's transcribe() so every
        # call from the parent's run_stt() picks up our beam_size/best_of
        # unless the caller explicitly overrides them.
        #
        # Idempotency matters now: with the shared-model path above, the
        # same WhisperModel instance is used across many service
        # instantiations. Wrapping in __init__ each time would build a
        # chain of wrappers (each layer adding its own setdefault). We
        # mark the wrapped function with an attribute and skip if seen.
        if self._model is not None and not getattr(
            self._model.transcribe, "_fastwhisper_wrapped", False
        ):
            original_transcribe = self._model.transcribe

            def transcribe_with_speedups(audio, **kw):
                kw.setdefault("beam_size", self._beam_size)
                kw.setdefault("best_of", self._best_of)
                return original_transcribe(audio, **kw)

            transcribe_with_speedups._fastwhisper_wrapped = True
            self._model.transcribe = transcribe_with_speedups
