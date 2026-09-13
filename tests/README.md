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
| `smoke-vaapi-sigint` | Wayland+VAAPI: DMA-BUF capture, SIGINT clean exit (Hyprland teardown race) |

**Out of scope for wf-recorder:** typing lag on idle ICC outputs when the compositor only `scheduleFrame`s on the first share — that is a Hyprland bug/patch, not a client failure.

Wayland smokes **skip** (exit 77) when `WAYLAND_DISPLAY` is unset — normal in headless CI. VAAPI smoke also skips without `renderD128` / `h264_vaapi`.

```bash
meson test -C build --print-errorlogs
meson test -C build buffer-pool-backlog smoke-backlog --print-errorlogs
```

## Layer 4 — FluxCast / Omarchy integration

Lives in the **fluxcast** tree (not this repo):

- `tests/test_icc_integration.py` — `FLUXCAST_WFD_WF_RECORDER_PROTO=icc` accept/reject,
  ICC capture rate (`-r` = stream `config.fps`, override via `FLUXCAST_WFD_ICC_CAPTURE_FPS`),
  LPCM `-D`/`-r`, optional live binary smoke when `FLUXCAST_WFD_WF_RECORDER_BIN`
  points at this build.

```bash
cd ~/code/other/fluxcast
python3 -m unittest tests.test_icc_integration -v
```
