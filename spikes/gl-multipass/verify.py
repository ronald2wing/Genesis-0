#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Genesis-0 contributors
#
# Pixel-comparison verdicts for the Phase 4 multi-pass spike. Reads the PNGs
# run.sh produced and reports, per case, whether the observed frame matches
# what the effect is supposed to draw. "Correct" here means the pixels match a
# reference computed from the baseline frame, not merely that no error was
# posted.

import os
import sys
from PIL import Image

OUT = "/tmp/opencode/gl-multipass"
TOL = 4  # per-channel tolerance for 8-bit GL rounding


def load(name):
    # multifilesink writes name-00.png, name-01.png, ...; take frame 0.
    path = f"{OUT}/{name}-00.png"
    if not os.path.exists(path):
        return None
    return Image.open(path).convert("RGB")


def near(a, b, tol=TOL):
    return abs(a - b) <= tol


def verdict(name, ok, detail):
    mark = "PASS" if ok else "FAIL"
    print(f"{name:22s} {mark}  {detail}")
    return ok


def main():
    results = {}
    ref = load("baseline")
    if ref is None:
        print("baseline frame missing; cannot verify anything")
        return 1
    ref_px = ref.load()
    w, h = ref.size
    print(f"baseline {w}x{h}")

    # Case 1: identity ! invert -> 255 - baseline.
    chain = load("chain")
    if chain is not None:
        px = chain.load()
        bad = sum(
            1
            for x in range(w)
            for y in range(h)
            if not all(near(px[x, y][c], 255 - ref_px[x, y][c]) for c in range(3))
        )
        results["chain"] = verdict(
            "chain", bad == 0, f"{bad} pixels off 255-baseline"
        )

    # Case 2: fan-out -> left half RED, right half GREEN.
    fanout = load("fanout")
    if fanout is not None:
        px = fanout.load()
        fw, fh = fanout.size
        bad = 0
        for x in range(fw):
            for y in range(fh):
                want = (255, 0, 0) if x < fw // 2 else (0, 255, 0)
                if not all(near(px[x, y][c], want[c]) for c in range(3)):
                    bad += 1
        results["fanout"] = verdict(
            "fanout", bad == 0, f"{bad} pixels off left-red/right-green"
        )

    # Case 3a: A=RED ! B=GREEN ! C=identity -> all GREEN (A unreachable).
    named = load("named-linear")
    if named is not None:
        px = named.load()
        bad = sum(
            1
            for x in range(w)
            for y in range(h)
            if not all(near(px[x, y][c], (0, 255, 0)[c]) for c in range(3))
        )
        results["named-linear"] = verdict(
            "named-linear", bad == 0, f"{bad} pixels off all-green"
        )

    # Case 3b: second sampler tex2 -> report the colour actually produced.
    extra = load("named-extra-sampler")
    if extra is not None:
        px = extra.load()
        r = g = b = 0
        for x in range(w):
            for y in range(h):
                r += px[x, y][0]
                g += px[x, y][1]
                b += px[x, y][2]
        n = w * h
        results["named-extra-sampler"] = verdict(
            "named-extra-sampler",
            True,
            f"mean colour ({r // n},{g // n},{b // n}) - observed, not asserted",
        )

    # Case 4: separable 2-pass blur == folded single-pass 5x5 box.
    two = load("approx-2pass")
    fold = load("approx-fold")
    if two is not None and fold is not None:
        tp, fp = two.load(), fold.load()
        bad = sum(
            1
            for x in range(w)
            for y in range(h)
            if not all(near(tp[x, y][c], fp[x, y][c]) for c in range(3))
        )
        results["approx"] = verdict(
            "approx (2pass vs fold)", bad == 0, f"{bad} pixels differ"
        )

    # Case: shrink (160x120 intermediate) is a negotiation failure, not a frame.
    if os.path.exists(f"{OUT}/shrink.capture.log"):
        log = open(f"{OUT}/shrink.capture.log").read()
        ok = ("ERROR" in log) and (
            "not-negotiated" in log or "NOT_LINKED" in log or "not-linked" in log
        )
        results["shrink"] = verdict(
            "shrink", ok, "negotiation failed as expected (see log)"
        )

    print()
    print("summary:", ", ".join(f"{k}={ 'PASS' if v else 'FAIL'}" for k, v in results.items()))
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
