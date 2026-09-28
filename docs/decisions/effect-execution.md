# Genesis-0 — Effects Execution Decision

Status: **Proposed** (decision material, 2026-09-29). Checked: 2026-09-29. No performance claim is
made or implied by this document. Companion evidence: `docs/history/rewrite.md` §2 line 74 / §3(b) /
§4 Phase 4 / §7 Q4, `docs/architecture.md` §1/§2.1 (R5–R8) / §9, `docs/decisions/compositor-path.md`,
`docs/decisions/engine-adapter-contract.md`, `docs/dependencies.md` §"Phase 0 spike result",
`src/core/ShaderPass.h`, `src/render/Resolve.h`, and the reference `effects` crate.

Date: 2026-09-29. Deciders: pending project owner.

Scope: how a Pack's effect declaration becomes executed pixels on GES — the host-side control
schema, the effect execution path, the shader language and its validation, and the multi-pass
question. It is **not** the compositor verdict (`docs/decisions/compositor-path.md`) or the engine
adapter contract (`docs/decisions/engine-adapter-contract.md`), and it is **not** the executable-Extension
path (T3, native extension UI; the web mini-app/WASM sandbox is **dropped**, `docs/architecture.md` §10).

## 1. Context

`docs/history/rewrite.md` §3(b) is flagged **HIGH RISK**: 123 effect packages authored in WGSL have no
direct equivalent in Genesis-0's Extension/Pack model. Phase 4's scope is "Convert the reference editor's package
model into Genesis-0's Extension/Pack contract and the signed install pipeline; decide the effect
execution path from Phase 0's verdict" (`docs/history/rewrite.md:382-385`); the Phase 4 artifact list
names `docs/decisions/effect-runtime.md (not created)` for that decision (`docs/history/rewrite.md:392`). **This
record is that artifact under its task-assigned name**; Phase 4 should treat `effect-execution.md`
as the planned `effect-runtime.md (not created)`.

`docs/decisions/compositor-path.md` §3 settled the render path: GES's compositor is kept, the
`FramePlan` adapter boundary is not required, and "effects are re-authored against GStreamer
primitives" (`compositor-path.md:71`). It deliberately left the effect-execution items open and
carried them to this record (`compositor-path.md:109-112`). The Phase 0 spike proved chained
`glshader` composition and a zero-copy GPU-resident frame path, but **did not** exercise
named-intermediate multi-pass graphs, the extended-linear half-float working space, or a
non-blocking scopes compute pass (`docs/dependencies.md:126-139`).

## 2. The reference package model, measured

Ground truth from the reference checkout's `effects` crate and its `packages/` directory. These are
observations, not proposals.

**Packages and format.** 123 packages exist; all manifests are `format = 2` (`docs/history/rewrite.md`
Appendix; `manifest.rs:28` `FORMAT = 2`, `manifest.rs:31` `SCENE_LINEAR = 2`). A package is
`effect.toml` plus `effect.wgsl` for picture effects or an FFmpeg chain template for sound. Of the
123: 78 are visual shader packages (`[wgsl]`), 20 are transitions (`[transition]`), and 25 are
audio filter chains (`[ffmpeg]`); 98 `effect.wgsl` files total.

**What a manifest declares** (`manifest.rs:34-68`): `format`; `[effect]` `Meta` (namespaced `id`
`author.name`, `name`, `kind` ∈ effect/filter/audio/transition/generator, `category`, `version`,
`intensity`, `aliases`, `order`, `description`; `manifest.rs:140-169`); `[[param]]` (key, label,
group, `type`, min, max, default, step, unit, animate, values, labels; `manifest.rs:205-244`);
`[ffmpeg]` (lets + chain, sound/format-1); `[wgsl]` (entry, `[[pass]]`, `space` ∈
linear/display/log); `[transition]` (entry, `xfade`, `fallback`; `manifest.rs:348-363`); `[lut]`
(a `.cube` beside the manifest; `manifest.rs:132-137`); `[card]`; `[[replaces]]` (retired-package
migration; `manifest.rs:76-94`).

**Parameter types** (`manifest.rs:251-279`): float, int, bool, enum, color, point, wheel, curve.
Point/wheel/curve are *compound* — the document stores dotted keys `<key>.x`/`.y`/`.m` and
`<key>.<n>.x`/`.y`, and the manifest maps an owned dotted key back to its parameter
(`manifest.rs:284-290`, `manifest.rs:780-795`). Curves cap at 8 points (`manifest.rs:282`).

**Multi-pass.** A `[[wgsl.pass]]` declares `target` and an optional `shrink = [across, down]` pair
of expressions over the knobs (`manifest.rs:431-443`); `effect` is always the last pass. At most 8
passes, shrink rounded down to a power of two from 1 to 64 (`manifest.rs:447-451`). The shader's
`fn <target>(uv)` draws each picture, and later passes read it through `<target>_at(uv)`; the host
stitches the binding, the reader, and a per-pass entry point `fs_<target>` (`shader.rs:35-42`,
`shader.rs:1142-1177`). **Only 5 of the 123 packages use `[[wgsl.pass]]`**: the reference `gaussian-blur` pack,
the reference `bloom-pulse` pack, the reference `glow` pack, the reference `motion-blur` pack, the reference `zoom-blur` pack.

**The shader contract and naga's role** (`shader.rs`). A package writes two things: a `struct
Params` whose fields are its knobs, and `fn effect(uv) -> vec4<f32>`; the host supplies bindings
(layer at group 0, frame + params uniforms at group 1, LUT 3D texture at group 2, reveal map at
group 3), the shared library, the vertex stage, and the intensity mix (`shader.rs:4-42`,
`shader.rs:55-91`). Transitions write `fn transition(uv, progress)` over two input pictures
(`shader.rs:178-216`). `naga` provides load-time WGSL parse + validation
(`shader.rs:1484-1495`), reads the `Params` struct for each field offset so a knob set becomes a
uniform buffer with no layout declared by the author (`shader.rs:1506-1566`), rejects bindings the
host does not serve and unbounded loops (`shader.rs:1056-1136`), and rejects a pass reading its own
or a later picture (`shader.rs:1184-1232`). Compiled pipelines are cached by
`id@version#hash(source)` (`shader.rs:1398-1403`). The catalogue resolves applied effects into
`[ShaderPass]` per frame, keyed parameters included (`catalogue.rs:1088-1106`), and transitions
into a `TransitionPass` (`catalogue.rs:1112-1127`). A community package is trialled once at 512 px
under a three-second timeout before it is enabled (the reference checkout's `src/studio.rs:6439-6452`;
the reference render crate's `src/gpu.rs:1787-1851`).

**Host types already ported.** `src/core/ShaderPass.h` carries `Lut`, `RevealMap`, `Stage`, and
`ShaderPass` (`ShaderPass.h:27,64,96,114`) plus `TransitionPass` (`ShaderPass.h:166`), ported from
the reference core crate's `src/shader.rs`. Their `source` fields are documented as "the complete WGSL
module" (`ShaderPass.h:128-129`, `ShaderPass.h:170-172`), `ShaderPass::stages` is the pre-last pass
list (`ShaderPass.h:149`), and `params` is a laid-out byte buffer with `MIN_PARAMS = 16`
(`ShaderPass.h:117,132`). `src/render/Resolve.h` already routes effects: `BuiltTimeline.chains`
holds each clip's `AppliedFilter`s (`Resolve.h:152-154`), `treatments` and `transitions` are
carried (`Resolve.h:158-160`), and `Treatment::passes_at` resolves the chain through the effect
catalogue over the layer's span (`Resolve.cpp:394-415`). `AppliedFilter` is a host type in
`src/project/model/Effects.h:425`. The GES adapter wires treatments through
`src/adapters/engine/ges/effects/{ShaderChain,ClipEffects}.cpp` at build time
(`GesBuilder.cpp:836,890`) and transitions through GES standard transitions
(`GesBuilder.cpp:910-933`).

## 3. Decision 1 — The Pack control schema

A new, engine-free schema lives under `src/effects/` and links into `genesis_host` (no Qt, no
GStreamer; `docs/decisions/engine-adapter-contract.md` §3). It declares what a Pack *is*: `id`, `version`, `kind`,
`category`, `aliases`, `order`, `description`; a list of typed `parameters`, each with `type`
(float/int/bool/enum/color/point/wheel/curve), range, default, step, unit, and animate flag; the
`passes` (target + shrink expression); and the transition form (`xfade` fallback). It mirrors
the reference editor's manifest (§2) but is expressed in host types and follows `docs/architecture.md` §9: **the
control schema is host-defined declarative data**, compiled to portable widgets; arbitrary QML/JS
is not an acceptable control payload.

**The ported `ShaderPass`/`Stage`/`Lut`/`RevealMap`/`TransitionPass` types are not the schema.**
They are the *resolved render payload* — the output of resolving the schema against a clip's
`AppliedFilter` values, and the input the engine adapter consumes. A Pack declares a pass as
`{target, shrink-expression}`; resolution evaluates that expression to a `Stage` (`entry`,
integer `shrink`) and assembles a `ShaderPass`; `Lut` and `RevealMap` are bound resources;
`TransitionPass` is the resolved two-input combine. `ShaderPass` is therefore **downstream of** the
control schema, not a synonym for it. `Treatment::passes_at` (`Resolve.h:105`,
`Resolve.cpp:243-257`) is where this resolution lands.

**Vocabulary.** A visual shader package is a **Pack**; it does not become an Extension. R8 requires
that presets/control schemas alone do not satisfy the ecosystem goal (`docs/architecture.md:41`),
so the Extension SDK and one independently packaged example remain a **separate Phase 4
deliverable** (`docs/history/rewrite.md:401`), as does the R7 signed install pipeline. The 123
packages are Packs; they cannot be the executable Extensions R8 demands (`docs/history/rewrite.md`
§3(b)).

## 4. Decision 2 — The execution path

**Selected: (c), the hybrid.** GES's compositor composes layers and transitions; a Pack's effects
execute as `glshader` chains where they are expressible; a Pack the chain cannot express is handled
by the documented R9 fallback in §6. This keeps the compositor verdict (`compositor-path.md` §3),
uses the one effect mechanism the spike proved (`docs/dependencies.md:112-114`), and settles the
multi-pass graph as an explicit negative rather than an assumption.

### 4.1 (a) GES `glshader` passes

Proven: two chained `glshader` elements compose and keep frames GPU-resident as `GLMemory`
(`docs/dependencies.md:112-114`). Concrete costs:

- **Language.** `glshader` takes GLSL via its `fragment`/`vertex` properties, not WGSL
  (`gst-inspect-1.0 glshader`, properties `fragment`/`vertex`/`uniforms`). Every one of the 98
  shader bodies is a re-author, not a port; the host-stitched WGSL prelude (`shader.rs:55-91`) has
  no direct translation.
- **No injection.** `glshader` does not inject varyings or uniforms; each fragment shader must
  declare its own `precision mediump float; varying vec2 v_texcoord; uniform sampler2D tex;`
  (`docs/dependencies.md:126-128`). Uniforms are supplied as a `GstStructure` on the `uniforms`
  property, so the host must own parameter layout rather than read it from naga.
- **Single input.** `glshader` has one sink pad and one source pad (`gst-inspect-1.0 glshader`), so
  the multi-pass model was verified against that shape; the result is the §6 negative.
- **Working space.** The element advertises `RGBA` only; the reference editor's working space is extended-linear
  half-float (`docs/history/rewrite.md:104-105`). The spike did not exercise it and it remains open
  (`docs/dependencies.md:137-139`); §6 carries it as a gate item.

### 4.2 (b) A custom RHI compositor for effects only

Rejected in general by `compositor-path.md` §4.1 and not re-opened: it contradicts "GES is the
engine", duplicates GES compositing, and would re-derive the working space and scopes work GES
already owns. It is not selected for effects specifically either — effects are per-layer transforms
on the frame GES already composites, so they do not justify a second compositor.

### 4.3 (c) Hybrid — selected

GES composes layers and transitions; effects run as `glshader` chains where expressible. The
"where expressible" boundary is the named-intermediate case, settled by the multipass prototype and
refined by the custom-element spike (§6): a `glshader` can transform its single input, so a linear
pass chain is expressible, and stock
`tee`→`glvideomixer` fan-out composes spatially, but a final pass that must read both the original
layer *and* a separately sampleable intermediate (the `across`→`down`→`effect` shape of
the reference `gaussian-blur` pack) is not expressible from stock elements. §6 closes that gap with a custom
element; the R9 path and single-pass re-author remain the downgrade alternatives.

**What this forecloses.** No WGSL execution path is selected; no naga-equivalent C++ validator is
adopted; no custom RHI compositor for effects. `ShaderPass::source`/`TransitionPass::source` change
semantics from WGSL to GLSL (`ShaderPass.h:128-129,170-172`) — a host-only change, no engine type
crosses. The multi-pass subset is settled (§6): stock elements cannot reach it — a precise negative —
but the custom-element spike proves a `GstGLMixer` subclass can bind the named intermediates. The 5
named-intermediate Packs are therefore excluded from the *stock* expressible set and expected to take
the custom-element path (§6), at a custom-element cost the owner must still accept (§10.4).

## 5. Decision 3 — Shader language and validation

**Author in GLSL for `glshader`.** The 123 packages are a **re-author**, not a port: 98 shader
bodies are rewritten, each declaring its own precision/varyings/uniforms per the spike's constraint
(`docs/dependencies.md:126-128`), the host prelude is rewritten in GLSL, and the `[[param]]` types
are delivered as a host-built `GstStructure` on `glshader.uniforms`.

**What replaces `naga`.** `naga` gave load-time WGSL validation, automatic `Params` layout, a
binding budget, and a bounded-loop check (`shader.rs:1056-1136,1484-1566`). Under GLSL:

- **Parameter layout becomes host-owned.** The host knows the uniform names and layout it emits, so
  the naga-read offset table (`shader.rs:1506-1566`) is replaced by the schema plus the resolver —
  a simplification, not a loss.
- **Validation moves to pipeline build.** GLSL is compiled by the GL driver when the `glshader`
  element builds its pipeline. **How a compile failure surfaces is not verified**: the record
  requires the Phase 4 prototype to identify the exact surface (the expected mechanism is a
  `GST_MESSAGE_ERROR` posted to the pipeline bus, mapped to the R9 unsupported-effect path) before
  the host relies on it.
- **The load-time security budget is not carried.** The binding and bounded-loop checks have no
  `glshader` equivalent; the trial-at-512 px timeout (the reference checkout's `src/studio.rs:6439-6452`) is the
  closest primitive and the spike did not exercise it on `glshader`. Re-deriving a static GLSL
  budget, or accepting the trial as the only defense, is a Phase 4 task and a noted security
  consequence (§8).

**Cost.** The rewrite is 98 GLSL bodies plus the prelude and the parameter plumbing; only the 5
multi-pass packages touch the multi-pass graph settled in §6. Reused reference shader assets remain GPL-3.0 and must
retain their notice (`docs/history/rewrite.md:166-170`; `ShaderPass.h:1-6`).

## 6. Decision 4 — The multi-pass question (settled)

The Phase 4 gate has been run. Spike `spikes/gl-multipass/` (`main.c`, `shaders.h`, `run.sh`,
`verify.py`) ran headless EGL on `videotestsrc pattern=smpte` 320×240, classified frames at an
appsink, captured PNGs, and compared them pixel-by-pixel; evidence is under
`/tmp/opencode/gl-multipass/`. The linear-chain baseline is reconfirmed and the gate resolves as a
precise negative for stock elements: `ShaderPass::stages` is the host model of named intermediates
(`ShaderPass.h:93-111,149`), and that model does not reach stock `glshader`. A follow-up spike
(`spikes/gl-custom-element/`) has since closed the custom-element question affirmatively: a custom
element *can* bind the named intermediates.

**Expressible with stock elements.**

- **Linear chain.** `glshader ! glshader`: EOS, 30/30 frames `GLMemory`, caps
  `video/x-raw(memory:GLMemory), format=RGBA, texture-target=2D`; pixel check `invert` output equals
  `255 − baseline`, 0 pixels off.
- **Spatial fan-out / fan-in, GPU-resident.** `tee` → two `glshader` → `glvideomixer` with per-pad
  `xpos`/`width`: EOS, 30/30 frames `GLMemory`, `texture-target=2D` through the mixer; pixel check
  left half RED / right half GREEN, 0 pixels off.

**Not expressible with stock elements — the settled negative.** A final pass cannot read a layer
plus N intermediates as N samplers. Three independent negatives:

- A linear chain exposes **only the previous** pass: `RED ! GREEN ! identity` yields all green, so
  A's intermediate is unreachable.
- `glshader` binds **exactly one texture**: a shader declaring a second `sampler2D` and sampling it
  produced output byte-identical to baseline (md5-equal) — the extra sampler silently aliases
  texture unit 0, with no error raised. No property surface carries a second texture in. The
  adapter's `lutgrade` element (a GstGLFilter subclass) lifts this limit for the colour-look case
  by binding the picture on unit 0 and a 3D LUT on unit 1 with explicit `glUniform1i`, so a
  resolved `ShaderPass::lut` is sampled as a real table instead of the pack's GLSL approximation.
- `glvideomixer` **composites spatially**; it cannot hand a later pass two separately sampleable
  inputs.

`Stage::shrink` is unrepresentable: `glshader ! video/x-raw,width=160,height=120 ! glshader` fails
negotiation (`reason not-linked`), 0 frames. `glshader` is a `GstBaseTransform` with identical in/out
dimensions, and no stock element renders a pass into a smaller offscreen texture.

**Warning — silent aliasing.** A shader that declares a second `sampler2D` is not rejected; it
aliases texture unit 0 and returns the same input. Any attempt to feed a second texture through
`glshader`'s property surface therefore fails silently, not at build time. The RGBA capsfilter is
load-bearing: without a forced `caps=video/x-raw(memory:GLMemory),format=RGBA`, `glcolorconvert`
emits a zero-copy `DMABuf` (`drm-format=Y412`) rather than a texture.

**Closest expressible approximation.** Folding N stages into one shader body works **only when every
stage reads the same source**: a single-pass 5×5 box over 25 fetches matched `H-blur ! V-blur` with 0
pixels differing. Cost: N² fetches instead of 2N, and the intermediate texture no longer exists.

**Gate items, as measured.**

1. **Named intermediate at a shrunk size.** Failed for stock elements: `shrink` is unrepresentable
   and a linear chain exposes only the previous pass. Whether the custom element can render a pass
   into a smaller offscreen texture is **not tested** by either spike and remains open.
2. **Two-input final pass.** Failed for stock elements. Fan-out exists only as spatial composition
   through `glvideomixer`; it cannot supply two separately sampleable inputs. Resolved by the custom
   element (`spikes/gl-custom-element/`), which supplies two separately sampleable inputs to one
   shader.
3. **Per-frame uniform update.** Not exercised by this prototype.
4. **Compile-error surface.** Not exercised by this prototype; §5 still requires the Phase 4
   prototype to identify the exact surface.
5. **Working space.** Not exercised by this prototype. Extended-linear half-float compositing remains
   unproven and affects every Pack, not only the 5 (`docs/dependencies.md:137-139`).

**Expressible set.** Decision (c)'s *stock* expressible set excludes the 5 named-intermediate Packs:
the reference `gaussian-blur` pack, the reference `bloom-pulse` pack, the reference `glow` pack, the reference `motion-blur` pack,
the reference `zoom-blur` pack. The custom-element spike shows they need not fall back to R9: a custom
`GstGLMixer` subclass can host the `across`→`down`→`effect` shape. The record now expects these 5 to
take the **custom-element path**, not the R9 downgrade. That path is a cost, not a free choice: it
requires a custom GStreamer element to build, ship and maintain, against R9's reduced function (a
capability record plus a warn/block) or a single-pass re-author that loses the `shrink` optimization
while keeping the look. Whether the project accepts that custom-element cost, or the owner chooses
the R9/single-pass path instead, remains the owner's call (§10.4). The spike proved the *mechanism*,
not that a production-quality element is built.

**Custom element — closed, affirmatively.** Spike `spikes/gl-custom-element/` (`gltexmix.c`,
`main.c`, `shaders.h`, `run.sh`, `verify.py`, `README.md`) answers the question the multi-pass
prototype left open. `gltexmix` subclasses `GstGLMixer`, overrides `process_textures`, dispatches GL
work via `gst_gl_context_thread_add`, and binds each sink pad's `current_texture`
(`gst_gl_mixer_pad_get_current_texture`) to `GL_TEXTURE0 + i`, with an explicit `glUniform1i` per
sampler so the silent-aliasing trap cannot mask a failure. What is now proven:

- **N input textures reach one fragment shader.** The element logs `inputs=2 texture-ids=[3,2]`
  (`[2,3]` / `[2,1]` on other cases): distinct texture ids, not one texture aliased.
- **The output depends on both inputs.** Pixel measurement, 320×240: `baseline` RGB(122,127,144) —
  the SMPTE-bar average, matching the multi-pass spike's baseline; `combine` RGB(255,255,0) = YELLOW
  (red + green, so both textures are genuinely sampled); `tex1-only` RGB(0,255,0) = GREEN (control,
  one input); `named` RGB(128,128,128) (control, constant).
- **Frames stay GPU-resident.** All four cases reach EOS with `frames=30 glmem_frames=30`, caps
  `GLMemory`, `texture-target=2D`, no readback.

The named-intermediate model (`ShaderPass::stages`, `ShaderPass.h:93-111,149`) is therefore
**expressible** — at the cost of a custom element. `GstGLMixer` (not `GstGLFilter`) is the base class
that carries multiple inputs. This closes the "single most important thing still unproven": the
difference is no longer a *possibility* but a *known custom-element cost* for the 5 Packs.

**One implementation trap, recorded.** `gst_gl_shader_compile_attach_stage` **takes ownership** of
the stage, so unrefing it on success is a double-unref (`gst_glsl_stage_compile` assertion, `ref_count
> 0` critical, then use-after-free). Stock `gstglfiltershader.c` does not unref after success; neither
should a custom element.

**Supporting dependency.** Uploading `vec2`/`vec3`/`vec4` uniforms to `glshader` requires
**graphene-gobject-1.0** (already a transitive dependency of `gstreamer-gl`; now declared explicitly
by the shader-chain adapter target).

**Documented R9 fallback (per §4.3).** A Pack whose passes are not expressible carries a capability
record; the project preserves `{plugin_id, version, parameters}` (`docs/architecture.md:159-161`);
the export warns or blocks unless the user bypasses or uses a saved render (R9,
`docs/architecture.md:42`).

## 7. Decision 5 — The `FramePlan`/effects seam

The seam is the resolved pass list, not `FramePlan` — `compositor-path.md` §3 removed the
`FramePlan` boundary. The path is:

1. `src/effects/` (engine-free) resolves a Pack's schema plus a clip's `AppliedFilter`s into
   `std::vector<core::ShaderPass>` (filling `Treatment::passes_at`; `Resolve.cpp:394-415`) and, for
   a cut, a `core::TransitionPass`.
2. `src/render/` already carries the inputs: `BuiltTimeline.chains` (`Resolve.h:152-154`),
   `treatments` (`Resolve.h:158`), `transitions` (`Resolve.h:160`).
3. The GES adapter — the only target that sees GStreamer (`docs/decisions/engine-adapter-contract.md` §3) — translates
   each resolved `ShaderPass` into a `glshader` element in the clip's chain and each
   `TransitionPass` into the cut combine; it is wired at build time (`GesBuilder.cpp:836,890,910-933`).

`EngineSession` needs no change: effects execute inside the pipeline, and the session surface stays
host-typed (`FrameTexture`/`GlHandles`/`FrameSink`; `EngineSession.h:18-38`). `ShaderPass` crosses
only into the adapter, which is permitted (it is a `genesis_host` type). After this decision, the
adapter's parameter delivery changes from a WGSL byte buffer to a `GstStructure` on
`glshader.uniforms`, and `source` carries GLSL (§4.3).

## 8. Consequences

- **Cheap now.** Host session and adapter boundaries are unaffected; effects resolve through a new
  engine-free `src/effects/` layer that links no engine.
- **Expensive now.** 98 GLSL re-authors plus the prelude; the 5 named-intermediate Packs fall outside
  the *stock* expressible set (§6) and are expected to take the custom-element path, at a cost (a
  custom GStreamer element to build, ship and maintain) versus the R9 or single-pass downgrade; the
  naga load-time budget is not carried and must be re-derived or accepted as a security reduction.
- **Working space unresolved.** Extended-linear half-float compositing is not exercised and the
  element advertises `RGBA`; effect correctness in the reference editor's working space is therefore unproven
  (`docs/dependencies.md:137-139`).
- **Licensing.** Re-authored shaders; any reused reference asset keeps its GPL-3.0 notice.
- **Phase 4 scope.** The Pack control schema and catalogue are Phase 4 host artifacts; the Extension
  SDK, R7 install pipeline, and R8 example are separate Phase 4 deliverables
  (`docs/history/rewrite.md:382-405`).

## 9. Revisit triggers

1. Resolved (2026-09-29, `spikes/gl-custom-element/`): a custom element binds the named
   intermediates, so (c) is retained with a custom-element cost for the 5 Packs. Re-open only if the
   owner declines that cost and chooses R9 or single-pass re-author, or if the element cannot be made
   production-quality.
2. A need appears for an effect `glshader` cannot express at all (per-layer effect, or multiple
   inputs), re-opening `compositor-path.md` §6 trigger 2.
3. The working space proves unavailable through `glshader`, forcing either a colour policy or a
   compositor revisit.
4. A C++ WGSL/spirv path becomes available and is preferred (re-opens decision 3).

## 10. Open questions for the owner

1. **Effect scope (§7 Q4).** Must all 123 packages be re-authored, or a curated subset? Only 5 touch
   the multi-pass graph; the 25 audio chains are FFmpeg and are a different track.
2. **Working space.** Is extended-linear half-float a correctness requirement for the first parity
   target, or may effects run 8-bit with a documented colour policy?
3. **Fallback acceptance.** Is the R9 warn/block acceptable for the first target, or must every
   package render?
4. **Custom element.** The §6 spike proves a custom `GstGLMixer` element can bind the named
   intermediates, so the 5 named-intermediate Packs can be supported without R9 or a single-pass
   re-author. Remaining owner calls: is a custom GStreamer element in scope, under which license, and
   is the custom-element cost accepted over the R9/single-pass downgrade?
5. **Validation.** Re-derive a static GLSL budget, or rely on the pipeline-build compile plus the
   trial timeout?

## 11. Evidence index

| Fact | Location |
|---|---|
| HIGH RISK; 123 packages vs Extension/Pack | `docs/history/rewrite.md:146-170` (§3(b)) |
| the reference effects crate verdict / module | `docs/history/rewrite.md:74`; `docs/architecture.md:71-75` |
| Phase 4 scope; `effect-runtime.md (not created)` artifact | `docs/history/rewrite.md:382-405` |
| Compositor verdict; effects re-authored | `docs/decisions/compositor-path.md:65-76`; §5 |
| Chained `glshader` composes; `GLMemory` | `docs/dependencies.md:112-114` |
| `glshader` no varying/uniform injection | `docs/dependencies.md:126-128` |
| Working space / scopes still open | `docs/dependencies.md:137-139` |
| `glshader` `fragment`/`vertex`/`uniforms`, one sink pad | `gst-inspect-1.0 glshader` (1.28.6) |
| `glvideomixer` present; `glmixer`/`glcompute` absent | `gst-inspect-1.0 glvideomixer` / `glmixer` / `glcompute` |
| Multipass spike source; evidence dir | `spikes/gl-multipass/` (`main.c`, `shaders.h`, `run.sh`, `verify.py`); `/tmp/opencode/gl-multipass/` |
| Linear chain expressible: EOS, 30/30 `GLMemory`, `texture-target=2D`; `invert` == `255 − baseline`, 0 px | `/tmp/opencode/gl-multipass/chain*` |
| Fan-out/fan-in expressible, GPU-resident: `tee` → 2 `glshader` → `glvideomixer`, 0 px off, left RED / right GREEN | `/tmp/opencode/gl-multipass/fanout*` |
| Linear chain exposes only the previous pass: `RED ! GREEN ! identity` = all green | `/tmp/opencode/gl-multipass/named-linear*` |
| `glshader` binds one texture; a second `sampler2D` silently aliases unit 0, md5-identical to baseline | `/tmp/opencode/gl-multipass/named-extra-sampler*` |
| `glvideomixer` composites spatially, not two separately sampleable inputs | `/tmp/opencode/gl-multipass/fanout*` |
| Fold N stages into one shader only when every stage reads the same source; 5×5 box == `H-blur ! V-blur`, 0 px | `/tmp/opencode/gl-multipass/approx-fold*` |
| `shrink` unrepresentable: `glshader ! width=160,height=120 ! glshader` fails `reason not-linked`, 0 frames | `/tmp/opencode/gl-multipass/shrink*` |
| Without the RGBA capsfilter, `glcolorconvert` emits `DMABuf` (`drm-format=Y412`), not a texture | `spikes/gl-multipass/`; `/tmp/opencode/gl-multipass/approx-2pass*` |
| Custom-element spike source | `spikes/gl-custom-element/` (`gltexmix.c`, `main.c`, `shaders.h`, `run.sh`, `verify.py`, `README.md`) |
| Custom `GstGLMixer` subclass binds N pad textures: `process_textures` + `gst_gl_context_thread_add`, `current_texture` → `GL_TEXTURE0+i` with per-sampler `glUniform1i` | `spikes/gl-custom-element/gltexmix.c` |
| Two distinct inputs arrive: `inputs=2 texture-ids=[3,2]` (`[2,3]` / `[2,1]` on other cases), not one aliased texture | `spikes/gl-custom-element/` |
| Output depends on both inputs: `baseline` RGB(122,127,144); `combine` RGB(255,255,0) YELLOW; `tex1-only` RGB(0,255,0) GREEN; `named` RGB(128,128,128); 320×240 | `spikes/gl-custom-element/verify.py`; PNGs under `/tmp/opencode/gltexmix-out/` |
| All cases GPU-resident: EOS, `frames=30 glmem_frames=30`, `GLMemory`, `texture-target=2D`, no readback | `spikes/gl-custom-element/` |
| Ownership trap: `gst_gl_shader_compile_attach_stage` takes ownership of the stage; unref after success = double-unref (assertion + `ref_count > 0` critical, then use-after-free); stock `gstglfiltershader.c` does not unref | `spikes/gl-custom-element/gltexmix.c`; stock `gstglfiltershader.c` |
| `vec2`/`vec3`/`vec4` uniforms to `glshader` need `graphene-gobject-1.0` (transitive from `gstreamer-gl`; now explicit in the shader-chain adapter target) | shader-chain adapter target |
| Custom element does not test `shrink`, half-float, production robustness, or performance | `spikes/gl-custom-element/` |
| Manifest format 2; scene-linear | `manifest.rs:28,31` |
| Manifest fields; params; kinds | `manifest.rs:34-244` |
| Compound params own dotted keys | `manifest.rs:284-290,780-795` |
| Pass declarations; MAX_PASSES; MAX_SHRINK | `manifest.rs:431-451` |
| Transition table; XFADE_NAMES | `manifest.rs:348-363` |
| Shader contract; host preludes | `shader.rs:4-42,55-91,178-239` |
| naga parse + validate | `shader.rs:1484-1495` |
| Binding + bounded-loop budget | `shader.rs:1056-1136` |
| Pass read-order check | `shader.rs:1184-1232` |
| Params layout read from naga | `shader.rs:1506-1566` |
| Multi-pass stitching / entry points | `shader.rs:1142-1177` |
| Pipeline cache key | `shader.rs:1398-1403` |
| Catalogue pass resolution | `catalogue.rs:1088-1106,1112-1127` |
| Trial: 512 px, 3 s timeout | the reference checkout's `src/studio.rs:6439-6452`; the reference render crate's `src/gpu.rs:1787-1851` |
| Allowed FFmpeg filters | the reference effects crate's `src/filters.rs:20-` |
| Ported `Lut`/`RevealMap`/`Stage`/`ShaderPass`/`TransitionPass` | `src/core/ShaderPass.h:27,64,96,114,166` |
| `source` documented as WGSL; `stages`; `MIN_PARAMS` | `src/core/ShaderPass.h:117,128-129,149,170-172` |
| `BuiltTimeline.chains`/`treatments`/`transitions` | `src/render/Resolve.h:152-160` |
| `Treatment::passes_at` catalogue resolution | `src/render/Resolve.cpp:394-415` |
| `AppliedFilter` host type | `src/project/model/Effects.h:425` |
| Effects/treatments/transitions wired in the adapter | `src/adapters/engine/ges/session/GesBuilder.cpp:836,890,910-933` |
| Engine-neutral session surface | `src/adapters/engine/EngineSession.h:18-38` |
| Host target links no engine; adapter is GES-only | `docs/decisions/engine-adapter-contract.md` §3 |
| R5–R8; control schema is host declarative data | `docs/architecture.md:38-42,166-209` |

See `docs/history/rewrite.md` §3(b)/§4 Phase 4 for the annotated risk and phase, `docs/architecture.md`
§1/§9 for the terminology and manifest contract, `docs/decisions/compositor-path.md` for the render
verdict this record sits under, and `docs/decisions/engine-adapter-contract.md` for the seam it extends.
