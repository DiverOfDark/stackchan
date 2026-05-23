# pipecat-voice-assistant

Self-hosted **streaming Russian voice agent** for Kubernetes —
microphone → Whisper STT → your local LLM → Piper TTS → audio back, all
local, in-cluster, over WebRTC. Browser-tested, ESP32-S3 friendly.

Built around [Pipecat](https://docs.pipecat.ai) (SmallWebRTC transport,
Silero VAD, faster-whisper, in-process Piper), an OpenAI-compatible LLM you
host yourself, and [STUNner](https://docs.l7mp.io) as the in-cluster
WebRTC TURN gateway.

```
 LAN browser ──HTTPS signaling──► Ingress ──► voice-assistant (FastAPI :7860)
            └──WebRTC media (UDP)──► STUNner TURN ──relay──► bot pod
 bot pod: Whisper large-v3-turbo INT8 · Silero VAD · Piper (ru) · OpenAI client ─► your LLM
```

No cloud STT/TTS/LLM. Everything in the hot path runs in the cluster.

---

## What you get

| | |
|---|---|
| **STT** | `faster-whisper` (`large-v3-turbo`, INT8, CPU) — Russian by default |
| **TTS** | Piper, in-process — Russian voice `ru_RU-irina-medium` by default |
| **LLM** | `OpenAILLMService` against any OpenAI-compatible endpoint you run |
| **VAD / barge-in** | Silero VAD via Pipecat (server-side, sub-300 ms interruptions) |
| **Transport** | Pipecat SmallWebRTC + a 200-line raw-WebRTC browser test client |
| **Networking** | STUNner TURN gateway — no `hostNetwork`, no node pinning |
| **ESP32-S3** | Forward-compatible custom SDP munge (off by default) |

---

## Quickstart

### 1. Verify your LLM endpoint streams Russian

This is the gate. Don't deploy until it passes.

```bash
cp config.example.env config.env  # then edit it
./verify_hermes.sh
```

Pass = multiple SSE chunks **and** the response is coherent Russian.

### 2. Install the STUNner operator (once per cluster)

```bash
helm repo add stunner https://l7mp.io/stunner
helm install stunner stunner/stunner --create-namespace -n stunner-system
```

### 3. Install the chart

```bash
helm install voice-assistant \
  oci://ghcr.io/diverofdark/charts/voice-assistant \   # or local: ./chart
  --create-namespace -n voice-assistant \
  -f my-values.yaml
```

Minimal `my-values.yaml`:

```yaml
image:
  tag: latest

hermes:
  baseUrl: "http://your-llm.example.com:8000/v1"
  model: "your-model-name"
  apiKey:
    value: "sk-your-key"            # or use externalSecret.enabled / existingSecret

stunner:
  loadBalancerIP: "192.168.1.50"    # a free IP in your LB pool
  turnUri: "turn:192.168.1.50:3478?transport=udp"
  auth:
    password: "set-something-random"   # or externalSecret / existingSecret

ingress:
  host: voice-assistant.your-domain.example
  className: traefik
  annotations:
    cert-manager.io/cluster-issuer: letsencrypt
```

### 4. Open the web client

`https://<your ingress host>` → «Подключиться и говорить» → mic permission →
speak Russian. The assistant greets on connect and answers within ~2 s of
you finishing a sentence. Interrupting it mid-reply stops it within ~300 ms.

> Browsers require HTTPS to grant microphone access. Plain `http://…:7860`
> works for `/health` only.

---

## Configuration reference

All values are in `chart/values.yaml`. The non-obvious ones:

### `hermes` — your LLM endpoint

```yaml
hermes:
  baseUrl: "http://hermes-api.kubevirt-vms.svc.cluster.local:8642/v1"
  model: "hermes-agent"
  apiKey:
    secretKey: HERMES_API_KEY
    # Pick one source:
    externalSecret:                 # (1) External Secrets Operator
      enabled: true
      store: openbao-store
      storeKind: ClusterSecretStore
      remoteKey: voice-assistant/hermes
      property: api_key
    # value: "sk-local"             # (2) inline (insecure)
    # existingSecret: my-key-secret # (3) Secret you create yourself
```

### `stunner` — WebRTC TURN relay

The chart creates `GatewayClass` + `GatewayConfig` + `Gateway` + `UDPRoute`
in the release namespace. The STUNner operator (separate install) sees them
and provisions the TURN dataplane + LoadBalancer Service.

```yaml
stunner:
  turnUri: "turn:192.168.179.21:3478?transport=udp"
  loadBalancerIP: "192.168.179.21"        # pins the LB IP via MetalLB annotation
  auth:
    username: voiceassistant
    externalSecret:
      enabled: true
      remoteKey: voice-assistant/turn
      usernameProperty: username
      passwordProperty: password
```

### `whisper`, `piper`, `systemPrompt`

```yaml
whisper:
  # Default is large-v3-turbo (fastest with strong Russian accuracy).
  # Override with a smaller model if RAM/CPU is tight.
  model: deepdml/faster-whisper-large-v3-turbo-ct2   # or: tiny | base | small | medium | large-v3
  computeType: int8
  ompThreads: 6

piper:
  voice: ru_RU-irina-medium   # rhasspy/piper-voices

systemPrompt: |-
  Ты — голосовой помощник. Всегда отвечай только на русском языке.
  …
```

### `persistence`

8 GiB by default — holds the Whisper HF cache and the Piper voice file.
Sized so pod restarts never re-download (validated end-to-end).

### Other knobs

`replicaCount`, `resources.bot / resources.initContainer`, `extraEnv`,
`service.type/port`, `ingress.*`, `esp32Compat`, `podLabels`.

---

## How the WebRTC path actually works

The bot pod lives on the normal CNI pod network (`10.x`, unreachable from
your LAN). STUNner runs an in-cluster TURN server, exposed via a
LoadBalancer at your chosen LAN IP. The web client treats that as its only
ICE server (handed to it via `GET /ice-servers`), becomes a TURN client,
and the `UDPRoute` permits its relay to reach the `voice-assistant`
Service's pods. Audio flows: browser ↔ STUNner ↔ bot pod.

This means the pod stays portable — no `hostNetwork`, no node pinning, no
aiortc port-range monkeypatching. Pipecat's stock `--esp32` flag is
deliberately **not** used here because its SDP filter drops `relay`
candidates; instead `bot.py` carries a custom `esp32_munge()` (env-gated
`ESP32_COMPAT`) that strips sha-384/512 fingerprints and reshapes the
relay candidate into an ESP32-friendly host candidate.

The web client is served and signaled over HTTPS via your Ingress —
browsers refuse `getUserMedia` outside a secure context.

---

## Local development

```bash
# Hermes gate
cp config.example.env config.env && $EDITOR config.env
./verify_hermes.sh

# Build the image
docker build -t pipecat-voice-assistant:dev ./app

# Or run the app directly (you'll need uv, ffmpeg, libgomp):
cd app
uv sync
HERMES_BASE_URL=… HERMES_MODEL=… HERMES_API_KEY=… \
  STUNNER_TURN_URI=… STUNNER_TURN_USERNAME=… STUNNER_TURN_PASSWORD=… \
  uv run python bot.py
```

The bot serves the test client at `/`, signaling at `/api/offer`, and
`/health`, `/ice-servers` for liveness and the browser's ICE config.

---

## Validation checklist

- `./verify_hermes.sh` reports multiple SSE chunks **and** coherent Russian
- `helm template` renders without errors; `helm install --dry-run` accepted by the cluster
- Pods reach `Ready` and stay there through one rollout
- STUNner Service holds the LB IP you configured
- Browser test: speak Russian → Russian reply begins within ~2 s
- Interrupt mid-reply → speech stops within ~300 ms
- `kubectl delete pod -l app.kubernetes.io/name=voice-assistant` → new pod's
  init container finds models on the PVC and skips the download

---

## ESP32-S3 (forward-compat)

Set `esp32Compat: true` when validating an ESP32 client. The bot then
applies a custom SDP munge: removes sha-384/512 fingerprints (the ESP32
mbedTLS only does sha-256) and rewrites the STUNner `relay` candidate into
a single `host`-type candidate at the relay address. Browser path is not
affected — leave it `false` unless you're testing hardware.

---

## License

MIT.
