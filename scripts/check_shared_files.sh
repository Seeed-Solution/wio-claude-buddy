#!/bin/sh
# Diff-guard for the files shared byte-identically between the Wio firmware
# and the SenseCAP Indicator port. The Indicator keeps copies (Arduino and
# ESP-IDF can't share one source tree cleanly); this script is what makes
# that safe: CI fails if any copy drifts from firmware/claude_buddy/.
# To change a shared file, edit BOTH copies in the same commit.
set -u
cd "$(dirname "$0")/.."

WIO=firmware/claude_buddy
IND=firmware/claude_buddy_indicator/components

fail=0
check() {
  if ! cmp -s "$1" "$2"; then
    echo "DRIFT: $2 differs from $1 (edit both copies in one commit)" >&2
    fail=1
  fi
}

for f in data.h xfer.h b64.h stats.h prefs_compat.h; do
  check "$WIO/$f" "$IND/buddy_core/$f"
done
check "$WIO/net_bridge_core.h" "$IND/net_bridge_core/net_bridge_core.h"

# Phase 2 adds the buddy engine (buddy.h/buddy.cpp/buddy_common.h and the 18
# buddy_sp_*.cpp species) to buddy_core — extend the list when they land.

[ "$fail" = 0 ] && echo "shared files in sync"
exit "$fail"
