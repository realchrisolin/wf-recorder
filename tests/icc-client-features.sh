#!/usr/bin/env bash
# Layer 2a (no compositor): ICC client feature detection.
# Verifies the binary advertises toplevel capture and was linked with
# ext-image-copy-capture protocol symbols.
set -euo pipefail

WF_RECORDER=${1:-}
if [[ -z "$WF_RECORDER" || ! -x "$WF_RECORDER" ]]; then
  echo "usage: $0 /path/to/wf-recorder" >&2
  exit 1
fi

fail() { echo "icc-client-features: FAIL $*" >&2; exit 1; }
ok() { echo "icc-client-features: $*"; }

ver="$("$WF_RECORDER" -v 2>&1 || true)"
[[ -n "$ver" ]] || fail "empty -v output"
ok "version: $ver"

help="$("$WF_RECORDER" --help 2>&1 || true)"
echo "$help" | grep -q -- '--toplevel' || fail "--help missing --toplevel (ICC/toplevel capture)"
echo "$help" | grep -q -- '--list-output\|-L' || fail "--help missing --list-output"
ok "help advertises --toplevel and list-output"

# Protocol client symbols must be present in an ICC build (not stock wlr-only).
# grep -a searches the binary directly (avoids strings|grep -q SIGPIPE under pipefail).
grep -a -q 'ext_image_copy_capture_manager_v1' "$WF_RECORDER" \
  || fail "binary missing ext_image_copy_capture_manager_v1 (not an ICC client?)"
grep -a -q 'ext_foreign_toplevel_list_v1' "$WF_RECORDER" \
  || fail "binary missing ext_foreign_toplevel_list_v1"
ok "binary contains ext-image-copy-capture + foreign-toplevel symbols"

ok "ok"
exit 0
