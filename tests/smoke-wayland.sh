#!/usr/bin/env bash
# Layer 2 Wayland smoke: list outputs, short capture, SIGINT clean exit.
# Skips (77) when no Wayland session — expected in headless CI.
set -euo pipefail

WF_RECORDER=${1:-}
if [[ -z "$WF_RECORDER" || ! -x "$WF_RECORDER" ]]; then
  echo "usage: $0 /path/to/wf-recorder" >&2
  exit 1
fi

if [[ -z "${WAYLAND_DISPLAY:-}" ]]; then
  echo "smoke-wayland: SKIP (WAYLAND_DISPLAY unset)"
  exit 77
fi

fail() { echo "smoke-wayland: FAIL $*" >&2; exit 1; }
ok() { echo "smoke-wayland: $*"; }

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/wf-smoke.XXXXXX")"
cleanup() { rm -rf "$tmpdir"; }
trap cleanup EXIT

# --- list outputs (multi-output awareness) ---
list_out="$tmpdir/list.txt"
if ! "$WF_RECORDER" -L >"$list_out" 2>"$tmpdir/list.err"; then
  # Some builds print to stderr
  cat "$tmpdir/list.err" >>"$list_out" || true
fi
cat "$list_out" >&2 || true

mapfile -t OUTPUTS < <(sed -n 's/^.*Name: \([^ ]*\).*/\1/p' "$list_out" | sed '/^$/d')
if [[ ${#OUTPUTS[@]} -eq 0 ]]; then
  # Fallback: "1. Name: eDP-1 Description: ..."
  mapfile -t OUTPUTS < <(awk -F'Name: ' 'NF>1{split($2,a," "); print a[1]}' "$list_out")
fi
[[ ${#OUTPUTS[@]} -ge 1 ]] || fail "no outputs from -L (is the compositor up?)"
ok "outputs(${#OUTPUTS[@]}): ${OUTPUTS[*]}"

OUT0="${OUTPUTS[0]}"

# --- short continuous capture, stop with SIGINT (flags-only teardown) ---
outfile="$tmpdir/cap.mp4"
log="$tmpdir/cap.log"
"$WF_RECORDER" -o "$OUT0" -f "$outfile" -y -c libx264 -r 30 -D \
  -p preset=ultrafast -p crf=28 -p bf=0 \
  >"$log" 2>&1 &
pid=$!

# Wait until the file has data, then record a little longer so encode runs.
deadline=$((SECONDS + 15))
while (( SECONDS < deadline )); do
  if [[ -s "$outfile" ]]; then
    break
  fi
  if ! kill -0 "$pid" 2>/dev/null; then
    wait "$pid" || true
    fail "recorder exited early before writing output; log: $(tail -20 "$log" | tr '\n' ' ')"
  fi
  sleep 0.2
done
[[ -s "$outfile" ]] || fail "timed out waiting for first output bytes"
sleep 1.5

kill -INT "$pid" 2>/dev/null || true
ec=0
wait "$pid" || ec=$?
# bash wait returns 128+N for signals on some shells; treat 0 and 130 (SIGINT) as ok
if [[ "$ec" -ne 0 && "$ec" -ne 130 ]]; then
  fail "SIGINT stop exit=$ec; log: $(tail -30 "$log" | tr '\n' ' ')"
fi
ok "SIGINT stop exit=$ec"

[[ -s "$outfile" ]] || fail "output file empty or missing"
size=$(wc -c <"$outfile" | tr -d ' ')
(( size > 1000 )) || fail "output too small (${size} bytes)"
ok "captured ${size} bytes from $OUT0"

# --- multi-output: if ≥2 heads, capturing one must still succeed (already did) ---
if [[ ${#OUTPUTS[@]} -ge 2 ]]; then
  ok "multi-output present (${OUTPUTS[1]} also connected); capture of $OUT0 succeeded"
else
  ok "single output only; multi-output check skipped"
fi

ok "ok"
exit 0
