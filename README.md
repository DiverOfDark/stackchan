# Femto

Custom firmware for the official M5Stack StackChan (CoreS3 + StackChan body): a
contemptuous dystopian robot butler that shows Claude usage, follows your face
with its head, and talks through its own [pipecat](https://docs.pipecat.ai)
voice backend (`backend/`). Rust on ESP-IDF 5.5, with a Svelte web UI for setup, settings,
camera, logs and OTA updates.

See [`docs/PRD.md`](docs/PRD.md) for the full spec.

## Layout

| Path | What |
|---|---|
| `crates/` | Host-testable Rust: face engine, renderer, drivers, desktop simulator |
| `firmware/` | ESP32-S3 binary (Xtensa `esp` toolchain) and C components |
| `web/` | Web UI, embedded in the firmware image |
| `backend/` | Voice backend (pipecat: STT → LLM → TTS over WebRTC), Dockerfile, Helm chart |
| `tools/` | OTA upload, stock-firmware restore, font/design export, wake-word training (`tools/wake-word/`) |
| `legacy/xiao/` | The original XIAO ESP32-S3 voice device firmware (frozen) |

## Build

```sh
cargo test --workspace                 # host crates
cargo run -p femto-sim --release       # desktop simulator

cd firmware && . ~/export-esp.sh
cp femto.env.example femto.env         # optional: bake in backend URLs
cargo run --release                    # flash over USB
../tools/ota.sh femto.local            # or update over Wi-Fi
```

Without `femto.env` the usage and voice URLs are entered on first boot in the
setup page: join the `FEMTO-SETUP` hotspot (key shown on screen) and open
`http://192.168.4.1`.

The backend image is `ghcr.io/diverofdark/stackchan-voice:sha-<commit>`;
see [`backend/README.md`](backend/README.md) to deploy it.

CI builds `femto.bin` on every push; tagged releases attach it. Flash it from
the web UI (System → Firmware).

## Licence

MIT. Fonts are OFL (see `assets/fonts/README.md`); vendored components keep
their own licences (see `firmware/components/README.md`).
