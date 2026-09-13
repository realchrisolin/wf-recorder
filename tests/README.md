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

`smoke-wayland` **skips** (exit 77) when `WAYLAND_DISPLAY` is unset — normal in headless CI.

```bash
meson test -C build --print-errorlogs
# Wayland-only:
meson test -C build smoke-wayland --print-errorlogs
```
