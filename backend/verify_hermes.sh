#!/usr/bin/env bash
#
# GATE CHECK — run this FIRST, before deploying anything.
#
# Confirms the Hermes LLM endpoint:
#   (a) actually STREAMS — returns multiple SSE chunks, not one buffered blob;
#   (b) answers a Russian prompt with coherent Russian.
#
# If either check fails, STOP: a non-streaming or non-Russian Hermes must be
# fixed before the rest of the voice stack matters.
#
# Usage:  cp config.example.env config.env  &&  edit config.env  &&  ./verify_hermes.sh
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "${HERE}/config.env" ]]; then
  set -a; . "${HERE}/config.env"; set +a
fi

: "${HERMES_BASE_URL:?set HERMES_BASE_URL — copy config.example.env to config.env and fill it}"
: "${HERMES_MODEL:?set HERMES_MODEL}"
: "${HERMES_API_KEY:?set HERMES_API_KEY}"

PROMPT="Привет! Ответь одним абзацем: что такое Kubernetes и зачем он нужен?"
URL="${HERMES_BASE_URL%/}/chat/completions"

echo "→ POST ${URL}"
echo "  model=${HERMES_MODEL}  stream=true"
echo

RAW="$(mktemp)"
trap 'rm -f "$RAW"' EXIT

# Stream the response, prefixing each line with its arrival timestamp so we
# can prove chunks trickle in over time rather than arriving as one blob.
curl -sS --no-buffer -N \
  -X POST "$URL" \
  -H "Authorization: Bearer ${HERMES_API_KEY}" \
  -H "Content-Type: application/json" \
  -d "{\"model\":\"${HERMES_MODEL}\",\"stream\":true,\"max_tokens\":300,\"messages\":[{\"role\":\"user\",\"content\":\"${PROMPT}\"}]}" \
  | while IFS= read -r line; do printf '%s\t%s\n' "$(date +%s.%N)" "$line"; done > "$RAW"

# ── Check (a): multiple SSE chunks, arriving incrementally ────────────────
mapfile -t TS < <(grep -F $'\tdata:' "$RAW" | grep -v '\[DONE\]' | cut -f1 || true)
CHUNKS=${#TS[@]}
if (( CHUNKS == 0 )); then
  echo "✗ FAIL: no SSE 'data:' chunks returned. Raw response:"
  cut -f2- "$RAW"
  exit 1
fi
SPREAD=$(awk -v a="${TS[0]}" -v b="${TS[$((CHUNKS-1))]}" 'BEGIN{printf "%.3f", b-a}')
echo "Streaming : ${CHUNKS} SSE chunks, first→last spread ${SPREAD}s"
if (( CHUNKS < 2 )); then
  echo "✗ FAIL: only one chunk — Hermes is buffering, not streaming."
  exit 1
fi

# ── Extract the assistant text ────────────────────────────────────────────
TEXT="$(cut -f2- "$RAW" | sed -n 's/^data: *//p' | grep -v '^\[DONE\]$' | python3 -c '
import sys, json
out = []
for ln in sys.stdin:
    ln = ln.strip()
    if not ln:
        continue
    try:
        d = json.loads(ln)
    except Exception:
        continue
    for ch in d.get("choices", []):
        out.append((ch.get("delta") or {}).get("content") or "")
print("".join(out))')"

echo
echo "Response  :"
echo "──────────────────────────────────────────────────────────"
echo "$TEXT"
echo "──────────────────────────────────────────────────────────"

# ── Check (b): coherent Russian ───────────────────────────────────────────
CYR=$(printf '%s' "$TEXT" | grep -oP '[А-Яа-яЁё]' | wc -l)
NONSPACE=$(printf '%s' "$TEXT" | grep -oP '\S' | wc -l)
echo
if (( NONSPACE < 20 )); then
  echo "✗ FAIL: response too short to judge."
  exit 1
fi
RATIO=$(( CYR * 100 / NONSPACE ))
echo "Russian   : ${CYR}/${NONSPACE} non-space chars are Cyrillic (${RATIO}%)"
if (( RATIO < 60 )); then
  echo "✗ FAIL: response is not predominantly Russian — Hermes is not Russian-capable."
  exit 1
fi

echo
echo "✓ PASS — Hermes streams (${CHUNKS} chunks over ${SPREAD}s) and answers in Russian."
echo "  Read the text above to confirm it is coherent, then continue."
