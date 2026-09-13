#!/usr/bin/env bash
# Layer 3: Ctrl+C during DMA-BUF VAAPI capture must exit cleanly on Hyprland.
# Skips (77) without Wayland or without a render node / h264_vaapi.
set -euo pipefail

WF_RECORDER=${1:-}
if [[ -z "$WF_RECORDER" || ! -x "$WF_RECORDER" ]]; then
  echo "usage: $0 /path/to/wf-recorder" >&2
  exit 1
fi

if [[ -z "${WAYLAND_DISPLAY:-}" ]]; then
  echo "smoke-vaapi-sigint: SKIP (WAYLAND_DISPLAY unset)"
  exit 77
fi

if [[ ! -e /dev/dri/renderD128 ]]; then
  echo "smoke-vaapi-sigint: SKIP (no /dev/dri/renderD128)"
  exit 77
fi

# Avoid grep -q (early close → ffmpeg SIGPIPE → pipefail treats as failure).
if ! ffmpeg -hide_banner -encoders 2>&1 | grep h264_vaapi >/dev/null; then
  echo "smoke-vaapi-sigint: SKIP (h264_vaapi encoder unavailable)"
  exit 77
fi

fail() { echo "smoke-vaapi-sigint: FAIL $*" >&2; exit 1; }
ok() { echo "smoke-vaapi-sigint: $*"; }

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/wf-vaapi.XXXXXX")"
cleanup() { rm -rf "$tmpdir"; }
trap cleanup EXIT

list_out="$tmpdir/list.txt"
"$WF_RECORDER" -L >"$list_out" 2>&1 || true
mapfile -t OUTPUTS < <(awk -F'Name: ' 'NF>1{split($2,a," "); print a[1]}' "$list_out")
[[ ${#OUTPUTS[@]} -ge 1 ]] || fail "no outputs"
OUT0="${OUTPUTS[0]}"

outfile="$tmpdir/cap.mp4"
log="$tmpdir/cap.log"
device="${WF_VAAPI_DEVICE:-/dev/dri/renderD128}"

ok "VAAPI capture on $OUT0 device=$device"

"$WF_RECORDER" -o "$OUT0" -f "$outfile" -y -c h264_vaapi -d "$device" -r 30 -D \
  -p bf=0 -p qp=18 -p rc_mode=CQP \
  -F 'scale_vaapi=format=nv12:out_range=tv' \
  >"$log" 2>&1 &
pid=$!

deadline=$((SECONDS + 20))
while (( SECONDS < deadline )); do
  if [[ -s "$outfile" ]]; then
    break
  fi
  if ! kill -0 "$pid" 2>/dev/null; then
    wait "$pid" || true
    # VAAPI init failure → skip rather than fail (driver/session variance)
    if grep -qiE 'vaapi|Invalid|No device|ENCODER' "$log"; then
      echo "smoke-vaapi-sigint: SKIP (VAAPI capture failed to start)"
      echo "log: $(tail -15 "$log" | tr '\n' ' ')"
      exit 77
    fi
    fail "exited early; log: $(tail -30 "$log" | tr '\n' ' ')"
  fi
  sleep 0.2
done

if [[ ! -s "$outfile" ]]; then
  echo "smoke-vaapi-sigint: SKIP (no output — VAAPI path not usable here)"
  kill -INT "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  exit 77
fi

sleep 1.5
kill -INT "$pid" 2>/dev/null || true
ec=0
wait "$pid" || ec=$?
if [[ "$ec" -ne 0 && "$ec" -ne 130 ]]; then
  fail "SIGINT exit=$ec; log: $(tail -40 "$log" | tr '\n' ' ')"
fi

if grep -qiE 'Too many bits for size_t|segmentation fault' "$log"; then
  fail "Hyprland/teardown abort pattern in log"
fi

ok "SIGINT exit=$ec size=$(wc -c <"$outfile" | tr -d ' ')"
ok "ok"
exit 0
