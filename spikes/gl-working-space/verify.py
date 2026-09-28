#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Genesis-0 contributors
#
# Verdicts for the working-space gate spike. Reads the logs run.sh produced
# and asserts the two gate questions:
#
#   Q1 - linear-light half-float working space: is a >8-bit frame carried
#        GLMemory-resident, and is a linear working space reachable via
#        glcolorconvert's colorimetry?
#   Q2 - non-blocking scopes readback: does a PBO+fence readback keep the
#        render path from stalling (vs. a naive gldownload readback)?

import os
import re
import sys

OUT = "/tmp/opencode/gl-working-space"


def log(name, mode):
    path = f"{OUT}/{name}.{mode}.log"
    return open(path).read() if os.path.exists(path) else ""


def verdict(name, ok, detail):
    print(f"{name:16s} {'PASS' if ok else 'FAIL'}  {detail}")
    return ok


def main():
    results = {}

    # --- Q1: format residency -------------------------------------------
    rgba64 = log("fmt-rgba64", "probe")
    results["fmt-rgba64"] = verdict(
        "fmt-rgba64",
        "memory: GLMemory" in rgba64 and "frames=30 glmem_frames=30" in rgba64,
        "RGBA64_LE carried GLMemory-resident (texture)",
    )

    rgb16 = log("fmt-rgb16", "probe")
    results["fmt-rgb16"] = verdict(
        "fmt-rgb16",
        "memory: GLMemory" in rgb16 and "frames=30 glmem_frames=30" in rgb16,
        "RGB16 carried GLMemory-resident",
    )

    f16 = log("fmt-f16", "probe")
    results["fmt-f16"] = verdict(
        "fmt-f16",
        "ERROR" in f16 and "frames=0" in f16,
        "no half-float format exists (negotiation fails)",
    )

    gs64 = log("fmt-glshader64", "probe")
    results["fmt-glshader64"] = verdict(
        "fmt-glshader64",
        "ERROR" in gs64 and "frames=0" in gs64,
        "glshader rejects 16-bit (RGBA8-only)",
    )

    # --- Q1: transfer / round-trip --------------------------------------
    # glcolorconvert must NOT have applied the transfer function: forcing a
    # linear output colorimetry must be byte-identical to sRGB, and the
    # sRGB->linear->sRGB round trip must be lossless (0 error). Any nonzero
    # error would mean transfer conversion happened.
    for name in ("transfer", "rtt8", "rtt64"):
        txt = log(name, "analyze")
        m = re.search(r"max_err=([0-9.]+)/255", txt)
        ok = m is not None and float(m.group(1)) == 0.0
        results[name] = verdict(
            name,
            ok,
            "max_err=0.00 (glcolorconvert ignores transfer)" if ok else "nonzero error",
        )

    # --- Q2: readback frame rate ----------------------------------------
    def fps(name):
        txt = log(name, "measure")
        m = re.search(r"main_frames=\d+ elapsed_ms=[0-9.]+ fps=([0-9.]+)", txt)
        return float(m.group(1)) if m else 0.0

    base = fps("noreadback")
    dl = fps("tee-download")
    pbo = fps("pbo")
    pbo_txt = log("pbo", "measure")

    results["q2-stall"] = verdict(
        "q2-stall",
        dl > 0 and base > dl * 3,
        f"blocking gldownload stalls render path ({base:.0f} -> {dl:.0f} fps)",
    )
    results["q2-pbo"] = verdict(
        "q2-pbo",
        pbo > dl * 2,
        f"PBO+fence readback recovers ({dl:.0f} -> {pbo:.0f} fps)",
    )
    results["q2-histogram"] = verdict(
        "q2-histogram",
        "nonzero_bins=256" in pbo_txt and "mean_luma=127" in pbo_txt,
        "PBO readback histogram correct (256 bins, mean ~127)",
    )

    print()
    print("summary:", ", ".join(f"{k}={'PASS' if v else 'FAIL'}" for k, v in results.items()))
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
