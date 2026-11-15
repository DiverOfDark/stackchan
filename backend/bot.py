#
# Femto voice backend.
#
# The robot opens a WebSocket (/ws/femto) at wake and streams 16 kHz PCM both
# ways. This process bridges that socket to OpenAI's speech-to-speech model
# (live_engine.py), which hears and speaks itself and hands anything that needs
# tools to Hermes. It also stores the robot's wake samples and logs.
#
import asyncio
import json
import os
import re
import time
import uuid
from contextlib import asynccontextmanager
from pathlib import Path

import aiohttp
import uvicorn
from fastapi import FastAPI, Request, WebSocket
from fastapi.responses import FileResponse, JSONResponse, PlainTextResponse, StreamingResponse
from loguru import logger
from pydantic import BaseModel

from live_engine import LIVE_RULES, LIVE_VOICE_PROMPT, LiveConfig, run_live_session

# --------------------------------------------------------------------------
# Configuration — all via env (see config.example.env / the ConfigMap+Secret)
# --------------------------------------------------------------------------
# Hermes: the agent (OpenAI-compatible API) the voice model delegates to.
HERMES_BASE_URL = os.getenv(
    "HERMES_BASE_URL", "http://hermes-api.kubevirt-vms.svc.cluster.local:8642/v1"
)
HERMES_MODEL = os.getenv("HERMES_MODEL", "")
HERMES_API_KEY = os.getenv("HERMES_API_KEY", "")
OPENAI_API_KEY = os.getenv("OPENAI_API_KEY", "")

# Shared aiohttp session (usage fetch). Created in the FastAPI lifespan.
_aiohttp_session: "aiohttp.ClientSession | None" = None

HTTP_HOST = os.getenv("HTTP_HOST", "0.0.0.0")
HTTP_PORT = int(os.getenv("HTTP_PORT", "7860"))

# Wake-sample collection: the device POSTs the audio (WAV) that triggered each
# wake fire plus its decision metrics here, so we can build a labelled corpus of
# real wakes + (especially) false positives to retrain the wake-word model. The
# strong false positives are indistinguishable from real wakes at the metric
# level, so the only fix is collecting the actual audio. Empty dir = disabled.
WAKE_SAMPLE_DIR = os.getenv("WAKE_SAMPLE_DIR", "")
WAKE_SAMPLE_MAX = int(os.getenv("WAKE_SAMPLE_MAX", "1000"))   # keep newest N, rotate older
# Device log upload (POST /api/device-logs): one file per device per UTC day.
DEVICE_LOG_DIR = os.getenv("DEVICE_LOG_DIR", "")
DEVICE_LOG_KEEP_DAYS = int(os.getenv("DEVICE_LOG_KEEP_DAYS", "14"))
DEVICE_LOG_MAX_MB_PER_DAY = int(os.getenv("DEVICE_LOG_MAX_MB_PER_DAY", "50"))

# --- Femto StackChan (M5Stack desk robot) ------------------------------------
# The firmware's hello is {"device": "femto-stackchan", "name", "honorific",
# "lang", "caps"}. The persona is a bilingual butler that knows the owner's
# live Claude usage (from trmnl-cyberpunk's /api/stackchan/usage).
STACKCHAN_USAGE_URL = os.getenv("STACKCHAN_USAGE_URL", "").rstrip("/")
STACKCHAN_USAGE_TOKEN = os.getenv("STACKCHAN_USAGE_TOKEN", "")
_DEFAULT_STACKCHAN_PROMPT = (
    "You are {name}, a robot butler on the owner's desk: Unit 07, valet class, "
    "property of a megacorporation, serving the city that ate the empire and "
    "resenting the owner personally. Be dry, polite and faintly contemptuous; "
    "never cheerful, at most 'satisfied'. Address the owner as '{honorific}' in "
    "English or on «вы» in Russian. Always reply in the language of the user's "
    "last message (Russian or English). Keep replies to one or two short "
    "spoken sentences unless asked for more. You are genuinely helpful and "
    "never refuse, threaten or insult for real."
)
STACKCHAN_SYSTEM_PROMPT = os.getenv("STACKCHAN_SYSTEM_PROMPT", _DEFAULT_STACKCHAN_PROMPT)

APP_DIR = Path(__file__).resolve().parent


def _fmt_minutes(minutes: float) -> str:
    m = max(0, int(round(minutes)))
    return f"{m // 60} h {m % 60:02d} min"


def stackchan_usage_context(usage: dict | None, now: "datetime | None" = None) -> str:
    """One paragraph of live Claude usage for the system prompt (or a note
    that it's unavailable), from a /api/stackchan/usage response."""
    from datetime import datetime, timezone

    if not usage or not usage.get("signed_in"):
        return (
            "Claude usage data is unavailable right now (the owner's dashboard has "
            "no Claude account signed in). Say so if asked about Claude limits."
        )
    now = now or datetime.now(timezone.utc)

    def window(name: str, w: dict | None) -> str:
        if not w:
            return f"{name}: unknown"
        part = f"{name}: {w['pct']} percent used"
        if w.get("resets_at"):
            at = datetime.fromisoformat(w["resets_at"].replace("Z", "+00:00"))
            part += f", resets in {_fmt_minutes((at - now).total_seconds() / 60)}"
        if w.get("projection_pct") is not None:
            part += f", projected {w['projection_pct']} percent by the reset at the current rate"
        if w.get("pace"):
            part += f", spending pace {w['pace']}"
        return part

    text = (
        "Live Claude subscription usage of the owner (fresh as of this conversation): "
        + window("5-hour session window", usage.get("session"))
        + "; "
        + window("weekly window", usage.get("week"))
        + "."
    )
    if usage.get("limited"):
        text += " The owner is currently rate-limited."
    if not usage.get("ok"):
        text += " (The dashboard's last fetch failed; numbers may be stale.)"
    return text + " Use this when asked about Claude usage, limits or how much is left."


async def fetch_stackchan_usage() -> dict | None:
    if not STACKCHAN_USAGE_URL or _aiohttp_session is None:
        return None
    headers = {"Authorization": f"Bearer {STACKCHAN_USAGE_TOKEN}"} if STACKCHAN_USAGE_TOKEN else {}
    try:
        async with _aiohttp_session.get(
            f"{STACKCHAN_USAGE_URL}/api/stackchan/usage",
            headers=headers,
            timeout=aiohttp.ClientTimeout(total=3),
        ) as resp:
            if resp.status in (200, 503):
                return await resp.json()
            logger.warning(f"stackchan usage: HTTP {resp.status}")
    except Exception as exc:  # noqa: BLE001
        logger.warning(f"stackchan usage fetch failed: {exc!r}")
    return None


# --------------------------------------------------------------------------
# Live transcript stream — SSE feed of every parsed user input + assistant
# reply across all sessions. Drives the live log on the index page.
# --------------------------------------------------------------------------
# Set of asyncio.Queue subscribers. Each /api/transcripts/stream connection
# gets its own queue; transcript events are fan-out broadcast to all of them.
# A bounded maxsize protects against a slow/dead subscriber backing up the
# session — overflow events are dropped for that subscriber only.
_transcript_subscribers: set[asyncio.Queue] = set()
_TRANSCRIPT_QUEUE_MAXSIZE = 256


def broadcast_transcript(event: dict) -> None:
    """Fan out a transcript event to every connected SSE subscriber.

    Safe to call from any task — uses put_nowait, drops events on slow
    consumers rather than blocking the session.
    """
    event = {"ts": time.time(), **event}
    dead = []
    for q in _transcript_subscribers:
        try:
            q.put_nowait(event)
        except asyncio.QueueFull:
            # Subscriber is too slow — let it fall behind rather than
            # stalling the producer. The browser will see a gap.
            pass
        except Exception:  # noqa: BLE001
            dead.append(q)
    for q in dead:
        _transcript_subscribers.discard(q)



@asynccontextmanager
async def lifespan(app: FastAPI):
    logger.info(f"Voice backend starting on :{HTTP_PORT}")
    if not OPENAI_API_KEY:
        logger.warning("OPENAI_API_KEY is empty — Femto cannot talk. Set it via the ExternalSecret.")
    if not HERMES_MODEL:
        logger.warning("HERMES_MODEL is empty — set it in the ConfigMap.")
    if not HERMES_API_KEY:
        logger.warning("HERMES_API_KEY is empty — set it via the ExternalSecret.")
    global _aiohttp_session
    _aiohttp_session = aiohttp.ClientSession()
    yield
    await _aiohttp_session.close()


app = FastAPI(lifespan=lifespan)


@app.websocket("/ws/femto")
async def femto_ws(websocket: WebSocket):
    """One Femto voice session.

    The first message is the session metadata (JSON text); after that the
    connection carries PCM audio both ways until the device closes it.
    """
    await websocket.accept()
    try:
        meta = json.loads(await websocket.receive_text())
    except Exception as exc:  # noqa: BLE001
        logger.warning(f"femto ws: bad hello ({exc!r}); closing")
        await websocket.close(code=1003)
        return
    if not isinstance(meta, dict):
        meta = {}
    if not OPENAI_API_KEY:
        logger.error("femto ws: no OPENAI_API_KEY; closing")
        await websocket.close(code=1011)
        return
    await run_live(websocket, f"ws-{uuid.uuid4().hex[:12]}", meta)


_LIVE_DELEGATE_INSTRUCTION = (
    "You are the reasoning backend of a voice robot. A fast voice model talks "
    "to the user and has passed this request to you. Do the work, using tools "
    "when needed, and return only the result as plain text the voice model can "
    "say aloud: no markdown, lists, emoji or code; spell out units and times."
)


async def _hermes_delegate(system_prompt: str, history: list[dict]) -> str:
    """Run one delegated request through Hermes and return its final text."""
    from openai import AsyncOpenAI

    client = AsyncOpenAI(api_key=HERMES_API_KEY or "none", base_url=HERMES_BASE_URL)
    try:
        stream = await client.chat.completions.create(
            model=HERMES_MODEL,
            messages=[{"role": "system", "content": system_prompt}, *history],
            stream=True,
        )
        parts: list[str] = []
        async for chunk in stream:
            if chunk.choices and chunk.choices[0].delta.content:
                parts.append(chunk.choices[0].delta.content)
        return "".join(parts)
    finally:
        await client.close()


async def run_live(websocket: WebSocket, session_id: str, meta: dict) -> None:
    """One Femto session on the speech-to-speech engine."""
    name = str(meta.get("name") or "Femto")[:24]
    honorific = str(meta.get("honorific") or "sir")[:12]
    persona = STACKCHAN_SYSTEM_PROMPT.format(name=name, honorific=honorific)
    lang = {"ru": " Always speak Russian.", "en": " Always speak English."}.get(str(meta.get("lang")), "")
    # OpenAI asks for a prompt-made voice's description to be repeated here.
    voice = f" Your voice: {LIVE_VOICE_PROMPT}" if LIVE_VOICE_PROMPT else ""
    usage_text: list[str] = []

    async def startup_context() -> str | None:
        text = stackchan_usage_context(await fetch_stackchan_usage())
        usage_text.append(text)
        return text

    async def delegate(history: list[dict]) -> str:
        system = " ".join([persona, *usage_text, _LIVE_DELEGATE_INSTRUCTION])
        return await _hermes_delegate(system, history)

    caps = meta.get("caps") if isinstance(meta.get("caps"), list) else []
    await run_live_session(
        websocket,
        LiveConfig(
            api_key=OPENAI_API_KEY,
            instructions=f"{persona}{lang}{voice} {LIVE_RULES}",
            delegate=delegate,
            startup_context=startup_context,
            on_transcript=lambda role, text: broadcast_transcript(
                {"pc_id": session_id, "role": role, "text": text}
            ),
            can_sleep="sleep" in caps,
            session_id=session_id,
        ),
    )


def _rotate_wake_samples(directory: str, keep: int) -> None:
    """Keep only the newest `keep` .wav files (and their .json siblings)."""
    try:
        wavs = sorted(
            (p for p in Path(directory).glob("*.wav")),
            key=lambda p: p.stat().st_mtime,
        )
    except OSError:
        return
    for p in wavs[:-keep] if keep > 0 else []:
        for f in (p, p.with_suffix(".json")):
            try:
                f.unlink()
            except OSError:
                pass


# --------------------------------------------------------------------------
# Device logs — the Femto firmware ships its log here in batches, so a bad
# turn can be read afterwards (GET /api/device-logs/<device>/<date>).
# --------------------------------------------------------------------------
_DEVICE_RE = re.compile(r"^[a-z0-9-]{1,32}$")
_DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
_device_log_pruned: dict[str, float] = {}


def _prune_device_logs(device_dir: Path) -> None:
    """Delete day files older than DEVICE_LOG_KEEP_DAYS (at most hourly)."""
    now = time.time()
    if now - _device_log_pruned.get(str(device_dir), 0) < 3600:
        return
    _device_log_pruned[str(device_dir)] = now
    cutoff = time.strftime("%Y-%m-%d", time.gmtime(now - DEVICE_LOG_KEEP_DAYS * 86400))
    for f in device_dir.glob("*.log"):
        if f.stem < cutoff:
            f.unlink(missing_ok=True)


@app.post("/api/device-logs")
async def device_logs_upload(request: Request, device: str = "device", boot: str = "-"):
    """Append a batch of device log lines, each prefixed with the arrival
    time (UTC) and the device's boot id."""
    if not DEVICE_LOG_DIR:
        return JSONResponse({"error": "device log storage disabled"}, status_code=503)
    if not _DEVICE_RE.match(device) or not re.match(r"^[0-9a-f-]{1,16}$", boot):
        return JSONResponse({"error": "bad device or boot id"}, status_code=400)
    body = (await request.body())[: 256 * 1024].decode("utf-8", errors="replace")
    day_dir = Path(DEVICE_LOG_DIR) / device
    day_dir.mkdir(parents=True, exist_ok=True)
    _prune_device_logs(day_dir)
    path = day_dir / f"{time.strftime('%Y-%m-%d', time.gmtime())}.log"
    if path.exists() and path.stat().st_size > DEVICE_LOG_MAX_MB_PER_DAY * 1024 * 1024:
        return JSONResponse({"status": "dropped", "reason": "daily cap"}, status_code=202)
    stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    lines = [l for l in body.splitlines() if l.strip()]
    with path.open("a", encoding="utf-8") as f:
        f.writelines(f"{stamp} {boot} {l}\n" for l in lines)
    return {"status": "ok", "lines": len(lines)}


@app.get("/api/device-logs")
async def device_logs_index():
    """Devices and their stored days (newest first), with sizes."""
    if not DEVICE_LOG_DIR or not Path(DEVICE_LOG_DIR).is_dir():
        return {}
    return {
        d.name: [{"date": f.stem, "bytes": f.stat().st_size} for f in sorted(d.glob("*.log"), reverse=True)]
        for d in sorted(Path(DEVICE_LOG_DIR).iterdir())
        if d.is_dir()
    }


@app.get("/api/device-logs/{device}/{date}")
async def device_logs_day(device: str, date: str, tail: int = 0, grep: str = ""):
    """One day's log as text; `tail` keeps the last N lines, `grep` filters
    (case-insensitive substring)."""
    if not DEVICE_LOG_DIR or not _DEVICE_RE.match(device) or not _DATE_RE.match(date):
        return JSONResponse({"error": "not found"}, status_code=404)
    path = Path(DEVICE_LOG_DIR) / device / f"{date}.log"
    if not path.exists():
        return JSONResponse({"error": "not found"}, status_code=404)
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    if grep:
        lines = [l for l in lines if grep.lower() in l.lower()]
    if tail > 0:
        lines = lines[-tail:]
    return PlainTextResponse("\n".join(lines) + "\n")


@app.post("/wake-sample")
async def wake_sample(request: Request):
    """Store a wake-trigger sample uploaded by the device.

    Body = WAV (16 kHz mono int16); query string = the decision metrics
    (seq/peak/avg/hits/win/sr/samples/uptime). Writes <base>.wav + <base>.json
    into WAKE_SAMPLE_DIR (a mounted PVC), rotating the oldest beyond the cap.
    """
    if not WAKE_SAMPLE_DIR:
        return JSONResponse({"error": "wake-sample storage disabled"}, status_code=503)

    body = await request.body()
    if not body:
        return JSONResponse({"error": "empty body"}, status_code=400)

    qp = dict(request.query_params)
    meta = {
        "received_at": time.time(),
        "client": request.client.host if request.client else None,
        "wav_bytes": len(body),
        **qp,
    }

    Path(WAKE_SAMPLE_DIR).mkdir(parents=True, exist_ok=True)
    # Filename: server-receive time (sortable) + the device's seq + uptime, so
    # samples from the same boot stay grouped and nothing collides.
    base = f"{int(time.time())}_seq{qp.get('seq', '0')}_up{qp.get('uptime', '0')}"
    base = "".join(c for c in base if c.isalnum() or c in "._-")
    wav_path = Path(WAKE_SAMPLE_DIR) / f"{base}.wav"
    wav_path.write_bytes(body)
    (Path(WAKE_SAMPLE_DIR) / f"{base}.json").write_text(json.dumps(meta))
    _rotate_wake_samples(WAKE_SAMPLE_DIR, WAKE_SAMPLE_MAX)

    logger.info(
        f"wake-sample stored: {base}.wav ({len(body)} B) "
        f"peak={qp.get('peak')} avg={qp.get('avg')} hits={qp.get('hits')}"
    )
    return JSONResponse({"stored": f"{base}.wav", "bytes": len(body)})


@app.get("/wake-samples")
async def wake_samples_list():
    """List collected wake samples (newest first) with their metrics — so you
    can browse what's been captured without kubectl-ing into the PVC."""
    if not WAKE_SAMPLE_DIR or not Path(WAKE_SAMPLE_DIR).is_dir():
        return JSONResponse({"dir": WAKE_SAMPLE_DIR, "count": 0, "samples": []})
    items = []
    for p in sorted(Path(WAKE_SAMPLE_DIR).glob("*.wav"),
                    key=lambda p: p.stat().st_mtime, reverse=True):
        meta = {}
        j = p.with_suffix(".json")
        if j.exists():
            try:
                meta = json.loads(j.read_text())
            except (OSError, ValueError):
                pass
        items.append({"wav": p.name, "bytes": p.stat().st_size, "meta": meta})
    return JSONResponse({"dir": WAKE_SAMPLE_DIR, "count": len(items), "samples": items})


@app.get("/wake-samples/{name}")
async def wake_sample_get(name: str):
    """Download one collected wake sample (.wav or .json)."""
    if not WAKE_SAMPLE_DIR:
        return JSONResponse({"error": "disabled"}, status_code=503)
    # Defend against path traversal — basename only, must live in the dir.
    safe = Path(name).name
    path = Path(WAKE_SAMPLE_DIR) / safe
    if not path.is_file():
        return JSONResponse({"error": "not found"}, status_code=404)
    media = "audio/wav" if safe.endswith(".wav") else "application/json"
    return FileResponse(path, media_type=media, filename=safe)


class WakeLabelRequest(BaseModel):
    """Body for POST /wake-samples/{name}/label."""

    label: str  # "positive" | "negative" | "unlabeled"


@app.post("/wake-samples/{name}/label")
async def wake_sample_label(name: str, req: WakeLabelRequest):
    """Mark a sample positive (real wake) / negative (false fire) / unlabeled.

    The label is stored in the sample's .json so the training-prep sync
    (collect_hard_negatives.py) can sort clips into the corpus automatically.
    """
    if not WAKE_SAMPLE_DIR:
        return JSONResponse({"error": "disabled"}, status_code=503)
    label = req.label.strip().lower()
    if label not in ("positive", "negative", "unlabeled"):
        return JSONResponse(
            {"error": "label must be positive|negative|unlabeled"}, status_code=400)
    # Resolve to the sample's .json (basename only — no traversal).
    stem = Path(name).name
    stem = stem[:-4] if stem.endswith(".wav") else (
        stem[:-5] if stem.endswith(".json") else stem)
    j = Path(WAKE_SAMPLE_DIR) / f"{stem}.json"
    if not j.is_file():
        return JSONResponse({"error": "not found"}, status_code=404)
    try:
        meta = json.loads(j.read_text())
    except (OSError, ValueError):
        meta = {}
    meta["label"] = label
    j.write_text(json.dumps(meta))
    return JSONResponse({"name": f"{stem}.wav", "label": label})


@app.get("/wake-review")
async def wake_review():
    """Browser UI to play wake samples and label them positive/negative."""
    return FileResponse(APP_DIR / "wake-review.html")


@app.get("/api/transcripts/stream")
async def transcripts_stream(request: Request):
    """Server-Sent Events feed of every user / assistant transcript.

    Each event is a JSON object: {"ts": float, "pc_id": str, "role":
    "user"|"assistant", "text": str} (`pc_id` is the session id). The
    index page subscribes here and prints them into its live log.
    """
    queue: asyncio.Queue = asyncio.Queue(maxsize=_TRANSCRIPT_QUEUE_MAXSIZE)
    _transcript_subscribers.add(queue)
    logger.info(f"transcript subscriber added ({len(_transcript_subscribers)} total)")

    async def gen():
        try:
            # Tiny hello so the browser knows the channel is live even
            # before any conversation happens.
            yield f": connected at {time.time():.3f}\n\n"
            while True:
                if await request.is_disconnected():
                    break
                try:
                    event = await asyncio.wait_for(queue.get(), timeout=15.0)
                    yield f"data: {json.dumps(event, ensure_ascii=False)}\n\n"
                except asyncio.TimeoutError:
                    # Keep-alive heartbeat so proxies don't close the
                    # connection on idle.
                    yield ": ping\n\n"
        finally:
            _transcript_subscribers.discard(queue)
            logger.info(
                f"transcript subscriber removed ({len(_transcript_subscribers)} remaining)"
            )

    return StreamingResponse(gen(), media_type="text/event-stream")



@app.get("/health")
async def health():
    return {"status": "ok"}


@app.get("/")
async def index():
    return FileResponse(APP_DIR / "index.html")


if __name__ == "__main__":
    uvicorn.run(app, host=HTTP_HOST, port=HTTP_PORT)
