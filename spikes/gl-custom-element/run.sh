#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Genesis-0 contributors
#
# Custom-element gate: run every case in probe mode (GL residency) and capture
# mode (PNG for pixel comparison), with GL caps negotiation logged. Output
# lands under /tmp/opencode/gl-custom-element.

set -u
BIN="$(dirname "$0")/build/gl-custom-element"
OUT=/tmp/opencode/gl-custom-element
mkdir -p "$OUT"
rm -f "$OUT"/*.png "$OUT"/*.log

export GST_DEBUG="GST_CAPS:5,gst_gl_*:4"

CASES="baseline combine tex1-only named"

for c in $CASES; do
    echo "=== $c probe ==="
    "$BIN" "$c" probe 2>&1 | tee "$OUT/$c.probe.log"
    echo "=== $c capture ==="
    "$BIN" "$c" capture "$OUT/$c-%02d.png" 2>&1 | tee "$OUT/$c.capture.log"
done

echo "=== done; PNGs and logs in $OUT ==="
