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
        if self._model is not None:
            original_transcribe = self._model.transcribe

            def transcribe_with_speedups(audio, **kw):
                kw.setdefault("beam_size", self._beam_size)
                kw.setdefault("best_of", self._best_of)
                return original_transcribe(audio, **kw)

            self._model.transcribe = transcribe_with_speedups
