# Genesis-0 — Effect Animation Decision

Status: Accepted (2026-09-30).

Date: 2026-09-30. Deciders: project owner + orchestrator.

Status note (2026-09-30): ordinary per-clip effect animation is integrated end to end — the
per-buffer probe rides keyed knobs through the production builder to the clip's top-effect
glshaders (`src/adapters/engine/ges/effects/ClipEffects.cpp`, consumed at
`src/adapters/engine/ges/session/GesBuilder.cpp:392-399,469-473`). This is bounded: only clip effects
expressible as one glshader at unit speed install; custom-GLSL/texture-sampling effects and the
`genesis.reveal` title path are out of scope for this decision (reveal is a separate mechanism,
built and wired for title clips - `src/adapters/engine/ges/effects/RevealEffect.h`). CI
has not been re-run since the edits below.

Scope: how a **layer clip's effect (a treatment)** animates once it is on the timeline — whether the
GES graph is rebuilt per frame to ride keyed knobs, or built once and only its uniforms re-uploaded.
It is **not** the effect execution path (fixed by `effect-execution.md`) or the present path (fixed by
`compositor-path.md`); it is the seam that sits on top of the former and feeds the latter.

---

## 1. Context

A treatment's effects carry keyed knobs: `genesis.exposure` declares `stops` with `animate = true`, so
its value at frame *t* is a function of *t* (`packs/genesis.exposure/effect.toml`). The host resolver
already resolves a treatment's passes at an arbitrary time — `Treatment::passes_at` normalises the time
to the layer's span and hands the chain to the catalogue (`src/render/Resolve.cpp:257-278`). What was
undecided was how the engine consumes that per-time resolution: the two live options were (a) rebuild
the glshader chain every frame, or (b) build the chain once and push only the changed uniform values.

Rebuilding per frame is structurally attractive — the chain always matches the resolution exactly — but
it tears down and re-creates GStreamer elements on the streaming thread every buffer, and any structural
change (a pass appearing or vanishing because a knob crossed a threshold) is not something a streaming
thread can safely do. The uniform-upload option keeps the graph static and sends only scalars/vectors
through glshader's `uniforms` property, which is exactly what a keyed knob needs to ride.

A second, independent problem rode along: the resolution path read `effect.glsl` from disk on every
`passes_for` call, so the probe's per-frame re-resolve would re-read the shader body once per frame —
a hot-path disk I/O that the header already promised against (`src/render/Catalogue.h:25`).

## 2. Decision

**Build the treatment's glshader chain once at build time; ride keyed knobs by re-resolving per buffer
and re-uploading only the uniforms that changed.** The graph is static; no element is created or
destroyed on the streaming thread. The per-frame cost is one pass-vector resolution plus an
allocation-free equality check; only a frame whose knobs moved uploads. The shader-body read is cached
in the catalogue, so the re-resolve no longer touches disk.

## 3. The graph is static; uniforms ride per buffer

`GesBuilder::build_timeline` installs each treatment's chain exactly once, ahead of track creation.
`resolve_treatment` resolves the passes at the treatment's `start` and composes their fragment sources
(`src/adapters/engine/ges/session/GesBuilder.cpp:168-183`); a `GESEffectClip` is created from a bare
`"glshader name=t0 ! ..."` description, one element per pass (`:188-198`). After `add_track`
materialises the effect's bin, the builder looks each named glshader up, sets its `fragment` and its
build-time `uniforms`, and attaches the probe to the first glshader's sink pad (`:394-429`). The
uniforms then ride per buffer through that probe — the graph never changes shape.

`attach_treatment_probe` (`src/adapters/engine/ges/effects/ShaderChain.cpp:285-298`) installs
`treatment_buffer_probe` on the sink pad; on each buffer it reads the buffer's PTS (absolute timeline
time, the same seam `to_ns` writes on the way in) and calls `update_uniforms` (`:264-281`). The probe
runs on the streaming thread, so it touches only the `TreatmentChain` it owns and the glshader it
already holds — nothing the UI owns (`src/adapters/engine/ges/effects/ShaderChain.h:106-113`).

### 3.1 Ordinary per-clip effects read source media time

An ordinary clip's effects take a separate path. `install_clip_effects` adds one GES top effect
per expressible pass to the clip itself; after `add_track`, `configure_clip_effects` finds each
effect's glshader and attaches a single per-buffer probe to the first shader's sink pad
(`src/adapters/engine/ges/effects/ClipEffects.cpp:106-296`). That probe reads the same PTS the top effects
see, and because the top effects sit between the source and the rest of the pipeline — before GES
restamps timestamps — it is **source media time**, not timeline time. The clip-local fraction is
therefore `(pts - source_start_ns) / duration_ns`, clamped to 0..1
(`src/adapters/engine/ges/effects/ClipEffects.cpp:298-306`). The formula holds at unit speed only; a clip
whose `speed != 1` is refused with a warning so a keyed knob cannot mis-time
(`src/adapters/engine/ges/session/GesBuilder.cpp:372-378`). Layer treatment timing is **separate** and
unchanged: a treatment's effect clip reads absolute timeline time as described above in §3.

## 4. Expressibility: all-or-nothing

A treatment whose pass cannot run as one glshader is refused whole, before any element is created.
`pass_is_expressible` (`src/adapters/engine/ges/effects/ShaderChain.cpp:135-147`) applies the same three
refusals `append_shader_chain` does: a pre-stage (the named-intermediate model), a field whose value
glshader's `uniforms` cannot carry (a curve's `vec4[8]`), or a fragment source that declares a second
sampler. `resolve_treatment` declines the whole treatment on any inexpressible pass
(`src/adapters/engine/ges/session/GesBuilder.cpp:175-183`), so a layer never renders a partial effect; it
renders nothing, matching the R9 fallback for a clip effect.

## 5. The per-frame re-resolve cost

`update_uniforms` re-resolves `treatment.passes_at(time)` per call (`src/adapters/engine/ges/
ShaderChain.cpp:232-254`). The re-resolve allocates a fresh pass vector; the comparison that follows
(`ShaderPass::operator==`) is allocation-free, so a frame whose knobs have not moved uploads nothing
and pays only the resolution. This is the documented cost of riding keyed knobs. Two bounds keep that
cost off the hot path:

- **The body read is cached.** `resolve_pass` lays out `params` but never reads the shader source; the
  body was read later, in `Catalogue::passes_for`, once per call (`src/render/Catalogue.cpp:139-157`).
  That made the probe's per-frame re-resolve re-read `effect.glsl` from disk every frame — the defect
  this record fixes. `passes_for` now looks the body up in a per-pack-id cache (`bodies_`), computing it
  only on the first resolve and caching a missing body too (nullptr), so disk I/O happens once per pack
  per session (`src/render/Catalogue.cpp:140-156`, `src/render/Catalogue.h:57-64`).
- **A structural change cannot ride.** A pass-count change between the built chain and a later
  resolution is a structural change a probe cannot rebuild; only the shared prefix maps to a glshader,
  so `update_uniforms` uploads that prefix and skips the rest (`src/adapters/engine/ges/
  ShaderChain.cpp:243-251`). A knob that flips a pass in or out is therefore a documented limitation,
  not a silent mis-render.

The cache is `mutable` and lock-free: `passes_for` is `const` and runs on the streaming thread, but a
`std::mutex` member would delete the catalogue's move constructor (which the factory's
`return catalogue;` relies on). It is safe because every pack a treatment uses is resolved at build time
(single-threaded) before streaming begins; the streaming thread only ever reads a cached entry
(`src/render/Catalogue.h:57-64`).

## 6. Consequences

**What this forecloses (for now).** Rebuilding the chain per frame, and with it any per-frame structural
change (a pass appearing or vanishing because a knob crossed a threshold). A treatment whose pass set
is not time-invariant keeps only the shared prefix of its built chain.

**What stays required.** `resolve_treatment` at build time still composes fragment sources and uploads
build-time uniforms, so the treatment renders correctly at its starting frame before the first probe
fires; the probe seeds `chain.last` with the build-time resolution, so the first buffer uploads nothing
when the knobs still read the same (`src/adapters/engine/ges/session/GesBuilder.cpp:332-334`).

**Follow-up.** The stale note in `src/adapters/engine/ges/effects/ShaderChain.h:100` and the `update_uniforms`
comment ("reads the pack body per call") predates the catalogue body cache; it should be corrected to
say the re-resolve allocates a pass vector per call, but this record's files were out of scope for that
edit.

**Residual risk carried forward.** The lock-free `mutable` cache is safe under the build-then-stream
ordering described in §5; if a future path resolves packs lazily *during* streaming from multiple
threads, that ordering argument must be revisited.

## 7. Alternatives considered

### 7.1 Rebuild the chain every frame (not selected)

Tear down and re-create the glshader elements each buffer so the graph always matches the resolution.
Rejected because element creation/destruction on the streaming thread is unsafe, a structural change
cannot be applied mid-stream anyway, and the win (a chain that tracks a structural change) is exactly
the case §5 says is unsupported either way.

### 7.2 Keyframe the uniforms as GES keyframes (not selected)

Drive glshader's `uniforms` through GES's own keyframe/interpolation machinery. Rejected because GES
keyframes operate on element properties with no mapping to the structured `uniforms` value, and it
would fork the knob evaluation away from the host's `passes_at`, risking two different answers for the
same knob at the same time.

### 7.3 A mutex-guarded body cache (not selected)

Guard `bodies_` with a `std::mutex`. Rejected because a mutex member deletes the move constructor that
a static `Catalogue` factory's `return catalogue;` depends on, for a lock that §5's build-then-stream
ordering shows is never contended.

## 8. Revisit triggers

1. **A treatment whose pass set is time-variant** (a knob that crosses a threshold and adds or drops a
   pass) needs a structural rebuild; revisit the "static graph" decision then.
2. **A path resolves packs lazily from multiple threads during streaming**, breaking the
   build-then-stream ordering the lock-free cache relies on.
3. **glshader gains array uniform support**, removing the `vec4[8]` refusal and widening what a single
   pass can express.

## 9. Evidence index

| Fact | Location |
|---|---|
| Keyed knob on a treatment (exposure `stops`, `animate = true`) | `packs/genesis.exposure/effect.toml` |
| Per-time resolution entry point | `src/render/Resolve.cpp:257-278` |
| Treatment carries no clip id; bare clip handed to catalogue | `src/render/Resolve.cpp:267-277` |
| Chain installed once at build time; probe attached after `add_track` | `src/adapters/engine/ges/session/GesBuilder.cpp:302-336,394-429` |
| Fragment composition and all-or-nothing refusal | `src/adapters/engine/ges/session/GesBuilder.cpp:168-183` |
| Bare `"glshader name=t0"` bin description | `src/adapters/engine/ges/session/GesBuilder.cpp:188-198` |
| `pass_is_expressible` (three refusals) | `src/adapters/engine/ges/effects/ShaderChain.cpp:135-147` |
| `uniforms_for` (float / graphene vec2/3/4) | `src/adapters/engine/ges/effects/ShaderChain.cpp:70-97` |
| `update_uniforms` re-resolve + allocation-free compare + shared-prefix bound | `src/adapters/engine/ges/effects/ShaderChain.cpp:232-254` |
| Probe reads PTS and re-resolves; streaming-thread ownership | `src/adapters/engine/ges/effects/ShaderChain.cpp:264-281` |
| `attach_treatment_probe` | `src/adapters/engine/ges/effects/ShaderChain.cpp:285-298` |
| Body read cached per pack id; missing body cached | `src/render/Catalogue.cpp:139-157` |
| `mutable` lock-free cache and its movability rationale | `src/render/Catalogue.h:57-64` |
| `passes_for` is `const` | `src/render/Catalogue.h:44-45` |
| Body file fixed name and `read_body` contract | `src/effects/PackSource.h:33-44` |
| `ShaderPass::operator==` compares params/values, not `source` | `src/core/ShaderPass.h` |
| Ordinary per-clip install / configure / probe | `src/adapters/engine/ges/effects/ClipEffects.cpp:106-296` |
| Ordinary clip fraction from source media time + unit-speed refusal | `src/adapters/engine/ges/effects/ClipEffects.cpp:298-306`; `src/adapters/engine/ges/session/GesBuilder.cpp:372-378` |
| Behavioural proof: ordinary clip chain renders through the production builder | `tests/adapters/ClipEffectTests.cpp`, `tests/adapters/ChainConsumptionTests.cpp` |
| Behavioural proof: cache, chain install, probe no-op/change | `tests/adapters/EffectApplicationTests.cpp`, `tests/render/CatalogueTests.cpp` |

See `docs/decisions/effect-execution.md` §6/§7 for the execution-path constraints this record builds on,
and `docs/decisions/compositor-path.md` for the present path the animated treatment feeds.
