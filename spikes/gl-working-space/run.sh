#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Genesis-0 contributors
#
# Working-space gate: run every case. Q1 format/transfer cases run in probe
# (negotiation/residency) and analyze (round-trip pixel error) modes; Q2
# readback cases run in measure mode and report the main-branch frame rate.
# Output lands under /tmp/opencode/gl-working-space.

set -u
BIN="$(dirname "$0")/build/gl-working-space"
OUT=/tmp/opencode/gl-working-space
mkdir -p "$OUT"
rm -f "$OUT"/*.log

export GST_DEBUG="GST_CAPS:5,gst_gl_*:4"

echo "=== Q1: format residency (probe) ==="
for c in fmt-rgba64 fmt-rgb16 fmt-f16 fmt-glshader64; do
    "$BIN" "$c" probe 2>&1 | tee "$OUT/$c.probe.log"
done

echo "=== Q1: transfer / round-trip (analyze) ==="
for c in transfer rtt8 rtt64; do
    "$BIN" "$c" analyze 2>&1 | tee "$OUT/$c.analyze.log"
done

echo "=== Q2: readback frame rate (measure) ==="
for c in noreadback tee-download pbo; do
    GLWS_SECONDS=8 "$BIN" "$c" measure 2>&1 | tee "$OUT/$c.measure.log"
done

echo "=== done; logs in $OUT ==="
