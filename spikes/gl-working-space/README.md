# Working-space gate spike

Two render-layer unknowns from `docs/decisions/effects-execution.md` §6 and
`docs/dependencies.md` "Phase 0 spike result":

* **Q1 — linear-light half-float working space.** Can the GL chain carry a
  >8-bit RGBA frame, and is a linear (gamma-decoded) working space reachable?
* **Q2 — non-blocking scopes readback.** Can a monitor histogram/waveform be
  read without stalling the render path?

Environment: GStreamer 1.28.6 + `gst-plugins-bad` + GL headers + graphene,
headless EGL, GLES2 3.2 (probe `/tmp/opencode/glprobe.c` confirmed
`MapBufferRange` / `FenceSync` / `ClientWaitSync` / `ReadPixels` all present).

## Build and run

```
cmake -S . -B build && cmake --build build
./run.sh        # runs every case, writes logs under /tmp/opencode/gl-working-space
python3 verify.py   # asserts the verdicts below from the logs
```

Every case is also runnable directly: `./build/gl-working-space <case> <mode>`.
Modes are `probe` (negotiation/residency), `analyze` (round-trip pixel error),
and `measure` (frame-rate). The GL harness reuses `spikes/gl-multipass/`:
headless EGL, `NEED_CONTEXT` bus handling, and the load-bearing forced
`capsfilter caps=video/x-raw(memory:GLMemory)` (without it `glcolorconvert`
emits DMABuf).

## Cases

| case | question | mechanism |
| --- | --- | --- |
| `fmt-rgba64` | Q1 | `RGBA64_LE` through `glupload ! glcolorconvert`, forced GLMemory |
| `fmt-rgb16` | Q1 | `RGB16` through the same chain |
| `fmt-f16` | Q1 | force `format=RGBA_F16` (no such format in GStreamer) |
| `fmt-glshader64` | Q1 | force 16-bit into `glshader` (RGBA8-only) |
| `transfer` | Q1 | `glcolorconvert` sRGB → linear colorimetry, compare pixels |
| `rtt8` / `rtt64` | Q1 | sRGB → linear → sRGB round trip through `glcolorconvert` |
| `noreadback` | Q2 | baseline: single render branch, no readback |
| `tee-download` | Q2 | naive monitor: `tee` + `gldownload` + CPU histogram |
| `pbo` | Q2 | `glscopes` element: double-buffered PBO + fence readback |

## Verdict

### Q1 — linear-light half-float working space: NOT available; 16-bit UNORM available without transfer conversion

* **Half-float does not exist.** No float/half-float video format exists
  anywhere in GStreamer (no `RGBA_F16`/`RGBF`); `fmt-f16` fails negotiation
  (`not-linked`, 0 frames). There is no linear-light *float* working space to
  obtain.
* **16-bit UNORM is carried GPU-resident.** `RGBA64_LE` survives
  `glupload ! glcolorconvert` end-to-end as a `GLMemory` texture
  (`fmt-rgba64`: 30/30 GLMemory frames, `texture-target=2D`). `RGB16` likewise.
* **`glshader` is RGBA8-only.** `fmt-glshader64` fails: forcing a 16-bit frame
  into `glshader` is refused. Any >8-bit effect pass cannot use stock
  `glshader`; a custom `GstGLFilter` (like `glscopes`) or a custom shader path
  is required.
* **`glcolorconvert` does not apply the transfer function.** `transfer` and
  `rtt8`/`rtt64` are byte-identical (`max_err=0.00`, `mean_err=0.0000`) when
  the output colorimetry is forced to linear (`1:1:1:1`) versus sRGB
  (`1:1:7:1`). The colorimetry string is carried through the caps but ignored
  for transfer. Control: CPU `videoconvert gamma-mode=remap` on the same source
  does convert (sRGB mean 131.3 → linear 125.9, `max_err=73/255`), proving the
  measurement detects a transfer change and the zero is not a false negative.

Implication: a linear working space must be entered **in a shader** (explicit
`pow(c, 1/2.4)` / `pow(c, 2.4)` decode/encode in the effect Pack's fragment),
not by tagging colorimetry on `glcolorconvert`. 16-bit UNORM intermediate
buffers are supported, but float is not — so the working space is at best
`RGBA64_LE` (linear-encoded UNORM), never `RGBA_F16`.

### Q2 — non-blocking scopes readback: available with caveat

* **Naive readback stalls the render path.** `tee-download` (`gldownload` on a
  tee branch) collapses the main branch from ~3100 fps to ~350 fps: the
  synchronous `glReadPixels` blocks the shared GL thread that the render path
  needs.
* **PBO + fence readback recovers it.** `pbo` (`glscopes`) keeps the main
  branch at ~3000 fps (within ~4% of the `noreadback` baseline) and still
  produces a correct 256-bin luma histogram (`nonzero_bins=256`,
  `mean_luma≈127`, `mode_bin=255` for the SMPTE pattern).

Mechanism (in `glscopes.c`): `glReadPixels` into a `GL_PIXEL_PACK_BUFFER`
returns immediately after queueing the DMA; the *previous* frame's PBO is
mapped only after its `GL_ARB_sync` fence is signaled, and the check uses
`ClientWaitSync(..., 0)` so `glMapBufferRange` never blocks the GL thread.
Frames whose fence has not signaled by the next frame are dropped — acceptable
for a monitor that only needs a recent frame.

Caveats: the `glscopes` element is a `GstGLFilter`, so it performs an extra
identity render pass (input → output) on the GL thread in addition to the
readback; a production monitor would read back from an existing texture rather
than re-render. The readback path competes for the single GL context/thread, so
headroom depends on render load — the result shows it does not *stall* the
pipeline the way `gldownload` does.

## Files

* `main.c` — harness, cases, `NEED_CONTEXT` handling, forced-GLMemory caps.
* `glscopes.h` / `glscopes.c` — the non-blocking `GstGLFilter` PBO readback.
* `shaders.h` — `SHADER_IDENTITY` fragment for the render-branch effect pass.
* `run.sh` — runs all cases and collects logs.
* `verify.py` — asserts the verdicts above from the logs.
