# Genesis-0 — Working-Space Decision

Status: **Proposed** (decision material, 2026-09-29). Checked: 2026-09-29. The only performance
figures in this document are the readback frame rates measured by the spike, labelled as measured on
this machine; no other performance claim is made or implied. Companion evidence:
`docs/decisions/effects-execution.md` §6/§8, `docs/rewrite-plan.md` §3(b) and §7 crate table,
`docs/dependencies.md` §"Phase 0 spike result", `docs/architecture.md` (T4, R5/R6, R11),
`src/core/ShaderPass.h`, `src/effects/Pack.h`, `src/effects/Resolve.h`, and the spike at
`spikes/gl-working-space/`.

Date: 2026-09-29. Deciders: pending project owner.

Scope: the **colour working space** the effect Pack chain and the compositor operate in — whether a
linear-light, extended-range, half-float space is available on the chosen stack, and if not, what the
host must substitute. It is **not** the effect execution path (`docs/decisions/effects-execution.md`
§4/§6) or the compositor verdict (`docs/decisions/compositor-path.md`); both are settled and this
record sits under them. It is **not** the scopes compute pass as a mechanism — only the readback
measurement is recorded here (§4).

## Terminology (enforced)

Public/product names are **Extension** and **Pack**; the public SDK is the **Extension SDK**
(`docs/architecture.md` §1). The word *plugin* is reserved for technical frameworks and their objects
(GStreamer/GES elements, Qt plugins, a manifest's `kind` field). A shader package in Genesis-0 is a
**Pack** — it declares a control schema and a shader body the host runs inside the pipeline; it is not
an executable Extension and cannot do arbitrary IO (`docs/rewrite-plan.md:158-162`).

---

## 1. Context

`docs/rewrite-plan.md` §3(a) records Concat's compositor working space as **linear light, Rec.709
primaries, extended scRGB, half floats**, converting on upload and rolling off HDR with BT.2390
(`docs/rewrite-plan.md:104-105`). That space is what makes compositing and per-layer effect maths
correct: blending, blur, bloom, and glow are linear operations, and running them on gamma-encoded
bytes produces the wrong result. The `concat-render` crate owns it (`docs/rewrite-plan.md` §7 crate
table, row `concat-render`) and `docs/rewrite-plan.md` §3(b) names its absence on GES as a top
render-layer risk, "not exercised; still open."

`docs/decisions/effects-execution.md` carried the same open item to this record: §6 gate item 5 says
the working space "remains unproven and affects every Pack, not only the 5
(`docs/dependencies.md:137-139`)", and §8 lists it as "unresolved". §4.1(b) records the concrete
constraint that forced this spike: "The element advertises `RGBA` only; Concat's working space is
extended-linear half-float."

The spike `spikes/gl-working-space/` was run to settle whether that space can be obtained. Its
harness reuses `spikes/gl-multipass/` (headless EGL, `NEED_CONTEXT` bus handling, the load-bearing
forced `caps=video/x-raw(memory:GLMemory)` capsfilter), and `verify.py` asserts its verdicts from the
run logs with 10/10 programmatic assertions. What it proves and what it does not is kept strictly
separate below.

## 2. The verified evidence

Read from the spike; the full raw detail is in `spikes/gl-working-space/README.md`.

- **No float or half-float format exists anywhere in the GStreamer chain.** `fmt-f16` forcing
  `format=RGBA_F16` fails negotiation (`not-linked`, 0 frames). There is no `RGBA_F16`/`RGBF` video
  format to obtain.
- **`glshader` is RGBA8-only.** `fmt-glshader64` forcing a 16-bit frame into `glshader` is refused
  (error, 0 frames). A >8-bit effect pass cannot use stock `glshader`.
- **16-bit UNORM is carried GLMemory-resident.** `RGBA64_LE` survives `glupload ! glcolorconvert`
  end to end as a `GLMemory` texture (`fmt-rgba64`: 30/30 GLMemory frames, `texture-target=2D`);
  `RGB16` likewise. So a 16-bit *buffer* is negotiable and stays on the GPU — the constraint is the
  pass element, not the format.
- **`glcolorconvert` performs no transfer conversion.** Forcing a linear output colorimetry
  (`1:1:1:1`) is byte-identical to sRGB (`1:1:7:1`): `transfer`, `rtt8`, and `rtt64` all measure
  `max_err=0.00`, `mean_err=0.0000`. The colorimetry string is carried in the caps and ignored for
  transfer. A CPU control (`videoconvert gamma-mode=remap`) on the same source *does* convert (sRGB
  mean 131.3 → linear 125.9, `max_err=73/255`), which proves the zero is a real negative — the
  measurement detects a transfer change — and not a failed read.
- **A non-blocking readback is achievable.** The `glscopes` element (double-buffered
  `GL_PIXEL_PACK_BUFFER`, `GL_ARB_sync` fence, non-blocking `ClientWaitSync(..., 0)`,
  `glMapBufferRange`) held the main branch at **~2343 fps** on an 8-second run where a naive
  `gldownload` tee collapsed it to **~375 fps** (baseline single branch, no readback: **~2503 fps**).
  These figures are measured on this machine and are not a general performance claim.

Consequence of the first four bullets: a linear working space must be entered **in a shader** by
explicit decode/encode, not by tagging colorimetry on `glcolorconvert`. And there is no float space
to obtain — the best the stack can carry as an intermediate buffer is `RGBA64_LE`, never `RGBA_F16`.

## 3. The decision

**Recommended: (a), an in-shader transfer contract over an `RGBA8` frame, with (b) reserved for Packs
whose precision demands it and (c) not selected for the first parity target.** The host guarantees
the picture between elements is gamma-encoded `RGBA8`; every Pack's fragment shader decodes at its
start, works, and re-encodes at its end. This is the only working space the proven execution path can
deliver: `glshader` is RGBA8-only (`fmt-glshader64`), so on the stock chain there is no element that
can render a pass into a wider target. The decision does not pretend Concat's extended-linear
half-float space exists; it defines the substitute the host can honour today and states its limits.

### (a) Encode gamma in-shader — recommended

Stay `RGBA8`, and have each Pack's shader do explicit `decode → work → encode` in its fragment body.
The picture between elements stays encoded sRGB, so every Pack sees the same format and no Pack
depends on its neighbour. Cost: the working space is only as linear as the shader is disciplined —
correctness inside one Pack is guaranteed by its own decode/encode, but an intermediate between two
Packs is 8-bit encoded, so cross-Pack blending is quantised and any Pack that skips its encode leaves
the next one a frame it did not expect. The contract is the discipline.

### (b) `RGBA64_LE` working format with in-shader transfer conversion — reserved

`RGBA64_LE` is negotiable GPU-resident (`fmt-rgba64`, 30/30 GLMemory) and halves the false economy of
8-bit intermediates, at bandwidth cost. But `glshader` refuses a 16-bit frame (`fmt-glshader64`), so
a 16-bit *pass* needs a custom element — it is not reachable on the stock chain. So (b) is not an
alternative to (a); it is the precision escalation a Pack takes only when its maths demonstrably
cannot run at 8-bit, and it rides the same custom-element cost already contemplated for the 5
named-intermediate Packs (`docs/decisions/effects-execution.md` §6, §10.4). It is recorded as
available, not selected by default.

### (c) Custom element owning a float render target — not selected

The mechanism `spikes/gl-custom-element/` proved (a `GstGLMixer` subclass binding its own textures)
is the only route to a true float render target. It is not selected. It buys a float space for the
custom element's own passes, not for GES's compositor, so it does not restore Concat's
extended-linear half-float compositing space — it adds a production element per shader path and still
leaves the composite between elements at the format the stock chain negotiates. The custom-element
cost is already carried, once, for the 5 named-intermediate Packs; duplicating it for every Pack to
approximate a space GES still does not own is not justified for the first parity target. Revisit if
HDR/extended-range becomes a product requirement (§5 trigger 2).

**What this forecloses.** Concat's linear-light extended-range half-float working space is not
selected and is not obtainable on the chosen stack; HDR BT.2390 rolloff and true extended-range
compositing are out of the first parity target. No `RGBA_F16` path is planned. Correct linear blending
across a Pack boundary is not guaranteed by the host — it is guaranteed only within a Pack whose
shader honours the contract. The `concat-render` crate's working-space behaviour is not ported; it is
re-derived as a per-Pack shader discipline plus a host format contract.

**Cost per Pack.** One decode and one encode in the fragment body, and the acceptance that the Pack
is only correct if it performs them. This is a re-author cost that overlaps the 98 GLSL bodies already
required by `docs/decisions/effects-execution.md` §5, not a separate rewrite. A Pack that needs
16-bit takes (b), which carries the custom-element cost.

## 4. Consequences for the Pack schema and the Extension SDK

**The Pack declares its precision need, not a space it demands.** A Pack should not declare the
working space it expects — the host guarantees one, and a declaration that disagrees is worse than
none. The concrete schema change is a per-Pack statement of the *minimum working precision* it needs
(8-bit vs 16-bit), so the resolver can refuse or route a Pack whose maths cannot run in the host's
space. Concretely:

- `src/effects/Pack.h`: the `Pack` struct gains one field stating the format floor the Pack requires
  (the two levels the spike leaves open: `RGBA8` and `RGBA64_LE`). It is host-declared data, no
  engine or shader type crosses into the schema, matching the existing rule that `Pack.h` holds the
  engine-free declaration only (`Pack.h:18-25`).
- `src/effects/Resolve.h`: `is_expressible` (`Resolve.h:34`) already decides whether a Pack runs on
  the chosen path; it becomes the place that checks the Pack's precision floor against the format the
  resolved path can render. A Pack whose floor the stock chain cannot meet is not expressible on the
  chosen path and takes the R9 fallback, exactly as a named-intermediate Pack does today. No new
  verdict branch is invented; the existing one gains a clause.
- `src/core/ShaderPass.h`: the resolved payload already carries `source`, `stages`, `lut`,
  `reveal_map`, and `intensity` (`ShaderPass.h:133-172`). The working space adds no new field there:
  the format is a property of the chain the adapter builds, not of a single pass. `ShaderPass::source`
  keeps its `docs/decisions/effects-execution.md` §4.3 change from WGSL to GLSL; the decode/encode
  contract is part of that GLSL, authored per Pack.

**The Extension SDK's promise.** The SDK must state a guaranteed working space rather than a
Concat-parity space. The honest promise is: *the host guarantees an encoded `RGBA8` frame between
elements; a shader Pack must decode and encode its own work; the host makes no extended-range or
half-float promise.* A Pack that needs 16-bit working precision declares that floor and is subject to
the custom-element path. An executable Extension (the R8 deliverable,
`docs/rewrite-plan.md:401`) is held to the same contract when it writes into the picture: it receives
and returns encoded `RGBA8` unless it and the host agree otherwise. The SDK does not promise Concat's
colour behaviour; it promises a documented format and a documented discipline.

**What is not claimed.** No Pack yet honours any working space — the schema field and the resolver
clause above are proposed, not implemented. The spike proved the *stack constraint*, not a Pack
contract; the record proposes the contract that constraint forces.

## 5. The readback finding

Settled for scopes: a non-blocking readback is achievable. `glscopes` uses a double-buffered
`GL_PIXEL_PACK_BUFFER` with a `GL_ARB_sync` fence, checks it with `ClientWaitSync(..., 0)`, and only
maps a frame when its fence has signalled; frames not ready by the next frame are dropped, which is
acceptable for a monitor that needs a recent frame. Measured on this machine: the render branch held
**~2343 fps** with the PBO readback against **~375 fps** for a naive `gldownload` tee, and the
histogram was correct (`nonzero_bins=256`, `mean_luma≈127`, `mode_bin=255` on the SMPTE pattern).
This settles the Q2 gate question raised in `docs/dependencies.md:137-139`.

**The caveat, recorded.** The `glscopes` element is a `GstGLFilter`, so it performs an extra identity
render pass (input → output) on the GL thread in addition to the readback. A production monitor would
read back from an **existing** texture rather than re-render one. That path — read-from-existing-
texture, on a production element, without the identity pass — is **not proven** by this spike. The
readback also competes for the single GL context/thread, so headroom depends on render load; the
result shows it does not *stall* the pipeline the way `gldownload` does, not that it is free at every
load.

## 6. Revisit triggers

1. HDR or extended-range becomes a product requirement for the first parity target, re-opening
   option (c) and the BT.2390 rolloff that Concat's `concat-render` owns.
2. A Pack proves, by measurement, that its maths cannot run at 8-bit precision, forcing (b) and its
   custom-element cost onto that Pack.
3. A stock chain emerges that can render a pass into a 16-bit or float target (a future `glshader` or
   GStreamer format), removing the custom-element requirement for (b).
4. The production read-from-existing-texture scope path is proven, closing the §5 caveat.

## 7. Open questions for the owner

1. **Is the guaranteed `RGBA8` encoded space acceptable for the first parity target**, with effects
   run through an in-shader decode/encode contract, or is extended-linear half-float a correctness
   requirement that justifies the custom-element cost across the chain? (`docs/decisions/
   effects-execution.md` §10.2 asked the same in weaker form; the spike now reduces it to a cost
   question, not a possibility question.)
2. **Is a per-Pack precision floor (8-bit vs 16-bit) in scope for the Pack schema**, or should every
   Pack be assumed 8-bit for the first target and 16-bit deferred entirely?
3. **Does the owner accept that the SDK promises a documented format discipline rather than Concat
   colour parity** — i.e. that cross-Pack linear blending is per-shader, not host-guaranteed?
4. **Is the 16-bit upgrade path (b) funded by the same custom-element decision as the 5
   named-intermediate Packs**, or is it a separate element to scope, license, and maintain?
5. **Is the read-from-existing-texture scopes path a Phase 4 task**, or is the identity-pass caveat
   accepted for the first monitor?

## 8. Evidence index

| Fact | Location |
|---|---|
| Working space is linear-light extended half-float; the top render-layer risk | `docs/rewrite-plan.md:104-105`; `docs/rewrite-plan.md` §7 `concat-render` row; `docs/rewrite-plan.md` §3(b) |
| Working space carried as the open gate item affecting every Pack | `docs/decisions/effects-execution.md` §6 item 5 / §8; `docs/dependencies.md:137-139` |
| `glshader` advertises RGBA only | `docs/decisions/effects-execution.md` §4.1; `gst-inspect-1.0 glshader` (1.28.6) |
| Spike source; raw detail | `spikes/gl-working-space/` (`main.c`, `glscopes.c`, `glscopes.h`, `shaders.h`, `run.sh`, `verify.py`, `README.md`) |
| Spike assertions, 10/10 | `spikes/gl-working-space/verify.py:31-107` |
| No half-float format exists; `RGBA_F16` negotiation fails, 0 frames | `fmt-f16` (`/tmp/opencode/gl-working-space/fmt-f16.probe.log`); `verify.py:49-54` |
| `glshader` is RGBA8-only; forcing 16-bit is refused, 0 frames | `fmt-glshader64`/`fmt-glshader64.probe.log`; `verify.py:56-61` |
| `RGBA64_LE` carried GLMemory-resident, 30/30, `texture-target=2D` | `fmt-rgba64.probe.log`; `verify.py:35-40` |
| `RGB16` likewise GPU-resident | `fmt-rgb16.probe.log`; `verify.py:42-47` |
| `glcolorconvert` performs no transfer conversion; linear output byte-identical to sRGB (`max_err=0.00`) | `transfer`, `rtt8`, `rtt64`; `verify.py:63-76` |
| CPU control `videoconvert gamma-mode=remap` does convert: sRGB mean 131.3 → linear 125.9, `max_err=73/255` | `spikes/gl-working-space/README.md` §Q1; CPU control run |
| Non-blocking readback measured: pbo ~2343 fps vs gldownload tee ~375 fps; no-readback baseline ~2503 fps (this machine) | `pbo.measure.log`, `tee-download.measure.log`, `noreadback.measure.log`; `verify.py:78-98` |
| PBO histogram correct: 256 bins, mean ~127, mode_bin=255 | `pbo.measure.log`; `verify.py:99-103` |
| Readback caveat: `glscopes` is a `GstGLFilter` performing an extra identity pass; production should read back from an existing texture — not proven | `spikes/gl-working-space/README.md` §Q2 |
| Custom element proved a `GstGLMixer` subclass binds N textures (basis of option (c)) | `spikes/gl-custom-element/`; `docs/decisions/effects-execution.md` §6 |
| `concat-render` owns the working space in Concat's custom compositor | `docs/rewrite-plan.md` §7 crate table |
| Render rules and C++23 constraint | `docs/architecture.md` §2.1 T4/R11; R5/R6 |
| Pack schema (engine-free declaration) | `src/effects/Pack.h:18-25,167-194` |
| Resolver seam; `is_expressible` | `src/effects/Resolve.h:29-43` |
| Resolved payload fields; `source` semantics | `src/core/ShaderPass.h:133-172` |
| Execution path: `glshader` GLSL re-author; multi-pass custom-element cost | `docs/decisions/effects-execution.md` §4/§5/§6 |
| Compositor verdict this record sits under | `docs/decisions/compositor-path.md` §3 |

See `docs/decisions/effects-execution.md` for the Pack schema and execution path this record
constrains, `docs/rewrite-plan.md` §3(b) for the original risk, and
`docs/decisions/compositor-path.md` for the compositor verdict.
