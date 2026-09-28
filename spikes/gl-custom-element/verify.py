#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Genesis-0 contributors
#
# Pixel-comparison verdicts for the custom-element gate spike. Reads the PNGs
# run.sh produced and reports whether the gltexmix output matches what N-input
# combining is supposed to draw. "Correct" means the pixels match a colour
# that is only reachable when tex0 and tex1 sample DIFFERENT textures: output
# equal to one input is a failure to combine, not success.
#
# combine     RED + GREEN -> YELLOW via vec4(tex0.r, tex1.g, 0, 1)
# tex1-only   sample tex1 only -> GREEN (anti-alias proof)
# named       avg(invert(orig), orig) -> constant ~127.5 gray (needs both)

import os
import sys
from PIL import Image

OUT = "/tmp/opencode/gl-custom-element"
TOL = 6  # per-channel tolerance for 8-bit GL rounding


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


def count_off(px, w, h, want):
    return sum(
        1
        for x in range(w)
        for y in range(h)
        if not all(near(px[x, y][c], want[c]) for c in range(3))
    )


def main():
    results = {}

    # combine: both samplers bound independently -> YELLOW. Any other colour
    # (RED if tex1 aliases tex0, GREEN if tex0 aliases tex1) fails.
    combine = load("combine")
    if combine is not None:
        px = combine.load()
        w, h = combine.size
        yellow_bad = count_off(px, w, h, (255, 255, 0))
        red_bad = count_off(px, w, h, (255, 0, 0))
        green_bad = count_off(px, w, h, (0, 255, 0))
        results["combine"] = verdict(
            "combine",
            yellow_bad == 0,
            f"yellow_off={yellow_bad} (red_off={red_bad} green_off={green_bad})",
        )

    # tex1-only: sampling tex1 (not tex0) is the anti-alias proof -> GREEN.
    t1 = load("tex1-only")
    if t1 is not None:
        px = t1.load()
        w, h = t1.size
        green_bad = count_off(px, w, h, (0, 255, 0))
        results["tex1-only"] = verdict(
            "tex1-only", green_bad == 0, f"{green_bad} pixels off green"
        )

    # named: invert(orig) + orig averages to constant ~127.5 gray. Only
    # reachable if tex0 and tex1 both sample distinct, correct textures.
    named = load("named")
    if named is not None:
        px = named.load()
        w, h = named.size
        bad = 0
        for x in range(w):
            for y in range(h):
                r, g, b = px[x, y]
                if not (near(r, 128) and near(g, 128) and near(b, 128)):
                    bad += 1
        results["named"] = verdict(
            "named", bad == 0, f"{bad} pixels off constant ~128 gray"
        )

    print()
    summary = ", ".join(f"{k}={'PASS' if v else 'FAIL'}" for k, v in results.items())
    print("summary:", summary)
    return 0 if (results and all(results.values())) else 1


if __name__ == "__main__":
    sys.exit(main())
