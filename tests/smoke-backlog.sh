#!/usr/bin/env bash
# Layer 3 Wayland smoke: high-fps continuous capture under encode pressure.
# Pass: no abort, SIGINT exit 0, non-empty output. "buffer pool full" is OK.
# Skips (77) without WAYLAND_DISPLAY.
set -euo pipefail

WF_RECORDER=${1:-}
if [[ -z "$WF_RECORDER" || ! -x "$WF_RECORDER" ]]; then
  echo "usage: $0 /path/to/wf-recorder" >&2
  exit 1
fi

if [[ -z "${WAYLAND_DISPLAY:-}" ]]; then
  echo "smoke-backlog: SKIP (WAYLAND_DISPLAY unset)"
  exit 77
fi

fail() { echo "smoke-backlog: FAIL $*" >&2; exit 1; }
ok() { echo "smoke-backlog: $*"; }

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/wf-backlog.XXXXXX")"
cleanup() { rm -rf "$tmpdir"; }
trap cleanup EXIT

list_out="$tmpdir/list.txt"
"$WF_RECORDER" -L >"$list_out" 2>&1 || true
mapfile -t OUTPUTS < <(awk -F'Name: ' 'NF>1{split($2,a," "); print a[1]}' "$list_out")
[[ ${#OUTPUTS[@]} -ge 1 ]] || fail "no outputs from -L"
OUT0="${OUTPUTS[0]}"
ok "capturing $OUT0 at 60fps continuous (libx264 pressure)"

outfile="$tmpdir/cap.mp4"
log="$tmpdir/cap.log"

# Continuous high FPS + CPU encode → capture can outrun encode (pool pressure).
"$WF_RECORDER" -o "$OUT0" -f "$outfile" -y -c libx264 -r 60 -D \
  -p preset=ultrafast -p crf=28 -p bf=0 \
  >"$log" 2>&1 &
pid=$!

deadline=$((SECONDS + 20))
while (( SECONDS < deadline )); do
  if [[ -s "$outfile" ]]; then
    break
  fi
  if ! kill -0 "$pid" 2>/dev/null; then
    wait "$pid" || true
    fail "exited early; log: $(tail -30 "$log" | tr '\n' ' ')"
  fi
  sleep 0.2
done
[[ -s "$outfile" ]] || fail "timed out waiting for output"

# Hold under load long enough for backlog / possible drop-oldest.
sleep 3

kill -INT "$pid" 2>/dev/null || true
ec=0
wait "$pid" || ec=$?
if [[ "$ec" -ne 0 && "$ec" -ne 130 ]]; then
  # Abort / "Too many bits" / uncaught errors
  fail "SIGINT exit=$ec; log: $(tail -40 "$log" | tr '\n' ' ')"
fi
ok "SIGINT exit=$ec"

size=$(wc -c <"$outfile" | tr -d ' ')
(( size > 5000 )) || fail "output too small (${size} bytes)"
ok "captured ${size} bytes"

if grep -q 'buffer pool full' "$log"; then
  ok "log notes buffer pool full (drop-oldest engaged — expected under pressure)"
else
  ok "no pool-full message (encode kept up — also OK)"
fi

# Must not have aborted via the old "too many failures" / fatal pool paths.
if grep -qiE 'too many|abort|segmentation|Too many bits' "$log"; then
  fail "fatal/abort pattern in log"
fi

ok "ok"
exit 0
