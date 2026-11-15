# Femto voice backend

The voice of the [Femto](../README.md) desk robot. The robot detects its wake
word locally, opens a WebSocket here and streams microphone audio; this
service bridges that socket to OpenAI's speech-to-speech model (`gpt-live-1`).

```
robot ── PCM 16 kHz ──▶ backend ──▶ OpenAI Live API (hears + speaks)
robot ◀─ PCM + events ─ backend ◀──
                           │
                           └─ delegation ──▶ Hermes (agent LLM: tools, long tasks)
```

- The voice model answers small talk itself, about a second after you stop.
- Anything that needs tools or real data it hands to **Hermes**, a self-hosted
  OpenAI-compatible agent, says so, and speaks the result when it arrives.
- After a false wake (nobody speaks for 8 s) the robot is told to go back to
  sleep silently.

`CLAUDE.md` in this directory is the detailed reference.

## Endpoints

| Path | What |
|---|---|
| `WS /ws/femto` | One voice session: hello JSON, then PCM16 16 kHz both ways + JSON events |
| `GET /` | Live transcript of what the robot hears and says |
| `GET /api/transcripts/stream` | The same as Server-Sent Events |
| `GET /wake-review`, `/wake-samples…`, `POST /wake-sample` | Wake-word samples for retraining |
| `POST/GET /api/device-logs` | The robot's uploaded logs |
| `GET /health` | Liveness |

## Configuration

Environment variables (the Helm chart in `chart/` sets them from `values.yaml`):

| Variable | Chart value | Meaning |
|---|---|---|
| `OPENAI_API_KEY` | `openai.apiKey.*` | Key with `gpt-live-1` access |
| `OPENAI_LIVE_MODEL` | `openai.liveModel` | Default `gpt-live-1` |
| `OPENAI_LIVE_VOICE` | `openai.liveVoice` | Built-in voice name or custom voice id (`voice_…`) |
| `OPENAI_LIVE_VOICE_PROMPT` | `openai.liveVoicePrompt` | Description the custom voice was made from |
| `HERMES_BASE_URL`, `HERMES_MODEL`, `HERMES_API_KEY` | `hermes.*` | The agent LLM |
| `STACKCHAN_SYSTEM_PROMPT` | `stackchan.systemPrompt` | Persona (`{name}`, `{honorific}`) |
| `STACKCHAN_USAGE_URL` | `stackchan.usageUrl` | trmnl-cyberpunk base URL for live Claude usage |
| `WAKE_SAMPLE_DIR`, `DEVICE_LOG_DIR` | `wakeSamples.*`, `deviceLogs.*` | Storage on the `/data` volume |

## Local development

```bash
cp config.example.env config.env      # fill in the keys
./verify_hermes.sh                    # Hermes streams and answers
uv sync --group dev
uv run pytest
set -a; . ./config.env; set +a; uv run python bot.py   # http://localhost:7860
```

## Deploy

Push to `main`. CI runs the tests, builds `ghcr.io/diverofdark/stackchan-voice`
and moves the `deploy` branch, which ArgoCD tracks.

## License

MIT.
