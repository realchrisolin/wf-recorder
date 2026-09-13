# Tests

## Layer 1 — unit (always run)

| Test | What |
|------|------|
| `buffer-pool` | Grow-to-cap, drop-oldest when full, never abort; capture/encode handshake |
| `signal-exit` | SIGINT path only sets flags (no Wayland teardown) |
| `ffmpeg-pixfmt` | FFmpeg 7.1+ `avcodec_get_supported_config` vs legacy `pix_fmts` |

## Layer 2 — client / Wayland smoke

| Test | What |
|------|------|
| `icc-client-features` | `--toplevel` in help; binary linked with `ext-image-copy-capture` symbols (no compositor) |
| `smoke-wayland` | `-L` outputs, ~1.5s capture, SIGINT exit 0; notes multi-output if ≥2 heads |

## Layer 3 — latency / backlog

| Test | What |
|------|------|
| `buffer-pool-backlog` | Tagged seq flood: after drop-oldest, drained frames are the **newest** window; slow-encode thread still tracks recent seqs |
| `smoke-backlog` | Wayland: `-D -r 60` + libx264 pressure, hold ~3s, SIGINT; no abort (`buffer pool full` OK) |
| `smoke-vaapi-sigint` | Wayland+VAAPI: DMA-BUF capture, SIGINT clean exit |

Wayland smokes **skip** (exit 77) when `WAYLAND_DISPLAY` is unset — normal in headless CI. VAAPI smoke also skips without `renderD128` / `h264_vaapi`.

```bash
meson test -C build --print-errorlogs
meson test -C build buffer-pool-backlog smoke-backlog --print-errorlogs
```

## Layer 4 — mock Wayland / ICC protocol contract

| Test | What |
|------|------|
| `icc-proto-check` | Classify/bind rules + `check_has_protos` messages for output vs toplevel vs DMA |
| `mock-wayland-icc-registry` | `wayland-server` stub compositor advertises ICC globals; client scan must pass (and fail when copy-capture is omitted) |

Shared logic: `src/icc-proto-check.hpp` (also used by `check_has_protos()` in `main.cpp`).

## Layer 5 — mock ICC frame session

| Test | What |
|------|------|
| `mock-wayland-icc-frame-session` | SHM path: `create_source` → `create_session` (buffer_size/shm_format/done) → `create_frame` → attach/damage/capture → **ready** |

## Layer 6 — mock ICC DMA-BUF frame session

| Test | What |
|------|------|
| `mock-wayland-icc-dmabuf-frame` | DMA path: session advertises `dmabuf_device`/`dmabuf_format`; client `linux-dmabuf` `create_immed` (memfd stand-in) → attach/capture → **ready** |

Real GBM import / GPU present still need a live compositor (Layer 2/3 VAAPI smokes).
